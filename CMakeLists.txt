# SPDX-License-Identifier: MIT
# Top-level project per UE4SS C++ mod guide:
#   https://docs.ue4ss.com/dev/guides/creating-a-c++-mod.html
#
# Expected sibling layout:
#   windrose-patch-idle/
#     RE-UE4SS/              (clone: https://github.com/UE4SS-RE/RE-UE4SS)
#     WindroseIdleCpuFix/
#     CMakeLists.txt         (this file)
cmake_minimum_required(VERSION 3.22)
project(WindroseIdleCpuFixMods)

add_subdirectory(RE-UE4SS)
add_subdirectory(WindroseIdleCpuFix)
