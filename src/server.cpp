#include <iostream>
#include <cstdio>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <mqueue.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>

#include "../include/message.hpp"
#include "../include/reservation.hpp"

using namespace std;

mqd_t request_mq;
Reservation reservations[RESOURCE_COUNT];
bool sync_enabled;
pthread_mutex_t reservations_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
unsigned long long log_sequence = 0; // Protected by log_mutex in both modes.

const char* command_name(Command command) {
    switch (command) {
        case Command::LIST: return "LIST";
        case Command::STATUS: return "STATUS";
        case Command::RESERVE: return "RESERVE";
        case Command::CANCEL: return "CANCEL";
        case Command::QUIT: return "QUIT";
        default: return "UNKNOWN";
    }
}

void log_worker(int worker_id, const string& message) {
    pthread_mutex_lock(&log_mutex);
    cout << '[' << setfill('0') << setw(4) << ++log_sequence << setfill(' ') << "] ";
    if (worker_id > 0) cout << "[Worker-" << worker_id << "] ";
    else cout << "[Server] ";
    cout << message << endl;
    pthread_mutex_unlock(&log_mutex);
}

void widen_race_window(int worker_id) {
    thread_local mt19937 generator(random_device{}());
    thread_local uniform_int_distribution<int> delay_ms(50, 500);
    int delay = delay_ms(generator);
    log_worker(worker_id, "delaying " + to_string(delay) + " ms");
    this_thread::sleep_for(chrono::milliseconds(delay));
}

void enter_critical_section(int worker_id) {
    if (!sync_enabled) return;
    pthread_mutex_lock(&reservations_mutex);
    log_worker(worker_id, "entering critical section");
}

void leave_critical_section(int worker_id) {
    if (!sync_enabled) return;
    log_worker(worker_id, "leaving critical section");
    pthread_mutex_unlock(&reservations_mutex);
}

void* worker(void* argument) {
    int worker_id = *static_cast<int*>(argument);

    while (true) {
        RequestMessage request{};
        ssize_t bytes_received = mq_receive(
            request_mq, reinterpret_cast<char*>(&request), sizeof(request), nullptr
        );
        if (bytes_received == -1) {
            if (errno == EINTR) continue;
            log_worker(worker_id, "mq_receive: " + string(strerror(errno)));
            return nullptr;
        }
        if (bytes_received != static_cast<ssize_t>(sizeof(request))) {
            log_worker(worker_id, "Invalid request size");
            continue;
        }
        if (request.command_type == Command::QUIT && request.client_id == -1) {
            return nullptr;
        }
        if (request.client_id <= 0) {
            log_worker(worker_id, "Invalid client_id; request discarded");
            continue;
        }

        string request_log = "received " + string(command_name(request.command_type));
        if (request.command_type == Command::STATUS ||
            request.command_type == Command::RESERVE ||
            request.command_type == Command::CANCEL) {
            request_log += " " + to_string(request.resource_id);
        }
        request_log += " from Client-" + to_string(request.client_id);
        log_worker(worker_id, request_log);

        string response_queue_name = "/response_" + to_string(request.client_id);
        // An abandoned/full response queue must not block a worker or shutdown.
        mqd_t response_mq = mq_open(response_queue_name.c_str(), O_WRONLY | O_NONBLOCK);
        if (response_mq == (mqd_t)-1) {
            log_worker(worker_id, "response mq_open: " + string(strerror(errno)));
            continue;
        }

        mq_attr response_attr{};
        int attr_result = mq_getattr(response_mq, &response_attr);
        if (attr_result == -1 ||
            response_attr.mq_msgsize != static_cast<long>(sizeof(ResponseMessage))) {
            log_worker(worker_id, attr_result == -1
                ? "response mq_getattr: " + string(strerror(errno))
                : "Invalid response queue message size");
            if (mq_close(response_mq) == -1) {
                log_worker(worker_id, "response mq_close: " + string(strerror(errno)));
            }
            continue;
        }

        ResponseMessage response{};
        response.client_id = request.client_id;
        int resource_id = request.resource_id;

        if (request.command_type != Command::LIST &&
            request.command_type != Command::STATUS &&
            request.command_type != Command::RESERVE &&
            request.command_type != Command::CANCEL) {
            snprintf(response.message, sizeof(response.message), "Unsupported command");
        } else if (request.command_type == Command::LIST) {
            if (resource_id != -1) {
                snprintf(response.message, sizeof(response.message),
                         "LIST does not accept a resource_id");
            } else {
                int owner_snapshot[RESOURCE_COUNT];

                enter_critical_section(worker_id);
                for (int i = 0; i < RESOURCE_COUNT; i++) {
                    owner_snapshot[i] = reservations[i].owner_id;
                }
                leave_critical_section(worker_id);

                string list = "\n===== Reservation List =====\n";
                for (int i = 0; i < RESOURCE_COUNT; i++) {
                    list += "Seat " + to_string(i + 1) + "\t: ";
                    if (owner_snapshot[i] == -1) {
                        list += "AVAILABLE\n";
                    } else {
                        list += "RESERVED by Client-" +
                                to_string(owner_snapshot[i]) + '\n';
                    }
                }
                list += "============================\n";
                response.success = true;
                snprintf(response.message, sizeof(response.message), "%s", list.c_str());
            }
        } else if (resource_id < 1 || resource_id > RESOURCE_COUNT) {
            snprintf(response.message, sizeof(response.message), "Invalid resource_id");
        } else {
            int resource_index = resource_id - 1;

            if (request.command_type == Command::RESERVE) {
                bool reserved = false;
                int owner_id;

                enter_critical_section(worker_id);
                owner_id = reservations[resource_index].owner_id;
                if (owner_id == -1) {
                    log_worker(worker_id, "check Seat " + to_string(resource_id) + ": AVAILABLE");
                    widen_race_window(worker_id);
                    reservations[resource_index].owner_id = request.client_id;
                    reserved = true;
                    log_worker(worker_id, "Seat " + to_string(resource_id) +
                                          " reserved by Client-" + to_string(request.client_id));
                } else {
                    log_worker(worker_id, "check Seat " + to_string(resource_id) +
                                          ": RESERVED by Client-" + to_string(owner_id));
                }
                leave_critical_section(worker_id);

                response.success = reserved;
                if (reserved) {
                    snprintf(response.message, sizeof(response.message),
                             "Seat %d reserved", resource_id);
                } else {
                    snprintf(response.message, sizeof(response.message),
                             "Seat %d is already reserved", resource_id);
                }
            } else if (request.command_type == Command::CANCEL) {
                int owner_id;
                bool canceled = false;

                enter_critical_section(worker_id);
                owner_id = reservations[resource_index].owner_id;
                if (owner_id == request.client_id) {
                    reservations[resource_index].owner_id = -1;
                    canceled = true;
                }
                leave_critical_section(worker_id);

                response.success = canceled;
                if (owner_id == -1) {
                    snprintf(response.message, sizeof(response.message),
                             "Seat %d is not reserved", resource_id);
                } else if (canceled) {
                    snprintf(response.message, sizeof(response.message),
                             "Seat %d reservation canceled", resource_id);
                } else {
                    snprintf(response.message, sizeof(response.message),
                             "Seat %d belongs to another client", resource_id);
                }
            } else if (request.command_type == Command::STATUS) {
                int owner_id;

                enter_critical_section(worker_id);
                owner_id = reservations[resource_index].owner_id;
                leave_critical_section(worker_id);

                response.success = true;
                if (owner_id == -1) {
                    snprintf(response.message, sizeof(response.message),
                             "Seat %d is available", resource_id);
                } else {
                    snprintf(response.message, sizeof(response.message),
                             "Seat %d is reserved by Client-%d", resource_id, owner_id);
                }
            }
        }

        int send_result;
        do {
            send_result = mq_send(response_mq, reinterpret_cast<const char*>(&response),
                                  sizeof(response), 0);
        } while (send_result == -1 && errno == EINTR);
        if (send_result == -1) {
            log_worker(worker_id, "response mq_send: " + string(strerror(errno)));
        }
        if (mq_close(response_mq) == -1) {
            log_worker(worker_id, "response mq_close: " + string(strerror(errno)));
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc != 4 || string(argv[1]) != "--workers" ||
        (string(argv[3]) != "--sync" && string(argv[3]) != "--no-sync")) {
        cerr << "Usage: " << argv[0] << " --workers <count> --sync|--no-sync\n";
        return 1;
    }

    int worker_count = 0;
    const char* count_end = argv[2] + strlen(argv[2]);
    auto parsed_count = from_chars(argv[2], count_end, worker_count);
    if (parsed_count.ec != errc{} || parsed_count.ptr != count_end || worker_count <= 0) {
        cerr << "worker count must be a positive integer\n";
        return 1;
    }

    // Sized before creation and never reallocated; IDs outlive every worker.
    vector<pthread_t> workers;
    vector<int> worker_ids;
    try {
        workers.resize(worker_count);
        worker_ids.resize(worker_count);
    } catch (const exception& error) {
        cerr << "Worker storage: " << error.what() << '\n';
        return 1;
    }

    sync_enabled = string(argv[3]) == "--sync";

    for (int i = 0; i < RESOURCE_COUNT; i++) {
        reservations[i] = {i + 1, -1};
    }

    sigset_t shutdown_signals;
    sigemptyset(&shutdown_signals);
    sigaddset(&shutdown_signals, SIGINT);
    sigaddset(&shutdown_signals, SIGTERM);
    int error = pthread_sigmask(SIG_BLOCK, &shutdown_signals, nullptr);
    if (error != 0) {
        cerr << "pthread_sigmask: " << strerror(error) << '\n';
        return 1;
    }

    const char* queue_name = "/request_queue";
    mq_attr attr{};
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = sizeof(RequestMessage);
    request_mq = mq_open(queue_name, O_CREAT | O_EXCL | O_RDWR, 0666, &attr);
    if (request_mq == (mqd_t)-1) {
        int open_error = errno;
        perror("mq_open");
        if (open_error == EEXIST) {
            cerr << "Only one server may run; remove a stale request queue after the old server exits.\n";
        }
        return 1;
    }

    int created_count = 0;
    int exit_status = 0;
    for (int i = 0; i < worker_count; i++) {
        worker_ids[i] = i + 1;
        error = pthread_create(&workers[i], nullptr, worker, &worker_ids[i]);
        if (error != 0) {
            log_worker(0, "pthread_create: " + string(strerror(error)));
            exit_status = 1;
            break;
        }
        ++created_count;
    }

    if (exit_status == 0) {
        log_worker(0, "Server running with " + to_string(worker_count) +
                   (worker_count == 1 ? " worker" : " workers") + " (sync " +
                   (sync_enabled ? "enabled" : "disabled") + "). Press Ctrl+C to stop.");
        int signal_number;
        error = sigwait(&shutdown_signals, &signal_number);
        if (error != 0) {
            log_worker(0, "sigwait: " + string(strerror(error)));
            exit_status = 1;
        }
    }
    log_worker(0, "Server stopping...");

    RequestMessage stop_request{};
    stop_request.client_id = -1;
    stop_request.command_type = Command::QUIT;
    for (int i = 0; i < created_count; i++) {
        timespec deadline{};
        int send_result = clock_gettime(CLOCK_REALTIME, &deadline);
        if (send_result == 0) {
            deadline.tv_sec += 5;
            do {
                send_result = mq_timedsend(request_mq,
                    reinterpret_cast<const char*>(&stop_request), sizeof(stop_request),
                    0, &deadline);
            } while (send_result == -1 && errno == EINTR);
        }
        if (send_result == -1) {
            log_worker(0, "Worker shutdown send: " + string(strerror(errno)));
            // Cannot safely join workers we could not wake. Process exit closes
            // descriptors and terminates them; do not destroy their live mutexes.
            if (mq_unlink(queue_name) == -1) perror("request mq_unlink");
            _Exit(1);
        }
    }
    for (int i = 0; i < created_count; i++) {
        error = pthread_join(workers[i], nullptr);
        if (error != 0) {
            log_worker(0, "pthread_join: " + string(strerror(error)));
            if (mq_unlink(queue_name) == -1) perror("request mq_unlink");
            _Exit(1);
        }
    }

    pthread_mutex_destroy(&reservations_mutex);
    pthread_mutex_destroy(&log_mutex);
    if (mq_close(request_mq) == -1) {
        perror("request mq_close");
        exit_status = 1;
    }
    if (mq_unlink(queue_name) == -1) {
        perror("request mq_unlink");
        exit_status = 1;
    }
    return exit_status;
}
