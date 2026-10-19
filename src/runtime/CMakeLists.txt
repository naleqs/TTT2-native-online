# The native game: XenonRecomp's output (generated on the player's PC by Setup.bat) plus the
# runtime. Configured by setup.ps1 with TT2_RECOMP_DIR and TT2_SIMDE_DIR.

# setup.ps1 passes Windows paths; backslashes would be read as escapes.
file(TO_CMAKE_PATH "${TT2_RECOMP_DIR}" TT2_RECOMP_DIR)
file(TO_CMAKE_PATH "${TT2_SIMDE_DIR}" TT2_SIMDE_DIR)

if(NOT TT2_RECOMP_DIR OR NOT EXISTS "${TT2_RECOMP_DIR}/ppc/ppc_recomp_shared.h")
    message(FATAL_ERROR "TT2_RECOMP_DIR must point to the recompiler output (run Setup.bat).")
endif()
if(NOT TT2_SIMDE_DIR)
    message(FATAL_ERROR "TT2_SIMDE_DIR must point to XenonRecomp's thirdparty/simde.")
endif()

tt2_fetch_sdl3()
tt2_fetch_json()

set(PPC_DIR ${TT2_RECOMP_DIR}/ppc)

# The executable the recompiler output was made from: "retail" or "tu3" (patched with the
# player's own Title Update 3). Setup passes it; addresses in game/ depend on it.
set(TT2_GAME_VERSION "retail" CACHE STRING "Game executable version: retail or tu3")
file(GLOB TT2_PPC_SOURCES CONFIGURE_DEPENDS ${PPC_DIR}/ppc_recomp.*.cpp)
list(APPEND TT2_PPC_SOURCES ${PPC_DIR}/ppc_func_mapping.cpp)

set(TT2_RUNTIME_SOURCES
    main.cpp
    memory.cpp
    image.cpp
    timebase.cpp
    netplay_glue.cpp
    kernel/objects.cpp
    kernel/threads.cpp
    kernel/sync.cpp
    kernel/memory_api.cpp
    kernel/io.cpp
    kernel/rtl.cpp
    kernel/xam.cpp
    kernel/system.cpp
    gpu/video.cpp
    gpu/capture.cpp
    gpu/renderer_d3d11.cpp
    gpu/shader_translator.cpp
    audio/audio.cpp
    input/input.cpp
    rollback/snapshot.cpp
    rollback/game_rollback.cpp
    game/tt2_frame.cpp
    game/tt2_fixes.cpp)

# Logging stubs for every import without a TT2_IMPORT implementation.
set(TT2_STUBS ${CMAKE_CURRENT_BINARY_DIR}/import_stubs.cpp)
set(TT2_STUB_SOURCES_FILE ${CMAKE_CURRENT_BINARY_DIR}/import_sources.txt)
list(TRANSFORM TT2_RUNTIME_SOURCES PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/ OUTPUT_VARIABLE TT2_RUNTIME_SOURCES_ABS)
file(WRITE ${TT2_STUB_SOURCES_FILE} "${TT2_RUNTIME_SOURCES_ABS}")
add_custom_command(
    OUTPUT ${TT2_STUBS}
    COMMAND ${CMAKE_COMMAND}
        -DSHARED_HEADER=${PPC_DIR}/ppc_recomp_shared.h
        -DSOURCES_FILE=${TT2_STUB_SOURCES_FILE}
        -DOUTPUT=${TT2_STUBS}
        -P ${PROJECT_SOURCE_DIR}/cmake/gen_import_stubs.cmake
    DEPENDS ${PPC_DIR}/ppc_recomp_shared.h ${TT2_RUNTIME_SOURCES_ABS} ${PROJECT_SOURCE_DIR}/cmake/gen_import_stubs.cmake
    COMMENT "Generating import stubs")

# Recompiled game code: large, generated, warning-free is not a goal.
add_library(tt2_ppc OBJECT ${TT2_PPC_SOURCES})
target_include_directories(tt2_ppc PRIVATE ${PPC_DIR} ${TT2_SIMDE_DIR})
target_compile_options(tt2_ppc PRIVATE
    -O2 -march=x86-64-v3 -fno-strict-aliasing -w
    -include ${CMAKE_CURRENT_SOURCE_DIR}/timebase.h)

add_executable(tt2 ${TT2_RUNTIME_SOURCES} ${TT2_STUBS} $<TARGET_OBJECTS:tt2_ppc>)
target_include_directories(tt2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PPC_DIR} ${TT2_SIMDE_DIR})
target_compile_options(tt2 PRIVATE -O2 -march=x86-64-v3 -fno-strict-aliasing)
if(TT2_GAME_VERSION STREQUAL "tu3")
    target_compile_definitions(tt2 PRIVATE TT2_TU3=1)
endif()
target_link_libraries(tt2 PRIVATE tt2_netplay SDL3::SDL3-static nlohmann_json::nlohmann_json winmm d3d11 dxgi)
if(MINGW)
    target_link_options(tt2 PRIVATE -mwindows)
endif()

# The decrypted image and its metadata sit next to the executable.
add_custom_command(TARGET tt2 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${TT2_RECOMP_DIR}/tt2_image.bin ${TT2_RECOMP_DIR}/tt2_image.json $<TARGET_FILE_DIR:tt2>)
