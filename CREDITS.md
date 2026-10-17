# Credits and third-party licenses

## Projects this work is based on

| Project | Author | License | Used for |
| --- | --- | --- | --- |
| [Tekken6-rollback](https://github.com/Ekspym/Tekken6-rollback) | Ekspym | GPL-2.0 | The rollback design: predicting remote input, re-simulating from snapshots of game memory, a 5-frame window, automatic input delay of 1 frame up to about 100 ms, the pad-read frame boundary, the fight-start sync point, not replaying sounds during re-simulation, and a virtual time base. TTT2 Native Online re-implements these ideas for the recompiled Xbox 360 game. No code was copied. |
| [Tekken6-native-online](https://github.com/Ekspym/Tekken6-native-online) | Ekspym | — | The player experience: a release with no game data, a first-time setup that verifies your own disc image and compiles the game locally, an online lobby, a first-to-3 format and an updating launcher. |
| [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) | hedge-dev (Skyth, Sajid, Hyper and others) | MIT | The Xbox 360 PowerPC to C++ static recompiler. Fetched at setup time and patched (`tools/recomp-patches`). `tt2-analyze` links its XenonUtils library. |
| [Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp) | hedge-dev | GPL-3.0 | Reference for a recompiled-game runtime: the guest thread and PCR layout, and the host/guest calling bridge in `src/runtime/guest.h`. |
| [Xenia](https://github.com/xenia-project/xenia) | Ben Vanik and contributors | BSD-3-Clause | Xbox 360 research: kernel export tables (via XenonRecomp), kernel and XAM behavior, and the PM4 command packet semantics in `src/runtime/gpu/video.cpp`. |
| [GGPO](https://github.com/pond3r/ggpo) | Tony Cannon | MIT | The rollback networking model (sync handshake, prediction, time sync) that both Tekken6-rollback and this project follow. |
| [Trystero](https://github.com/dmotz/trystero) | Dan Motzenbecker | MIT | The idea of using public WebTorrent trackers as serverless WebRTC signaling. |

## Libraries (fetched at build time, not stored in this repository)

| Library | License |
| --- | --- |
| [SDL 3](https://github.com/libsdl-org/SDL) | Zlib |
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT |
| [libdatachannel](https://github.com/paullouisageneau/libdatachannel) (with libjuice, usrsctp, plog) | MPL-2.0 (libjuice MPL-2.0, usrsctp BSD-3-Clause, plog MIT) |
| [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) | Apache-2.0 |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT |
| XenonRecomp's submodules: fmt, tomlplusplus, xxHash, libmspack, tiny-AES-c, SIMDe | MIT / MIT / BSD-2-Clause / LGPL-2.1 / Unlicense / MIT |

## Toolchain downloaded by setup (into `native\toolchain`)

| Tool | License |
| --- | --- |
| [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (Clang/LLVM, mingw-w64) | Apache-2.0 WITH LLVM-exception; mingw-w64 runtime licenses |
| [CMake](https://cmake.org) | BSD-3-Clause |
| [Ninja](https://ninja-build.org) | Apache-2.0 |
| [MinGit](https://gitforwindows.org) | GPL-2.0 |

## Game

*Tekken Tag Tournament 2* © Bandai Namco Entertainment. No part of the game is included. Each
player supplies their own copy.
