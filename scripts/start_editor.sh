#!/bin/sh

EDITOR=$([ -f /lib/ld-linux-armhf.so.3 ] && echo "radio_cli" || echo "radio_cli-pw2")

cd /mnt/us/KinAMP
./$EDITOR
