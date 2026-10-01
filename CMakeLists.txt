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

# splice: the variant chevron's nodes and choices are held in, and the
# visiting -- with chevron's step count, so that they share one std.
set(SPLICE_CONSTEXPR_STEPS ${CHEVRON_CONSTEXPR_STEPS} CACHE STRING
  "clang's -fconstexpr-steps for splice, as chevron sets it")
# Not SYSTEM NEVER: an installed splice -- find_package(splice) answered by a
# prefix -- is as good as one fetched, and found first where there is one.
cme_declare_port(NAME splice PROVIDES splice
  GITHUB_REPOSITORY j4niwzis/splice
  GIT_TAG 2086e011a4a09dc5d3634649a4842f1fbb0cedde
  LICENSE GPL-3.0-only
  TARGETS splice::splice)
cme_options(splice "SPLICE_TESTS OFF")
cme_find_package(splice REQUIRED)

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
# By the names an installed copy has too: splice::splice. Boost.PFR in this
# build only: built here as a module, it is a target of this build directory,
# and install(EXPORT) refuses one that needs it -- an installed copy finds
# Boost.PFR again, as a module, in its config.
target_link_libraries(chevron PUBLIC $<BUILD_INTERFACE:Boost::pfr> splice::splice)
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

# ---- What this library is, said by the library ----
#
# A consumer writes find_package(chevron) and this says the rest: where it
# comes from, what it needs, and what it produces. cmake-everywhere installs
# the declaration beside the library, under share/cmake-everywhere/ports, so
# a prefix that has chevron has its description too -- what lets a machine
# answer for it, instead of everyone fetching it again.
if(COMMAND cme_declare_port)
  cme_declare_port(
    NAME chevron
    PROVIDES chevron
    VERSION ${PROJECT_VERSION}
    GITHUB_REPOSITORY j4niwzis/chevron
    GIT_TAG main
    LICENSE AGPL-3.0-only
    DEPENDS splice "boost-pfr[modules]"
    TARGETS chevron::chevron)
endif()

# ---- Installing it ----
#
# The library, its module interface units -- a consumer compiles its own
# binary interfaces from them -- and a package: find_package(chevron) on a
# machine that has it installed finds it there, and what it needs again by
# the same names, splice::splice and Boost.PFR as a module.
option(CHEVRON_INSTALL "Install chevron and its CMake package" ${PROJECT_IS_TOP_LEVEL})
if(CHEVRON_INSTALL)
  include(GNUInstallDirs)
  include(CMakePackageConfigHelpers)
  install(TARGETS chevron
    EXPORT chevronTargets
    FILE_SET CXX_MODULES DESTINATION ${CMAKE_INSTALL_LIBDIR}/chevron/modules
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(EXPORT chevronTargets
    NAMESPACE chevron::
    CXX_MODULES_DIRECTORY modules
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/chevron)
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/chevronConfig.cmake"
    "include(CMakeFindDependencyMacro)\n"
    "find_dependency(splice)\n"
    "find_dependency(boost_pfr COMPONENTS modules)\n"
    "include(\"\${CMAKE_CURRENT_LIST_DIR}/chevronTargets.cmake\")\n"
    "set_property(TARGET chevron::chevron APPEND PROPERTY INTERFACE_LINK_LIBRARIES Boost::pfr)\n"
    "set_property(TARGET chevron::chevron APPEND PROPERTY IMPORTED_CXX_MODULES_LINK_LIBRARIES Boost::pfr)\n"
    "if(NOT TARGET chevron)\n"
    "  add_library(chevron ALIAS chevron::chevron)\n"
    "endif()\n")
  write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/chevronConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/chevronConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/chevronConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/chevron)
endif()

# Fuzz targets, for libFuzzer: test/fuzz/CMakeLists.txt says how.
option(CHEVRON_FUZZ "Build the fuzz targets" OFF)
if(CHEVRON_FUZZ)
  add_subdirectory(test/fuzz)
endif()
