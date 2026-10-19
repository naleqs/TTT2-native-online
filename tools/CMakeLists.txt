# Host tools for the first-time setup: XenonRecomp (patched), tt2-analyze and tt2-xexpatch.
# Built by tools/setup/setup.ps1 into native/build/tools.
cmake_minimum_required(VERSION 3.20)
project(tt2_tools C CXX)

include(FetchContent)

# Executables land directly in the build directory, where setup.ps1 expects them.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

if(MINGW)
    # No libc++/libunwind DLLs next to the tools.
    add_link_options(-static)
endif()

# XenonRecomp by hedge-dev (MIT). Pinned; tools/recomp-patches/ adds the instructions this
# game uses that upstream does not implement yet.
set(TT2_XENONRECOMP_COMMIT ddd128bcca99fe8bfbb99bea583c972351fa6ace)
FetchContent_Declare(xenonrecomp
    GIT_REPOSITORY https://github.com/hedge-dev/XenonRecomp.git
    GIT_TAG ${TT2_XENONRECOMP_COMMIT}
    GIT_SUBMODULES_RECURSE TRUE
    GIT_SHALLOW FALSE
    GIT_CONFIG core.autocrlf=false
    PATCH_COMMAND ${CMAKE_COMMAND}
        -DPATCH_DIR=${CMAKE_CURRENT_SOURCE_DIR}/recomp-patches
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/apply_patches.cmake
    UPDATE_DISCONNECTED TRUE
)
FetchContent_MakeAvailable(xenonrecomp)

add_executable(tt2-analyze tt2-analyze/main.cpp)
target_compile_features(tt2-analyze PRIVATE cxx_std_20)
target_link_libraries(tt2-analyze PRIVATE LibXenonAnalyse XenonUtils fmt::fmt)

# Applies the player's own title update (Default.xexp) before analysis and recompilation.
add_executable(tt2-xexpatch tt2-xexpatch/main.cpp)
target_compile_features(tt2-xexpatch PRIVATE cxx_std_20)
target_link_libraries(tt2-xexpatch PRIVATE XenonUtils)

# The recompiler needs the path of ppc_context.h from the fetched sources.
file(WRITE ${CMAKE_BINARY_DIR}/ppc_context_path.txt "${xenonrecomp_SOURCE_DIR}/XenonUtils/ppc_context.h")
