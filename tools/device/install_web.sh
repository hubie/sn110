#!/bin/sh
#==============================================================================
# SN110 Web UI Install — runs ON THE DEVICE via /etc/rc install trigger
#
# Copies web UI files from /tmp/ to their final locations.
# Triggered by creating /tmp/install.arm after uploading files.
#==============================================================================

echo sn110-web: Installing web configuration UI...

# Free space: remove old Strand CGI binaries if present
rm /cgi-bin/cfg2html
rm /cgi-bin/html2cfg

# Copy CGI binary to /usr/bin/
echo sn110-web: Installing /usr/bin/cgi_config...
cp /tmp/cgi_config /usr/bin/cgi_config
chmod 755 /usr/bin/cgi_config

# Overwrite CGI shell scripts
echo sn110-web: Installing /cgi-bin/cfgget.cgi...
cp /tmp/cfgget.cgi /cgi-bin/cfgget.cgi
chmod 755 /cgi-bin/cfgget.cgi

echo sn110-web: Installing /cgi-bin/cfgpost.cgi...
cp /tmp/cfgpost.cgi /cgi-bin/cfgpost.cgi
chmod 755 /cgi-bin/cfgpost.cgi

echo sn110-web: Installing /cgi-bin/fwget.cgi...
cp /tmp/fwget.cgi /cgi-bin/fwget.cgi
chmod 755 /cgi-bin/fwget.cgi

echo sn110-web: Installing /cgi-bin/fwpost.cgi...
cp /tmp/fwpost.cgi /cgi-bin/fwpost.cgi
chmod 755 /cgi-bin/fwpost.cgi

# Overwrite landing page
echo sn110-web: Installing /index.html...
cp /tmp/index.html /index.html

# Clean up temp files
rm /tmp/cgi_config
rm /tmp/cfgget.cgi
rm /tmp/cfgpost.cgi
rm /tmp/fwget.cgi
rm /tmp/fwpost.cgi
rm /tmp/index.html

echo sn110-web: Web UI installation complete!
/usr/bin/sn110lcd /dev/lcd0 usermsg Web_config installed. Browse_to node_IP.
