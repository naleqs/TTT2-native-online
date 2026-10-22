# Project status

Last updated: 2026-10-22, version 0.1.0.

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
| Netplay between two game instances | ✅ Two instances on one PC (Title Update 3 build): menus in lockstep, rounds with rollback. A full two-round VS match with 1,596 re-run frames stayed in sync: same health on every frame, both rounds over on the same frames as in lockstep, no crash |
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

- **Rollback only in rounds, Title Update 3 only so far.** Menus, intros and KOs run in lockstep
  and each round starts at a sync point. Tested on one PC (two instances); a test across two PCs
  is still to do, and the addresses for the base game version are still to be found.
- **Rollback and the game's threads.** The fight runs in the scene update (`sub_825E04F0`, which
  re-run frames repeat) and in the game's job threads (character movement). Memory is rolled back
  per page; what other threads own is kept apart: their heap blocks go to a side heap, the job
  pool's, loaders' and sound library's writes never roll back, and objects of theirs that share a
  page with game state are kept as they are (see `src/runtime/game/tt2_side_heap.cpp` and
  `tt2_frame.cpp`).
- **Sound and rollback:** as in Tekken6-rollback, sound ignores rollbacks. A sound effect a re-run
  frame plays again gets the handle of the one already playing (same sound within 3 frames), any
  other is not started (`src/runtime/game/tt2_sound_rollback.cpp`). A sound only the corrected
  timeline has is not heard.
- **A see-through yellow layer in fights** (around the health bars, and over the whole winner
  screen). It comes from the glow (bloom) post-process: its bright pass (glow draw 3 of 4) weighs
  the scene by its colour, its alpha and a depth texture (a float depth resolve, read as
  `k_24_8_FLOAT`), and one of these inputs is wrong in the renderer, so the glow covers more than
  it should. Some of the glow chain's intermediate targets also show stale EDRAM data.
- **Small float differences between the two games** (positions up to a few units apart, health and
  round results identical), also in lockstep: the same match on two PCs is not bit-identical.
- **Occasional crash in controller-slot code** (`sub_8241ABC0`/`sub_82420C40`, a slot pointer of
  -1). Netplay's player 2 sign-in mismatch was one cause and is fixed; it has also been seen once
  without netplay.
- **Long stall when a character is tagged in** during a match: the game freezes and buffers
  heavily. Probably the incoming character's data loading or its shaders compiling on first use.
- **Some stage backgrounds and water shaders render incorrectly.**
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
5. **Character customization online:** each player's customized outfits and colours should show
   on both PCs. The two games' saved customizations differ, so they need to be exchanged at match
   start.
6. **Lobby across two networks** (different NATs) and more tracker fallbacks.
7. The 4 `vpkd3d128 ...,5,3,0` sites (64-bit float16 pack, mode 3) emit a warning; they need a
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
| `TT2_VSYNC=1` | Wait for the display refresh when presenting (default: tearing allowed, about a frame less input lag) |
| `TT2_LATENCY=1` | Every 300 frames, log frames issued vs presented and main loop iterations |
| `TT2_SHOW_FPS=0` | Hide the frames-per-second counter (top right; shown by default) |
| `TT2_LOG_IO=1` | Log every file the game opens and the result |
| `TT2_DLC=0` | Hide the installed DLC from the game |
| `TT2_DEBUG_KEYBOARD=1` | The keyboard acts as player 1's pad (arrows, Enter = Start, Z/X/A/S = A/B/X/Y), for test scripts |
| `TT2_RENDERER=null` | Don't draw; show the placeholder screen |
| `TT2_D3D_DEBUG=1` | Direct3D debug layer (needs the Graphics Tools Windows feature) |
| `TT2_DUMP_SHADERS=1` | Write every shader the game loads to `native\gpu\shaders`; check them with `tt2_shader_check <dir>` |
| `TT2_GPU_DUMP_FRAME=N` | Log every draw, texture, constant and resolve of frame N, with a thumbnail after each draw, to `native\gpu\dump` |
| `TT2_GPU_DUMP_ON_RESOLVE=hex` | Dump the frame after a resolve to that address instead (`TT2_GPU_DUMP_NTH`: the Nth burst, `TT2_GPU_DUMP_DELAY`: frames later) |
| `TT2_GPU_PICK=x,y` | With a dump: log every draw that changes that pixel of its render target |
| `TT2_WATCH=hex` | Log the guest functions that write that page of guest memory (an unaligned address: only writes to its 16 bytes) |
| `TT2_INSTANCE=name` | Keep hard disk, cache and saves in `native\instances\<name>` (two games on one PC) |
| `TT2_NET_DUMP=frame,...` | Netplay: write the game's data and heaps at those frames to `native\logs` (diff two players' dumps) |
| `TT2_NET_PREDICTION=n` | Netplay: allow `n` frames of prediction (rollback) instead of lockstep (also in menus, which is not supported) |
| `TT2_FIGHT_CHECK=1` | Log the fighters' health and positions every second, and every re-run frame that ends differently from its first run (`TT2_FIGHT_LOG_ALL=1`: every frame, to compare two players' logs) |
| `TT2_STRESS_ALTER=1` | Stress test: re-run frames with other inputs for player 1 (the fight must react) |
| `TT2_READ_WATCH=hex` | Hardware watchpoint: log every function that reads or writes those 4 bytes |
| `TT2_EXCLUDE=begin-end,...` | Rollback: more guest ranges that never roll back (experiments) |
| `TT2_SCENE_STEP_GAME=0`, `TT2_GAME_WINS=0`, `TT2_SIDE_HEAP=0`, `TT2_SOUND_ROLLBACK=0` | Rollback: turn off re-running the scene update, the "a page that held game state stays game state" rule, the side heap, or the sound handling (testing) |
| `TT2_INPUT_SCRIPT=ms=button[:hold_ms],...` | Press player 1's buttons at fixed times after start (in netplay: game frames), for unattended tests |

## Known limitations

- Only *Tekken Tag Tournament 2 (USA, Europe)*, base disc version, is supported. Title updates and
  DLC are not.
- Windows only. The runtime uses Win32 APIs: page protection, threads, files.
- The original Xbox Live online mode will never work; the game sees "no network cable". Online
  play goes through this project's netplay instead.
