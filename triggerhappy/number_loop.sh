#!/bin/sh

INPUT=$1
VALUE=$2

TIMEOUT=1.5
PIDFILE="/tmp/number_input.pid"
NUMBERFILE="/tmp/number_input"
#. /usr/local/bin/config
VOLUMIO_IP=192.168.39.30

if [ -f "$PIDFILE" ]; then
    OLD_PID=$(cat "$PIDFILE")
    kill "$OLD_PID" 2>/dev/null
    rm -f "$PIDFILE"
fi

# On command key down or keep pressed
if [[ ! "$INPUT" =~ ^[0-9]$ ]]; then

    curl "http://$VOLUMIO_IP:3000/api/v1/commands/?cmd=$INPUT"

    curl -s "http://$VOLUMIO_IP/api/v1/getState" | jq 'if .mute == true then 104 else .volume end' | /root/a.out

    exit 0
fi

# Long press number key
if [[ "$VALUE" = "keep" && "$INPUT" =~ ^[0-9]$ ]]; then
/*
    # Cancel pending number entry
    if [ -f "$PIDFILE" ]; then
        OLD_PID=$(cat "$PIDFILE")
        kill "$OLD_PID" 2>/dev/null
        rm -f "$PIDFILE"
    fi
*/
    rm -f "$NUMBERFILE"

    # Start the new playlist at number 1
    echo "Long press: changing playlist"

    curl "http://$VOLUMIO_IP:3000/api/v1/commands/?cmd=playplaylist&name=IR_$INPUT"

    exit 0
fi

# Only number presses from here
[ "$VALUE" = "press" ] || exit 0

# Add digit to number
if [ ! -f "$NUMBERFILE" ]; then
    echo -n "$INPUT" > "$NUMBERFILE"
else
    NUMBER=$(cat "$NUMBERFILE")

    if [ "${#NUMBER}" -lt 3 ]; then
        echo -n "${NUMBER}${INPUT}" > "$NUMBERFILE"
    fi
fi
/*
# Restart timeout
if [ -f "$PIDFILE" ]; then
    OLD_PID=$(cat "$PIDFILE")
    kill "$OLD_PID" 2>/dev/null
    rm -f "$PIDFILE"
fi
*/
(
    echo $$ > "$PIDFILE"
    sleep "$TIMEOUT"

    if [ -f "$NUMBERFILE" ]; then
        NUMBER=$(cat "$NUMBERFILE")
        rm -f "$NUMBERFILE"
        rm -f "$PIDFILE"

        NUMBER=$((NUMBER - 1))

        curl "http://$VOLUMIO_IP:3000/api/v1/commands/?cmd=play&N=$NUMBER"
    fi
) &
