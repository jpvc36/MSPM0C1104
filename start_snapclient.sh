#!/usr/bin/bash

SSH_OPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=3 -o BatchMode=yes"
timeout=30
elapsed=0

while true; do
    if ip route show default >/dev/null 2>&1; then
        break
    fi
    sleep 1
    ((elapsed++))
    if ((elapsed >= timeout)); then
        echo "No default route after $timeout seconds, continuing anyway" >&2
        break
    fi
done

# Get the real local IP used for routing
ip=$(ip -4 route get 1.1.1.1 2>/dev/null | awk '{for(i=1;i<=NF;i++) if($i=="src"){print $(i+1)}}')
# ip=$(ip addr show wlan0 | awk '/inet / {print $2}')

handle_speaker() {
    local host="$1"

    # Check if reachable
    if ssh $SSH_OPTS root@"$host" uci show snapclient >/dev/null 2>&1; then

        ipremote=$(ssh $SSH_OPTS root@"$host" uci get snapclient.config.serverip 2>/dev/null)

        if [[ "$ip" != "$ipremote" ]]; then
            ssh $SSH_OPTS root@"$host" \
                "uci set snapclient.config.serverip='$ip'; uci commit snapclient;" \
                >/dev/null 2>&1
        fi

        ssh $SSH_OPTS root@"$host" \
            "/etc/init.d/snapclient restart; sleep 1; logread | tail" \
            >/dev/null 2>&1
    fi
}

handle_speaker speaker_left
handle_speaker speaker_right

exit 0
