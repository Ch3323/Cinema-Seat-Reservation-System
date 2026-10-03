#include <iostream>
#include <cstdio>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <sstream>
#include <string>
#include <mqueue.h>
#include <fcntl.h>

#include "../include/message.hpp"

using namespace std;

int main(int argc, char* argv[]) {
    if (argc != 2) {
        cerr << "Usage: " << argv[0] << " <client_id>\n";
        return 1;
    }

    int client_id = 0;
    const char* id_end = argv[1] + strlen(argv[1]);
    auto parsed_id = from_chars(argv[1], id_end, client_id);
    if (parsed_id.ec != errc{} || parsed_id.ptr != id_end || client_id <= 0) {
        cerr << "client_id must be a positive integer\n";
        return 1;
    }

    string response_queue_name = "/response_" + to_string(client_id);

    mq_attr attr{};
    attr.mq_flags = 0;
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = sizeof(ResponseMessage);

    mqd_t response_mq = mq_open(
        response_queue_name.c_str(),
        O_CREAT | O_EXCL | O_RDONLY,
        0666,
        &attr
    );

    if (response_mq == (mqd_t)-1) {
        int error = errno;
        perror("response_mq open");
        if (error == EEXIST) {
            cerr << "Use a unique client ID; remove a stale queue only after its client exits.\n";
        }
        return 1;
    }

    mqd_t mq = (mqd_t)-1;
    auto cleanup = [&](int result) {
        if (mq != (mqd_t)-1 && mq_close(mq) == -1) {
            perror("request mq_close");
            result = 1;
        }
        if (mq_close(response_mq) == -1) {
            perror("response mq_close");
            result = 1;
        }
        if (mq_unlink(response_queue_name.c_str()) == -1 && errno != ENOENT) {
            perror("response mq_unlink");
            result = 1;
        }
        return result;
    };

    const char* queue_name = "/request_queue";

    mq = mq_open(
        queue_name,
        O_WRONLY
    );

    if (mq == (mqd_t)-1) {
        perror("mq_open");
        return cleanup(1);
    }
    mq_attr request_attr{};
    if (mq_getattr(mq, &request_attr) == -1) {
        perror("request mq_getattr");
        return cleanup(1);
    }
    if (request_attr.mq_msgsize != static_cast<long>(sizeof(RequestMessage))) {
        cerr << "Invalid request queue message size; rebuild both programs together\n";
        return cleanup(1);
    }

    while (true) {
        RequestMessage request{};
        ResponseMessage response{};
        string line;
        string command;

        cout << "Client-" << client_id << "> ";

        if (!getline(cin, line)) {
            break;
        }
        istringstream input(line);

        if (!(input >> command)) continue;

        request.client_id = client_id;

        if (command == "LIST") {
            request.command_type = Command::LIST;
            request.resource_id = -1;
        } else if (command == "QUIT") {
            request.command_type = Command::QUIT;
        } else if (command == "STATUS" || command == "RESERVE" || command == "CANCEL") {
            string resource_argument;
            if (!(input >> resource_argument)) {
                cerr << "This command requires a resource_id\n";
                continue;
            }
            const char* begin = resource_argument.data();
            const char* end = begin + resource_argument.size();
            auto parsed_resource = from_chars(begin, end, request.resource_id);
            if (parsed_resource.ec != errc{} || parsed_resource.ptr != end) {
                cerr << "resource_id must be an integer\n";
                continue;
            }

            if (command == "STATUS") request.command_type = Command::STATUS;
            if (command == "RESERVE") request.command_type = Command::RESERVE;
            if (command == "CANCEL") request.command_type = Command::CANCEL;
        } else {
            cerr << "Unknown command\n\n";
            continue;
        }

        string extra;
        if (input >> extra) {
            cerr << "Invalid command format\n\n";
            continue;
        }

        if (request.command_type == Command::QUIT) {
            break;
        }
    
        int send_result;
        do {
            send_result = mq_send(
                mq,
                reinterpret_cast<const char*>(&request),
                sizeof(RequestMessage),
                0
            );
        } while (send_result == -1 && errno == EINTR);
        if (send_result == -1) {
            perror("mq_send");
            return cleanup(1);
        }

        ssize_t bytes_received;
        do {
            bytes_received = mq_receive(
                response_mq,
                reinterpret_cast<char*>(&response),
                sizeof(ResponseMessage),
                nullptr
            );
        } while (bytes_received == -1 && errno == EINTR);

        if (bytes_received == -1) {
            perror("mq_receive");
            return cleanup(1);
        } else if (bytes_received != static_cast<ssize_t>(sizeof(ResponseMessage))) {
            cerr << "Invalid response size\n";
            return cleanup(1);
        } else if (response.client_id != client_id ||
                   memchr(response.message, '\0', sizeof(response.message)) == nullptr) {
            cerr << "Invalid response contents\n";
            return cleanup(1);
        } else {
            if (response.success) {
                cout << "SUCCESS: ";
            } else {
                cout << "FAILED: ";
            }
            cout << response.message << "\n\n";
        }

    }

    return cleanup(cin.bad() ? 1 : 0);
}
