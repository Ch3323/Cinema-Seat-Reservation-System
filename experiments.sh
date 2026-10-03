#!/usr/bin/env bash
# Run on Linux or inside the project Docker image, with no other server/clients.
set -euo pipefail
cd "$(dirname "$0")"

selection=${1:-all}
if [[ $# -gt 1 || ! "$selection" =~ ^(all|1|2|3)$ ]]; then
    echo "Usage: bash experiments.sh [all|1|2|3]" >&2
    exit 1
fi
if [[ ! -d /dev/mqueue ]]; then
    echo 'POSIX message queues are unavailable; run inside the Linux Docker image.' >&2
    exit 1
fi
if [[ -e /dev/mqueue/request_queue ]] || compgen -G '/dev/mqueue/response_*' >/dev/null; then
    echo 'Stop existing server/clients and remove their stale queues before running.' >&2
    exit 1
fi
make
logs=$(mktemp -d)
server_pid=''
client_pids=()
owns_queues=0

cleanup() {
    for pid in "${client_pids[@]}"; do
        kill -TERM "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
    if [[ -n "$server_pid" ]]; then
        kill -TERM "$server_pid" 2>/dev/null || true
        for ((n=0; n<100; n++)); do
            kill -0 "$server_pid" 2>/dev/null || break
            sleep 0.02
        done
        if kill -0 "$server_pid" 2>/dev/null; then kill -KILL "$server_pid" 2>/dev/null || true; fi
        wait "$server_pid" 2>/dev/null || true
    fi
    if [[ "$owns_queues" -eq 1 ]]; then
        rm -f /dev/mqueue/request_queue /dev/mqueue/response_{1..5}
    fi
    rm -rf "$logs"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
fail() { echo "FAILED: $*" >&2; cat "$logs/server.log" >&2; exit 1; }

run_experiment() {
    local number=$1 workers=$2 mode=$3
    echo
    echo "Experiment $number: ./server --workers $workers $mode"
    ./server --workers "$workers" "$mode" >"$logs/server.log" 2>&1 &
    server_pid=$!
    for ((n=0; n<100; n++)); do
        if grep -q 'Server running with' "$logs/server.log"; then break; fi
        kill -0 "$server_pid" 2>/dev/null || fail 'Server exited during startup'
        sleep 0.05
    done
    grep -q 'Server running with' "$logs/server.log" || fail 'Server startup timed out'
    owns_queues=1

    # Each background pipeline has its own client, so requests can overlap.
    client_pids=()
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
    available_workers=$(awk '
        /Seat 10 reserved by/ {exit}
        /check Seat 10: AVAILABLE/ {seen[$2]=1}
        END {for (worker in seen) n++; print n+0}
    ' "$logs/server.log")

    printf 'STATUS 10\nQUIT\n' | timeout --kill-after=2s 15s ./client 1 >"$logs/status.log" 2>&1 ||
        fail 'Final STATUS failed'
    kill -INT "$server_pid"
    for ((n=0; n<100; n++)); do
        kill -0 "$server_pid" 2>/dev/null || break
        sleep 0.05
    done
    if kill -0 "$server_pid" 2>/dev/null; then fail 'Server shutdown timed out'; fi
    wait "$server_pid" || fail 'Server shutdown failed'
    server_pid=''
    [[ ! -e /dev/mqueue/request_queue ]] || fail 'Request queue was not removed'
    for id in 1 2 3 4 5; do
        [[ ! -e /dev/mqueue/response_$id ]] || fail "Client-$id queue was not removed"
    done
    owns_queues=0

    cat "$logs/server.log"
    for id in 1 2 3 4 5; do
        echo "Client-$id: $(grep -oE '(SUCCESS|FAILED):.*' "$logs/client-$id.log")"
    done
    grep -oE '(SUCCESS|FAILED):.*' "$logs/status.log"
    echo "Results: SUCCESS=$successes, FAILED=$failures; workers seeing AVAILABLE before first update=$available_workers"

    if [[ "$mode" == --no-sync ]]; then
        if grep -q 'critical section' "$logs/server.log"; then fail 'Unexpected critical-section logs'; fi
    else
        grep -q 'entering critical section' "$logs/server.log" || fail 'Missing mutex entry logs'
        grep -q 'leaving critical section' "$logs/server.log" || fail 'Missing mutex exit logs'
    fi
    if [[ "$number" != 2 ]]; then
        [[ "$successes" -eq 1 && "$failures" -eq 4 && "$available_workers" -eq 1 ]] ||
            fail 'Expected exactly one reservation success'
        echo "Experiment $number VERIFIED"
    fi
}

if [[ "$selection" == all || "$selection" == 1 ]]; then run_experiment 1 1 --no-sync; fi
if [[ "$selection" == all || "$selection" == 2 ]]; then
    race_observed=0
    for ((attempt=1; attempt<=5; attempt++)); do
        echo "Race attempt $attempt of 5"
        run_experiment 2 3 --no-sync
        if [[ "$successes" -ge 2 && "$available_workers" -ge 2 ]]; then
            race_observed=1
            echo 'Experiment 2 VERIFIED: race observed'
            break
        fi
    done
    if [[ "$race_observed" -eq 0 ]]; then
        echo 'Experiment 2 RACE NOT OBSERVED in 5 attempts; retry (this is nondeterministic).'
    fi
fi
if [[ "$selection" == all || "$selection" == 3 ]]; then run_experiment 3 3 --sync; fi
echo
echo 'Finished: experiment processes, queues, and temporary logs cleaned up.'
