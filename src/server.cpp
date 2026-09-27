#include <iostream>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <mqueue.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>

#include "../include/message.hpp"
#include "../include/reservation.hpp"

using namespace std;

mqd_t request_mq;
Reservation reservations[RESOURCE_COUNT];
bool sync_enabled;
pthread_mutex_t reservations_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

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
    cout << "[Worker-" << worker_id << "] " << message << '\n';
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
            perror("mq_receive");
            continue;
        }
        if (bytes_received != sizeof(request)) {
            cerr << "Invalid request size\n";
            continue;
        }
        if (request.command_type == Command::QUIT && request.client_id == -1) {
            return nullptr;
        }

        string request_log = "received " + string(command_name(request.command_type));
        if (request.command_type != Command::LIST) {
            request_log += " " + to_string(request.resource_id);
        }
        request_log += " from Client-" + to_string(request.client_id);
        log_worker(worker_id, request_log);

        string response_queue_name = "/response_" + to_string(request.client_id);
        mqd_t response_mq = mq_open(response_queue_name.c_str(), O_WRONLY);
        if (response_mq == (mqd_t)-1) {
            perror("response mq_open");
            continue;
        }

        ResponseMessage response{};
        response.client_id = request.client_id;
        int resource_id = request.resource_id;

        if (request.command_type == Command::LIST) {
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
            } else {
                snprintf(response.message, sizeof(response.message), "Unsupported command");
            }
        }

        if (mq_send(response_mq, reinterpret_cast<const char*>(&response),
                    sizeof(response), 0) == -1) {
            perror("response mq_send");
        }
        mq_close(response_mq);
    }
}

int main(int argc, char* argv[]) {
    if (argc != 4 || string(argv[1]) != "--workers" ||
        (string(argv[3]) != "--sync" && string(argv[3]) != "--no-sync")) {
        cerr << "Usage: " << argv[0] << " --workers <count> --sync|--no-sync\n";
        return 1;
    }

    int worker_count;
    try {
        string count_argument = argv[2];
        size_t parsed_length;
        worker_count = stoi(count_argument, &parsed_length);
        if (parsed_length != count_argument.length() || worker_count <= 0) {
            throw invalid_argument("worker count");
        }
    } catch (...) {
        cerr << "Usage: " << argv[0] << " --workers <count> --sync|--no-sync\n";
        return 1;
    }

    sync_enabled = string(argv[3]) == "--sync";

    for (int i = 0; i < RESOURCE_COUNT; i++) {
        reservations[i] = {i + 1, -1};
    }

    sigset_t sigint_set;
    sigemptyset(&sigint_set);
    sigaddset(&sigint_set, SIGINT);
    pthread_sigmask(SIG_BLOCK, &sigint_set, nullptr);

    const char* queue_name = "/request_queue";
    mq_attr attr{};
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = sizeof(RequestMessage);
    mq_unlink(queue_name);

    request_mq = mq_open(queue_name, O_CREAT | O_RDWR, 0666, &attr);
    if (request_mq == (mqd_t)-1) {
        perror("mq_open");
        return 1;
    }

    vector<pthread_t> workers(worker_count);
    vector<int> worker_ids(worker_count);
    for (int i = 0; i < worker_count; i++) {
        worker_ids[i] = i + 1;
        int error = pthread_create(&workers[i], nullptr, worker, &worker_ids[i]);
        if (error != 0) {
            cerr << "pthread_create: " << strerror(error) << '\n';
            return 1;
        }
    }

    cout << "Server running with " << worker_count
         << (worker_count == 1 ? " worker" : " workers") << " (sync "
         << (sync_enabled ? "enabled" : "disabled")
         << "). Press Ctrl+C to stop.\n";

    int signal_number;
    sigwait(&sigint_set, &signal_number);
    cout << "\nServer stopping...\n";

    RequestMessage stop_request{};
    stop_request.client_id = -1;
    stop_request.command_type = Command::QUIT;
    for (int i = 0; i < worker_count; i++) {
        mq_send(request_mq, reinterpret_cast<const char*>(&stop_request),
                sizeof(stop_request), 0);
    }
    for (pthread_t thread : workers) {
        pthread_join(thread, nullptr);
    }

    pthread_mutex_destroy(&reservations_mutex);
    pthread_mutex_destroy(&log_mutex);
    if (mq_close(request_mq) == -1) perror("mq_close");
    if (mq_unlink(queue_name) == -1) perror("mq_unlink");
}
