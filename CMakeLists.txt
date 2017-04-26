cmake_minimum_required(VERSION 3.1.3)

project(udp-proxy VERSION 0.1)

set(CMAKE_CXX_FLAGS "-Wall -Wextra -Wpedantic")

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Boost 1.62 REQUIRED)
include_directories(${BOOST_INCLUDE_DIR})

if(${CMAKE_CXX_COMPILER_ID} STREQUAL "Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
endif()

aux_source_directory(src LIB_SOURCES)
file(GLOB HEADERS include/udp_proxy/*.h*)
include_directories(include)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/out)

add_executable(udpproxy src/main.cpp ${HEADERS})
target_link_libraries(udpproxy boost_system)
