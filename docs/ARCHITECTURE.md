# Architecture

## The build pipeline (runs on each player's PC)

```
your .iso ──► tools/setup/Xdvdfs.cs ──► native\game\ (46 files, SHA-1 verified)
                                            │
                                     Default.xex
                                            │
                    tools/tt2-analyze  ◄────┤  (links XenonUtils)
                    · register save/restore helpers (byte patterns)
                    · jump tables (mtctr/bctr back-tracking, byte/half/word tables)
                    · function boundaries for switch-containing leaf functions
                    · decrypted image + kernel data imports + XEX headers
                                            │
                    tt2_recomp.toml + tt2_switch_tables.toml + tt2_image.{bin,json}
                                            │
                    XenonRecomp (pinned, + tools/recomp-patches) ──► ppc_recomp.*.cpp (~300 files)
                                            │
                    clang (llvm-mingw) + src/runtime ──► native\bin\tt2.exe
```

Why the analysis happens at setup and isn't shipped: every address is derived from the player's
own executable, so the repository contains nothing taken from the game.

## Patches to XenonRecomp

- `0001`: about 70 instructions the game uses that upstream lacks (VMX shifts, rotates,
  saturating arithmetic, packs, compares with CR6 updates, update-form loads and stores,
  `bdzf`/`bdnzt`, `cror`/`crorc`, `eqv`, `addc`/`addme`/`subfze`, 128-bit multiply-high), CR0
  updates for `rldicl.`, and division that doesn't trap on x/0 (PowerPC returns an undefined
  value; x86 would fault).
- `0002`: the XEX loader allocated only the compressed blocks' size for "basic" compression and
  then read the rest of the image out of bounds. The tail is now zero-filled.

## The runtime (`src/runtime`)

| File | Role |
| --- | --- |
| `memory.cpp` | 4 GB guest address space (committed, write-watched), allocators for virtual (`0x4…`), system (`0x7E…`) and physical (`0xA…`) memory, the function lookup table |
| `image.cpp` | Loads the decrypted image, fills the kernel's exported data (`KeTimeStampBundle`, `XboxHardwareInfo`, …), answers `RtlImageXexHeaderField` |
| `timebase.*` | `mftb` mapped to a 50 MHz clock (force-included into the generated code), indirect-call guard |
| `kernel/objects.cpp` | Handle table, dispatcher objects, waits (any/all, timeouts) on one mutex and condition variable |
| `kernel/threads.cpp` | Guest threads: PCR/TLS/TEB layout, static TLS copy, guest stacks, `ExCreateThread` |
| `kernel/sync.cpp` | Events, semaphores, mutants, timers, DPCs, spinlocks, critical sections |
| `kernel/io.cpp` | Xbox device paths onto `native\game` (read-only), `native\hdd`, `native\cache` and save content |
| `kernel/rtl.cpp` | Strings, time, and the kernel's printf family with PPC varargs |
| `kernel/xam.cpp` | Controllers, offline profiles, save content, system UI stubs, notifications, tasks |
| `kernel/system.cpp` | Config settings, XEX module queries, SHA-1, offline network stack |
| `gpu/video.cpp` | `Vd*` imports and a null command processor: register writes, scratch writeback, fences, waits, interrupts, 60 Hz vblank |
| `audio/audio.cpp` | XAudio render client to an SDL audio stream (6-channel to stereo downmix) |
| `input/input.cpp` | SDL3 gamepads, 2 ports, positional face buttons |
| `netplay_glue.cpp` | Frame boundary and synchronized inputs for netplay |
| `rollback/snapshot.cpp` | Per-frame guest memory snapshots for rollback |
| generated `import_stubs.cpp` | A logging stub for every import without an implementation |

Debugging: set `TT2_TRACE=1` to log every kernel call. Crashes are logged with guest addresses in
`native\logs\tt2.log`, and an indirect call to an address the recompiler did not see as a function
stops with that address in the message.
