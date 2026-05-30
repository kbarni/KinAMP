#!/bin/sh

export LD_LIBRARY_PATH=/mnt/us/KinAMP/libs_hf/ 

is_process_running() {
    local process_name="$1"
    pgrep "$process_name" > /dev/null 2>&1
}

alert() {
    TITLE="$1"
    TEXT="$2"

    TITLE_ESC=$(printf '%s' "$TITLE" | sed 's/"/\\"/g')
    TEXT_ESC=$(printf '%s' "$TEXT" | sed 's/"/\\"/g')

    JSON='{ "clientParams":{ "alertId":"appAlert1", "show":true, "customStrings":[ { "matchStr":"alertTitle", "replaceStr":"'"$TITLE_ESC"'" }, { "matchStr":"alertText", "replaceStr":"'"$TEXT_ESC"'" } ] } }'

    lipc-set-prop com.lab126.pillow pillowAlert "$JSON"
}

KINAMP=$([ -f /lib/ld-linux-armhf.so.3 ] && echo "KinAMP" || echo "KinAMP-armel")
KINAMPMIN=$([ -f /lib/ld-linux-armhf.so.3 ] && echo "KinAMP-minimal" || echo "KinAMP-minimal-armel") 

# Check if KinAMP is running in background
if is_process_running $KINAMPMIN; then
    echo "Kinamp is running in background. Stopping it..."
    pkill $KINAMPMIN
    sleep 2
    if is_process_running $KINAMPMIN; then
        echo "Process didn't terminate gracefully. Force killing..."
        pkill -9 $KINAMPMIN
    fi
    alert "KinAMP","Background music playback stopped"
else
    echo "Starting KinAMP GUI..."
    lipc-set-prop -s com.lab126.btfd BTenable 0:1
    sleep 1
    cd /mnt/us/KinAMP
    ./$KINAMP
    exit_code=$?
    
    # Check if exit code is 10
    if [ $exit_code -eq 10 ]; then
        # Pillow dialog
        alert "KinAMP","Continuing playing music in background. Click the KinAMP booklet again to stop."
        ./$KINAMPMIN --music &
    elif [ $exit_code -eq 11 ]; then
        # Pillow dialog
        alert "KinAMP","Continuing playing music in background. Click the KinAMP booklet again to stop."
        ./$KINAMPMIN --radio &
    fi
fi
