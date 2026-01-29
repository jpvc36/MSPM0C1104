#!/usr/bin/bash

SSH_OPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=3 -o BatchMode=yes"
timeout=40
elapsed=0

while ! ip -4 addr show wlan0 | grep -q 'inet '; do
logger $(ip route show default) time: $(date +%T)
    sleep 1
    ((elapsed++))
    if ((elapsed >= timeout)); then
        echo "No default route after $timeout seconds, continuing anyway" >&2
        break
    fi
done

#sleep 15

# Get the real local IP used for routing
ip=$(ip -4 route get 1.1.1.1 2>/dev/null | awk '{for(i=1;i<=NF;i++) if($i=="src"){print $(i+1)}}')

handle_speaker() {
    local host="$1"

    # Check if reachable
    if ssh $SSH_OPTS root@"$host" uci show snapclient >/dev/null 2>&1; then

        ipremote=$(ssh $SSH_OPTS root@"$host" uci get snapclient.config.serverip 2>/dev/null)
        if [[ "$ip" != "$ipremote" ]]; then
            ssh $SSH_OPTS root@"$host" \
                "uci set snapclient.config.serverip='$ip'; uci commit snapclient;" \
                >/dev/null 2>&1
            logger $host snapclient connects to $ip time: $(date +%T)
        fi

        ssh $SSH_OPTS root@"$host" \
            /etc/init.d/snapclient restart >/dev/null 2>&1
    fi
}

handle_speaker speaker_left
handle_speaker speaker_right

exit 0
