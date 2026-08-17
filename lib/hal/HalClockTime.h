#pragma once

#include <cstdint>

namespace hal_clock_detail {

struct RtcDateTime {
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
  uint8_t weekday;
};

// Converts a structurally valid RTC UTC value to Unix seconds.
bool rtcDateTimeToUnix(const RtcDateTime& dateTime, int64_t& unixSeconds);

}  // namespace hal_clock_detail
