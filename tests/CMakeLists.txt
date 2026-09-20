# Collective Traffic Fabric - test suites.
# Copyright 2026 Summon Software Labs.
#
# Every suite is a separate executable registered with CTest.  Suites are named
# after the proof surface they cover.  No test sets a timeout: this project does
# not use test timeouts, and a hanging test is treated as a defect to diagnose.

set(CTF_TEST_SUPPORT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

add_library(ctf_test_support INTERFACE)
target_include_directories(ctf_test_support INTERFACE "${CTF_TEST_SUPPORT_DIR}")
target_link_libraries(ctf_test_support INTERFACE ctf::collective_traffic_fabric)
include(${CMAKE_SOURCE_DIR}/cmake/CTFWarnings.cmake)


# Paths of the helper executables.  The target file expression is resolved by the
# generator, so a suite is handed the path the helpers were actually written to:
# a single configuration generator writes them flat into the helper directory,
# while a multi configuration generator (Visual Studio, Xcode) appends the
# configuration name, and a fixed path would name a file that does not exist.
set(CTF_HELPER_BIN_DIR "${CMAKE_CURRENT_BINARY_DIR}/bin")
set(CTF_PARTICIPANT_FILE "$<TARGET_FILE:ctf_test_participant>")
set(CTF_COORDINATOR_FILE "$<TARGET_FILE:ctf_test_coordinator_process>")

function(ctf_add_helper helper_name)
  add_executable(${helper_name} ${ARGN}
    support/helper_process.cpp
    support/client_session.cpp
    support/process_helper.cpp)
  target_link_libraries(${helper_name} PRIVATE ctf_test_support)
  ctf_apply_warnings(${helper_name})
  target_compile_definitions(${helper_name} PRIVATE
    CTF_TEST_PARTICIPANT_PATH="${CTF_PARTICIPANT_FILE}"
    CTF_TEST_COORDINATOR_PATH="${CTF_COORDINATOR_FILE}")
  set_target_properties(${helper_name} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CTF_HELPER_BIN_DIR}")
endfunction()

ctf_add_helper(ctf_test_participant support/participant_process.cpp)
ctf_add_helper(ctf_test_coordinator_process support/coordinator_process.cpp)

function(ctf_add_suite suite_name source_file)
  add_executable(${suite_name} ${source_file}
    support/client_session.cpp
    support/process_helper.cpp)
  target_link_libraries(${suite_name} PRIVATE ctf_test_support)
  ctf_apply_warnings(${suite_name})
  target_compile_definitions(${suite_name} PRIVATE
    CTF_TEST_PARTICIPANT_PATH="${CTF_PARTICIPANT_FILE}"
    CTF_TEST_COORDINATOR_PATH="${CTF_COORDINATOR_FILE}")
  add_dependencies(${suite_name} ctf_test_participant ctf_test_coordinator_process)
  set_target_properties(${suite_name} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CTF_HELPER_BIN_DIR}")
  add_test(NAME ${suite_name} COMMAND ${suite_name})
  set_tests_properties(${suite_name} PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()

ctf_add_suite(ctf_unit_core unit/unit_core_test.cpp)
ctf_add_suite(ctf_unit_persistence unit/unit_persistence_test.cpp)
ctf_add_suite(ctf_property_engine property/property_engine_test.cpp)
ctf_add_suite(ctf_adversarial_decoder adversarial/adversarial_decoder_test.cpp)
ctf_add_suite(ctf_adversarial_persistence adversarial/adversarial_persistence_test.cpp)
ctf_add_suite(ctf_concurrency_service concurrency/concurrency_service_test.cpp)
ctf_add_suite(ctf_concurrency_authority concurrency/concurrency_authority_test.cpp)
ctf_add_suite(ctf_protocol_framing protocol/protocol_framing_test.cpp)
ctf_add_suite(ctf_integration_loopback integration/integration_loopback_test.cpp)
ctf_add_suite(ctf_integration_lifecycle integration/integration_lifecycle_test.cpp)
ctf_add_suite(ctf_persistence_restart persistence/persistence_restart_test.cpp)
ctf_add_suite(ctf_multiprocess_fabric multiprocess/multiprocess_fabric_test.cpp)
ctf_add_suite(ctf_scale_fabric scale/scale_fabric_test.cpp)
