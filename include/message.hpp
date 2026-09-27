#pragma once

enum class Command {
    LIST = 1,
    STATUS = 2,
    RESERVE = 3,
    CANCEL = 4,
    QUIT = 5
};

struct RequestMessage {
    int client_id;
    Command command_type;
    int resource_id;
};

struct ResponseMessage {
    int client_id;
    bool success;
    char message[1024];
};