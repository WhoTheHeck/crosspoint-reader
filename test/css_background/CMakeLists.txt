add_executable(CssBackgroundTest
  CssBackgroundTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
)

target_include_directories(CssBackgroundTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/Memory
)

target_compile_definitions(CssBackgroundTest PRIVATE
  FIXTURE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/fixture.css"
)

target_link_libraries(CssBackgroundTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CssBackgroundTest)
