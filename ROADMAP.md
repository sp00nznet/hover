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
- **`hover.ini` beside the exe** (started: `[pad]` is in): one settings file for everything below
  (`[video]`, `[audio]`, `[pad]`, `[levels]`, `[net]`), read and written with
  `Get/WritePrivateProfile*` as SimCity 2000 does. Command-line flags
  override it. Portable mode: the game's own registry settings move into
  the ini too (the headless profile trick already redirects HKCU).
- **The "Recomp" menu** (in: controller items, edit/reload `hover.ini`)
  grows a submenu per feature below. An ImGui overlay (F1) is the
  alternative for the graphics sliders; decide once the presenter exists.

## 1. Graphics

- **The presenter** is in ([docs/presenter.md](docs/presenter.md)): its own
  window on Direct3D 11, sharp/smooth/nearest/integer, borderless fullscreen,
  CRT (scanlines, grille, curvature, vignette), 16/8-bit dithering, vivid.
  Next: Scale2x/EPX and xBR, SimCity 2000's glow and per-effect sliders
  (an ImGui overlay?), gamma, a de-dither pass, colour-blind-safe flag
  colours, and mouse forwarding (the game's own mouse use is untested).
- **The game's own Full Screen (F4)** is DISPDIB, which modern Windows lacks:
  map it to the presenter's fullscreen.
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

- **Xbox controllers (XInput)**: in ([docs/controller.md](docs/controller.md)).
  Next: try it with a real pad at the console (none reaches an RDP session).
- **Pad settings dialog**: the Recomp menu has deadzone and slot presets;
  a real dialog for remapping each action by pressing the button, trigger
  threshold and invert. The ini already holds all of it.
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

- **Seeds and saved levels** are in ([docs/levels.md](docs/levels.md)):
  pinned seeds, the seed in the title, saved levels and share codes. Next:
  names and best times per saved level, and share codes that carry a
  custom maze once there are custom mazes.
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

The goal: **up to 16 players** in one arena, several on each PC (split
screen, one pad each) and the rest over the internet, **joining and leaving
while the game runs**. Worlds scale up for it, and a map may be empty at
times: robots fill the empty seats, and a player who joins takes one over.

The original is one human against robots, so each step needs the one before:

1. **Determinism audit.** Pin the seed, record inputs, replay, compare frame
   hashes. Find every clock read (`GetTickCount`, `timeGetTime`, the 50 ms
   timer) and every other source of non-determinism.
2. **Find the craft model.** The player's craft and the robots' crafts are
   probably one struct with a controller (human keys / robot AI following
   `BEACON`s). Locate it, its array, and the AI's steering entry point.
   The level table already varies robot counts per level, so the count is
   data, not code.
3. **Humans in robot seats**: replace a robot's AI steering with an input
   source (pad 2, or a network peer). With seats as the unit, drop-in is
   "a human takes over a robot" and drop-out is "the robot takes it back".
4. **More seats**: raise the craft count past what the levels ship (16),
   with the spawn points a bigger map provides (section 4's generator makes
   those maps: more `HUMAN_nn`/`ROBOT_nn` and flag points).
5. **Teams**: the game has two (red/blue flags). 16 players as 8 v 8, or
   free-for-all variants if the flag logic allows it.
6. **Split screen on one PC (2 to 4 views)**: the renderer draws one camera.
   Run it once per view per frame (camera swap, render into separate DIBs,
   composite in the presenter), or run one synchronized process per view.
   The first is better if the renderer's state is re-entrant enough.
7. **Internet play**: an authoritative host (the PC that created the game)
   runs the simulation, and clients send inputs and receive state. With
   drop-in/out and 16 players that fits better than pure lockstep, which
   stalls everyone on the slowest peer. UDP, a join code through a small
   relay, client-side prediction for the local crafts. Snapshots of the
   craft and flag state are the unit, and are also what a joining player
   receives.
8. **Ghost races**: the cheap multiplayer on the way. Record a run (seed +
   inputs), play it back as a translucent craft. Needs 1 and 2 only.

## 6. Mods

- **A mod folder**: `mods\<name>\` holding any of `MAZES\`, `SOUNDS\`,
  textures, level-table overrides and a `mod.ini`, layered over the game's
  own files by the host's file shims (`CreateFileA`, `mmioOpenA`,
  `OpenFile`). Nothing in `game\hover` is ever changed. A Recomp > Mods menu
  to switch, and `--mod NAME`.
- **Doom maps as Hover! mazes** (`tools/wad2hover.py`): a good fit, because
  both are 2D line maps. Doom's linedefs become `CMerlinStatic` walls, sector
  floor and ceiling heights become wall heights, `THINGS` give player and
  item points (`HUMAN_nn`, `POD_RANDOM_nn`, flags), and the textures are
  palette-quantized into a `.TEX`. The converter reads a WAD the user
  supplies (the shareware `DOOM1.WAD` works); nothing from Doom is ever in
  the repo, and the converted mod stays in the user's `mods\`. Heretic,
  Hexen and Freedoom WADs use the same format; Freedoom is free content, so
  a converted Freedoom mod could even ship in releases.
- **Quake / Half-Life maps** as a stretch: `.map` brush files flatten to
  line maps too (hlmapgen writes them), so the generator can target both.
- **Texture and sound packs** (see sections 1 and 3) are mods like any other.
- **Community content**: archive.org has only the game itself
  (`microsoft_hover`, the Windows 95 folder). No fan levels or editors
  turned up, so the level tools here are the first.

## 7. Everything else

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
