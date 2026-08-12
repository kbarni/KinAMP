#!/bin/sh

EDITOR=$([ -f /lib/ld-linux-armhf.so.3 ] && echo "radio_cli" || echo "radio_cli-armel")

cd /mnt/us/KinAMP
./$EDITOR
