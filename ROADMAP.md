# Roadmap

## Next

- **Merge the toolkit fix** ([pcrecomp#24](https://github.com/sp00nznet/pcrecomp/pull/24)) and drop the branch pin from `Setup.cmd` and the
  README.
- **Win a level.** Every recorded run so far ended with the opponents taking
  the red flags. A scripted route that collects the blue flags would reach
  level 2's load, the "Enter High Score" dialog and the score screen, each a
  conformance milestone.
- **Levels 2 and 3** (`MAZE2`/`MAZE3`, their own textures and music).
- **Sound in the recording.** `waveOut` and MIDI play on the real devices,
  headless included. A headless run should mute them, or better, put the
  mix in the mp4.
- **Keys timed in frames.** `--key` is in milliseconds from entry, so a much
  slower machine could press UP before level 1 has loaded. A "wait for this
  frame" form would remove the guess.

## Deferred

- **Driving dialogs headless.** Every modal dialog answers OK unseen (Quick
  Help is the only one at startup). Keyboard Settings, Sound Options and the
  High Score entry would need the real dialog, cloaked, with scripted buttons.
- **GDI drawn straight onto the window** (text or lines rather than blits)
  is not in recordings. Nothing seen in level 1 needs it.
- **The four `fnstenv`/`fldenv`** in the CRT's floating-point exception
  helpers lift as no-ops, and three garbage decodes (`sti`, `fisttp`) sit in
  data the catalog took for code. No run has been seen to reach them.
- **Joystick.** `joyGetPos` goes to the real API; untested.

## Out of scope

- Redistributing anything from the game, including the recompiled C.
- `DISPDIB.DLL` full-screen mode (Windows 95's low-resolution display
  driver). No run has loaded it; the game plays in its window.
- `WMCONFIG.EXE` (the WaveMix configuration tool) and the WinHelp file.
