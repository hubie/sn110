/*
 * sn110dmx — Main daemon for Strand SN110 open firmware
 *
 * Multi-protocol DMX gateway daemon supporting:
 *   - sACN (E1.31) — modern multicast streaming DMX
 *   - Art-Net — widely-used DMX over IP
 *   - ShowNet — legacy Strand protocol compatibility
 *
 * Architecture: single-threaded event loop
 *   1. select() on network sockets with 23ms timeout
 *   2. Process any received protocol packets → update DMX buffers
 *   3. Write fresh DMX data to /dev/dmxN hardware
 *   4. Check for source timeouts → zero DMX on timeout
 *
 * The ~23ms cycle gives ~44Hz DMX refresh rate, matching DMX512 spec.
 * Single-threaded avoids clone() instability on uClinux/Linux 2.0.
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

#include "common.h"
#include "dmx/dmx.h"
#include "sacn/sacn.h"
#include "sacn/sacn_tx.h"
#include "artnet/artnet.h"
#include "shownet/shownet.h"
#include "config/config.h"

#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>

/* PID file path — matches what /etc/rc watchdog checks */
#define PID_FILE "/var/run/lxnetdmx.pid"

#ifdef HOST_BUILD
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#define LOG(fmt, ...) fprintf(stderr, "[sn110dmx] " fmt "\n", ##__VA_ARGS__)
#else
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#define LOG(fmt, ...) dprintf(2, "[sn110dmx] " fmt "\n", ##__VA_ARGS__)
#endif

/* ========================================================================= */
/* Global state (single-threaded — no mutex needed)                          */
/* ========================================================================= */

static dmx_frame_t  g_dmx_out[DMX_MAX_PORTS];    /* Network → DMX output */
static dmx_frame_t  g_dmx_in[DMX_MAX_PORTS];     /* DMX input → Network */
static sacn_tx_t    g_sacn_tx[DMX_MAX_PORTS];     /* sACN transmitters for RX ports */
static int          g_sacn_tx_active[DMX_MAX_PORTS]; /* 1 if sacn_tx initialized */
static uint32_t     g_dmx_in_last_send[DMX_MAX_PORTS]; /* Last sACN TX timestamp */
static volatile int    g_running = 1;
static node_config_t   g_config;
static uint32_t g_sacn_rx_count = 0;
static uint32_t g_artnet_rx_count = 0;
static uint32_t g_shownet_rx_count = 0;
static uint32_t g_dmx_tx_count = 0;
static uint32_t g_sacn_tx_count = 0;

/* ========================================================================= */
/* Timestamp helper                                                          */
/* ========================================================================= */

static uint32_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

/* ========================================================================= */
/* Thread 1: Network receiver                                                */
/* ========================================================================= */

static void handle_sacn_packet(const sacn_packet_t *pkt)
{
    int port;
    if (pkt->start_code != 0)
        return; /* Only handle DMX start code 0 */

    g_sacn_rx_count++;

    for (port = 0; port < DMX_MAX_PORTS; port++) {
        if (g_config.ports[port].universe != pkt->universe)
            continue;
        if (g_config.ports[port].mode != DMX_MODE_TX)
            continue;

        /* Priority-based merging: higher priority wins */
        if (pkt->priority >= g_dmx_out[port].priority ||
            now_ms() - g_dmx_out[port].last_update_ms > SACN_TIMEOUT_MS) {
            memcpy(g_dmx_out[port].data, pkt->dmx_data, pkt->dmx_length);
            g_dmx_out[port].length = pkt->dmx_length;
            g_dmx_out[port].priority = pkt->priority;
            g_dmx_out[port].sequence = pkt->sequence;
            g_dmx_out[port].last_update_ms = now_ms();
        }
    }
}

static void handle_artnet_packet(const artnet_dmx_packet_t *pkt)
{
    int port;
    for (port = 0; port < DMX_MAX_PORTS; port++) {
        if (g_config.ports[port].universe != pkt->universe)
            continue;
        if (g_config.ports[port].mode != DMX_MODE_TX)
            continue;

        memcpy(g_dmx_out[port].data, pkt->dmx_data, pkt->dmx_length);
        g_dmx_out[port].length = pkt->dmx_length;
        g_dmx_out[port].sequence = pkt->sequence;
        g_dmx_out[port].last_update_ms = now_ms();
    }
}

static void handle_shownet_packet(const shownet_packet_t *pkt)
{
    int port;
    /* ShowNet uses network slots (1-based). Map to ports by slot range. */
    uint16_t universe = (pkt->net_slot - 1) / DMX_UNIVERSE_SIZE;

    for (port = 0; port < DMX_MAX_PORTS; port++) {
        if (g_config.ports[port].universe != universe)
            continue;
        if (g_config.ports[port].mode != DMX_MODE_TX)
            continue;

        memcpy(g_dmx_out[port].data, pkt->dmx_data, pkt->dmx_length);
        g_dmx_out[port].length = pkt->dmx_length;
        g_dmx_out[port].last_update_ms = now_ms();
    }
}

/* ========================================================================= */
/* Signal handler                                                            */
/* ========================================================================= */

static void signal_handler(int sig)
{
    if (sig == 15) /* SIGTERM */
        g_running = 0;
}

/* ========================================================================= */
/* PID file management                                                       */
/* ========================================================================= */

#ifndef HOST_BUILD
static void write_pid_file(void)
{
    char buf[16];
    int fd, n;
    pid_t pid = getpid();

    n = snprintf(buf, sizeof(buf), "%d\n", (int)pid);
    fd = open(PID_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, buf, n);
        close(fd);
    }
}

static void remove_pid_file(void)
{
    unlink(PID_FILE);
}
#endif

/* ========================================================================= */
/* DMX output — write fresh data to hardware ports                           */
/* ========================================================================= */

static void dmx_output_cycle(const dmx_ops_t *ops, int *fds,
                             uint32_t *last_write)
{
    uint32_t t = now_ms();
    uint8_t frame[DMX_UNIVERSE_SIZE];
    int i;

    for (i = 0; i < DMX_MAX_PORTS; i++) {
        if (fds[i] < 0)
            continue;
        if (g_config.ports[i].mode != DMX_MODE_TX)
            continue;

        /* Check if we have fresh data */
        if (g_dmx_out[i].last_update_ms > last_write[i]) {
            memcpy(frame, g_dmx_out[i].data, DMX_UNIVERSE_SIZE);
            last_write[i] = g_dmx_out[i].last_update_ms;

            ops->write_frame(fds[i], frame, DMX_UNIVERSE_SIZE);
            g_dmx_tx_count++;
        } else {
            /* Check for source timeout */
            uint32_t age = t - g_dmx_out[i].last_update_ms;
            int timed_out = (g_dmx_out[i].last_update_ms > 0) &&
                            (age > (uint32_t)g_config.dmx_hold_time * 1000);

            if (timed_out) {
                memset(g_dmx_out[i].data, 0, DMX_UNIVERSE_SIZE);
                g_dmx_out[i].last_update_ms = 0;
                g_dmx_out[i].priority = 0;
                memset(frame, 0, DMX_UNIVERSE_SIZE);
                ops->write_frame(fds[i], frame, DMX_UNIVERSE_SIZE);
            }
        }
    }
}

/* ========================================================================= */
/* DMX input — read hardware ports and send sACN                             */
/* ========================================================================= */

#define SACN_KEEPALIVE_MS 900  /* Re-send if no change within 900ms */

static void dmx_input_cycle(const dmx_ops_t *ops, int *fds)
{
    uint32_t t = now_ms();
    uint8_t frame[DMX_UNIVERSE_SIZE];
    int i, n;

    for (i = 0; i < DMX_MAX_PORTS; i++) {
        if (fds[i] < 0)
            continue;
        if (g_config.ports[i].mode != DMX_MODE_RX)
            continue;
        if (!g_sacn_tx_active[i])
            continue;

        /* Non-blocking read from DMX port */
        n = ops->read_frame(fds[i], frame, DMX_UNIVERSE_SIZE);
        if (n <= 0) {
            g_dmx_in_zero_count++;
            /* No data available — check keep-alive */
            if (g_dmx_in[i].length > 0 &&
                (t - g_dmx_in_last_send[i]) > SACN_KEEPALIVE_MS) {
                /* Re-send last data as keep-alive */
                if (sacn_tx_send(&g_sacn_tx[i], g_dmx_in[i].data,
                                 g_dmx_in[i].length) == 0) {
                    g_dmx_in_last_send[i] = t;
                    g_sacn_tx_count++;
                }
            }
            continue;
        }

        /* Check if data changed or keep-alive needed */
        if (n == g_dmx_in[i].length &&
            memcmp(frame, g_dmx_in[i].data, n) == 0 &&
            (t - g_dmx_in_last_send[i]) < SACN_KEEPALIVE_MS) {
            continue; /* Same data, no keep-alive needed yet */
        }

        /* Update stored frame */
        memcpy(g_dmx_in[i].data, frame, n);
        g_dmx_in[i].length = n;
        g_dmx_in[i].last_update_ms = t;

        /* Send sACN */
        if (sacn_tx_send(&g_sacn_tx[i], frame, n) == 0) {
            g_dmx_in_last_send[i] = t;
            g_sacn_tx_count++;
        }
    }
}

/* ========================================================================= */
/* Main — single-threaded event loop                                         */
/* ========================================================================= */

int main(int argc, char *argv[])
{
    const char *config_path = CONFIG_FILE_PATH;
    const dmx_ops_t *ops;
    int sacn_sock = -1, artnet_sock = -1, shownet_sock = -1;
    int dmx_fds[DMX_MAX_PORTS];
    uint32_t dmx_last_write[DMX_MAX_PORTS] = {0};
    int i;

    LOG("sn110dmx v0.2.0 — Open-source DMX gateway");
    LOG("Strand SN110 multi-protocol firmware");

    /* Parse optional config path argument */
    if (argc > 1)
        config_path = argv[1];

    /* Load configuration */
    if (config_load(config_path, &g_config) < 0) {
        LOG("No config at %s, using defaults", config_path);
        config_defaults(&g_config);
    }

    LOG("Protocol: %s", g_config.active_protocol == PROTO_SACN ? "sACN" :
        g_config.active_protocol == PROTO_ARTNET ? "Art-Net" :
        g_config.active_protocol == PROTO_SHOWNET ? "ShowNet" : "none");
    for (i = 0; i < DMX_MAX_PORTS; i++) {
        const char *mode_str = g_config.ports[i].mode == DMX_MODE_TX ? "TX" :
                               g_config.ports[i].mode == DMX_MODE_RX ? "RX" : "OFF";
        LOG("Port %d: universe %d, mode %s", i, g_config.ports[i].universe, mode_str);
    }

    /* Initialize shared state */
    memset(g_dmx_out, 0, sizeof(g_dmx_out));
    memset(g_dmx_in, 0, sizeof(g_dmx_in));
    memset(g_sacn_tx_active, 0, sizeof(g_sacn_tx_active));
    memset(g_dmx_in_last_send, 0, sizeof(g_dmx_in_last_send));

    /* Install signal handlers for clean shutdown */
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);
    signal(SIGPIPE, SIG_IGN); /* ignore broken pipe from stderr writes */

#ifndef HOST_BUILD
    write_pid_file();
#endif

    /* Initialize protocol sockets */
    if (g_config.active_protocol == PROTO_SACN) {
        sacn_sock = sacn_init(g_config.ports[0].universe);
        if (sacn_sock < 0)
            LOG("WARNING: failed to init sACN socket");
    } else if (g_config.active_protocol == PROTO_ARTNET) {
        artnet_sock = artnet_init();
        if (artnet_sock < 0)
            LOG("WARNING: failed to init Art-Net");
    } else if (g_config.active_protocol == PROTO_SHOWNET) {
        shownet_sock = shownet_init();
        if (shownet_sock < 0)
            LOG("WARNING: failed to init ShowNet");
    }

    /* Open and configure DMX ports */
    ops = dmx_get_ops();
    for (i = 0; i < DMX_MAX_PORTS; i++) {
        const char *dev = (i == 0) ? DMX_DEVICE_0 : DMX_DEVICE_1;
        int port_mode = g_config.ports[i].mode;

        if (port_mode == DMX_MODE_OFF) {
            dmx_fds[i] = -1;
            continue;
        }

        dmx_fds[i] = ops->open(dev);
        if (dmx_fds[i] < 0) {
            LOG("WARNING: cannot open %s", dev);
            continue;
        }
        ops->set_mode(dmx_fds[i], port_mode);

        /* Initialize sACN transmitter for RX-mode ports */
        if (port_mode == DMX_MODE_RX) {
            char src_name[64];
            snprintf(src_name, sizeof(src_name), "SN110 Port %d", i);
            if (sacn_tx_init(&g_sacn_tx[i], g_config.ports[i].universe,
                             src_name) == 0) {
                g_sacn_tx_active[i] = 1;
            } else {
                LOG("WARNING: sacn_tx_init failed for port %d", i);
            }
        }
    }

    LOG("Running. Send SIGTERM to stop.");

    /* ---- Main event loop ---- */
    while (g_running) {
        fd_set readfds;
        int maxfd = -1;
        struct timeval tv;

        FD_ZERO(&readfds);

        if (sacn_sock >= 0) {
            FD_SET(sacn_sock, &readfds);
            if (sacn_sock > maxfd) maxfd = sacn_sock;
        }
        if (artnet_sock >= 0) {
            FD_SET(artnet_sock, &readfds);
            if (artnet_sock > maxfd) maxfd = artnet_sock;
        }
        if (shownet_sock >= 0) {
            FD_SET(shownet_sock, &readfds);
            if (shownet_sock > maxfd) maxfd = shownet_sock;
        }

        /* 23ms timeout ≈ 44Hz DMX refresh rate */
        tv.tv_sec = 0;
        tv.tv_usec = 23000;

        if (maxfd >= 0 && select(maxfd + 1, &readfds, NULL, NULL, &tv) > 0) {
            /* Handle sACN packets */
            if (sacn_sock >= 0 && FD_ISSET(sacn_sock, &readfds)) {
                sacn_packet_t sacn_pkt;
                if (sacn_receive(sacn_sock, &sacn_pkt) == 0)
                    handle_sacn_packet(&sacn_pkt);
            }

            /* Handle Art-Net packets */
            if (artnet_sock >= 0 && FD_ISSET(artnet_sock, &readfds)) {
                artnet_dmx_packet_t artnet_pkt;
                if (artnet_receive(artnet_sock, &artnet_pkt) == 0)
                    handle_artnet_packet(&artnet_pkt);
            }

            /* Handle ShowNet packets */
            if (shownet_sock >= 0 && FD_ISSET(shownet_sock, &readfds)) {
                shownet_packet_t shownet_pkt;
                if (shownet_receive(shownet_sock, &shownet_pkt) == 0)
                    handle_shownet_packet(&shownet_pkt);
            }
        } else if (maxfd < 0) {
            usleep(23000);
        }

        /* Write DMX output each cycle */
        dmx_output_cycle(ops, dmx_fds, dmx_last_write);

        /* Read DMX input and send sACN each cycle */
        dmx_input_cycle(ops, dmx_fds);
    }

    /* Shutdown */
    LOG("Shutting down...");

    if (sacn_sock >= 0) sacn_cleanup(sacn_sock);
    if (artnet_sock >= 0) artnet_cleanup(artnet_sock);
    if (shownet_sock >= 0) shownet_cleanup(shownet_sock);

    for (i = 0; i < DMX_MAX_PORTS; i++) {
        if (g_sacn_tx_active[i])
            sacn_tx_cleanup(&g_sacn_tx[i]);
        if (dmx_fds[i] >= 0)
            ops->close(dmx_fds[i]);
    }

#ifndef HOST_BUILD
    remove_pid_file();
#endif

    LOG("Shutdown complete.");
    return 0;
}
