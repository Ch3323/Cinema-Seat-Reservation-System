#!/usr/bin/env bash
# Send different commands concurrently to a server started by the user.
set -euo pipefail
cd "$(dirname "$0")"

if [[ $# -ne 0 ]]; then
    echo 'Usage: bash demo1.sh' >&2
    exit 1
fi
if [[ ! -e /dev/mqueue/request_queue || ! -x ./client ]]; then
    echo 'Build the client and start the server in another container terminal first.' >&2
    exit 1
fi
if compgen -G '/dev/mqueue/response_[1-5]' >/dev/null; then
    echo 'QUIT clients 1–5 and remove their stale queues before running.' >&2
    exit 1
fi

logs=$(mktemp -d)
client_pids=()
cleanup() {
    for pid in "${client_pids[@]}"; do
        kill -TERM "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
    rm -f /dev/mqueue/response_{1..5}
    rm -rf "$logs"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
fail() { echo "FAILED: $*" >&2; cat "$logs"/*.log >&2; exit 1; }
launch_client() {
    printf '%b\nQUIT\n' "$2" |
        timeout --kill-after=2s 15s ./client "$1" >"$3" 2>&1 &
    client_pids+=("$!")
}
wait_clients() {
    for pid in "${client_pids[@]}"; do
        wait "$pid" || fail 'A client failed or timed out'
    done
    client_pids=()
}

commands=('LIST' 'STATUS 1' 'RESERVE 2' 'CANCEL 3' 'RESERVE 4')
echo 'Demo 1: five clients sending different commands concurrently'
for id in 1 2 3 4 5; do
    command=${commands[$((id - 1))]}
    echo "Client-$id sends: $command"
    launch_client "$id" "$command" "$logs/client-$id.log"
done
wait_clients

for id in 1 2 3 4 5; do
    echo
    echo "--- Client-$id: ${commands[$((id - 1))]} ---"
    cat "$logs/client-$id.log"
    grep -Eq 'SUCCESS:|FAILED:' "$logs/client-$id.log" || fail "Client-$id received no response"
done
echo
echo 'Demo 1 completed: all five clients received responses. Server and container remain running.'
echo 'Server logs are in the other terminal. LIST may show a snapshot before or after the concurrent changes.'
