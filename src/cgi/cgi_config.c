/*
 * cgi_config — Web configuration CGI for SN110
 *
 * Single CGI binary that handles both GET (show form) and POST (save config).
 * Called by the device's httpd via shell-script wrappers (cfgget.cgi, cfgpost.cgi).
 *
 * NOTE: The device httpd generates HTTP headers itself — CGI programs output
 * raw HTML only (no Content-type header). Confirmed by examining original
 * Strand CGI binaries.
 *
 * Reuses config_load()/config_save() from src/config/config.c.
 * Uses getenv() (from minilib) to read REQUEST_METHOD and CONTENT_LENGTH.
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

#include "../common.h"
#include "../config/config.h"
#include "cgi_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

/* ioctl constants for reading MAC from network interface */
#define SIOCGIFHWADDR 0x8927
#define IFNAMSIZ      16

struct ifreq {
    char           ifr_name[IFNAMSIZ];
    struct sockaddr ifr_hwaddr;
};

/* Read MAC address from eth0 network interface via ioctl */
static void read_iface_mac(uint8_t *mac)
{
    struct ifreq ifr;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return;

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, "eth0", IFNAMSIZ - 1);

    if (ioctl(fd, SIOCGIFHWADDR, &ifr) == 0)
        memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);

    close(fd);
}

/* IP address formatting helper */
#define IP_A(ip) (((ip) >> 24) & 0xFF)
#define IP_B(ip) (((ip) >> 16) & 0xFF)
#define IP_C(ip) (((ip) >>  8) & 0xFF)
#define IP_D(ip) ( (ip)        & 0xFF)

/* HTML entity escaping for XSS prevention */
static void html_escape(const char *s)
{
    while (*s) {
        switch (*s) {
        case '&':  printf("&amp;");  break;
        case '<':  printf("&lt;");   break;
        case '>':  printf("&gt;");   break;
        case '"':  printf("&quot;"); break;
        case '\'': printf("&#39;");  break;
        default:   printf("%c", *s); break;
        }
        s++;
    }
}

static void emit_css(void)
{
    printf("<style>\n");
    printf("*{box-sizing:border-box;margin:0;padding:0}\n");
    printf("body{font-family:-apple-system,sans-serif;background:#1a1a2e;color:#e0e0e0;padding:0}\n");
    printf(".header{background:#16213e;padding:14px 24px;border-bottom:2px solid #0f3460;"
           "display:flex;justify-content:space-between;align-items:center}\n");
    printf(".header h1{font-size:1.3em;color:#e94560;font-weight:600}\n");
    printf(".mac{font-family:'Courier New',monospace;color:#8a8a9a;font-size:0.95em}\n");
    printf("form{max-width:560px;margin:20px auto;padding:0 16px}\n");
    printf("fieldset{border:1px solid #2a2a4a;border-radius:6px;padding:14px 16px;margin-bottom:16px;background:#16213e}\n");
    printf("legend{color:#e94560;font-weight:600;padding:0 8px;font-size:0.95em}\n");
    printf(".row{display:flex;align-items:center;margin:8px 0}\n");
    printf(".row label{width:130px;text-align:right;padding-right:12px;font-size:0.9em;color:#b0b0c0;flex-shrink:0}\n");
    printf(".row input[type=text],.row input[type=number]"
           "{flex:1;padding:6px 8px;border:1px solid #2a2a4a;border-radius:4px;"
           "background:#0f0f23;color:#e0e0e0;font-size:0.9em}\n");
    printf(".radio-group{display:flex;gap:16px;flex:1}\n");
    printf(".radio-group label{width:auto;text-align:left;padding-right:0;cursor:pointer}\n");
    printf(".note{font-size:0.8em;color:#7a7a8a;margin:4px 0 4px 142px}\n");
    printf("input[type=submit]{display:block;width:100%%;padding:10px;margin:8px auto 24px;"
           "background:#e94560;color:#fff;border:none;border-radius:6px;"
           "font-size:1em;font-weight:600;cursor:pointer}\n");
    printf("input[type=submit]:hover{background:#c73650}\n");
    printf("a{color:#e94560}\n");
    printf("</style>\n");
}

static void emit_html_form(const node_config_t *cfg)
{
    printf("<!DOCTYPE html>\n<html><head><title>");
    html_escape(cfg->hostname);
    printf(" Configuration</title>\n");
    emit_css();
    printf("</head><body>\n");

    /* Header bar with device name + MAC */
    printf("<div class=\"header\">\n");
    printf("<h1>");
    html_escape(cfg->hostname);
    printf("</h1>\n");
    printf("<span class=\"mac\">MAC %02X:%02X:%02X:%02X:%02X:%02X</span>\n",
           cfg->mac[0], cfg->mac[1], cfg->mac[2],
           cfg->mac[3], cfg->mac[4], cfg->mac[5]);
    printf("</div>\n");

    printf("<form method=\"POST\" action=\"/cgi-bin/cfgpost.cgi\">\n");

    /* Network fieldset */
    printf("<fieldset><legend>Network</legend>\n");

    printf("<div class=\"row\"><label>Hostname</label>"
           "<input type=\"text\" name=\"hostname\" value=\"");
    html_escape(cfg->hostname);
    printf("\" maxlength=\"15\"></div>\n");

    printf("<div class=\"row\"><label>Address Mode</label><div class=\"radio-group\">"
           "<label><input type=\"radio\" name=\"addr_mode\" value=\"dhcp\"%s> DHCP</label>"
           "<label><input type=\"radio\" name=\"addr_mode\" value=\"static\"%s> Static</label>"
           "<label><input type=\"radio\" name=\"addr_mode\" value=\"dhcp_static\"%s> DHCP+Static</label>"
           "</div></div>\n",
           cfg->addr_mode == ADDR_MODE_DHCP ? " checked" : "",
           cfg->addr_mode == ADDR_MODE_STATIC ? " checked" :
               (cfg->addr_mode == ADDR_MODE_SENTINEL ? " checked" : ""),
           cfg->addr_mode == ADDR_MODE_DHCP_STATIC ? " checked" : "");

    printf("<p class=\"note\">DHCP+Static tries DHCP first, falls back to static IP.</p>\n");

    printf("<div class=\"row\"><label>IP Address</label>"
           "<input type=\"text\" name=\"ipaddr\" value=\"%u.%u.%u.%u\"></div>\n",
           IP_A(cfg->ip_addr), IP_B(cfg->ip_addr),
           IP_C(cfg->ip_addr), IP_D(cfg->ip_addr));

    printf("<div class=\"row\"><label>Netmask</label>"
           "<input type=\"text\" name=\"netmask\" value=\"%u.%u.%u.%u\"></div>\n",
           IP_A(cfg->netmask), IP_B(cfg->netmask),
           IP_C(cfg->netmask), IP_D(cfg->netmask));

    printf("<div class=\"row\"><label>Gateway</label>"
           "<input type=\"text\" name=\"gateway\" value=\"%u.%u.%u.%u\"></div>\n",
           IP_A(cfg->gateway), IP_B(cfg->gateway),
           IP_C(cfg->gateway), IP_D(cfg->gateway));

    printf("</fieldset>\n");

    /* Protocol fieldset */
    printf("<fieldset><legend>Protocol</legend>\n");

    printf("<div class=\"row\"><label>Protocol</label><div class=\"radio-group\">"
           "<label><input type=\"radio\" name=\"protocol\" value=\"sacn\"%s> sACN</label>"
           "<label><input type=\"radio\" name=\"protocol\" value=\"artnet\"%s> Art-Net</label>"
           "<label><input type=\"radio\" name=\"protocol\" value=\"shownet\"%s> ShowNet</label>"
           "</div></div>\n",
           cfg->active_protocol == PROTO_SACN ? " checked" : "",
           cfg->active_protocol == PROTO_ARTNET ? " checked" : "",
           cfg->active_protocol == PROTO_SHOWNET ? " checked" : "");

    printf("<div class=\"row\"><label>DMX Hold (s)</label>"
           "<input type=\"number\" name=\"dmx_hold_time\" value=\"%d\" min=\"0\" max=\"300\"></div>\n",
           cfg->dmx_hold_time);

    printf("</fieldset>\n");

    /* Port 0 fieldset */
    printf("<fieldset><legend>Port 0</legend>\n");

    printf("<div class=\"row\"><label>Mode</label><div class=\"radio-group\">"
           "<label><input type=\"radio\" name=\"port0_mode\" value=\"tx\"%s> TX</label>"
           "<label><input type=\"radio\" name=\"port0_mode\" value=\"rx\"%s> RX</label>"
           "<label><input type=\"radio\" name=\"port0_mode\" value=\"off\"%s> Off</label>"
           "</div></div>\n",
           cfg->ports[0].mode == DMX_MODE_TX ? " checked" : "",
           cfg->ports[0].mode == DMX_MODE_RX ? " checked" : "",
           cfg->ports[0].mode == DMX_MODE_OFF ? " checked" : "");

    printf("<div class=\"row\"><label>Universe</label>"
           "<input type=\"number\" name=\"port0_universe\" value=\"%d\" min=\"1\" max=\"63999\"></div>\n",
           cfg->ports[0].universe);

    printf("<div class=\"row\"><label>Label</label>"
           "<input type=\"text\" name=\"port0_label\" value=\"");
    html_escape(cfg->ports[0].label);
    printf("\" maxlength=\"8\"></div>\n");

    printf("<div class=\"row\"><label>Slot Monitor</label>"
           "<input type=\"number\" name=\"slot_monitor_0\" value=\"%d\" min=\"0\" max=\"512\"></div>\n",
           cfg->dmx_slot_monitor[0]);
    printf("<p class=\"note\">DMX slot to monitor (0 = disabled, 1-512)</p>\n");

    printf("</fieldset>\n");

    /* Port 1 fieldset */
    printf("<fieldset><legend>Port 1</legend>\n");

    printf("<div class=\"row\"><label>Mode</label><div class=\"radio-group\">"
           "<label><input type=\"radio\" name=\"port1_mode\" value=\"tx\"%s> TX</label>"
           "<label><input type=\"radio\" name=\"port1_mode\" value=\"rx\"%s> RX</label>"
           "<label><input type=\"radio\" name=\"port1_mode\" value=\"off\"%s> Off</label>"
           "</div></div>\n",
           cfg->ports[1].mode == DMX_MODE_TX ? " checked" : "",
           cfg->ports[1].mode == DMX_MODE_RX ? " checked" : "",
           cfg->ports[1].mode == DMX_MODE_OFF ? " checked" : "");

    printf("<div class=\"row\"><label>Universe</label>"
           "<input type=\"number\" name=\"port1_universe\" value=\"%d\" min=\"1\" max=\"63999\"></div>\n",
           cfg->ports[1].universe);

    printf("<div class=\"row\"><label>Label</label>"
           "<input type=\"text\" name=\"port1_label\" value=\"");
    html_escape(cfg->ports[1].label);
    printf("\" maxlength=\"8\"></div>\n");

    printf("<div class=\"row\"><label>Slot Monitor</label>"
           "<input type=\"number\" name=\"slot_monitor_1\" value=\"%d\" min=\"0\" max=\"512\"></div>\n",
           cfg->dmx_slot_monitor[1]);
    printf("<p class=\"note\">DMX slot to monitor (0 = disabled, 1-512)</p>\n");

    printf("</fieldset>\n");

    /* LCD fieldset */
    printf("<fieldset><legend>LCD</legend>\n");

    printf("<div class=\"row\"><label>Contrast</label>"
           "<input type=\"number\" name=\"lcd_contrast\" value=\"%d\" min=\"0\" max=\"255\"></div>\n",
           cfg->lcd_contrast);

    printf("<div class=\"row\"><label>Backlight</label><div class=\"radio-group\">"
           "<label><input type=\"radio\" name=\"lcd_backlight\" value=\"off\"%s> Off</label>"
           "<label><input type=\"radio\" name=\"lcd_backlight\" value=\"on\"%s> On</label>"
           "<label><input type=\"radio\" name=\"lcd_backlight\" value=\"auto\"%s> Auto</label>"
           "</div></div>\n",
           cfg->lcd_backlight == LCD_BACKLIGHT_OFF ? " checked" : "",
           cfg->lcd_backlight == LCD_BACKLIGHT_ON ? " checked" : "",
           cfg->lcd_backlight == LCD_BACKLIGHT_AUTO ? " checked" : "");

    printf("</fieldset>\n");

    printf("<input type=\"submit\" value=\"Save Configuration\">\n");
    printf("</form>\n");
    printf("</body></html>\n");
}

int main(void)
{
    node_config_t cfg;
    static const uint8_t zero_mac[6] = {0};
    char *method = getenv("REQUEST_METHOD");

    if (!method)
        method = "GET";

    config_load(CONFIG_FILE_PATH, &cfg);

    /* If config file has no MAC, read from network interface */
    if (memcmp(cfg.mac, zero_mac, 6) == 0)
        read_iface_mac(cfg.mac);

    if (strcmp(method, "POST") == 0) {
        char *cl_str = getenv("CONTENT_LENGTH");
        int content_length = cl_str ? atoi(cl_str) : 0;
        char body[2048];

        if (content_length > 0 && content_length < (int)sizeof(body) - 1) {
            int n = read(0, body, content_length);
            body[n > 0 ? n : 0] = '\0';
        } else {
            body[0] = '\0';
        }

        parse_formdata(body, &cfg);
        config_save(CONFIG_FILE_PATH, &cfg);
        config_save_strand(STRAND_CONFIG_PATH, &cfg);
        config_generate_ifup(IFUP_FILE_PATH, &cfg);

        printf("<!DOCTYPE html>\n<html><head><title>");
        html_escape(cfg.hostname);
        printf(" — Saved</title>\n");
        printf("<meta http-equiv=\"refresh\" content=\"3;url=/cgi-bin/cfgget.cgi\">\n");
        emit_css();
        printf("</head><body>\n");
        printf("<div class=\"header\"><h1>");
        html_escape(cfg.hostname);
        printf("</h1></div>\n");
        printf("<form style=\"text-align:center;padding-top:40px\">\n");
        printf("<fieldset><legend>Status</legend>\n");
        printf("<p style=\"padding:16px;font-size:1.1em\">Configuration saved.</p>\n");
        printf("<p class=\"note\" style=\"margin-left:0\">Redirecting in 3 seconds...</p>\n");
        printf("</fieldset>\n");
        printf("<p style=\"margin-top:16px\"><a href=\"/cgi-bin/cfgget.cgi\">Back to configuration</a></p>\n");
        printf("</form>\n");
        printf("</body></html>\n");
    } else {
        emit_html_form(&cfg);
    }

    return 0;
}
