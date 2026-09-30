# Changelog

All notable changes to this project. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow [SemVer](https://semver.org/).

## [Unreleased]

### Removed
- The "not running a 256 color video driver" message at every start. Only
  the startup check is answered; rendering is unchanged.

### Added
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
