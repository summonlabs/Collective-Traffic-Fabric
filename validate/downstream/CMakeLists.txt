# Collective Traffic Fabric - independent downstream consumer.
# Copyright 2026 Summon Software Labs.
#
# This project exists only to prove that the installed package can be found and
# used by code that has no access to this repository's build tree.  It is
# deliberately built from its own directory with its own CMakeLists.txt.
cmake_minimum_required(VERSION 3.20)
project(CollectiveTrafficFabricConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CollectiveTrafficFabric 1.0 REQUIRED)

add_executable(ctf_consumer consumer.cpp)
target_link_libraries(ctf_consumer PRIVATE ctf::collective_traffic_fabric)

if(MSVC)
  target_compile_options(ctf_consumer PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8)
else()
  target_compile_options(ctf_consumer PRIVATE -Wall -Wextra -Werror)
endif()

message(STATUS "consumer: CollectiveTrafficFabric ${CollectiveTrafficFabric_VERSION}")
message(STATUS "consumer: library target ctf::collective_traffic_fabric")
