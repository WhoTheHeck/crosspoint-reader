#include "HalClockTime.h"

namespace hal_clock_detail {
namespace {

constexpr bool isLeapYear(const uint16_t year) {
  return (year % 4U == 0U) && ((year % 100U != 0U) || (year % 400U == 0U));
}

constexpr uint8_t daysInMonth(const uint16_t year, const uint8_t month) {
  constexpr uint8_t DAYS_IN_MONTH[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2U && isLeapYear(year)) return 29U;
  return DAYS_IN_MONTH[month - 1U];
}

bool isValidDateTime(const RtcDateTime& dateTime) {
  if (dateTime.year < 2000U || dateTime.year > 2099U) return false;
  if (dateTime.month < 1U || dateTime.month > 12U) return false;
  if (dateTime.day < 1U || dateTime.day > daysInMonth(dateTime.year, dateTime.month)) return false;
  if (dateTime.hour > 23U || dateTime.minute > 59U || dateTime.second > 59U) return false;

  // The epoch conversion does not depend on weekday, but an RTC read must not
  // accept an impossible value from the device.
  if (dateTime.weekday > 6U) return false;
  return true;
}

constexpr uint16_t daysInYear(const uint16_t year) { return isLeapYear(year) ? 366U : 365U; }

}  // namespace

bool rtcDateTimeToUnix(const RtcDateTime& dateTime, int64_t& unixSeconds) {
  if (!isValidDateTime(dateTime)) return false;

  int64_t days = 0;
  for (uint16_t year = 1970U; year < dateTime.year; ++year) days += daysInYear(year);
  for (uint8_t month = 1U; month < dateTime.month; ++month) days += daysInMonth(dateTime.year, month);

  days += static_cast<int64_t>(dateTime.day) - 1;
  unixSeconds = days * 86400LL + static_cast<int64_t>(dateTime.hour) * 3600LL +
                static_cast<int64_t>(dateTime.minute) * 60LL + static_cast<int64_t>(dateTime.second);
  return true;
}

}  // namespace hal_clock_detail
