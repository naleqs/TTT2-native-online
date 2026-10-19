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
- **Missing:** a sync point at fight start (both players must roll back from the same state),
  and in-game desync detection. See `docs/STATUS.md`.

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
