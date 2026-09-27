#include <iostream>
#include <cstdio>
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

    int client_id;
    try {
        size_t parsed = 0;
        int id = stoi(argv[1], &parsed);
        if (argv[1][parsed] != '\0') {
            cerr << "client_id must be an integer\n";
            return 1;
        }
        if (id > 0) {
            client_id = id;
        } else {
            cout << "client_id must be greater than 0\n";
            return 0;
        }
    } catch (...) {
        cerr << "client_id must be an integer\n";
        return 1;
    }

    string response_queue_name = "/response_" + to_string(client_id);
    mq_unlink(response_queue_name.c_str());

    mq_attr attr{};
    attr.mq_flags = 0;
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = sizeof(ResponseMessage);

    mqd_t response_mq = mq_open(
        response_queue_name.c_str(),
        O_CREAT | O_RDONLY,
        0666,
        &attr
    );

    if (response_mq == (mqd_t)-1) {
        perror("response_mq open");
        return 1;
    }

    const char* queue_name = "/request_queue";

    mqd_t mq = mq_open(
        queue_name,
        O_WRONLY
    );

    if (mq == (mqd_t)-1) {
        perror("mq_open");
        mq_close(response_mq);
        mq_unlink(response_queue_name.c_str());
        return 1;
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
            if (!(input >> request.resource_id)) {
                cerr << "This command requires a resource_id\n";
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
    
        if (mq_send(
            mq,
            reinterpret_cast<const char*>(&request),
            sizeof(RequestMessage),
            0
        ) == -1) {
            perror("mq_send");
    
            mq_close(mq);
            return 1;
        }

        ssize_t bytes_received = mq_receive(
            response_mq,
            reinterpret_cast<char*>(&response),
            sizeof(ResponseMessage),
            nullptr
        );

        if (bytes_received == -1) {
            perror("mq_receive");
        } else if (bytes_received != sizeof(ResponseMessage)) {
            cout << "Invalid response size\n\n";
        } else {
            if (response.success) {
                cout << "SUCCESS: ";
            } else {
                cout << "FAILED: ";
            }
            cout << response.message << "\n\n";
        }

    }

    if (mq_close(mq) == -1) {
        perror("mq_close");
        return 1;
    }

    if (mq_close(response_mq) == -1) {
        perror("response_mq_close");
        return 1;
    }
    mq_unlink(response_queue_name.c_str());

    return 0;
}