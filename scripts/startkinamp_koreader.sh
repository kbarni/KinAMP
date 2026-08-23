#!/bin/sh
#
# Launcher used by the KOReader plugin. Unlike startkinamp.sh (which hands
# playback over from the GTK player and lets it end on its own), this starts the
# player in --daemon mode: it stays alive after the playlist ends and takes
# commands on the .kinamp_cmd FIFO, so the plugin can pause/skip/seek without
# restarting the process.

KINAMP_DIR=/mnt/us/KinAMP

# Is a shared library installed on this device? ldconfig is not on every
# firmware, so look for the soname on disk first and only then ask it.
have_lib() {
    for dir in /usr/lib /lib /usr/lib/arm-linux-gnueabihf /lib/arm-linux-gnueabihf /usr/local/lib; do
        [ -e "$dir/$1" ] && return 0
    done
    ldconfig -p 2>/dev/null | grep -q -- "$1"
}

# Same flavour pick as startkinamp.sh: the plain armhf binaries link GStreamer
# 0.10, the *-gst ones link 1.0 (newer firmwares). Keep both scripts in step.
gst_suffix() {
    have_lib libgstreamer-0.10.so.0 && return 0
    if have_lib libgstreamer-1.0.so.0 && [ -f "$KINAMP_DIR/KinAMP-minimal-gst" ]; then
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

# The player cleans up its FIFO and status file on SIGTERM, so give it a chance
# before escalating.
pid=$(background_pid)
if [ -n "$pid" ]; then
    kill "$pid" 2>/dev/null
    i=0
    while [ $i -lt 3 ] && kill -0 "$pid" 2>/dev/null; do
        sleep 1
        i=$((i + 1))
    done
    if kill -0 "$pid" 2>/dev/null; then
        kill -9 "$pid" 2>/dev/null
        rm -f "$STATUS_FILE" "$CMD_FIFO"
    fi
fi

cd "$KINAMP_DIR"
./$KINAMPMIN --daemon "$@"
