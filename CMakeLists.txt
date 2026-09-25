cmake_minimum_required(VERSION 4.3.4 FATAL_ERROR)

# chevron: the XML of streams, as C++23 modules -- events pulled from bytes
# as they arrive, returned as a std::variant, and no callbacks.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/import-std.cmake)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_EXTENSIONS OFF)

project(chevron VERSION 0.1 LANGUAGES CXX)

option(CHEVRON_TESTS "Build the tests" ${PROJECT_IS_TOP_LEVEL})

# The bound on the steps of one constant evaluation, for chevron and what
# imports it.
set(CHEVRON_CONSTEXPR_STEPS 100000000 CACHE STRING
  "clang's -fconstexpr-steps for chevron and what imports it")

# One module to a target, so that a change to one compiles again only it and
# what imports it.
function(chevron_module target file)
  add_library(${target})
  target_sources(${target}
    PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES ${file})
  target_compile_features(${target} PUBLIC cxx_std_23)
  if(ARGN)
    target_link_libraries(${target} PUBLIC ${ARGN})
  endif()
  # A usage requirement of every module, as alef has. Besides giving constant
  # evaluation room, it makes the modules' settings differ from those of the
  # std target, so that CMake builds them against a std synthesized for
  # them -- the same one it synthesizes for their consumers, the tests among
  # them, and for the googletest those consume. Without it, a test would see
  # chevron's modules built against one std and googletest's against
  # another, which clang does not survive.
  target_compile_options(${target} PUBLIC
    $<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:-fconstexpr-steps=${CHEVRON_CONSTEXPR_STEPS}>)
endfunction()

chevron_module(chevron_parser src/parser.cc)
chevron_module(chevron_events src/events.cc chevron_parser)
chevron_module(chevron src/chevron.cc chevron_parser chevron_events)

add_library(chevron::parser ALIAS chevron_parser)
add_library(chevron::events ALIAS chevron_events)
add_library(chevron::chevron ALIAS chevron)

if(CHEVRON_TESTS)
  enable_testing()
  add_subdirectory(test)
endif()
