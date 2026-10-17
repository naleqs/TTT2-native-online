# Rollback netcode (no dependencies) and, optionally, the WebRTC lobby.
add_library(tt2_netplay STATIC
    src/session.cpp
    src/udp_transport.cpp)
target_include_directories(tt2_netplay PUBLIC include)
target_link_libraries(tt2_netplay PUBLIC ws2_32)

if(TT2_WITH_WEBRTC AND TT2_BUILD_LAUNCHER)
    tt2_fetch_json()
    tt2_fetch_libdatachannel()
    add_library(tt2_lobby STATIC src/lobby.cpp)
    target_link_libraries(tt2_lobby PUBLIC tt2_netplay PRIVATE LibDataChannel::LibDataChannelStatic nlohmann_json::nlohmann_json)
    target_compile_definitions(tt2_lobby PUBLIC TT2_WITH_LOBBY=1)
endif()
