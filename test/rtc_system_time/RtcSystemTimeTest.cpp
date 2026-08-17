#include <gtest/gtest.h>

#include <cstdlib>
#include <string>
#include <time.h>

#include "HalClockTime.h"

namespace {

using hal_clock_detail::RtcDateTime;

int64_t toUnix(const RtcDateTime& dateTime) {
  int64_t unixSeconds = -1;
  EXPECT_TRUE(hal_clock_detail::rtcDateTimeToUnix(dateTime, unixSeconds));
  return unixSeconds;
}

void expectInvalid(const RtcDateTime& dateTime) {
  int64_t unixSeconds = 0;
  EXPECT_FALSE(hal_clock_detail::rtcDateTimeToUnix(dateTime, unixSeconds));
}

class ScopedTimezone {
 public:
  ScopedTimezone() : originalTimezone_(std::getenv("TZ")) {
    if (originalTimezone_ != nullptr) originalValue_ = originalTimezone_;
  }

  ~ScopedTimezone() {
    if (originalTimezone_ == nullptr) {
      unsetenv("TZ");
    } else {
      setenv("TZ", originalValue_.c_str(), 1);
    }
    tzset();
  }

  void set(const char* timezone) {
    ASSERT_EQ(setenv("TZ", timezone, 1), 0);
    tzset();
  }

 private:
  const char* originalTimezone_;
  std::string originalValue_;
};

}  // namespace

TEST(RtcSystemTimeTest, ConvertsKnownUtcEpochs) {
  EXPECT_EQ(toUnix({2000, 1, 1, 0, 0, 0, 6}), 946684800);
  EXPECT_EQ(toUnix({2000, 2, 29, 23, 59, 59, 2}), 951868799);
  EXPECT_EQ(toUnix({2023, 6, 15, 17, 42, 8, 4}), 1686850928);
  EXPECT_EQ(toUnix({2024, 2, 29, 12, 34, 56, 4}), 1709210096);
  EXPECT_EQ(toUnix({2038, 1, 19, 3, 14, 7, 2}), 2147483647);
}

TEST(RtcSystemTimeTest, AcceptsRtcYearBoundaries) {
  EXPECT_EQ(toUnix({2000, 1, 1, 0, 0, 0, 6}), 946684800);
  EXPECT_EQ(toUnix({2099, 12, 31, 23, 59, 59, 4}), 4102444799LL);
}

TEST(RtcSystemTimeTest, EnforcesLeapYearsAndMonthLengths) {
  int64_t unixSeconds = 0;
  EXPECT_TRUE(hal_clock_detail::rtcDateTimeToUnix({2000, 2, 29, 0, 0, 0, 2}, unixSeconds));
  expectInvalid({2001, 2, 29, 0, 0, 0, 4});
  expectInvalid({2099, 2, 29, 0, 0, 0, 0});
  expectInvalid({2100, 2, 29, 0, 0, 0, 1});
  expectInvalid({2024, 4, 31, 0, 0, 0, 3});
}

TEST(RtcSystemTimeTest, RejectsInvalidFields) {
  expectInvalid({1999, 12, 31, 23, 59, 59, 5});
  expectInvalid({2100, 1, 1, 0, 0, 0, 5});
  expectInvalid({2024, 0, 1, 0, 0, 0, 1});
  expectInvalid({2024, 13, 1, 0, 0, 0, 1});
  expectInvalid({2024, 1, 0, 0, 0, 0, 1});
  expectInvalid({2024, 1, 32, 0, 0, 0, 1});
  expectInvalid({2024, 1, 1, 24, 0, 0, 1});
  expectInvalid({2024, 1, 1, 0, 60, 0, 1});
  expectInvalid({2024, 1, 1, 0, 0, 60, 1});
  expectInvalid({2024, 1, 1, 0, 0, 0, 7});
}

TEST(RtcSystemTimeTest, ConversionIsIndependentOfHostTimezone) {
  ScopedTimezone timezone;
  const RtcDateTime dateTime{2024, 2, 29, 12, 34, 56, 4};

  timezone.set("UTC0");
  const int64_t utcResult = toUnix(dateTime);
  timezone.set("CET-1CEST,M3.5.0,M10.5.0");
  const int64_t daylightResult = toUnix(dateTime);
  timezone.set("EST5EDT,M3.2.0,M11.1.0");
  const int64_t easternResult = toUnix(dateTime);

  EXPECT_EQ(utcResult, 1709210096);
  EXPECT_EQ(daylightResult, utcResult);
  EXPECT_EQ(easternResult, utcResult);
}
