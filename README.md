# Concurrent Reservation System

## 1. Project Overview

This Operating Systems project implements a concurrent client-server reservation system in C++. It demonstrates inter-process communication with POSIX Message Queues, pthread worker concurrency, shared resources, race conditions, critical sections, mutex synchronization, and multi-terminal testing inside one Docker container.

The server manages 20 in-memory reservation resources. Clients submit commands through a shared request queue and receive results through separate per-client response queues.

## 2. Architecture

```text
Client-1 --\
Client-2 ---+--> /request_queue --> Worker-1 --\
Client-N --/                     --> Worker-2 ---+--> reservations[20]
                                 --> Worker-N --/       |
                                                   reservations_mutex

Server --> /response_1 --> Client-1
       --> /response_2 --> Client-2
       --> /response_N --> Client-N
```

All pthread workers consume requests from the same request queue and share the same reservation table. The worker count and reservation synchronization mode are selected when the server starts.

## 3. Message Queue Design

- Request queue: `/request_queue`
- Client response queue: `/response_<client_id>`

Every client sends to the shared request queue. The server uses the request's `client_id` to open that client's response queue and return a result. Concurrent clients must use unique IDs so that each owns a distinct response queue.

## 4. Message Structure

`RequestMessage` contains:

- `client_id`: identifies the requesting client.
- `command_type`: the requested `Command` enum value.
- `resource_id`: the target reservation resource, or `-1` for `LIST`.

`ResponseMessage` contains:

- `client_id`: identifies the destination client.
- `success`: reports whether the operation succeeded.
- `message`: a fixed-size character buffer containing the result.

The queue payloads use fixed-size, raw-memory-compatible fields because POSIX Message Queues transfer byte buffers. `std::string` is not stored directly in a queue message because it contains process-local dynamic state.

## 5. Supported Commands

```text
LIST                    Show every resource and its current owner.
STATUS <resource_id>    Show whether one resource is available or reserved.
RESERVE <resource_id>   Reserve an available resource for the current client.
CANCEL <resource_id>    Cancel a reservation owned by the current client.
QUIT                    Exit the client.
```

Valid resource IDs are 1 through 20.

## 6. Build Locally on Linux or WSL

This project targets Linux or WSL because it uses POSIX Message Queues.

```bash
make
```

The build creates `server` and `client` in the project root. Other useful targets are:

```bash
make clean
make rebuild
```

Start the server before starting clients:

```bash
./server --workers 1 --no-sync
./client 1
```

## 7. Docker Build

```bash
docker build -t os-reservation .
```

The image uses Ubuntu 24.04, installs `build-essential`, and builds both binaries with `make`.

## 8. Docker Run

Create one named, long-running demo container:

```bash
docker run -dit --name os-reservation os-reservation
```

The standard Linux Docker runtime provides `/dev/mqueue` inside the container. No privileged mode or host IPC namespace is required because the server and all clients run in the same container. Verify the mount with:

```bash
docker exec os-reservation ls -ld /dev/mqueue
```

## 9. Open Multiple Terminals

Open each process in a separate terminal attached to the same container:

```bash
docker exec -it os-reservation bash
```

Run that command again for Terminal 2, Terminal 3, and any additional clients. Using one container is intentional: every process shares its Linux POSIX Message Queue namespace.

## 10. Experiment 1 — Sequential Baseline

In Terminal 1:

```bash
./server --workers 1 --no-sync
```

Open five more terminals and run one unique client in each:

```bash
./client 1
./client 2
./client 3
./client 4
./client 5
```

Have several clients submit `RESERVE 10`. With one worker, requests are processed sequentially even though clients may submit concurrently. One request succeeds and later requests observe that Seat 10 is already reserved; no worker race occurs.

## 11. Experiment 2 — Concurrent Without Synchronization

In Terminal 1:

```bash
./server --workers 3 --no-sync
```

Use at least five uniquely numbered clients and issue `RESERVE 10` as close together as possible. The intentional race window is:

```text
check AVAILABLE
random delay of 50–500 ms
update owner
```

A possible server log is:

```text
[Worker-1] check Seat 10: AVAILABLE
[Worker-2] check Seat 10: AVAILABLE
[Worker-1] delaying 240 ms
[Worker-2] delaying 410 ms
[Worker-1] Seat 10 reserved by Client-1
[Worker-2] Seat 10 reserved by Client-2
```

Multiple clients may receive `SUCCESS`, although the final table contains only the owner written last. The random delay exists only to make the race condition easier to observe; no particular client is guaranteed to win.

## 12. Experiment 3 — Concurrent With Synchronization

In Terminal 1:

```bash
./server --workers 3 --sync
```

Repeat the same concurrent `RESERVE 10` test. The protected sequence is:

```text
lock
check
delay
update
unlock
```

Exactly one client should succeed. Later workers enter the critical section only after the first worker leaves and therefore observe the resource as reserved.

```text
[Worker-1] entering critical section
[Worker-1] check Seat 10: AVAILABLE
[Worker-1] delaying 275 ms
[Worker-1] Seat 10 reserved by Client-1
[Worker-1] leaving critical section
[Worker-2] entering critical section
[Worker-2] check Seat 10: RESERVED by Client-1
[Worker-2] leaving critical section
```

## 13. Synchronization Toggle

Disable reservation-table synchronization:

```bash
--no-sync
```

Enable reservation-table synchronization:

```bash
--sync
```

`log_mutex` remains active in both modes, but it protects only console output from interleaving. `--no-sync` disables synchronization of shared reservation data; it does not mean that every mutex is removed.

## 14. Shared Resource and Critical Sections

The shared resource is:

```cpp
Reservation reservations[RESOURCE_COUNT];
```

The reservation critical sections are:

- `RESERVE`: check availability, perform the intentional delay, and update the owner.
- `CANCEL`: read the owner and clear it when the requesting client owns it.
- `STATUS`: copy the current owner.
- `LIST`: copy all ownership values into a local snapshot.

The message queue serializes queue operations, not the work performed after different workers receive requests. Therefore, the queue alone cannot prevent races on `reservations[]`.

## 15. Graceful Shutdown

Press `Ctrl+C` in the server terminal. The main thread receives SIGINT through `sigwait()`, sends one internal stop request per worker, joins every worker, closes the request queue, and unlinks `/request_queue`.

## 16. Cleanup and Troubleshooting

Inspect active POSIX Message Queue names:

```bash
ls /dev/mqueue
```

The server removes a previous `/request_queue` name during startup and removes its own queue during graceful shutdown. Start the server before clients. If a client reports `mq_open: No such file or directory`, the request queue does not exist because the server is not running.

Stop and remove the Docker demo container with:

```bash
docker rm -f os-reservation
```

## 17. Known Limitations

- Concurrent clients must use unique client IDs.
- Reservation state is stored only in memory and resets whenever the server restarts.
- Synchronized mode uses one global reservation mutex rather than per-resource locks.
- The system is intended for a local Linux, WSL, or single-container Docker demonstration.
- No persistent database is used.
