# Netplay

## Overview

```
 player A                                                   player B
 tt2.exe ── loopback UDP ── launcher ══ WebRTC (P2P) ══ launcher ── loopback UDP ── tt2.exe
                              │                            │
                              └── WebTorrent trackers ─────┘   (signaling only)
```

Direct connect skips the launchers: the two `tt2.exe` processes exchange UDP packets with each
other directly.

## Rollback session (`src/netplay/src/session.cpp`)

The design follows Tekken6-rollback (Ekspym), which is itself GGPO-style:

1. **Handshake.** The sides exchange 5 request/reply round trips to measure the ping, check that
   the build IDs match and that the players picked opposite sides, and agree on an input delay
   (the larger of the two proposals).
2. **Every frame:**
   - The local input is queued for `frame + input_delay` and sent together with every input the
     peer hasn't acknowledged yet. That redundancy covers packet loss, so no retransmits are needed.
   - The remote input for the current frame is the real one if it has arrived, otherwise a
     prediction: the last remote input that was confirmed.
   - If the session would be more than `max_prediction` (5) frames ahead of the remote input, it
     returns `Wait` and the game stalls for a moment.
3. **Rollback.** When a remote input arrives that differs from the prediction used for that frame,
   the next frame boundary loads the snapshot of that frame and re-simulates every frame up to
   the present with the corrected inputs. Re-simulated frames are not presented.
4. **Time sync.** Each packet carries the sender's frame number. With the RTT, each side estimates
   how far ahead it runs, and the side that is ahead skips a frame now and then.
5. **Desync detection.** Every 60 frames, once a frame's inputs are all confirmed, both sides
   exchange a checksum of that frame's state.
6. **Automatic input delay:** 1 frame up to 100 ms RTT, then +1 frame per extra 50 ms (at most 4).

`tests/netplay/rollback_test.cpp` runs two sessions over a simulated network (latency, jitter,
loss, reordering, clock drift) with a deterministic toy game. It checks that both sides end with
identical state for every confirmed frame, and that every applied input is the one the player
actually pressed.

## Game state (`src/runtime/rollback/snapshot.cpp`)

After every save the game's pages are write-protected. The first write to a page decides what it
is, following Tekken6-rollback's "foreign pages":

- written by the main thread's update pass: game state, saved and rolled back
- written by any other thread, or by the main thread while drawing: presentation or worker
  state, never rolled back (restoring it under its owner's feet would corrupt it)

Saving a frame copies the previous contents of the pages the frame changed into an undo record.
Loading replays undo records from the newest back to the target. A few hundred 4 KB pages per
frame: about 0.07 ms per save and 0.2 ms per load in the game.

## Integration with the game (`src/runtime/rollback/game_rollback.cpp`)

- **Frame boundary:** a mid-asm hook at the top of the game's main loop (`Tt2FrameBoundary`,
  `config/recomp/tt2_hooks.toml`). One loop iteration is one frame. Re-simulation re-runs the
  iteration's update calls (`src/runtime/game/tt2_frame.cpp`) without the draw pass.
- **Inputs** are digital on the wire: the left stick becomes the D-pad and the triggers become
  buttons. They are frozen at the frame boundary, so a frame and its re-run read identical values.
- **Time:** the main thread sees frame-locked time (frame x 1/60 s), other threads the real
  clock.
- **Locks:** kernel lock state lives outside game memory, so a rollback never resurrects a held
  lock.
- **Lockstep outside fights.** A rollback re-runs only the game's update calls, and the menus
  react to input outside them, so the side that rolls back would lose menu inputs. The session
  therefore runs with no prediction (`max_prediction` 0): a frame runs once both inputs are
  confirmed and nothing is ever rolled back. `Session::set_max_prediction` switches prediction on;
  that is meant for fights once fight start is detected. `TT2_NET_PREDICTION=n` forces it (tests).
- **Same game on both PCs.** Everything the game reads must be identical on both sides:
  - keystrokes (`XamInputGetKeystrokeEx`) come from each frame's shared pad state, with key
    repeat counted in frames, and are rebuilt when a frame runs again
  - every clock (timebase, system time, the kernel's time stamp bundle, all threads) follows
    the frame-locked time from a fixed start, so boot-time seeds match
  - both players are signed in and ports 3-4 are empty, whatever controllers each PC has
  - **loader barrier:** at each frame boundary the main thread waits (up to 100 ms) until
    every thread that used the file system in the last 3 seconds is blocked, so a load finishes
    on the same frame on both PCs
- **Tested** with two games on one PC (`TT2_INSTANCE=A`/`B`, frame-timed `TT2_INPUT_SCRIPT`):
  boot, menus, VS character and stage select stay identical frame for frame.
- **Missing:** random choices (random stage, intro animations) still differ: some state the
  game's random numbers depend on is reached a frame apart on the two PCs. Fight start
  detection (for rollback in fights) and in-game desync detection are also still to do. See
  `docs/STATUS.md`.

## Profiles

- Name and profile picture are set in the launcher's Settings. The picture is any image file,
  cropped to a square and stored as a 96x96 PNG in `native\profile\avatar.png`.
- The lobby sends both in the `hello` message (the picture as base64, at most 64 KB) and shows
  every player's picture next to their name.
- The game shows the names as the players' gamertags: the launcher starts it with
  `--name1 <player 1> --name2 <player 2>`, the same on both PCs. Direct connect does not know the
  other player's name and uses "Player N" for them.

## Lobby (`src/netplay/src/lobby.cpp`)

- The room hash is `sha1("ttt2-native-online/" + build_id + "/" + room)`, truncated to 20
  characters. Only players with the same build and room can meet.
- Each client announces itself to the trackers with 4 pre-gathered WebRTC offers. A tracker
  forwards an offer to other peers in the same room, they answer, and the connection is direct.
- Each peer connection has a reliable `control` channel (hello, status, ping, challenge,
  accept/decline) and an unreliable, unordered `match` channel for the rollback traffic.
- When a challenge is accepted, the launcher starts `tt2.exe` with `--netplay --peer
  127.0.0.1:<port+1>` and relays its UDP packets onto the `match` channel. The challenger plays
  as player 1.

## Ports

| Port | Use |
| --- | --- |
| `TT2_NETPLAY_PORT` (7000) | The game's netplay socket. Forward it, or use a VPN, for direct connect |
| `TT2_NETPLAY_PORT + 1` | The launcher's loopback relay (lobby matches, 127.0.0.1 only) |
