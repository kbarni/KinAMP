#!/bin/sh
#
# Launcher used by the KOReader plugin. Unlike startkinamp.sh (which hands
# playback over from the GTK player and lets it end on its own), this starts the
# player in --daemon mode: it stays alive after the playlist ends and takes
# commands on the .kinamp_cmd FIFO, so the plugin can pause/skip/seek without
# restarting the process.

KINAMPMIN=$([ -f /lib/ld-linux-armhf.so.3 ] && echo "KinAMP-minimal" || echo "KinAMP-minimal-armel")

LIBDIR=$([ -f /lib/ld-linux-armhf.so.3 ] && echo "libs_hf/" || echo "libs_pw2/")
export LD_LIBRARY_PATH=$LIBDIR

KINAMP_DIR=/mnt/us/KinAMP
# Runtime files live on a filesystem that can hold a FIFO; /mnt/us (vfat)
# cannot. Must match get_runtime_path() in cli_player.cpp.
KINAMP_RUNTIME_DIR="${KINAMP_RUNTIME_DIR:-/tmp}"
STATUS_FILE="$KINAMP_RUNTIME_DIR/kinamp_status"
CMD_FIFO="$KINAMP_RUNTIME_DIR/kinamp_cmd"

# Prefer the pid the player publishes in its status file.
#
# The fallback has to match the process name, not the command line:
# /proc/<pid>/comm is truncated to 15 characters, so a plain `pgrep
# KinAMP-minimal-armel` never matches on PW2, but `pgrep -f` is worse - it
# matches any process whose arguments merely mention the binary, including the
# shell invoking this script. Killing that leaves the real player running while
# a second one starts, and two players then fight over the same status file and
# command FIFO. So match the truncated name exactly.
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
