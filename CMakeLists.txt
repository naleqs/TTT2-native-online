cmake_minimum_required(VERSION 3.24)

file(READ ${CMAKE_CURRENT_SOURCE_DIR}/VERSION TT2_VERSION)
string(STRIP "${TT2_VERSION}" TT2_VERSION)
project(ttt2_native_online VERSION ${TT2_VERSION} LANGUAGES C CXX RC)

# The program icon (assets/tt2.ico, drawn for this project) for the game and the launcher.
set(TT2_ICON_RC ${CMAKE_BINARY_DIR}/tt2_icon.rc)
file(WRITE ${TT2_ICON_RC} "1 ICON \"${PROJECT_SOURCE_DIR}/assets/tt2.ico\"
")

option(TT2_BUILD_LAUNCHER "Build the launcher (setup front end, local play, online lobby)" ON)
option(TT2_BUILD_GAME "Build the recompiled game (run Setup.bat; needs TT2_RECOMP_DIR)" OFF)
option(TT2_BUILD_TESTS "Build the tests" ON)
option(TT2_WITH_WEBRTC "Online lobby and NAT traversal over WebRTC (libdatachannel)" ON)

set(TT2_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin" CACHE PATH "Where executables are written")

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${TT2_OUTPUT_DIR})

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(WARNING "Clang is the supported compiler (the recompiled game code requires it).")
endif()

if(MINGW)
    # Self-contained executables: no libc++/libunwind/libwinpthread DLLs to ship.
    add_link_options(-static)
endif()

include(cmake/Dependencies.cmake)

add_subdirectory(src/netplay)

if(TT2_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(TT2_BUILD_LAUNCHER)
    add_subdirectory(src/launcher)
endif()

if(TT2_BUILD_GAME)
    add_subdirectory(src/runtime)
endif()
