#!/bin/sh
# cfgpost.cgi — POST handler for web config
# Runs cgi_config (which reads stdin, saves config), then syncs to flash.

/usr/bin/cgi_config

# Turn off DMX ports before reconfiguring
/usr/bin/dmxtst /dev/dmx0 silentoff
/usr/bin/dmxtst /dev/dmx1 silentoff

# Write Strand-compatible keys to flash via nodecfg
STRAND_CFG="/etc/220node.cfg.strand"
if [ -f "$STRAND_CFG" ]; then
    SIZE=$(wc -c < "$STRAND_CFG")
    if [ "$SIZE" -lt 4096 ]; then
        /sbin/nodecfg put "$STRAND_CFG"
    fi
fi

# Apply network config in background (2s delay for HTTP response)
(sleep 2; /bin/sh /etc/ifup-eth0) &
