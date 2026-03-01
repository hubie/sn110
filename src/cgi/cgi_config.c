/*
 * cgi_config — Web configuration CGI for SN110
 *
 * Single CGI binary that handles both GET (show form) and POST (save config).
 * Called by the device's httpd via shell-script wrappers (cfgget.cgi, cfgpost.cgi).
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

/* Raw read syscall (from syscalls.S on device, libc on host) */
extern int read(int fd, void *buf, size_t count);

/* IP address formatting helper */
#define IP_A(ip) (((ip) >> 24) & 0xFF)
#define IP_B(ip) (((ip) >> 16) & 0xFF)
#define IP_C(ip) (((ip) >>  8) & 0xFF)
#define IP_D(ip) ( (ip)        & 0xFF)

static void emit_html_form(const node_config_t *cfg)
{
    printf("Content-type: text/html\r\n\r\n");
    printf("<!DOCTYPE html>\n<html><head><title>SN110 Configuration</title>\n");
    printf("<style>\n");
    printf("body{font-family:sans-serif;margin:20px;background:#f5f5f5}\n");
    printf("h1{color:#333;border-bottom:2px solid #666;padding-bottom:8px}\n");
    printf("table{border-collapse:collapse}\n");
    printf("td{padding:4px 8px}\n");
    printf("td.lbl{font-weight:bold;text-align:right}\n");
    printf("input[type=text],input[type=number]{width:150px;padding:2px 4px}\n");
    printf("hr{border:none;border-top:1px solid #ccc}\n");
    printf(".section{font-size:1.1em;color:#444;margin-top:4px}\n");
    printf("input[type=submit]{padding:6px 20px;font-size:1em;margin-top:10px}\n");
    printf("</style>\n</head><body>\n");

    printf("<h1>SN110 Configuration</h1>\n");
    printf("<form method=\"POST\" action=\"/cgi-bin/cfgpost.cgi\">\n");
    printf("<table>\n");

    /* Network settings */
    printf("<tr><td class=\"lbl\">Hostname:</td>"
           "<td><input type=\"text\" name=\"hostname\" value=\"%s\" maxlength=\"15\"></td></tr>\n",
           cfg->hostname);

    printf("<tr><td class=\"lbl\">IP Address:</td>"
           "<td><input type=\"text\" name=\"ipaddr\" value=\"%u.%u.%u.%u\"></td></tr>\n",
           IP_A(cfg->ip_addr), IP_B(cfg->ip_addr),
           IP_C(cfg->ip_addr), IP_D(cfg->ip_addr));

    printf("<tr><td class=\"lbl\">Netmask:</td>"
           "<td><input type=\"text\" name=\"netmask\" value=\"%u.%u.%u.%u\"></td></tr>\n",
           IP_A(cfg->netmask), IP_B(cfg->netmask),
           IP_C(cfg->netmask), IP_D(cfg->netmask));

    printf("<tr><td class=\"lbl\">Gateway:</td>"
           "<td><input type=\"text\" name=\"gateway\" value=\"%u.%u.%u.%u\"></td></tr>\n",
           IP_A(cfg->gateway), IP_B(cfg->gateway),
           IP_C(cfg->gateway), IP_D(cfg->gateway));

    /* Protocol */
    printf("<tr><td colspan=\"2\"><hr></td></tr>\n");
    printf("<tr><td class=\"lbl\">Protocol:</td><td>\n");
    printf("<input type=\"radio\" name=\"protocol\" value=\"sacn\"%s> sACN\n",
           cfg->active_protocol == PROTO_SACN ? " checked" : "");
    printf("<input type=\"radio\" name=\"protocol\" value=\"artnet\"%s> Art-Net\n",
           cfg->active_protocol == PROTO_ARTNET ? " checked" : "");
    printf("<input type=\"radio\" name=\"protocol\" value=\"shownet\"%s> ShowNet\n",
           cfg->active_protocol == PROTO_SHOWNET ? " checked" : "");
    printf("</td></tr>\n");

    printf("<tr><td class=\"lbl\">DMX Hold Time (s):</td>"
           "<td><input type=\"number\" name=\"dmx_hold_time\" value=\"%d\" min=\"0\" max=\"300\"></td></tr>\n",
           cfg->dmx_hold_time);

    /* Port 0 */
    printf("<tr><td colspan=\"2\"><hr><span class=\"section\">Port 0</span></td></tr>\n");
    printf("<tr><td class=\"lbl\">Mode:</td><td>\n");
    printf("<input type=\"radio\" name=\"port0_mode\" value=\"tx\"%s> TX\n",
           cfg->ports[0].mode == DMX_MODE_TX ? " checked" : "");
    printf("<input type=\"radio\" name=\"port0_mode\" value=\"rx\"%s> RX\n",
           cfg->ports[0].mode == DMX_MODE_RX ? " checked" : "");
    printf("<input type=\"radio\" name=\"port0_mode\" value=\"off\"%s> Off\n",
           cfg->ports[0].mode == DMX_MODE_OFF ? " checked" : "");
    printf("</td></tr>\n");
    printf("<tr><td class=\"lbl\">Universe:</td>"
           "<td><input type=\"number\" name=\"port0_universe\" value=\"%d\" min=\"1\" max=\"63999\"></td></tr>\n",
           cfg->ports[0].universe);
    printf("<tr><td class=\"lbl\">Label:</td>"
           "<td><input type=\"text\" name=\"port0_label\" value=\"%s\" maxlength=\"8\"></td></tr>\n",
           cfg->ports[0].label);

    /* Port 1 */
    printf("<tr><td colspan=\"2\"><hr><span class=\"section\">Port 1</span></td></tr>\n");
    printf("<tr><td class=\"lbl\">Mode:</td><td>\n");
    printf("<input type=\"radio\" name=\"port1_mode\" value=\"tx\"%s> TX\n",
           cfg->ports[1].mode == DMX_MODE_TX ? " checked" : "");
    printf("<input type=\"radio\" name=\"port1_mode\" value=\"rx\"%s> RX\n",
           cfg->ports[1].mode == DMX_MODE_RX ? " checked" : "");
    printf("<input type=\"radio\" name=\"port1_mode\" value=\"off\"%s> Off\n",
           cfg->ports[1].mode == DMX_MODE_OFF ? " checked" : "");
    printf("</td></tr>\n");
    printf("<tr><td class=\"lbl\">Universe:</td>"
           "<td><input type=\"number\" name=\"port1_universe\" value=\"%d\" min=\"1\" max=\"63999\"></td></tr>\n",
           cfg->ports[1].universe);
    printf("<tr><td class=\"lbl\">Label:</td>"
           "<td><input type=\"text\" name=\"port1_label\" value=\"%s\" maxlength=\"8\"></td></tr>\n",
           cfg->ports[1].label);

    printf("</table>\n");
    printf("<br><input type=\"submit\" value=\"Save Configuration\">\n");
    printf("</form>\n");
    printf("</body></html>\n");
}

int main(void)
{
    node_config_t cfg;
    char *method = getenv("REQUEST_METHOD");

    if (!method)
        method = "GET";

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

        config_load(CONFIG_FILE_PATH, &cfg);
        parse_formdata(body, &cfg);
        config_save(CONFIG_FILE_PATH, &cfg);

        printf("Content-type: text/html\r\n\r\n");
        printf("<!DOCTYPE html>\n<html><head><title>SN110 — Saved</title>\n");
        printf("<style>body{font-family:sans-serif;margin:20px}</style>\n");
        printf("</head><body>\n");
        printf("<h1>Configuration Saved</h1>\n");
        printf("<p>Settings have been updated.</p>\n");
        printf("<p><a href=\"/cgi-bin/cfgget.cgi\">Back to configuration</a></p>\n");
        printf("</body></html>\n");
    } else {
        config_load(CONFIG_FILE_PATH, &cfg);
        emit_html_form(&cfg);
    }

    return 0;
}
