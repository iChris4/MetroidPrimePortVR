#include "../lib/time_internal.hpp"

#include <aurora/time.hpp>
#include <dolphin/os.h>
#include <gtest/gtest.h>

#include <chrono>
#include <ctime>

namespace {

using namespace std::chrono_literals;

aurora::time::native_clock::time_point s_now{};

aurora::time::native_clock::time_point fake_now() noexcept { return s_now; }

class GameOSTimeTest : public testing::Test {
protected:
  void SetUp() override {
    s_now = {};
    aurora::time::internal::set_now_function(fake_now);
  }

  void TearDown() override { aurora::time::internal::set_now_function(nullptr); }
};

TEST(OSTimeTest, SystemTimeUsesUtcGcnEpoch) {
  constexpr auto gcnEpoch = 946684800s;
  const auto expected =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch() - gcnEpoch);
  const auto actual = std::chrono::seconds{OSTicksToSeconds(OSGetSystemTime())};

  EXPECT_LE(std::chrono::abs(actual - expected), 1s);
}

TEST(OSTimeTest, CalendarConversionRoundTripsLocalTime) {
  OSCalendarTime input{
      .sec = 56,
      .min = 34,
      .hour = 12,
      .mday = 29,
      .mon = 1,
      .year = 2024,
      .msec = 123,
      .usec = 456,
  };

  OSCalendarTime output{};
  OSTicksToCalendarTime(OSCalendarTimeToTicks(&input), &output);

  EXPECT_EQ(output.sec, input.sec);
  EXPECT_EQ(output.min, input.min);
  EXPECT_EQ(output.hour, input.hour);
  EXPECT_EQ(output.mday, input.mday);
  EXPECT_EQ(output.mon, input.mon);
  EXPECT_EQ(output.year, input.year);
  EXPECT_EQ(output.msec, input.msec);
  EXPECT_EQ(output.usec, input.usec);
  EXPECT_EQ(output.yday, 59);
}

TEST(OSTimeTest, SaveTimestampMatchesCrtLocalTime) {
  // Check both seasons and UTC date boundaries, including subsecond precision.
  for (const std::time_t wallClock : {1704067200, 1719791999}) {
    std::tm expected{};
#if defined(_WIN32)
    ASSERT_EQ(localtime_s(&expected, &wallClock), 0);
#else
    ASSERT_NE(localtime_r(&wallClock, &expected), nullptr);
#endif
    constexpr std::time_t gcnEpoch = 946684800;
    const OSTime ticks = OSSecondsToTicks(wallClock - gcnEpoch) + OSMicrosecondsToTicks(123456);
    OSCalendarTime actual{};
    OSTicksToCalendarTime(ticks, &actual);
    EXPECT_EQ(actual.year, expected.tm_year + 1900);
    EXPECT_EQ(actual.mon, expected.tm_mon);
    EXPECT_EQ(actual.mday, expected.tm_mday);
    EXPECT_EQ(actual.hour, expected.tm_hour);
    EXPECT_EQ(actual.min, expected.tm_min);
    EXPECT_EQ(actual.sec, expected.tm_sec);
    EXPECT_EQ(actual.wday, expected.tm_wday);
    EXPECT_EQ(actual.yday, expected.tm_yday);
    EXPECT_EQ(actual.msec, 123);
    EXPECT_EQ(actual.usec, 456);
    EXPECT_EQ(OSCalendarTimeToTicks(&actual), ticks);
  }
}

TEST_F(GameOSTimeTest, StopsWhilePausedAndRespectsScale) {
  const OSTime start = OSGetTime();

  s_now += 1s;
  EXPECT_EQ(OSTicksToSeconds(OSGetTime() - start), 1);

  aurora::time::internal::set_pause_reason(aurora::time::internal::PauseReason::Background, true);
  s_now += 10s;
  EXPECT_EQ(OSTicksToSeconds(OSGetTime() - start), 1);

  aurora::time::internal::set_pause_reason(aurora::time::internal::PauseReason::Background, false);
  aurora::time::set_scale(4.0f);
  s_now += 1s;
  EXPECT_EQ(OSTicksToSeconds(OSGetTime() - start), 5);
}

} // namespace
