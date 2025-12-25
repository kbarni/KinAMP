#!/bin/sh

is_process_running() {
    local process_name="$1"
    pgrep "$process_name" > /dev/null 2>&1
}

alert() {
    TITLE="$1"
    TEXT="$2"

    TITLE_ESC=$(printf '%s' "$TITLE" | sed 's/"/\\"/g')
    TEXT_ESC=$(printf '%s' "$TEXT" | sed 's/"/\\"/g')

    JSON='{ "clientParams":{ "alertId":"KinAMP", "show":true, "customStrings":[ { "matchStr":"alertTitle", "replaceStr":"'"$TITLE_ESC"'" }, { "matchStr":"alertText", "replaceStr":"'"$TEXT_ESC"'" } ] } }'

    lipc-set-prop com.lab126.pillow pillowAlert "$JSON"
}

# Check if KinAMP is running in background
if is_process_running "KinAMP-minimal"; then
    echo "Kinamp is running in background. Stopping it..."
    pkill "KinAMP-minimal"
    sleep 2
    if is_process_running "KinAMP-minimal"; then
        echo "Process didn't terminate gracefully. Force killing..."
        pkill -9 "KinAMP_minimal"
    fi
    alert "KinAMP","Background music playback stopped"
else
    echo "Starting KinAMP GUI..."
    cd /nmt/us/KinAMP
    ./KinAMP
    exit_code=$?
    
    # Check if exit code is 10
    if [ $exit_code -eq 10 ]; then
        # Pillow dialog
        alert "KinAMP","Continuing playing music in background.<br/>Click the KinAMP booklet again to stop."
        ./KinAMP-minimal &
    fi
fi
