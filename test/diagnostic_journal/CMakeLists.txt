add_executable(DiagnosticJournalTest
  DiagnosticJournalTest.cpp
  ${REPO_ROOT}/lib/diagnostics/DiagnosticRecord.cpp
)

target_include_directories(DiagnosticJournalTest PRIVATE
  ${REPO_ROOT}/lib/diagnostics
)

target_link_libraries(DiagnosticJournalTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DiagnosticJournalTest)
