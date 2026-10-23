# Copies the launcher next to Setup.bat after every build, so it is always in the root folder.
# A launcher that is running cannot be replaced (Rebuild the game runs setup from it): that only
# gives a warning, the running one stays until the next build.
#   cmake -DSRC=<built exe> -DDEST=<root folder> -P copy_launcher.cmake
file(COPY_FILE "${SRC}" "${DEST}/TTT2 Native Online.exe" ONLY_IF_DIFFERENT RESULT result)
if(result)
    message(WARNING "Could not update ${DEST}/TTT2 Native Online.exe (is it running?): ${result}")
endif()
