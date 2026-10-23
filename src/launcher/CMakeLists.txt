# The launcher contains no game code and is what a release ships prebuilt (with Setup.bat).
tt2_fetch_imgui()
tt2_fetch_json()

add_executable(tt2-launcher WIN32
    main.cpp
    avatar.cpp
    config.cpp
    osk.cpp
    process.cpp
    update_check.cpp
    ${TT2_ICON_RC})
target_link_libraries(tt2-launcher PRIVATE tt2_imgui tt2_netplay nlohmann_json::nlohmann_json windowscodecs ole32 comdlg32 winhttp shell32)
target_compile_definitions(tt2-launcher PRIVATE TT2_LAUNCHER_VERSION="${PROJECT_VERSION}")
if(TARGET tt2_lobby)
    target_link_libraries(tt2-launcher PRIVATE tt2_lobby)
endif()
set_target_properties(tt2-launcher PROPERTIES OUTPUT_NAME "TTT2 Native Online")
# Everyone finds the launcher in the root folder, next to Setup.bat.
add_custom_command(TARGET tt2-launcher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DSRC=$<TARGET_FILE:tt2-launcher> -DDEST=${PROJECT_SOURCE_DIR}
            -P ${PROJECT_SOURCE_DIR}/cmake/copy_launcher.cmake
    VERBATIM)
