cmake_minimum_required(VERSION 4.3.4 FATAL_ERROR)

# chevron: the XML of streams, as C++23 modules -- events pulled from bytes
# as they arrive, returned as a std::variant, and no callbacks.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/import-std.cmake)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_EXTENSIONS OFF)

project(chevron VERSION 0.1 LANGUAGES CXX)

option(CHEVRON_TESTS "Build the tests" ${PROJECT_IS_TOP_LEVEL})

# Dependencies come from cmake-everywhere, asked for without it being
# installed as the dependency provider: which provider a build has is for
# whoever builds chevron.
if(NOT COMMAND cme_find_package)
  set(CME_WITHOUT_PROVIDER ON)
  include(${CMAKE_CURRENT_LIST_DIR}/cmake/get_cme.cmake)
endif()
# Boost.PFR, as a module: the names and the members of an aggregate, for the
# typed reading.
cme_find_package(Boost COMPONENTS pfr[modules] REQUIRED)

# The bound on the steps of one constant evaluation, for chevron and what
# imports it.
set(CHEVRON_CONSTEXPR_STEPS 100000000 CACHE STRING
  "clang's -fconstexpr-steps for chevron and what imports it")

# One module to a target, so that a change to one compiles again only it and
# what imports it.
# One library, libchevron.a, with its modules in one file set: modules are
# what separates it, and archives did not need to as well. It has no part a
# consumer would leave out -- the parser, the events over it, the typed
# reading and the module that imports them -- so it has no components.
add_library(chevron)
target_sources(chevron PUBLIC FILE_SET CXX_MODULES BASE_DIRS src
  FILES src/parser.cc src/events.cc src/typed.cc src/chevron.cc)
target_compile_features(chevron PUBLIC cxx_std_26)
target_link_libraries(chevron PUBLIC Boost::pfr)
# A usage requirement of the library, as alef has. Besides giving constant
# evaluation room, it makes the modules' settings differ from those of the
# std target, so that CMake builds them against a std synthesized for them
# -- the same one it synthesizes for their consumers, the tests among them,
# and for the googletest those consume. Without it, a test would see
# chevron's modules built against one std and googletest's against another,
# which clang does not survive.
target_compile_options(chevron PUBLIC
  $<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:-fconstexpr-steps=${CHEVRON_CONSTEXPR_STEPS}>)

# The names the targets used to have, each the one library now.
add_library(chevron::chevron ALIAS chevron)
foreach(name IN ITEMS parser events typed)
  add_library(chevron::${name} ALIAS chevron)
  add_library(chevron_${name} ALIAS chevron)
endforeach()

if(CHEVRON_TESTS)
  enable_testing()
  add_subdirectory(test)
endif()

# Fuzz targets, for libFuzzer: test/fuzz/CMakeLists.txt says how.
option(CHEVRON_FUZZ "Build the fuzz targets" OFF)
if(CHEVRON_FUZZ)
  add_subdirectory(test/fuzz)
endif()
