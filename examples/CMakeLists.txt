# Collective Traffic Fabric - examples.
# Copyright 2026 Summon Software Labs.
add_executable(ctf_example_decision_types example_decision_types.cpp)
target_link_libraries(ctf_example_decision_types PRIVATE ctf::collective_traffic_fabric)
include(${CMAKE_SOURCE_DIR}/cmake/CTFWarnings.cmake)
ctf_apply_warnings(ctf_example_decision_types)
target_compile_definitions(ctf_example_decision_types PRIVATE
  CTF_EXAMPLE_VERSION="${PROJECT_VERSION}")
install(TARGETS ctf_example_decision_types RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
set_target_properties(ctf_example_decision_types PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
