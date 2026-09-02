add_executable(WifiDiagnosticPolicyTest
  WifiDiagnosticPolicyTest.cpp
)

target_include_directories(WifiDiagnosticPolicyTest PRIVATE
  ${REPO_ROOT}/lib/diagnostics
)

target_link_libraries(WifiDiagnosticPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(WifiDiagnosticPolicyTest)
