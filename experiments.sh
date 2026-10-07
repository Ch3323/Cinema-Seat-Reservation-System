#!/usr/bin/env bash
# Run the same five-client test against whichever server the user starts.
set -euo pipefail
cd "$(dirname "$0")"

if [[ $# -ne 0 ]]; then
    echo 'Usage: bash experiments.sh (start the server manually in another terminal).' >&2
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
    # These IDs were unused at startup. Never remove the server's request queue.
    rm -f /dev/mqueue/response_{1..5}
    rm -rf "$logs"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
fail() { echo "FAILED: $*" >&2; cat "$logs"/*.log >&2; exit 1; }
echo 'Launching five clients: RESERVE 10'
for id in 1 2 3 4 5; do
    printf 'RESERVE 10\nQUIT\n' |
        timeout --kill-after=2s 15s ./client "$id" >"$logs/client-$id.log" 2>&1 &
    client_pids+=("$!")
done
for pid in "${client_pids[@]}"; do
    wait "$pid" || fail 'A client failed or timed out'
done
client_pids=()

successes=$(awk '/SUCCESS: Seat 10 reserved/ {n++} END {print n+0}' "$logs"/client-*.log)
failures=$(awk '/FAILED: Seat 10 is already reserved/ {n++} END {print n+0}' "$logs"/client-*.log)
[[ $((successes + failures)) -eq 5 ]] || fail 'Expected five reservation responses'
for id in 1 2 3 4 5; do
    echo "Client-$id: $(grep -oE '(SUCCESS|FAILED):.*' "$logs/client-$id.log")"
done
echo "Results: SUCCESS=$successes, FAILED=$failures"
echo 'Finished: server and container remain running. See server logs in the other terminal.'
