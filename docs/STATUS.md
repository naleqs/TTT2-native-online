# Project status

Last updated: 2026-10-16, version 0.1.0.

## Summary

| Area | State |
| --- | --- |
| First-time setup (verify, extract, toolchain, build) | ✅ Works end to end on the retail USA/Europe disc |
| Recompilation of `Default.xex` | ✅ All code translates and compiles: 600 jump tables, 0 unknown instructions |
| Runtime: boot and CRT startup | ✅ Runs natively: heaps, 22 game threads, file I/O, first frame presented |
| Runtime: reaching the title screen | 🚧 Stalls in a job-system loop (see "Next steps") |
| Graphics (Xenos to Direct3D 12/Vulkan) | ❌ Not started. A null GPU executes fences and interrupts only, so nothing is drawn |
| Audio | 🟡 XAudio output path done; XMA decoding (music, voices) missing |
| Controllers (2 pads, any brand) | ✅ |
| Rollback session | ✅ Tested: bit-identical over 3,600 frames at up to 160 ms with 10% packet loss |
| Guest memory snapshots | ✅ Tested: 217 random rollbacks over 1,200 frames restored exactly |
| Rollback in the game | 🟡 Lockstep (delay-based) works without game hooks; rollback needs the main-loop hook (below) |
| P2P lobby (trackers + WebRTC) | ✅ Tested through the real public trackers: discovery, WebRTC connection, challenge/accept, 200/200 match datagrams (`tt2_lobby_smoke`, two peers on one PC). A test across two networks is still to do |
| Launcher | ✅ Controller navigation, setup, local, lobby, direct connect, settings |

## What was verified on real data

- `Setup.bat` on the retail disc image: all 46 game files SHA-1-verified while copying, the tools
  built from a clean clone, the code recompiled and compiled. `tt2.exe` builds in about 2 minutes
  on 16 threads.
- `tt2.exe` boots from the game's real entry point. It initializes the CRT, creates its heaps
  (68 MB and 248 MB of physical memory), starts its engine threads (`nuscMain`,
  `nuscFileAsyncRequest` and the job workers), opens disc files and presents a frame.

## Next steps, roughly in order

1. **Kernel accuracy for the job system.** At the stall the game calls `NtSuspendThread` about
   1,500 times a second, which the runtime does not enforce yet, and `XMsgInProcessCall`, which
   currently fails. Implement real suspend/resume (Win32 `SuspendThread` on the host thread)
   and the XAM messages the game sends.
2. **File system details**: `\Device\Image`, `\Device\Harddisk0\partition0` and
   `NtQueryInformationFile` class 27.
3. **GPU.** This is the big one: translate the Xenos command stream (PM4) and shaders to a host
   API. Candidates are XenosRecomp (hedge-dev) for shaders and the plume RHI from Unleashed
   Recompiled, or porting the relevant parts of Xenia's Direct3D 12 backend. Expect months of work.
4. **XMA audio decoding** (FFmpeg's `xma2` decoder, as Xenia does).
5. **Rollback hooks** (`config/recomp/tt2_hooks.toml`): find the main-loop frame function, the
   fight start and the fight end. Then switch the netplay glue from lockstep
   (`max_prediction = 0`) to rollback with `rollback::SnapshotRing`, exclude audio and streaming
   buffers from snapshots, and freeze the guest time base during re-simulation, as
   Tekken6-rollback does.
6. **Lobby across two networks** (different NATs) and more tracker fallbacks
   (`tracker.files.fm` was unreachable during testing).
7. The 4 `vpkd3d128 ...,5,3,0` sites (64-bit float16 pack, mode 3) emit a warning; they need a
   correct implementation in `tools/recomp-patches`.

## Known limitations

- Only *Tekken Tag Tournament 2 (USA, Europe)*, base disc version, is supported. Title updates and
  DLC are not.
- Windows only. The runtime uses Win32 APIs: write-watch, threads, files.
- The original Xbox Live online mode will never work; the game sees "no network cable".
  Online play goes through this project's netplay instead.
