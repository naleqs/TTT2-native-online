# Third-party libraries, pinned and fetched into the build directory at configure time.
# Nothing is installed system-wide.
include(FetchContent)

set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

function(tt2_fetch_sdl3)
    if(TARGET SDL3::SDL3-static)
        return()
    endif()
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(sdl3
        URL https://github.com/libsdl-org/SDL/releases/download/release-3.2.8/SDL3-3.2.8.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(sdl3)
endfunction()

function(tt2_fetch_imgui)
    if(TARGET tt2_imgui)
        return()
    endif()
    tt2_fetch_sdl3()
    FetchContent_Declare(imgui
        URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(imgui)
    add_library(tt2_imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
    target_include_directories(tt2_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    target_link_libraries(tt2_imgui PUBLIC SDL3::SDL3-static)
endfunction()

function(tt2_fetch_json)
    if(TARGET nlohmann_json::nlohmann_json)
        return()
    endif()
    FetchContent_Declare(nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(nlohmann_json)
endfunction()

# libdatachannel (MPL-2.0) for WebRTC data channels and the tracker WebSockets, with
# mbedTLS (Apache-2.0) so no OpenSSL is needed.
function(tt2_fetch_libdatachannel)
    if(TARGET LibDataChannel::LibDataChannelStatic)
        return()
    endif()

    set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
    set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
    set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
    # WebRTC data channels run over DTLS; libdatachannel needs mbedTLS's DTLS-SRTP support,
    # which the default mbedTLS configuration leaves out.
    add_compile_definitions(MBEDTLS_SSL_DTLS_SRTP)
    FetchContent_Declare(MbedTLS
        GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
        GIT_TAG v3.6.2
        GIT_SHALLOW TRUE
        GIT_SUBMODULES framework)
    FetchContent_MakeAvailable(MbedTLS)
    # libdatachannel links MbedTLS::MbedTLS; mbedTLS 3.6 only defines the lower-case targets.
    # (An IMPORTED target, so libdatachannel's install(EXPORT) does not try to export it.)
    if(NOT TARGET MbedTLS::MbedTLS)
        add_library(MbedTLS::MbedTLS INTERFACE IMPORTED GLOBAL)
        set_target_properties(MbedTLS::MbedTLS PROPERTIES
            INTERFACE_LINK_LIBRARIES "mbedtls;mbedx509;mbedcrypto;bcrypt;ws2_32")
    endif()

    set(USE_MBEDTLS ON CACHE BOOL "" FORCE)
    set(NO_MEDIA ON CACHE BOOL "" FORCE)
    set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
    set(NO_TESTS ON CACHE BOOL "" FORCE)
    set(NO_WEBSOCKET OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(libdatachannel
        GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
        GIT_TAG v0.23.3
        GIT_SHALLOW TRUE
        GIT_SUBMODULES deps/libjuice deps/usrsctp deps/plog)
    FetchContent_MakeAvailable(libdatachannel)
endfunction()
