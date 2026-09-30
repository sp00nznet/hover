# Architecture

## The game

Microsoft Hover! (1995) is one PE image and two resource DLLs, built with
MSVC 2.x (linker 2.55) and statically linked MFC 3. No packer, no copy
protection.

| File | Base | What it is |
|---|---|---|
| `HOVER.EXE` | `0x00400000` | The whole game: 377 KB of code, 5,090 functions. An MFC frame and view, a software 3D renderer on its own thread drawing into 8-bit DIB sections, WaveMix-style mixing through `waveOut`, MIDI music (`.MUZ`), 350 imports from 9 system DLLs and nothing else |
| `DASHRES.DLL` | `0x10000000` | The dashboard art as bitmap resources. Its only code is the CRT's `DllMain` |
| `HOVERHLP.DLL` | `0x10000000` | Help resources; the game never loads it |

`HOVER.EXE` reads `mazes\*.MAZ` / `*.TEX` and `Sounds\...` relative to the
working directory, and keeps its settings and high scores under
`HKCU\Software\Microsoft\Hover!`.

## The pipeline

```
game\hover\HOVER.EXE ─ tools\catalog.py ─ work\functions_hover.json ─ run_lift.py ─┬─ src\recomp\gen\recomp_00NN.c
                         (pcrecomp disasm32)                          (pcrecomp    ├─ recomp_dispatch.c
                                                                       lift32)     └─ recomp_funcs.h
                                                                                          │
src\runtime\host.c + pcrecomp runtime\native32 ───────────────────────────── build.cmd ──┴─ build\hover.exe
```

1. **`tools/gamefiles.py`** copies the game folder into `game\hover`, from a
   drive (the Windows 95 CD keeps it in `FUNSTUFF\HOVER`), a folder, a zip
   (zips inside zips too) or a BIN/CUE/ISO (pcrecomp's `iso_peek` walker).
2. **`tools/catalog.py`** runs pcrecomp `disasm32`: the entry point is the
   seed, the rest is recursive descent, prologues and a data scan (5,090
   functions, 96.6% of the code bytes).
3. **`run_lift.py`** lifts every catalogued function with pcrecomp
   `generate` + `lift32` into one dispatch table. A direct branch to an
   address the catalog lacks becomes an entry of the next round (45 of them),
   so every `RECOMP_ITAIL` resolves: 5,135 functions, 0 errors.
4. **`build.cmd`** compiles the generated C, the host and pcrecomp's
   `native32`, `image_loader` and `recomp_trace` into one 32-bit executable,
   linked at `0x60000000` so the guest's `0x00400000` is free.

## At run time

`build\hover.exe` is a 32-bit process holding `HOVER.EXE` at its own base,
mapped by hand, and running only lifted code (the guest `.text` is mapped
non-executable).

- **Guest → Windows.** Lifted `call [iat]` reaches native32's bridge, which
  calls the real function with the guest's stack slots. 338 of the 350
  imports go straight to Windows; 12 are shimmed in the host (17 headless).
- **Windows → guest.** MFC's window procedure, the dialog procedure and the
  50 ms multimedia-timer callback fault on the non-executable guest code, and
  native32's handler enters the lifted function.
- **Threads.** The renderer runs on its own guest thread, handshaking with
  the UI thread through events. native32's machine lock hands the register
  file between them around every native call.
- **DASHRES.DLL** is loaded as a data file, so none of its x86 runs.

Everything the host changes about the game's view of the world is in
`src/runtime/host.c`, and why is in [host.md](host.md).
