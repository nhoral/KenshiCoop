cmake_minimum_required(VERSION 3.15)
project(nettest C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ENet vendored under third_party/enet/enet (see third_party/enet/README.md).
set(ENET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/enet/enet")

if(NOT EXISTS "${ENET_DIR}/include/enet/enet.h")
    message(FATAL_ERROR
        "ENet not found at ${ENET_DIR}. "
        "See third_party/enet/README.md to fetch it (git submodule or download).")
endif()

# Build ENet via its own CMake so the correct platform backend is selected
# (win32.c on Windows, unix.c elsewhere). This produces the `enet` target.
add_subdirectory("${ENET_DIR}" "${CMAKE_BINARY_DIR}/enet_build")

add_executable(nettest main.cpp)

target_include_directories(nettest PRIVATE "${ENET_DIR}/include")
target_link_libraries(nettest PRIVATE enet)

if(WIN32)
    target_link_libraries(nettest PRIVATE ws2_32 winmm)
endif()
