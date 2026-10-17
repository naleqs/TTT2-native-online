# Building from source

## What can be built without the game

The launcher, the netplay library (with lobby) and the tests contain no game code:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build
ctest --test-dir build --output-on-failure
```

Requirements: Clang 18 or newer (llvm-mingw, or clang-cl with Visual Studio), CMake 3.24 or newer,
Ninja and Git. The dependencies (SDL3, Dear ImGui, libdatachannel, Mbed TLS, nlohmann/json) are
fetched into the build folder.

Options:

| Option | Default | Meaning |
| --- | --- | --- |
| `TT2_BUILD_LAUNCHER` | ON | Build `TTT2 Native Online.exe` |
| `TT2_WITH_WEBRTC` | ON | Include the online lobby (libdatachannel) |
| `TT2_BUILD_TESTS` | ON | Build `tt2_rollback_test` and `tt2_snapshot_test` |
| `TT2_BUILD_GAME` | OFF | Build `tt2.exe`. Needs `TT2_RECOMP_DIR` and `TT2_SIMDE_DIR`, which `Setup.bat` provides |

## Building the game

Only `Setup.bat` builds the game, from your own disc. Developers who already have the tools can
skip the toolchain download:

```
set TT2_USE_SYSTEM_TOOLS=1
Setup.bat -Image "D:\path\to\Tekken Tag Tournament 2 (USA, Europe).iso"
```

Rebuild after changing runtime code (keeps the verified game data):

```
Setup.bat -Rebuild
```

or, faster, rebuild only the game: `cmake --build native\build\game --target tt2`.

## Making a release

A release zip contains the launcher, `Setup.bat`, `tools\`, `config\`, `src\`, `cmake\`,
`CMakeLists.txt`, `VERSION`, `LICENSE`, `README.md`, `CREDITS.md`, `.env.example` and an empty
`iso\` folder. It contains no game data and no generated code. The GitHub workflow in
`.github/workflows/build.yml` builds the launcher and runs the tests for every push, and
packages a release for tags `v*`.

## Updating XenonRecomp

1. Change `TT2_XENONRECOMP_COMMIT` in `tools/CMakeLists.txt`.
2. Re-create the patches against the new commit
   (`git -c core.autocrlf=true diff > tools/recomp-patches/000N-....patch`).
3. Bump `VERSION`. The netplay build ID changes, so players can only match others with the same
   release.
