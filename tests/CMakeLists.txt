add_executable(tt2_rollback_test netplay/rollback_test.cpp)
target_link_libraries(tt2_rollback_test PRIVATE tt2_netplay)
add_test(NAME rollback COMMAND tt2_rollback_test)

add_executable(tt2_snapshot_test runtime/snapshot_test.cpp ${PROJECT_SOURCE_DIR}/src/runtime/rollback/snapshot.cpp)
target_include_directories(tt2_snapshot_test PRIVATE ${PROJECT_SOURCE_DIR}/src/runtime)
add_test(NAME snapshot COMMAND tt2_snapshot_test)

# Needs dumped shaders (TT2_DUMP_SHADERS=1): run by hand with the dump directory.
add_executable(tt2_shader_check runtime/shader_check.cpp ${PROJECT_SOURCE_DIR}/src/runtime/gpu/shader_translator.cpp)
target_include_directories(tt2_shader_check PRIVATE ${PROJECT_SOURCE_DIR}/src/runtime)

# Needs internet (public trackers + STUN): built with the lobby, run by hand.
if(TARGET tt2_lobby)
    add_executable(tt2_lobby_smoke netplay/lobby_smoke.cpp)
    target_link_libraries(tt2_lobby_smoke PRIVATE tt2_lobby)
endif()
