# The launcher contains no game code and is what a release ships prebuilt (with Setup.bat).
tt2_fetch_imgui()
tt2_fetch_json()

add_executable(tt2-launcher WIN32
    main.cpp
    avatar.cpp
    config.cpp
    osk.cpp
    process.cpp)
target_link_libraries(tt2-launcher PRIVATE tt2_imgui tt2_netplay nlohmann_json::nlohmann_json windowscodecs ole32 comdlg32)
if(TARGET tt2_lobby)
    target_link_libraries(tt2-launcher PRIVATE tt2_lobby)
endif()
set_target_properties(tt2-launcher PROPERTIES OUTPUT_NAME "TTT2 Native Online")
