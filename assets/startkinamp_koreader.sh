#!/bin/sh

export LD_LIBRARY_PATH=/mnt/us/KinAMP/libs_hf/ 
KINAMPMIN=$([ -f /lib/ld-linux-armhf.so.3 ] && echo "KinAMP-minimal" || echo "KinAMP-minimal-armel") 

is_process_running() {
    local process_name="$1"
    pgrep "$process_name" > /dev/null 2>&1
}

# Check if KinAMP needs to be stopped.
if is_process_running $KINAMPMIN; then
    pkill $KINAMPMIN
    sleep 2
    if is_process_running $KINAMPMIN; then
        pkill -9 $KINAMPMIN
    fi
fi
    
cd /mnt/us/KinAMP/
./$KINAMPMIN $1 $2
