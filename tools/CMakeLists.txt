# Collective Traffic Fabric - command line tools.
# Copyright 2026 Summon Software Labs.
#
# The tool is a consumer of the public library, exactly like any downstream
# program: it links ctf::collective_traffic_fabric and includes only the
# installed public headers.
add_executable(ctfctl ctfctl.cpp)
target_link_libraries(ctfctl PRIVATE ctf::collective_traffic_fabric)
include(${CMAKE_SOURCE_DIR}/cmake/CTFWarnings.cmake)
ctf_apply_warnings(ctfctl)
set_target_properties(ctfctl PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
install(TARGETS ctfctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
