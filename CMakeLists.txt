cmake_minimum_required(VERSION 3.21)
project(KingsglaiveFX VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(extern/CommonLibSSE-NG CommonLibSSE EXCLUDE_FROM_ALL)

include(extern/CommonLibSSE-NG/cmake/CommonLibSSE.cmake)

add_commonlibsse_plugin(KingsglaiveFX
    NAME "KingsglaiveFX"
    AUTHOR "Kingsglaive scriptless edit"
    USE_ADDRESS_LIBRARY
    SOURCES src/main.cpp)

target_precompile_headers(KingsglaiveFX PRIVATE src/PCH.h)
