#!bin/bash
# sudo apt install -y netcat-openbsd socat
# /etc/default/snapclient2: SNAPCLIENT_OPTS="--sampleformat 48000:24:* -s14 -hCD-player"

HOST="CD-player"
PORT=1705
LAST_STATUS=""

log() {
    echo "$(date '+%Y-%m-%d %H:%M:%S') [CD Status] $1"
}

# Subscribe to OnUpdate events
printf '{"jsonrpc":"2.0","method":"Server.OnUpdate"}\n' \
    | nc "$HOST" "$PORT" \
    | while read -r line; do
        # Extract status if it exists
        STATUS=$(echo "$line" | jq -r '.params.stream.status? // .result.server.streams[0].status? // empty')

        # Act only if status changed
        if [[ -n "$STATUS" && "$STATUS" != "$LAST_STATUS" ]]; then
            if [[ "$STATUS" == "playing" ]]; then
                log "CD started playing stopping Volumio, starting Snapclient"
                /usr/local/bin/volumio stop
                sudo /bin/systemctl start snapclient2
                /usr/bin/printf '{"bmp_number":107}\n' | socat - UNIX-CONNECT:/tmp/volumio.sock
            elif [[ "$STATUS" == "idle" ]]; then
                log "CD stopped stopping Snapclient"
                sudo /bin/systemctl stop snapclient2
                /usr/bin/printf '{"bmp_number":102}\n' | socat - UNIX-CONNECT:/tmp/volumio.sock
            fi
            LAST_STATUS="$STATUS"
        fi
    done
