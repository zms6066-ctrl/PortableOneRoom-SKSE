cmake_minimum_required(VERSION 3.21)
project(PortableOneRoomMag VERSION 1.3.5 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(extern/CommonLibSSE-NG CommonLibSSE EXCLUDE_FROM_ALL)

include(extern/CommonLibSSE-NG/cmake/CommonLibSSE.cmake)

add_commonlibsse_plugin(PortableOneRoomMag
    NAME "PortableOneRoomMag"
    AUTHOR "Portable One Room"
    USE_ADDRESS_LIBRARY
    SOURCES src/main.cpp)

target_precompile_headers(PortableOneRoomMag PRIVATE src/PCH.h)
