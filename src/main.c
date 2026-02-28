/*
 * sn110dmx — Main daemon for Strand SN110 open firmware
 *
 * Multi-protocol DMX gateway daemon supporting:
 *   - sACN (E1.31) — modern multicast streaming DMX
 *   - Art-Net — widely-used DMX over IP
 *   - ShowNet — legacy Strand protocol compatibility
 *
 * Architecture (mirrors original lxnetdmx 4-thread design):
 *   Thread 1: Network receiver — listens for protocol packets
 *   Thread 2: DMX output — writes received data to /dev/dmxN
 *   Thread 3: DMX input — reads from /dev/dmxN (future: for DMX IN ports)
 *   Thread 4: Housekeeping — timeout detection, LCD updates
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

#include "common.h"
#include "dmx/dmx.h"
#include "sacn/sacn.h"
#include "artnet/artnet.h"
#include "shownet/shownet.h"
#include "config/config.h"

#include <string.h>
#include <signal.h>
#include <unistd.h>

#ifdef HOST_BUILD
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/time.h>
#define LOG(fmt, ...) fprintf(stderr, "[sn110dmx] " fmt "\n", ##__VA_ARGS__)
#else
/*
 * uClinux/Linux 2.0 - pthreads via linuxthreads or clone()
 * The original lxnetdmx uses pthreads, so we know they work.
 */
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/time.h>
#define LOG(fmt, ...) dprintf(2, "[sn110dmx] " fmt "\n", ##__VA_ARGS__)
#endif

/* ========================================================================= */
/* Global shared state (protected by mutex)                                  */
/* ========================================================================= */

static dmx_frame_t  g_dmx_out[DMX_MAX_PORTS];    /* Network → DMX output */
static pthread_mutex_t g_dmx_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile int    g_running = 1;
static node_config_t   g_config;

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

    for (port = 0; port < DMX_MAX_PORTS; port++) {
        if (g_config.ports[port].universe != pkt->universe)
            continue;
        if (g_config.ports[port].mode != DMX_MODE_TX)
            continue;

        pthread_mutex_lock(&g_dmx_mutex);

        /* Priority-based merging: higher priority wins */
        if (pkt->priority >= g_dmx_out[port].priority ||
            now_ms() - g_dmx_out[port].last_update_ms > SACN_TIMEOUT_MS) {
            memcpy(g_dmx_out[port].data, pkt->dmx_data, pkt->dmx_length);
            g_dmx_out[port].length = pkt->dmx_length;
            g_dmx_out[port].priority = pkt->priority;
            g_dmx_out[port].sequence = pkt->sequence;
            g_dmx_out[port].last_update_ms = now_ms();
        }

        pthread_mutex_unlock(&g_dmx_mutex);
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

        pthread_mutex_lock(&g_dmx_mutex);
        memcpy(g_dmx_out[port].data, pkt->dmx_data, pkt->dmx_length);
        g_dmx_out[port].length = pkt->dmx_length;
        g_dmx_out[port].sequence = pkt->sequence;
        g_dmx_out[port].last_update_ms = now_ms();
        pthread_mutex_unlock(&g_dmx_mutex);
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

        pthread_mutex_lock(&g_dmx_mutex);
        memcpy(g_dmx_out[port].data, pkt->dmx_data, pkt->dmx_length);
        g_dmx_out[port].length = pkt->dmx_length;
        g_dmx_out[port].last_update_ms = now_ms();
        pthread_mutex_unlock(&g_dmx_mutex);
    }
}

static void *thread_net_receive(void *arg)
{
    int sacn_socks[DMX_MAX_PORTS];
    int artnet_sock = -1;
    int shownet_sock = -1;
    int i;
    fd_set readfds;
    int maxfd;
    struct timeval tv;

    (void)arg;

    /* Initialize protocol sockets based on active protocol */
    memset(sacn_socks, -1, sizeof(sacn_socks));

    if (g_config.active_protocol == PROTO_SACN) {
        for (i = 0; i < DMX_MAX_PORTS; i++) {
            sacn_socks[i] = sacn_init(g_config.ports[i].universe);
            if (sacn_socks[i] < 0)
                LOG("WARNING: failed to init sACN for universe %d",
                    g_config.ports[i].universe);
        }
    } else if (g_config.active_protocol == PROTO_ARTNET) {
        artnet_sock = artnet_init();
        if (artnet_sock < 0)
            LOG("WARNING: failed to init Art-Net");
    } else if (g_config.active_protocol == PROTO_SHOWNET) {
        shownet_sock = shownet_init();
        if (shownet_sock < 0)
            LOG("WARNING: failed to init ShowNet");
    }

    while (g_running) {
        FD_ZERO(&readfds);
        maxfd = -1;

        for (i = 0; i < DMX_MAX_PORTS; i++) {
            if (sacn_socks[i] >= 0) {
                FD_SET(sacn_socks[i], &readfds);
                if (sacn_socks[i] > maxfd) maxfd = sacn_socks[i];
            }
        }
        if (artnet_sock >= 0) {
            FD_SET(artnet_sock, &readfds);
            if (artnet_sock > maxfd) maxfd = artnet_sock;
        }
        if (shownet_sock >= 0) {
            FD_SET(shownet_sock, &readfds);
            if (shownet_sock > maxfd) maxfd = shownet_sock;
        }

        if (maxfd < 0) {
            usleep(100000);
            continue;
        }

        tv.tv_sec = 1;
        tv.tv_usec = 0;

        if (select(maxfd + 1, &readfds, NULL, NULL, &tv) <= 0)
            continue;

        /* Handle sACN packets */
        for (i = 0; i < DMX_MAX_PORTS; i++) {
            if (sacn_socks[i] >= 0 && FD_ISSET(sacn_socks[i], &readfds)) {
                sacn_packet_t sacn_pkt;
                if (sacn_receive(sacn_socks[i], &sacn_pkt) == 0)
                    handle_sacn_packet(&sacn_pkt);
            }
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
    }

    /* Cleanup */
    for (i = 0; i < DMX_MAX_PORTS; i++)
        if (sacn_socks[i] >= 0) sacn_cleanup(sacn_socks[i]);
    if (artnet_sock >= 0) artnet_cleanup(artnet_sock);
    if (shownet_sock >= 0) shownet_cleanup(shownet_sock);

    return NULL;
}

/* ========================================================================= */
/* Thread 2: DMX output                                                      */
/* ========================================================================= */

static void *thread_dmx_output(void *arg)
{
    const dmx_ops_t *ops = dmx_get_ops();
    int fds[DMX_MAX_PORTS];
    int i;
    uint32_t last_write[DMX_MAX_PORTS] = {0};
    uint8_t frame[DMX_UNIVERSE_SIZE];

    (void)arg;

    /* Open and configure DMX ports */
    for (i = 0; i < DMX_MAX_PORTS; i++) {
        const char *dev = (i == 0) ? DMX_DEVICE_0 : DMX_DEVICE_1;
        fds[i] = ops->open(dev);
        if (fds[i] < 0) {
            LOG("WARNING: cannot open %s", dev);
            continue;
        }
        ops->set_mode(fds[i], DMX_MODE_TX);
    }

    while (g_running) {
        uint32_t t = now_ms();

        for (i = 0; i < DMX_MAX_PORTS; i++) {
            if (fds[i] < 0)
                continue;
            if (g_config.ports[i].mode != DMX_MODE_TX)
                continue;

            pthread_mutex_lock(&g_dmx_mutex);

            /* Check if we have fresh data */
            if (g_dmx_out[i].last_update_ms > last_write[i]) {
                memcpy(frame, g_dmx_out[i].data, DMX_UNIVERSE_SIZE);
                last_write[i] = g_dmx_out[i].last_update_ms;
                pthread_mutex_unlock(&g_dmx_mutex);

                ops->write_frame(fds[i], frame, DMX_UNIVERSE_SIZE);
            } else {
                /* Check for source timeout */
                uint32_t age = t - g_dmx_out[i].last_update_ms;
                int timed_out = (g_dmx_out[i].last_update_ms > 0) &&
                                (age > (uint32_t)g_config.dmx_hold_time * 1000);

                if (timed_out) {
                    memset(g_dmx_out[i].data, 0, DMX_UNIVERSE_SIZE);
                    g_dmx_out[i].last_update_ms = 0;
                    g_dmx_out[i].priority = 0;
                }
                pthread_mutex_unlock(&g_dmx_mutex);

                if (timed_out) {
                    memset(frame, 0, DMX_UNIVERSE_SIZE);
                    ops->write_frame(fds[i], frame, DMX_UNIVERSE_SIZE);
                }
            }
        }

        /* ~44fps DMX output rate (roughly matching DMX512 refresh) */
        usleep(23000);
    }

    /* Shutdown: turn off ports */
    for (i = 0; i < DMX_MAX_PORTS; i++) {
        if (fds[i] >= 0)
            ops->close(fds[i]);
    }

    return NULL;
}

/* ========================================================================= */
/* Signal handler                                                            */
/* ========================================================================= */

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ========================================================================= */
/* Main                                                                      */
/* ========================================================================= */

int main(int argc, char *argv[])
{
    const char *config_path = CONFIG_FILE_PATH;
    pthread_t net_thread, dmx_thread;

    LOG("sn110dmx v0.1.0 — Open-source DMX gateway");
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
    LOG("Port 0: universe %d", g_config.ports[0].universe);
    LOG("Port 1: universe %d", g_config.ports[1].universe);

    /* Initialize shared state */
    memset(g_dmx_out, 0, sizeof(g_dmx_out));

    /* Install signal handlers for clean shutdown */
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

    /* Create threads */
    if (pthread_create(&net_thread, NULL, thread_net_receive, NULL) != 0) {
        LOG("FATAL: cannot create network thread");
        return 1;
    }
    if (pthread_create(&dmx_thread, NULL, thread_dmx_output, NULL) != 0) {
        LOG("FATAL: cannot create DMX output thread");
        g_running = 0;
        pthread_join(net_thread, NULL);
        return 1;
    }

    LOG("Running. Send SIGTERM to stop.");

    /* Main thread just waits */
    pthread_join(net_thread, NULL);
    pthread_join(dmx_thread, NULL);

    LOG("Shutdown complete.");
    return 0;
}
