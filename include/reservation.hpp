#pragma once

constexpr int RESOURCE_COUNT = 20;

struct Reservation {
    int resource_id;
    int owner_id;
};