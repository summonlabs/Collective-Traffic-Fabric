# Collective Traffic Fabric - top level build.
# Copyright 2026 Summon Software Labs.
#
# Portable C++20 with CMake.  The only dependency is a C++20 standard library,
# a threads implementation, and the platform socket API.  No third party code is
# vendored, downloaded, or required.
cmake_minimum_required(VERSION 3.20)

project(CollectiveTrafficFabric
  VERSION 1.0.0
  DESCRIPTION "Vendor neutral C++20 runtime for collective communication traffic treatment"
  HOMEPAGE_URL "https://github.com/summonlabs/Collective-Traffic-Fabric"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(CTF_BUILD_TESTS "Build the test suites" ON)
option(CTF_BUILD_TOOLS "Build the ctfctl inspection tool" ON)
option(CTF_BUILD_EXAMPLES "Build the examples" ON)
option(CTF_WARNINGS_AS_ERRORS "Treat first party warnings as errors" ON)
option(CTF_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer where supported" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# Warnings.  Zero first party warnings is a build requirement, not a goal.
# ---------------------------------------------------------------------------
include(cmake/CTFWarnings.cmake)

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
# The library sources are listed explicitly.  A glob would silently build an
# empty library if a file were renamed, and a build system that can quietly
# produce nothing is worse than one that fails.
set(CTF_LIBRARY_SOURCES
  src/admission.cpp
  src/authority.cpp
  src/catalog.cpp
  src/collective.cpp
  src/coordinator.cpp
  src/decision.cpp
  src/engine.cpp
  src/error.cpp
  src/evidence.cpp
  src/flow_group.cpp
  src/history.cpp
  src/identity.cpp
  src/participant.cpp
  src/persistence.cpp
  src/protocol.cpp
  src/protocol_evidence.cpp
  src/protocol_messages.cpp
  src/service.cpp
  src/sync.cpp
  src/traffic.cpp
  src/transport.cpp
)

foreach(ctf_source IN LISTS CTF_LIBRARY_SOURCES)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${ctf_source}")
    message(FATAL_ERROR "library source is missing: ${ctf_source}")
  endif()
endforeach()
add_library(collective_traffic_fabric STATIC ${CTF_LIBRARY_SOURCES})
add_library(ctf::collective_traffic_fabric ALIAS collective_traffic_fabric)

target_include_directories(collective_traffic_fabric
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

target_compile_features(collective_traffic_fabric PUBLIC cxx_std_20)
# The warning interface is applied directly rather than linked, so that the
# exported target set stays free of build-only dependencies.
ctf_apply_warnings(collective_traffic_fabric)

find_package(Threads REQUIRED)
target_link_libraries(collective_traffic_fabric PUBLIC Threads::Threads)

if(WIN32)
  target_link_libraries(collective_traffic_fabric PUBLIC ws2_32)
endif()

if(CTF_ENABLE_SANITIZERS)
  if(MSVC)
    message(STATUS "CTF: AddressSanitizer requested with MSVC")
    target_compile_options(collective_traffic_fabric PUBLIC /fsanitize=address)
  else()
    target_compile_options(collective_traffic_fabric PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(collective_traffic_fabric PUBLIC -fsanitize=address,undefined)
  endif()
endif()

set_target_properties(collective_traffic_fabric PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  OUTPUT_NAME collective_traffic_fabric)

# ---------------------------------------------------------------------------
# Install and export
# ---------------------------------------------------------------------------
install(TARGETS collective_traffic_fabric
  EXPORT CollectiveTrafficFabricTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ctf DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT CollectiveTrafficFabricTargets
  FILE CollectiveTrafficFabricTargets.cmake
  NAMESPACE ctf::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CollectiveTrafficFabric)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CollectiveTrafficFabricConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/CollectiveTrafficFabricConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CollectiveTrafficFabric)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/CollectiveTrafficFabricConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/CollectiveTrafficFabricConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/CollectiveTrafficFabricConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CollectiveTrafficFabric)

# ---------------------------------------------------------------------------
# Tools, tests, examples
# ---------------------------------------------------------------------------
if(CTF_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(CTF_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(CTF_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "Collective Traffic Fabric ${PROJECT_VERSION}")
message(STATUS "  build type          : ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler            : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  warnings as errors  : ${CTF_WARNINGS_AS_ERRORS}")
message(STATUS "  sanitizers          : ${CTF_ENABLE_SANITIZERS}")
message(STATUS "  tests               : ${CTF_BUILD_TESTS}")
message(STATUS "  tools               : ${CTF_BUILD_TOOLS}")
message(STATUS "  examples            : ${CTF_BUILD_EXAMPLES}")