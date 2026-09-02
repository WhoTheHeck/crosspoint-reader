add_executable(RtcSystemTimeTest
  RtcSystemTimeTest.cpp
  ${REPO_ROOT}/lib/hal/HalClockTime.cpp
)

target_include_directories(RtcSystemTimeTest PRIVATE
  ${REPO_ROOT}/lib/hal
)

target_link_libraries(RtcSystemTimeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(RtcSystemTimeTest)
