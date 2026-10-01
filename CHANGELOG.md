# Changelog

All notable changes to this project. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow [SemVer](https://semver.org/).

## [Unreleased]

### Removed
- The "not running a 256 color video driver" message at every start. Only
  the startup check is answered; rendering is unchanged.

### Fixed
- A rare fault with more than one player: the render passes walked the
  game's thinker list while the game's thread freed nodes in it. They now
  read a per-tick copy made on the game's thread.

### Added
- Each player's radar shows only what that player has explored.
- A cloaked craft is hidden from the other players' views.
- A late joiner reports how long catching up took (a minute of play
  replays in under a second).
- Online: players join and leave while the game runs. Seats nobody holds
  are driven by their robot's AI (marked in the input record, decided by
  the host); a late joiner replays the session's inputs from tick 0 to
  catch up, then takes its seats at an agreed tick; a PC that leaves (or
  goes quiet) hands its seats back to the AI. Open games (`--seats N`),
  join codes (`HOVER-XXXXX-XXXXX`), and the game's own Start/Pause locked
  while online.
- Every player's own dashboard: radar, speed, height, pod counters and
  gauges, score and flags.
- Powerups for players in robot seats: they pick up pods and use
  jump/wall/cloak (player 2: U/O/P), each effect on their own craft.
- Player 1's hovercraft is drawn for the others (the human never had a
  sprite), each sprite facing each viewer.
- Teams by seat (`[mp] teams=`, `--teams`): co-op with player 1 or on the
  robots' side; wins count a side's flags; hunters chase the nearest
  human-side craft.
- Online play: lockstep over UDP. The host takes over the game's 50 ms tick
  and runs it only when every seat's input has arrived; inputs are sampled
  3 ticks ahead and repeated for loss; the host relays everyone to
  everyone (up to 16 seats, 1-4 per PC with split screen); every 20 ticks
  each PC hashes every craft and a mismatch stops the game. Recomp >
  Multiplayer > Host / Join, `--host`, `--join`, `--clients`, `--local`
  ([docs/multiplayer.md](docs/multiplayer.md)).
- Headless runs get a scratch profile per process, so two can run at once.

### Fixed
- On a small screen (a phone over RDP, a disconnected session) the game
  fitted its frame to the desktop, no layout matched and nothing was
  rendered: a cloaked frame now always gets its native 516x388.
- The desync hash read past pods (0x100 bytes) as if they were crafts,
  giving false alarms when a pod's effect was running.
- Split screen for 2 to 4 players: players 2-4 drive robots of their own
  (one extra flag-runner per player in every level) with controllers 2-4,
  each with their own view. The frame draw runs once per player with the
  camera redirected (lift-time patches in `run_lift.py`), each pass into
  its cell of the capture buffer. Recomp > Multiplayer, `--players N`
  ([docs/multiplayer.md](docs/multiplayer.md)).
- `run_lift.py` source patches: hooks the host needs inside lifted
  functions, applied to every copy of the instruction and checked.
- The presenter: the game in a resizable window of the host's own on
  Direct3D 11 (WARP, then GDI, as fallbacks), with sharp/smooth/nearest/
  integer scaling, borderless fullscreen (F11, Alt+Enter), CRT (scanlines,
  aperture grille, curvature, vignette), 16-bit and 8-bit ordered
  dithering, and vivid colour. The game keeps its own frame, cloaked; the
  menus, keys and focus are forwarded ([docs/presenter.md](docs/presenter.md)).
  `--classic` for the original window.
- Level seeds: `time()` returns the seed the host picks, so a level (walls
  + seed) replays exactly. Recomp > Level pins it, saves levels, and copies
  or plays share codes (`L3-1789123456`); the title shows level and seed.
  `--seed N` ([docs/levels.md](docs/levels.md)).
- `--shot N:file.bmp`, `--levels-selftest`; conformance now runs on a
  pinned seed: 15/15.
- Xbox controller support (XInput): the pad presses the keys the game has
  bound, read live from its key table, so in-game rebinding carries over.
  Pause and new game on Start/Back ([docs/controller.md](docs/controller.md)).
- `hover.ini` beside the exe, written with its defaults on first run: the
  `[pad]` layout, deadzone, trigger threshold and controller slot.
- A **Recomp** menu on the game's menu bar: controller status and toggle,
  deadzone and slot presets, edit and reload `hover.ini`.
- `--pad-selftest`, and two conformance milestones (controller mapping,
  Recomp menu): 13/13.
- The whole pipeline, from your copy to a running executable:
  `tools/gamefiles.py` (drive, folder, zip or BIN/CUE/ISO into `game\hover`),
  `tools/catalog.py` (pcrecomp `disasm32`: 5,090 functions), `run_lift.py`
  (every function of `HOVER.EXE` into one tree: 5,135 functions, 0 lift
  errors) and `build.cmd` (32-bit MSVC host on pcrecomp `native32`).
- `src/runtime/host.c`: the guest image mapped at `0x00400000` and bound to
  real Windows, `DASHRES.DLL` loaded as a data file, capture of every blit to
  the game window ([docs/host.md](docs/host.md)).
- Boots to the attract screen, starts a game on F2 and plays level 1 to the
  end, recompiled.
- `--headless` (a cloaked, never-activated window, with a fresh settings
  profile per run), `--record out.mp4`, `--frames N`, `--diff A,B`,
  `--key NAME@MS[+HOLD]`, `--watchdog S` (with a per-thread stack dump),
  `--native-trace`, `--callbacks`.
- `tools/conformance.py`: 11 milestones from a headless run (boot, start,
  drive, 1,000 frames) plus lift health, against `conformance.json`. First
  baseline: 11/11, 0 lift errors, 0 unresolvable tail calls.
- `tools/addr2line.py`: host addresses to lines of generated C, from the PDB.
- `Setup.cmd`: the one-click route, running the same steps as the README.

### Toolkit
- Needs [pcrecomp#24](https://github.com/sp00nznet/pcrecomp/pull/24) (an entry never points
  into its own jump table); without it the game faults as level 1 starts
  ([docs/toolkit.md](docs/toolkit.md)).
