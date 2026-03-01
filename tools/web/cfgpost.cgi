#!/bin/sh
/usr/bin/cgi_config
/usr/bin/dmxtst /dev/dmx0 silentoff
/usr/bin/dmxtst /dev/dmx1 silentoff
/sbin/nodecfg put /etc/220node.cfg
/sbin/nodecfg process /etc/220node.cfg
