#pragma once

namespace RtcClock {

struct Time {
    bool valid = false;
    int year = 0;
    int month = 0;
    int day = 0;
    int weekday = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
};

bool start();
Time now();
bool set_local(int year, int month, int day, int weekday,
               int hour, int minute, int second);

}  // namespace RtcClock
