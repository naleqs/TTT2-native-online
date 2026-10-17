# Applies every *.patch in PATCH_DIR to the current directory (the fetched XenonRecomp
# checkout). Idempotent: a patch that is already applied is skipped.
find_package(Git REQUIRED)

file(GLOB patches "${PATCH_DIR}/*.patch")
list(SORT patches)

foreach(patch IN LISTS patches)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} apply --reverse --check --ignore-whitespace --whitespace=nowarn ${patch}
        RESULT_VARIABLE already_applied
        OUTPUT_QUIET ERROR_QUIET)
    if(already_applied EQUAL 0)
        message(STATUS "patch already applied: ${patch}")
        continue()
    endif()

    execute_process(
        COMMAND ${GIT_EXECUTABLE} apply --ignore-whitespace --whitespace=nowarn ${patch}
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "failed to apply ${patch}")
    endif()
    message(STATUS "applied ${patch}")
endforeach()
