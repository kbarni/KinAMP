#!/bin/sh

KINAMP_DIR=/mnt/us/KinAMP

# Is a shared library installed on this device? ldconfig is not on every
# firmware, so look for the soname on disk first and only then ask it.
have_lib() {
    for dir in /usr/lib /lib /usr/lib/arm-linux-gnueabihf /lib/arm-linux-gnueabihf /usr/local/lib; do
        [ -e "$dir/$1" ] && return 0
    done
    ldconfig -p 2>/dev/null | grep -q -- "$1"
}

# The armhf binaries come in two flavours: the plain ones link GStreamer 0.10
# (what every firmware KinAMP has supported so far ships), the *-gst ones link
# GStreamer 1.0 (newer firmwares). A device carries one or the other, so pick
# by what is installed, keeping 0.10 first where both are present. Falls back
# to the plain binaries when neither is found, so the failure the user sees is
# the loader naming the library it is missing.
gst_suffix() {
    have_lib libgstreamer-0.10.so.0 && return 0
    if have_lib libgstreamer-1.0.so.0 && [ -f "$KINAMP_DIR/KinAMP-gst" ]; then
        echo "-gst"
    fi
}

if [ -f /lib/ld-linux-armhf.so.3 ]; then
    LIBDIR="libs_hf/"
    SUFFIX=$(gst_suffix)
else
    LIBDIR="libs_pw2/"
    SUFFIX="-pw2"
fi
export LD_LIBRARY_PATH=$LIBDIR

KINAMP="KinAMP$SUFFIX"
KINAMPMIN="KinAMP-minimal$SUFFIX"
# Runtime files live on a filesystem that can hold a FIFO; /mnt/us (vfat)
# cannot. Must match get_runtime_path() in cli_player.cpp.
KINAMP_RUNTIME_DIR="${KINAMP_RUNTIME_DIR:-/tmp}"
STATUS_FILE="$KINAMP_RUNTIME_DIR/kinamp_status"
CMD_FIFO="$KINAMP_RUNTIME_DIR/kinamp_cmd"

background_pid() {
    if [ -f "$STATUS_FILE" ]; then
        pid=$(sed -n 's/^pid=//p' "$STATUS_FILE" | head -n 1)
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            echo "$pid"
            return 0
        fi
    fi
    pgrep -x "$(printf '%s' "$KINAMPMIN" | cut -c1-15)" 2>/dev/null | head -n 1
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
    rm -f "$STATUS_FILE" "$CMD_FIFO"
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
