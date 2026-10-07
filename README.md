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

The server owns creation and unlinking of `/request_queue`; each client owns creation and unlinking of its response queue. Both use `O_CREAT | O_EXCL`: a second process cannot silently replace a live queue. A stale queue after forced termination must be removed explicitly, as described below. Only one server runs in a queue namespace.

Both queue types have `mq_maxmsg = 10`. The request queue uses `mq_msgsize = sizeof(RequestMessage)` and responses use `mq_msgsize = sizeof(ResponseMessage)`. Sends use these same sizes; receives reject unexpected byte counts. Clients validate the request queue attributes, and workers validate response queue attributes. Every message is sent at priority zero, so pending requests of equal priority are received in enqueue order; different workers can finish those requests in a different order.

## 4. Message Structure

`RequestMessage` contains:

- `client_id`: identifies the requesting client.
- `command_type`: the requested `Command` enum value.
- `resource_id`: the target reservation resource, or `-1` for `LIST`.

`ResponseMessage` contains:

- `client_id`: identifies the destination client.
- `success`: reports whether the operation succeeded.
- `message`: a fixed-size character buffer containing the result.

The enum values are `LIST=1`, `STATUS=2`, `RESERVE=3`, `CANCEL=4`, and `QUIT=5`. IDs use `int`, success uses `bool`, and the text buffer is `char[1024]`. `LIST` sets `resource_id = -1`. `QUIT` is handled locally by the client; it is not sent to the server. The server reserves `QUIT` with `client_id = -1` for its internal worker shutdown messages.

The queue payloads use fixed-size, raw-memory-compatible fields because POSIX Message Queues transfer byte buffers. `std::string` is not stored directly in a queue message because it contains process-local dynamic state.

Both structures are checked to be trivially copyable. Raw messages include native padding and require the same compiler ABI and structure definitions, supplied by building server and clients together in one Linux image. This is not a portable wire protocol between different builds or architectures.

## 5. Supported Commands

```text
LIST                    Show every resource and its current owner.
STATUS <resource_id>    Show whether one resource is available or reserved.
RESERVE <resource_id>   Reserve an available resource for the current client.
CANCEL <resource_id>    Cancel a reservation owned by the current client.
QUIT                    Exit the client.
```

Valid resource IDs are 1 through 20.

Commands are uppercase. Missing arguments, malformed or overflowing integer tokens, unknown commands, and extra arguments are rejected. The server remains authoritative for the resource range and ownership checks. Clients print `SUCCESS:` or `FAILED:` for responses. `QUIT` and input EOF both close descriptors and unlink the client's response queue.

## 6. Build Locally on Linux or WSL

This project targets Linux or WSL because it uses POSIX Message Queues.

```bash
make clean
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

Full syntax:

```text
./server --workers <positive_integer> --sync|--no-sync
./client <positive_integer_client_id>
```

The worker count is selected at runtime; at least three workers and five unique clients are supported. Both integer arguments reject overflow, leading whitespace, and a leading `+`. Client IDs do not have to be consecutive. `-std=c++17 -Wall -Wextra -Wpedantic` is used for both programs, with `-pthread` for the server and `-lrt` for POSIX MQ linkage. Windows MinGW alone cannot build the Linux POSIX MQ project; use this Docker image or a Linux/WSL development environment.

## 7. Docker Build

```bash
docker build -t os-reservation .
```

The image uses Ubuntu 24.04, installs `build-essential`, and builds both binaries with `make`.

Its working directory is `/app`, and its default command is `sleep infinity`, allowing the presenter to choose each experiment mode manually. `.dockerignore` excludes Git metadata, editor settings, and local binaries.

## 8. Docker Run

Create one named, long-running demo container:

```bash
docker run -dit --name os-reservation os-reservation
```

Starting or restarting the container runs only `sleep infinity`. Docker provides the Linux environment; it does not start the server, any client, or the experiment script. Open terminals with `docker exec -it os-reservation bash`, then launch the server and clients yourself as shown below. Experiment scripts run only when you explicitly invoke them.

If that name already belongs to an old, stopped demo container, remove the old container with `docker rm os-reservation` before creating the new one. A rebuilt image does not update an existing container; recreate the demo container to use changed source.

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

Have several clients submit `RESERVE 10`. With one worker, requests are processed sequentially even though clients may submit concurrently. The artificial random delay is disabled whenever the server has exactly one worker, in either sync mode. One request succeeds and later requests observe that Seat 10 is already reserved; no worker race occurs.

Before each next experiment: finish pending commands, enter `QUIT` in every client terminal, press `Ctrl+C` in the server terminal, then start the new server and reopen clients 1–5. Restarting the server resets every seat to AVAILABLE. Reusing Seat 10 without resetting state will invalidate the experiment.

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

An excerpt actually recorded during finalization is:

```text
[0003] [Worker-1] check Seat 10: AVAILABLE
[0006] [Worker-2] check Seat 10: AVAILABLE
[0009] [Worker-3] check Seat 10: AVAILABLE
[0011] [Worker-3] Seat 10 reserved by Client-4
[0016] [Worker-1] Seat 10 reserved by Client-1
[0017] [Worker-2] Seat 10 reserved by Client-3
```

Multiple clients may receive `SUCCESS`, although the final table contains only the owner written last. The random delay exists only to make the race condition easier to observe; no particular client is guaranteed to win.

The observed run had three successes and two failures. For the live demo, show multiple workers checking AVAILABLE before the first update, multiple client successes, then `STATUS 10` or `LIST` to show the single final owner. If a race is not observed, reset the server and retry; absence of a race in one run is not proof of safety.

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

The same 50–500 ms delay is still executed while holding `reservations_mutex`. The observed finalization run had one success and four failures. The following actual excerpt shows the successful sequence; intervening received-request logs are omitted here:

```text
[0004] [Worker-1] check Seat 10: AVAILABLE
[0005] [Worker-1] delaying 246 ms
[0008] [Worker-1] Seat 10 reserved by Client-1
[0012] [Worker-2] check Seat 10: RESERVED by Client-1
[0015] [Worker-3] check Seat 10: RESERVED by Client-1
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

All server log lines carry a sequence number incremented under `log_mutex`. Worker request logs include command, client ID, and resource ID where applicable. Reservation critical-section entry/exit logs are emitted only with `--sync`. Sequence numbers describe console-log order, not the exact instant of an unprotected memory read.

## 14. Shared Resource and Critical Sections

The shared resource is:

```cpp
Reservation reservations[RESOURCE_COUNT];
```

The reservation critical sections are:

- `RESERVE`: check availability, perform the intentional delay when there is more than one worker, and update the owner.
- `CANCEL`: read the owner and clear it when the requesting client owns it.
- `STATUS`: copy the current owner.
- `LIST`: copy all ownership values into a local snapshot.

An `owner_id` of `-1` means AVAILABLE; a positive ID means RESERVED. Response formatting, opening/checking response queues, and sending responses occur outside `reservations_mutex`. The local LIST snapshot keeps the synchronized read consistent while avoiding holding the lock during text formatting. In no-sync mode its reads are intentionally unprotected and need not form a consistent snapshot.

Workers use `pthread_create` and receive addresses in a pre-sized `vector<int> worker_ids`. Neither vector is resized after thread creation, and their storage remains valid until the created workers have joined. A thread-local random generator avoids sharing generator state between workers.

The message queue serializes queue operations, not the work performed after different workers receive requests. Therefore, the queue alone cannot prevent races on `reservations[]`.

## 15. Graceful Shutdown

Press `Ctrl+C` in the server terminal. SIGINT and SIGTERM are blocked before thread creation so the workers inherit the mask. The main thread receives the shutdown signal through `sigwait()`, sends one internal stop request per created worker, joins every worker, closes the request queue, and unlinks `/request_queue`.

Already queued requests precede the priority-zero stop messages. Finish client commands before stopping the server; do not keep submitting during shutdown. The response send is nonblocking so an abandoned/full client queue cannot stall a worker. A full queue produces a logged delivery error; this does not roll back an already completed reservation.

Partial `pthread_create` failure takes the same stop/join path for workers that did start, then returns failure after cleanup. Shutdown sends have a five-second deadline. If a stop message cannot be sent or a worker cannot be joined, the server reports failure, unlinks its queue, and exits the process without destroying mutexes or argument storage still used by a live worker. Normal shutdown joins all workers first.

## 16. Cleanup and Troubleshooting

Inspect active POSIX Message Queue names:

```bash
ls /dev/mqueue
```

Start the server before clients. If a client reports `mq_open: No such file or directory`, the request queue does not exist because the server is not running. Client failure to open the request queue also removes the response queue it created.

`File exists` means a server/client with that queue name is still active, or a previous process was forcibly killed. Do not unlink an active process's queue. After verifying the owning processes have exited, remove only the stale name, for example inside the demo container:

```bash
rm -f /dev/mqueue/request_queue
rm -f /dev/mqueue/response_3
```

`Ctrl+C` on a client or `SIGKILL` is abrupt termination and may leave a response queue; prefer `QUIT` or EOF. Server `SIGKILL` also bypasses its cleanup. A new container provides a fresh queue namespace.

If `Permission denied` occurs, run clients/server as the same container user. If `/dev/mqueue` is missing, check the Linux Docker runtime and use one shared container. If queue attributes are rejected, rebuild both binaries and remove stale queues after their owners stop. If a client is waiting after a server crash, terminate that client and clean its stale response queue before restarting.

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
- Unsynchronized concurrent reads/writes are deliberately C++ data races (formally undefined behavior). Race reproduction is an observed property of the tested Linux build, not a guarantee for every scheduler, compiler, or optimization setting. Use `--sync` for correct operation.
- The 50–500 ms delay is an experiment aid, not real reservation work. It is disabled with one worker and remains enabled with multiple workers in both modes. In concurrent sync mode it serializes all seat operations behind one global lock.
- Queue capacity is ten messages. A client sends one request and waits for one response; it has no response timeout or automatic recovery if the server exits or delivery fails. A completed reservation can outlive a lost response.
- Queue names and client IDs are intended for trusted local processes. The native structures and internal stop request are not an authenticated or cross-platform protocol.
- Forced termination requires explicit stale-queue cleanup. Clients must restart after the server restarts because existing descriptors refer to the old queue object.

## 18. Experiment Script

Use the named container created in section 8. If it is stopped, start it with `docker start os-reservation`. Open its terminal:

```bash
docker exec -it os-reservation bash
```

Start the server for the desired experiment in one terminal. Run the same client-only script in a second container terminal for every experiment:

| Experiment | Server terminal | Second terminal |
| --- | --- | --- |
| 1 | `./server --workers 1 --no-sync` | `bash experiments.sh` |
| 2 | `./server --workers 3 --no-sync` | `bash experiments.sh` |
| 3 | `./server --workers 3 --sync` | `bash experiments.sh` |

The script takes no arguments and only runs clients. It does not build programs, start or stop the server, change its mode, or manage containers. Server logs appear live in the server terminal; client results appear in the second terminal. QUIT any existing clients 1–5 first, since the script uses their IDs. It launches five clients concurrently to send only `RESERVE 10` to the server, then displays the SUCCESS/FAILED counts. Each client exits with the local `QUIT` command. It sends no STATUS requests and does not check or reset Seat 10. It cleans up its client processes, response queues, and temporary logs, leaving the server and container running.

Restart the server before each experiment to reset Seat 10. Experiments 1 and 3 should produce one success and four failures. Experiment 2 may produce multiple successes; confirm multiple AVAILABLE checks in the server logs. If the race is not observed, restart the server and run the same script again. The script reports actual results without claiming which server mode is active.

If the existing container has an older script, update it from the host before opening the terminal:

```bash
docker cp experiments.sh os-reservation:/app/experiments.sh
```

On Linux, use `bash experiments.sh` from the project directory with a running server. The manual multi-terminal procedures above remain available.

### Demo 1 — Different Commands at the Same Time

In the server terminal, start a fresh server:

```bash
./server --workers 3 --sync
```

In a second terminal attached to the same container, run:

```bash
bash demo1.sh
```

| Client | Concurrent command |
| --- | --- |
| 1 | `LIST` |
| 2 | `STATUS 1` |
| 3 | `RESERVE 2` |
| 4 | `CANCEL 3` |
| 5 | `RESERVE 4` |

The script sends only the five commands above concurrently, with no setup requests, and prints each response, including the full LIST. On a fresh server, `CANCEL 3` fails because Seat 3 is already available; the other four commands succeed. Execution order depends on scheduling; LIST may capture state before or after the reservation changes. QUIT any existing clients 1–5 before running it. Each scripted client exits with QUIT, and the server and container stay running. Existing reservations affect results when repeating the demo.

For an existing container, copy the new script from the host first:

```bash
docker cp demo1.sh os-reservation:/app/demo1.sh
```

## 19. Report Preparation

No assignment specification or report source was present in the repository. The audit used the supplied requirements and rubric; confirm any instructor-specific report format separately. Use the following factual sources to assemble the report:

| Required section | Evidence to include |
| --- | --- |
| Project Description | Twenty seats; client commands and reservation ownership rules in sections 1 and 5. |
| System Architecture | Client/queue/worker/table diagram in section 2; all processes in one Linux container. |
| Message Queue Design | Queue ownership, names, capacity, sizes, priority, and lifecycle in section 3. |
| Message Structure | Native fields, enum values, fixed text buffer, ABI boundary in section 4 and `include/message.hpp`. |
| Server Concurrency Model | Shared receive queue, runtime pthread count, stable worker-ID storage in sections 2 and 14. |
| Shared Resource and Critical Section | `reservations[]`, `owner_id`, and each operation's boundaries in section 14. |
| Cause of Race Condition | Multiple workers check the same available owner and later overwrite it; MQ does not lock application memory. |
| Role of Random Delay | Same 50–500 ms check/update window in both concurrent experiments; held inside the mutex in sync mode. |
| Synchronization Mechanism | One `reservations_mutex`; independent `log_mutex` remains enabled in both modes. |
| Results of All Three Experiments | Experiment procedures, observed counts, and sequence-numbered excerpts in sections 10–12; add screenshots from the live demo. |
| Limitations | Section 17; nondeterminism, deliberate unsafe mode, global lock, volatile state, queue lifetime/delivery. |
| Future Improvements | Possible follow-up work: remove artificial delay for real use, persist state, add bounded response waits and request IDs for recovery; use finer locking only if measured throughput requires it. These are not implemented in this lab. |
