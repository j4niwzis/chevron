# The tests: googletest's, as a module.
#
# googletest is j4niwzis/googletest-modules, the fork that builds it as one:
# a test here imports std, and a header that includes the standard library
# textually would give a translation unit two views of it. It comes from
# cmake-everywhere, pinned to a commit, and never from the machine -- a
# googletest installed anywhere is the one that is a header.
#
# cmake-everywhere is asked without being installed as the dependency
# provider: which provider a build has is for whoever builds chevron, and only
# the tests need anything fetched.
if(NOT COMMAND cme_find_package)
  set(CME_WITHOUT_PROVIDER ON)
  include(${PROJECT_SOURCE_DIR}/cmake/get_cme.cmake)
endif()
cme_declare_port(NAME googletest-modules PROVIDES GTest
  GITHUB_REPOSITORY j4niwzis/googletest-modules
  GIT_TAG 706920f1d230f0316d271466d8885735be3e300e
  LICENSE BSD-3-Clause
  SYSTEM NEVER
  TARGETS gtest_main)
cme_options(googletest-modules
  "BUILD_GMOCK OFF" "INSTALL_GTEST OFF" "gtest_build_modules ON"
  "gtest_build_tests OFF" "gtest_build_samples OFF")
cme_find_package(GTest REQUIRED)
# A copy from a prefix names it GTest::gtest_main, and one built here
# gtest_main.
if(TARGET GTest::gtest_main)
  set(chevron_gtest GTest::gtest_main)
else()
  set(chevron_gtest gtest_main)
endif()

add_executable(chevron-parser-test parser_test.cc)
target_link_libraries(chevron-parser-test PRIVATE chevron ${chevron_gtest})
add_test(NAME parser COMMAND chevron-parser-test)
add_executable(chevron-escape-test escape_test.cc)
target_link_libraries(chevron-escape-test PRIVATE chevron ${chevron_gtest})
add_test(NAME escape COMMAND chevron-escape-test)
add_executable(chevron-read-test read_test.cc)
target_link_libraries(chevron-read-test PRIVATE chevron ${chevron_gtest})
add_test(NAME read COMMAND chevron-read-test)

add_executable(chevron-bench-text bench/text.cc)
target_link_libraries(chevron-bench-text PRIVATE chevron)
