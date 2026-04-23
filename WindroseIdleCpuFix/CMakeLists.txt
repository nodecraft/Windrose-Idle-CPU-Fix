# SPDX-License-Identifier: MIT
set(TARGET WindroseIdleCpuFix)

add_library(${TARGET} SHARED
    dllmain.cpp
    Patcher.cpp
)

target_include_directories(${TARGET} PRIVATE .)
target_link_libraries(${TARGET} PUBLIC UE4SS)

# Stage a drop-in install layout and zip it. The zip is tracked as an
# output file (not just a POST_BUILD hook) so CMake rebuilds it whenever
# it's missing — including after `rm -rf build/` or a fresh clone — not
# only when the DLL itself gets relinked.
# Extracting the zip inside a UE4SS `Mods/` directory produces:
#   Mods/WindroseIdleCpuFix/
#     enabled.txt
#     dlls/main.dll
set(_PKG_STAGE "${CMAKE_CURRENT_BINARY_DIR}/package/WindroseIdleCpuFix")
set(_PKG_ZIP   "${CMAKE_BINARY_DIR}/WindroseIdleCpuFix.zip")

add_custom_command(
    OUTPUT "${_PKG_ZIP}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${_PKG_STAGE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_PKG_STAGE}/dlls"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${TARGET}>" "${_PKG_STAGE}/dlls/main.dll"
    COMMAND ${CMAKE_COMMAND} -E touch "${_PKG_STAGE}/enabled.txt"
    COMMAND ${CMAKE_COMMAND} -E rm -f "${_PKG_ZIP}"
    COMMAND ${CMAKE_COMMAND} -E chdir "${CMAKE_CURRENT_BINARY_DIR}/package"
            ${CMAKE_COMMAND} -E tar cf "${_PKG_ZIP}" --format=zip WindroseIdleCpuFix
    DEPENDS ${TARGET}
    COMMENT "Packaging ${_PKG_ZIP} (drop-in for ue4ss/Mods/)"
    VERBATIM
)

add_custom_target(${TARGET}Package ALL DEPENDS "${_PKG_ZIP}")

