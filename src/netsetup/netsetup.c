/*
 * netsetup — Network setup helper for SN110
 *
 * Called from /etc/ifup-eth0 in DHCP mode (twice):
 *   1. Before pump: assigns link-local IP from MAC for immediate reachability
 *   2. After pump:  reads actual interface IP and updates 220node.cfg
 *
 * The config update lets sn110lcd display the correct IP address.
 * Only the RAM copy (/etc/220node.cfg) is modified — flash is untouched
 * until the user explicitly saves via the web UI.
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

#include "../common.h"
#include "../config/config.h"

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>

/*
 * Extract IPv4 address from sockaddr (network byte order → host uint32_t).
 * sockaddr_in layout: family(2) + port(2) + addr(4) + zero(8)
 */
static uint32_t sockaddr_to_ip(const struct sockaddr *sa)
{
    const uint8_t *b = (const uint8_t *)&sa->sa_data[2];
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8)  |  (uint32_t)b[3];
}

/* Write host uint32_t IP into sockaddr for ioctl */
static void ip_to_sockaddr(struct sockaddr *sa, uint32_t ip)
{
    uint8_t *b = (uint8_t *)&sa->sa_data[2];
    memset(sa, 0, sizeof(*sa));
    sa->sa_family = AF_INET;
    b[0] = (ip >> 24) & 0xFF;
    b[1] = (ip >> 16) & 0xFF;
    b[2] = (ip >>  8) & 0xFF;
    b[3] =  ip        & 0xFF;
}

/* Read IPv4 address from interface. Returns 0 on failure. */
static uint32_t read_iface_ip(int fd, const char *ifname)
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFADDR, &ifr) < 0)
        return 0;
    return sockaddr_to_ip(&ifr.ifr_addr);
}

/* Read netmask from interface. Returns 0 on failure. */
static uint32_t read_iface_netmask(int fd, const char *ifname)
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFNETMASK, &ifr) < 0)
        return 0;
    return sockaddr_to_ip(&ifr.ifr_addr);
}

/* Read MAC address from interface */
static int read_iface_mac(int fd, const char *ifname, uint8_t *mac)
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0)
        return -1;
    memcpy(mac, ifr.ifr_addr.sa_data, 6);
    return 0;
}

/* Set IPv4 address on interface */
static int set_iface_ip(int fd, const char *ifname, uint32_t ip)
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    ip_to_sockaddr(&ifr.ifr_addr, ip);
    return ioctl(fd, SIOCSIFADDR, &ifr);
}

/* Set netmask on interface */
static int set_iface_netmask(int fd, const char *ifname, uint32_t mask)
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    ip_to_sockaddr(&ifr.ifr_addr, mask);
    return ioctl(fd, SIOCSIFNETMASK, &ifr);
}

static void log_ip(int logfd, const char *label, uint32_t ip)
{
    if (logfd >= 0)
        dprintf(logfd, "%s: %u.%u.%u.%u\n", label,
                (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                (ip >> 8) & 0xFF, ip & 0xFF);
}

int main(void)
{
    int fd;
    uint32_t ip;
    uint8_t mac[6];
    node_config_t config;
    int logfd;

    logfd = open("/tmp/netsetup.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        if (logfd >= 0) { write(logfd, "ERR: socket\n", 12); close(logfd); }
        return 1;
    }

    ip = read_iface_ip(fd, "eth0");
    log_ip(logfd, "eth0 current", ip);

    if (ip == 0) {
        /* No IP — assign link-local derived from MAC.
         * Range: 169.254.1.0 – 169.254.254.255 (RFC 3927)
         * Octets 3-4 from MAC bytes 4-5, octet 3 clamped to 1-254. */
        if (read_iface_mac(fd, "eth0", mac) == 0) {
            uint8_t o3 = (mac[4] % 254) + 1;
            uint8_t o4 = mac[5];
            uint32_t ll = (169u << 24) | (254u << 16) |
                          ((uint32_t)o3 << 8) | o4;
            log_ip(logfd, "assign link-local", ll);
            set_iface_ip(fd, "eth0", ll);
            set_iface_netmask(fd, "eth0", 0xFFFF0000); /* 255.255.0.0 */
        }
        ip = read_iface_ip(fd, "eth0");
        log_ip(logfd, "after link-local", ip);
    }

    close(fd);

    if (ip == 0) {
        if (logfd >= 0) { write(logfd, "no IP, exiting\n", 15); close(logfd); }
        return 0;
    }

    /* Update config with actual interface address.
     * addr_mode is NOT touched — it stays DHCP. */
    if (config_load(CONFIG_FILE_PATH, &config) == 0) {
        uint32_t mask;

        log_ip(logfd, "config old ip", config.ip_addr);
        config.ip_addr = ip;

        fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd >= 0) {
            mask = read_iface_netmask(fd, "eth0");
            if (mask != 0)
                config.netmask = mask;
            close(fd);
        }

        config_save(CONFIG_FILE_PATH, &config);
        log_ip(logfd, "config saved ip", config.ip_addr);
    } else {
        if (logfd >= 0) write(logfd, "config_load failed\n", 19);
    }

    if (logfd >= 0) close(logfd);
    return 0;
}
