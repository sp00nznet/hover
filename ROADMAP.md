# Roadmap

The port plays. What follows turns it into the best way to play Hover!:
better pictures, pads, sound, new levels, and other people. Each milestone
lands with its own conformance check and a line in `docs/`.

Where the ideas come from: gunman's D3D11 presenter, SimCity 2000's frontend
(ImGui, ini, palette emulation, shader chain), and hlmapgen
(`F:\projects\tools\tilde\dist\hlmapgen`, seeded grid layouts).

## 0. Housekeeping

- **Merge the toolkit fix** ([pcrecomp#24](https://github.com/sp00nznet/pcrecomp/pull/24)) and drop the
  branch pin from `Setup.cmd`, the README and CI.
- **`hover.ini` beside the exe**: one settings file for everything below
  (`[video]`, `[audio]`, `[pad]`, `[levels]`, `[net]`), read and written with
  `Get/WritePrivateProfile*` as SimCity 2000 does. Command-line flags
  override it. Portable mode: the game's own registry settings move into
  the ini too (the headless profile trick already redirects HKCU).
- **A "Recomp" menu** next to the game's own Game/Options menus, so every
  new feature is one click away in the 1995 style, with native dialogs. An
  ImGui overlay (F1) is the alternative for the graphics sliders; decide
  once the presenter exists.

## 1. Graphics

- **GPU presenter** from gunman's `present.c` + `present_d3d.c`: the game's
  blits land in a frame buffer (the capture shadow already does this) and one
  D3D11 draw puts it on screen. Needs 8-bit input: expand through the DIB
  colour table to BGRA on the CPU (516x388 is nothing), or upload R8 + a
  palette texture. GDI StretchBlt fallback when D3D11 is missing.
- **Window and scaling**: resizable window, integer scale, aspect-correct
  letterbox (4:3 or square pixels), borderless fullscreen (F11 / Alt+Enter).
- **Filters**: nearest, sharp-bilinear, bilinear, Scale2x/EPX, xBR later.
- **CRT**: scanlines, aperture grille / shadow mask, curvature, vignette,
  bloom/glow (SimCity 2000's chain has each as its own slider).
- **Dithering and colour**: the game renders 256 colours. Options: an
  ordered-dither "authentic 1995 16-bit desktop" look, a de-dither/smooth
  pass, "vivid" saturation (gunman), gamma, and colour-blind-safe palette
  remaps for red/blue flags.
- **Higher internal resolution**: the renderer draws a 512-wide view into a
  DIB it allocates itself, and Customize Game has a Speed & Detail slider.
  Find the view size variables and raise them (1024, 2048 wide), with the
  dashboard scaled to match. The biggest visual win if the renderer allows it.
- **Widescreen**: a wider view DIB plus a matching field of view, if the
  projection is a variable and not a constant.
- **Texture packs**: `.TEX` is a palette plus column-run bitmaps
  ([docs/maz-format.md](docs/maz-format.md)). Export to PNG, re-import, and
  later a high-colour path that bypasses the palette for upscaled textures.
- **Frame pacing**: the frame loop is paced by a 50 ms multimedia timer
  (20 Hz). Try 60 Hz pacing (`timeSetEvent` period, as SimCity 2000's speed
  slider does) and check the physics still time off the clock, not frames.
- **Screenshot key** (F12, PNG beside the exe) and `--record` from the menu.

## 2. Input

- **Xbox controllers (XInput)**: folded into the `GetAsyncKeyState` shim that
  scripted keys already use. Stick/d-pad to the four steering keys, triggers
  to throttle/reverse, buttons to the jump/wall/cloak keys the game reads
  (its key table is at `0x004606EC`..`0x00460708`, set by Keyboard Settings),
  Start = F3 pause, Back = F2 new game.
- **Pad settings dialog**: remap each action, stick deadzone, trigger
  threshold, invert, which pad is player 1. Saved in `[pad]`.
- **Rumble** on hits and pickups: key off the sounds the game already plays
  (`HIT_WALL`, `HIT_ROBT`, `OBT_*` through `sndPlaySoundA`/the mixer).
- **Analogue steering**: the game only knows keys held or not. Pulse-width
  modulate the turn keys from stick deflection for finer turning.
- The game's own `joyGetPos` joystick path: test it, and hide it if XInput
  replaces it.

## 3. Sound

- **Options dialog**: master, effects and music volume, mute when unfocused,
  and the WaveMix settings (`WAVEMIX.INF`: 11 kHz mono by default) raised to
  22/44 kHz stereo where the samples allow.
- **Music**: the `.MUZ` files are RIFF `MIDS` streamed MIDI. Play them
  through a built-in SoundFont synth (TinySoundFont-class, a GM SoundFont
  the user supplies) instead of the Windows GS wavetable, with a choice of
  font. Replacement tracks (OGG) per level as a mod option.
- **Audio in recordings** and a muted headless mode (carried over).

## 4. Levels

What we know ([docs/maz-format.md](docs/maz-format.md)): a `.MAZ` is an MFC
`CArchive` of one `CMerlinWorld`: wall/floor line segments on a 256-unit
grid, named points (`HUMAN_nn`, `ROBOT_nn`, `FLAG_*_nn`, `POD_RANDOM_nn`,
`BEACON_nn`), and a precomputed BSP. The walls are fixed; **everything else
is placed at load from `srand(time(NULL))`** (0x004154DC): player and robot
starts, the 28 pods, the flags, and the level itself in random-level mode.
So a playable level is **`.MAZ` + a 32-bit seed**.

- **Seeds**: shim `GetLocalTime` (only `time()` uses it) to pin the seed.
  Show the seed on the level-start screen, `--seed N`, and a "replay this
  seed" menu item. Same seed = same pods and flags, which is also what
  racing a friend's time needs.
- **Saved levels**: a small library in `levels\` of (maze, seed, name,
  best time/score), favourites, and share codes (`MAZE2-1f3a9c` style).
- **Level select**: the 21-entry level table at `0x004C4000` (music, maze,
  texture set, pods per type, robots, flags) is writable memory. Pick any
  entry, or edit the entry: robot count, pods per type, difficulty.
- **Maze writer**: `tools/maz.py` reads; add the writer (the record layouts
  are fully known, including the 5-byte static tail) and a BSP builder,
  since the game loads a precomputed tree and cannot build one. Round-trip
  the four shipped mazes byte for byte first.
- **Generator**: port hlmapgen's `core/layout.js` + Mulberry32 (rooms, Prim's
  MST, extra loop edges) to Python, plus a recursive-backtracker carver for
  narrow maze sections, and emit walls on the 256-unit grid with texture sets
  per region (the shipped `.TEX` names). The constraints the loader imposes:
  every point prefix present and numbered from 00 without gaps, at least 28
  pod points and enough well-spaced flag points per team, coordinates below
  23040. One seed drives both the layout and the in-game `srand`.
- **Custom levels in game**: a `CreateFileA` shim maps `mazes\MAZEn.maz` to
  the chosen file, so nothing in `game\hover` is ever overwritten.
- **Editor** (stretch): a 2D top-down editor on the same Python model, or
  as a page, for hand-made mazes.

## 5. Multiplayer

The original is one human against robots. Everything here is research
first, in this order, because each step needs the one before.

1. **Determinism audit.** Pin the seed, record inputs, replay, compare frame
   hashes. Find every clock read (`GetTickCount`, `timeGetTime`, the 50 ms
   timer) and every other source of non-determinism.
2. **Find the craft model.** The player's craft and the robots' crafts are
   probably one struct with a controller (human keys / robot AI following
   `BEACON`s). Locate it, and the AI's steering entry point.
3. **A second human craft**: replace one robot's AI steering with a second
   input source (pad 2). Both teams get humans; the game already has two
   teams and a flag per team.
4. **Split screen (2 and 4 players)**: the renderer draws one camera. Either
   run it once per viewport per frame (camera swap, render into separate
   DIBs, composite), or run N synchronized processes, one per viewport,
   composited by the presenter. The first is better if the renderer's state
   is re-entrant enough.
5. **Internet play**: lockstep on inputs over UDP (8 bytes a player a
   tick), a relay or direct connection with a join code, and rollback
   (GGPO-style) later if latency needs it. Determinism (1) is the
   prerequisite, and save-state snapshots of guest memory make rollback
   possible.
6. **Ghost races**: the cheap multiplayer. Record a run (seed + inputs),
   play it back as a translucent ghost craft. Needs 1 and 2 only.

## 6. Everything else

- **Save states** (F5/F9): snapshot the guest's memory (image, heap,
  stacks) and registers. Hard with live Windows handles (DIBs, timers),
  but the level state is plain guest memory; a level-restart snapshot is the
  realistic first step.
- **Speedrun timer and per-seed best times**, stored in the levels library.
- **High scores** in `hover.ini` instead of the registry (portable, and
  sharable).
- **Cheats menu**: all powerups, invincibility, robot speed (addresses from
  the craft struct work in section 5).
- **Discord-free "now playing" overlay**: level, seed, time, flags.
- **Mod/texture/music packs** in `mods\`, loaded in order.
- **Linux / Steam Deck** under Wine or Proton: the host is plain Win32 and
  should work; test it.

## Carried over

- **Win a level** in a scripted run: reach level 2's load, the High Score
  dialog and the score screen, each a conformance milestone.
- **Levels 2 and 3** (`MAZE2`/`MAZE3`, their own textures and music).
- **Keys timed in frames** for scripted runs, not milliseconds.
- **Driving dialogs headless** (the real dialog, cloaked, with scripted
  buttons) instead of answering OK unseen.
- **GDI drawn straight onto the window** is not in recordings.
- **The four `fnstenv`/`fldenv`** lift as no-ops, and three garbage decodes
  (`sti`, `fisttp`) sit in data the catalog took for code. No run has been
  seen to reach them.

## Out of scope

- Redistributing anything from the game, including the recompiled C, the
  shipped mazes or textures (the tools read the user's own copy).
- `DISPDIB.DLL` full-screen mode (Windows 95's low-resolution display
  driver). The presenter's fullscreen replaces it.
- `WMCONFIG.EXE` (the WaveMix configuration tool) and the WinHelp file.
