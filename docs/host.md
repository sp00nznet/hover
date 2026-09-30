# The host

`src/runtime/host.c`. The mechanism (the native bridge, callbacks, the machine
lock) is pcrecomp's `runtime/native32`; what is here is what this game needs
on top of it. Each item was found by running the game and reading where it
stopped.

## Shims: the game's view of the world

| Import | Why |
|---|---|
| `GetModuleHandleA(NULL)`, `GetModuleFileNameA`, `GetCommandLineA` | The guest is `HOVER.EXE` in `game\hover`, not the host: its hInstance (resources, window classes) must be `0x00400000` |
| `GetDeviceCaps` | Only for the startup display check (`0x0041CEE3`): it is told the screen is a 256-colour palette device, so the "not running a 256 color video driver" box is gone. The renderer's own calls see the real display; it draws into its own 8-bit DIBs either way |
| `GetLocalTime`, `GetTimeZoneInformation` | Level seeds: `time()` returns exactly the seed `levels.c` picks, so `srand` gets it ([levels.md](levels.md)) |
| `LoadLibraryA("dashres.dll")` | Loaded with `LOAD_LIBRARY_AS_DATAFILE`: the dashboard bitmaps load as before and the DLL's original `DllMain` never runs |
| `GetAsyncKeyState`, `GetKeyState` | Scripted keys (`--key`). Headless, the real keyboard is not read at all, so typing on the machine cannot steer a recording |
| `BitBlt`, `StretchBlt` | Capture: see below |
| `CreateWindowExA` | Remembers the frame window; headless, cloaks it (below) |
| `ExitProcess` | Closes the `--record` pipe first, so the mp4 is complete |

With the frame cloaked (headless, or the presenter showing the game) the
host adds `ShowWindow` (never activates), `SetForegroundWindow` and
`SetActiveWindow` (no-ops). Headless adds `MessageBoxA` (to stderr; none appear in a normal run now), `DialogBoxParamA` (answered OK
unseen; the only one at startup is Quick Help), `ShowWindow` (never
activates), `SetForegroundWindow` and `SetActiveWindow` (no-ops).

## Capture

The game draws its frame as separate blits onto the window DC: the 512-wide
3D view, then each dashboard piece. Every `BitBlt`/`StretchBlt` whose
destination is the game window (`WindowFromDC`) is repeated onto a 32-bit
shadow DIB of the client area (516x388), and `--record` pipes that shadow to
ffmpeg at a fixed 30 fps against the wall clock. A frame, for `--frames` and
`--diff`, is a blit of at least 256x128: the 3D view. Windowed runs capture
the same way, so both modes record identically.

## Headless: a cloaked window, not a hidden one

This took four tries, and each failure is instructive.

1. **Hidden window** (`WS_VISIBLE` stripped, `ShowWindow` a no-op). The game
   ran, but MFC's idle loop, which is the game's frame loop, asks
   `IsWindowVisible`, so that had to lie too. Worse, Windows never generates
   `WM_PAINT` for an invisible window, and the dashboard art is drawn only
   in `WM_PAINT`: recordings had a black dashboard.
2. **Faked paints into the hidden window.** `RDW_INTERNALPAINT` does nothing
   for an invisible window. Posting `WM_PAINT` to the frame and its view did
   paint, but the game's paint of one window invalidates the other, so the
   requests ping-ponged forever and starved the idle loop. A `WM_NULL` after
   each (MFC does not count `WM_PAINT` as an "idle message", so a paint alone
   parks the loop in `GetMessage`) did not save it.
3. **A visible window, cloaked** (`DWMWA_CLOAK`). DWM composes it nowhere,
   so nothing reaches any screen, RDP included; to USER32 it is an ordinary
   visible window, so it gets real `WM_PAINT` and needs no lies. The first
   run deadlocked: see the machine lock, next.
4. **Cloaked, and never told it lost focus.** Runs parked at random, about
   one in three. See activation, below.

The frame is created `WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE` and shown
`SW_SHOWNOACTIVATE`: off the taskbar, and never taking the keyboard.

## The machine lock and shims that call back

A shim runs inside the lifted model, holding native32's machine lock. That
is fine for a shim that only computes, but `ShowWindow` and `CreateWindowExA`
run the game's own window procedure inside them. The game's `WM_SIZE`
handler (`0x0041DCE0`) waits there, polling `PeekMessage` and
`WaitForSingleObject`, for its render thread to acknowledge a resize, and the
render thread needs the lock to run. The callback's own native calls release
one level of a nested lock, not the one the shim held, so both threads waited
forever. The watchdog's thread dump showed it:

```
thread 32128: ... native32.c:245 (bridge call)  <- sub_00427A10 <- sub_004274B0 <- sub_0041F480 <- sub_0041DCE0
thread 69380: ... native32.c:91 (mach_enter: EnterCriticalSection)
```

So those two shims copy their arguments, `mach_leave()`, call Windows, and
`mach_enter()` again. Any shim that can run guest code inside Windows must.

## Activation: the game pauses in the background

The original stops when it is not the active application, like most games of
its time. Its frame loop is paced by a 50 ms `timeSetEvent` callback
(`0x00408AA0`) that posts `WM_USER` to the frame (only while a flag says the
last one was handled); `WM_ACTIVATEAPP(FALSE)` kills that timer, and with no
messages coming, MFC's idle loop parks in `GetMessage`:

```
[callback] sub_0044C4CB (006C037E 0000001C 00000000 00019CC0)     <- WM_ACTIVATEAPP, deactivating
[native] WINMM.dll!timeKillEvent (00000010 ...) from sub_00410280
[native] USER32.dll!GetMessageA (004605A0 ...) from sub_0044B3BC  <- and there it stayed
```

A cloaked window is still part of activation: whenever the desktop's
foreground moved (a person using the machine, another process starting), a
run could stop. Headless therefore subclasses the game's top-level windows
in the host (`always_active`) and drops `WM_ACTIVATEAPP(FALSE)`,
`WM_ACTIVATE(WA_INACTIVE)` and `WM_NCACTIVATE(FALSE)` before the game sees
them. Eight runs in a row then reached 1,000 frames. Windowed runs keep the
original behaviour: click away and the game pauses.

## A fresh profile per headless run

The game keeps Quick Help's "skip" flag, its settings and its high scores in
`HKCU\Software\Microsoft\Hover!`. A headless run points `HKEY_CURRENT_USER`
at `HKCU\Software\hover-recomp\headless` (`RegOverridePredefKey`), emptied at
every start, so a recording neither depends on nor changes what a player
chose. That is also why the conformance run always sees Quick Help.

## Scripted keys

`--key NAME@MS[+HOLD]`, in milliseconds from entry: names `UP DOWN LEFT
RIGHT SPACE ENTER ESC SHIFT CTRL TAB F1-F12`, a single letter or digit, or a
VK code. The menu keys (F2 starts a game, F3 pauses) arrive as `WM_KEYDOWN`
through the accelerator table, and steering is polled with
`GetAsyncKeyState` from the `WM_USER` handler (`0x00408AF0`), so a scripted
key is posted down and up and reported held in between. Scripted keys are
timed in milliseconds, not frames, because the menu before F2 presents only
a handful of frames.

## Diagnostics

- `--watchdog S` prints every thread's `eip` and the host return addresses
  on its stack, then the last indirect calls. `py -3 tools/addr2line.py ADDR
  ...` turns host addresses into lines of generated C (from `build\hover.pdb`,
  through dbghelp).
- `--native-trace` (every Windows call, with string arguments) and
  `--callbacks` (every Windows → guest entry) are native32's.
- The `recomp_trace` options (`--calltrace`, `--firsthit`, `--poison`, ...)
  need a `-DHOVER_TRACE=ON` build.
