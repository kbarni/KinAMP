#!/bin/sh

LIBDIR=$([ -f /lib/ld-linux-armhf.so.3 ] && echo "libs_hf/" || echo "libs_pw2/")
export LD_LIBRARY_PATH=$LIBDIR

KINAMP=$([ -f /lib/ld-linux-armhf.so.3 ] && echo "KinAMP" || echo "KinAMP-armel")
KINAMPMIN=$([ -f /lib/ld-linux-armhf.so.3 ] && echo "KinAMP-minimal" || echo "KinAMP-minimal-armel")

KINAMP_DIR=/mnt/us/KinAMP
STATUS_FILE="$KINAMP_DIR/.kinamp_status"

# /proc/<pid>/comm is truncated to 15 characters, so a plain `pgrep
# KinAMP-minimal-armel` never matches on PW2 and background playback went
# undetected there. Prefer the pid the player publishes in its status file and
# fall back to a full-cmdline match.
background_pid() {
    if [ -f "$STATUS_FILE" ]; then
        pid=$(sed -n 's/^pid=//p' "$STATUS_FILE" | head -n 1)
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            echo "$pid"
            return 0
        fi
    fi
    pgrep -f "$KINAMPMIN" 2>/dev/null | head -n 1
}

# SIGTERM is handled by the player: it stops playback and removes its command
# FIFO and status file before exiting. Only escalate if it ignores us.
stop_background() {
    pid=$(background_pid)
    [ -n "$pid" ] || return 1

    kill "$pid" 2>/dev/null
    i=0
    while [ $i -lt 3 ]; do
        kill -0 "$pid" 2>/dev/null || return 0
        sleep 1
        i=$((i + 1))
    done

    echo "Process didn't terminate gracefully. Force killing..."
    kill -9 "$pid" 2>/dev/null
    rm -f "$STATUS_FILE" "$KINAMP_DIR/.kinamp_cmd"
    return 0
}

alert() {
    TITLE="$1"
    TEXT="$2"

    TITLE_ESC=$(printf '%s' "$TITLE" | sed 's/"/\\"/g')
    TEXT_ESC=$(printf '%s' "$TEXT" | sed 's/"/\\"/g')

    JSON='{ "clientParams":{ "alertId":"appAlert1", "show":true, "customStrings":[ { "matchStr":"alertTitle", "replaceStr":"'"$TITLE_ESC"'" }, { "matchStr":"alertText", "replaceStr":"'"$TEXT_ESC"'" } ] } }'

    lipc-set-prop com.lab126.pillow pillowAlert "$JSON"
}

# Check if KinAMP is running in background
if [ -n "$(background_pid)" ]; then
    echo "Kinamp is running in background. Stopping it..."
    stop_background
    alert "KinAMP" "Background music playback stopped"
else
    echo "Starting KinAMP GUI..."
    lipc-set-prop -s com.lab126.btfd BTenable 0:1
    sleep 1
    cd "$KINAMP_DIR"
    ./$KINAMP
    exit_code=$?

    # Check if exit code is 10
    if [ $exit_code -eq 10 ]; then
        # Pillow dialog
        alert "KinAMP" "Continuing playing music in background. Click the KinAMP booklet again to stop."
        ./$KINAMPMIN --music &
    elif [ $exit_code -eq 11 ]; then
        # Pillow dialog
        alert "KinAMP" "Continuing playing music in background. Click the KinAMP booklet again to stop."
        ./$KINAMPMIN --radio &
    fi
fi
