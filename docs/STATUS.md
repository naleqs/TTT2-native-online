# Project status

Last updated: 2026-10-17, version 0.1.0.

## Summary

| Area | State |
| --- | --- |
| First-time setup (verify, extract, toolchain, build) | ✅ Works end to end on the retail USA/Europe disc |
| Recompilation of `Default.xex` | ✅ All code translates and compiles: 600 jump tables, 0 unknown instructions |
| Runtime: boot | ✅ Boots every time (12/12 test boots) and runs the main loop at about 60 fps: 40+ game threads, Havok physics, file streaming, audio engine |
| Graphics (Xenos to Direct3D 11) | 🚧 In progress. The command processor handles every packet the game sends (including the CPU callbacks and vsync waits) and records draws, resolves and shaders. The shader translator (Xenos microcode to HLSL) compiles every shader captured so far. The Direct3D 11 renderer draws the intro, title screen, menus (with their 3D backgrounds) and character select correctly. Not done yet: cube and 3D textures, mipmaps, small packed textures, MSAA, fights untested |
| Title update and DLC | ✅ Setup verifies the player's own packages (STFS hash tree), applies Title Update 3 to `Default.xex` (`tools/tt2-xexpatch`, XenonRecomp's XEX patcher) and installs DLC; the game reads the Bonus Tracks packs. Per-version addresses for the hooks are in `config/recomp/tt2_hooks*.toml` and `src/runtime/game/` |
| Audio | 🟡 The game's audio engine runs and submits frames; XMA decoding (music, voices) missing |
| Controllers (2 pads, any brand) | ✅ |
| Rollback session | ✅ Tested: bit-identical over 3,600 frames at up to 160 ms with 10% packet loss |
| Guest memory snapshots | ✅ Tested: exact restores, including memory written by other threads |
| Rollback in the game | ✅ Works in the running game: 480 rollbacks / 1,440 re-run frames with no crash or hang at 60 fps; 0.07 ms save, 0.2 ms load, 0.3 ms per re-run frame |
| Netplay between two game instances | ✅ Two instances on one PC connect over UDP and run the game in lockstep with rollback enabled |
| P2P lobby (trackers + WebRTC) | ✅ Tested through the real public trackers: discovery, WebRTC connection, challenge/accept, 200/200 match datagrams (`tt2_lobby_smoke`, two peers on one PC). A test across two networks is still to do |
| Launcher | ✅ Controller navigation, setup, local, lobby, direct connect, settings |

## How rollback works in the game

- **Frame boundary:** a mid-asm hook at the top of the main loop (`sub_8256CB80`, `0x8256CBE8`).
  One loop iteration is one game frame. `src/runtime/game/tt2_frame.cpp` can re-run an iteration.
- **Snapshots:** after each save the game's pages are write-protected. The first write to a page
  tells who owns it:
  - the main thread's update pass → game state, rolled back
  - another thread, or the main thread's draw pass → presentation or worker state, never rolled
    back. This is Tekken6-rollback's "foreign pages" approach.
- **Determinism:**
  - inputs are frozen at the frame boundary
  - the main thread sees frame-locked time
  - the input packet number is derived from the input itself
  - kernel lock state lives outside game memory, so a rollback can't resurrect a held lock
  - re-run frames skip the draw pass and play no sounds
- **Stress test:** `TT2_ROLLBACK_STRESS=interval,depth,start_frame` rolls back `depth` frames every
  `interval` frames and compares what each re-run frame changed with the original frame.
  Remaining differences are draw-pass bookkeeping: "used on frame N" stamps off by one, and
  render command lists. Those are written by the draw pass, which re-runs skip.

## Known issues

- **No sync point yet.** Both players' games must start rolling back from the same game state.
  Netplay currently starts at the first frame (boot), but boot involves asynchronous loading, so
  the two games can drift apart before a fight. Tekken6-rollback starts at the fight; the TTT2
  equivalent (fight start/end detection) still has to be found.
- **No desync detection in the game yet.** The session supports checksums, but the per-frame
  memory hash also includes presentation pages, so it is turned off.
- Some intro or attract sequences use a movie player with its own threads. They survive rollback
  (their pages are foreign), but rollback is meant for fights.

## Next steps, roughly in order

1. **Graphics.** Direct3D 11, with shaders translated to HLSL at runtime and compiled by
   `d3dcompiler_47.dll` (part of Windows 10/11, so nothing to install). Done: the command
   processor (`src/runtime/gpu/video.cpp`), draw/shader capture (`gpu/capture.cpp`) and the
   shader translator (`gpu/shader_translator.cpp`), and a first renderer (`gpu/renderer_d3d11.cpp`):
   render targets per EDRAM surface, resolves to host textures, vertex fetch from guest memory,
   index buffers, rectangle/quad/point primitives through geometry shaders, 2D textures (tiled and
   linear, mip 0 only), samplers, blend/depth/stencil/cull state and front buffer presentation.
   Next: check the menus and fights, then mipmaps, cube/3D textures, packed small textures, MSAA
   surfaces, EDRAM aliasing between formats and writing resolves back to guest memory. Seeing the screen is also what makes it possible to navigate to a fight and
   finish the rollback work.
2. **Fight sync point** for netplay (see above), then checksum-based desync detection limited to
   game-state pages.
3. **XMA audio decoding** (FFmpeg's XMA2 decoder, as Xenia does). A placeholder decoder that
   consumes buffers and outputs silence is in place.
4. **File system details:** `\Device\Image`, `\Device\Harddisk0\partition0` and
   `NtQueryInformationFile` class 27.
5. **Lobby across two networks** (different NATs) and more tracker fallbacks.
6. The 4 `vpkd3d128 ...,5,3,0` sites (64-bit float16 pack, mode 3) emit a warning; they need a
   correct implementation in `tools/recomp-patches`.

## Fixes for timing problems in the game

`src/runtime/game/tt2_fixes.cpp` holds patches for races in the game that the console's timing
hid. For example, a Havok job wait held a lock that its worker needed, which deadlocked about
every other boot on a fast PC.

## Debugging aids

| Variable | Effect |
| --- | --- |
| `TT2_TRACE=1` or `TT2_TRACE=Name1,Name2` | Log kernel calls (all, or only these) |
| `TT2_TRACE_STACK=1` | Add a guest backtrace to each traced call |
| `TT2_TRACE_R3=hex` | Only trace calls whose first argument is this value |
| `TT2_DUMP_THREADS=seconds` | After that long, log every thread's position, current kernel call and held locks |
| `TT2_ROLLBACK_STRESS=i,d,s` | Offline rollback stress test (see above) |
| `TT2_LOG_NAME=file` | Log file name in `native\logs` (two instances on one PC) |
| `TT2_GPU_STATS=1` | Every 120 frames, log the frame's draws, resolves, primitive types and render target setup |
| `TT2_SCREENSHOT=N` | Save the presented picture every N frames to `native\screenshots` (PNG) |
| `TT2_LOG_IO=1` | Log every file the game opens and the result |
| `TT2_DLC=0` | Hide the installed DLC from the game |
| `TT2_DEBUG_KEYBOARD=1` | The keyboard acts as player 1's pad (arrows, Enter = Start, Z/X/A/S = A/B/X/Y), for test scripts |
| `TT2_RENDERER=null` | Don't draw; show the placeholder screen |
| `TT2_D3D_DEBUG=1` | Direct3D debug layer (needs the Graphics Tools Windows feature) |
| `TT2_DUMP_SHADERS=1` | Write every shader the game loads to `native\gpu\shaders`; check them with `tt2_shader_check <dir>` |

## Known limitations

- Only *Tekken Tag Tournament 2 (USA, Europe)*, base disc version, is supported. Title updates and
  DLC are not.
- Windows only. The runtime uses Win32 APIs: page protection, threads, files.
- The original Xbox Live online mode will never work; the game sees "no network cable". Online
  play goes through this project's netplay instead.
