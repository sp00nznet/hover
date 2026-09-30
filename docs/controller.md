# Controllers and hover.ini

`src/runtime/pad.c`. Any XInput pad (Xbox 360, One, Series, and most modern
pads in XInput mode) drives the hovercraft. Nothing in the game changes: the
pad presses the game's own keys.

## How it works

Hover! steers by polling `GetAsyncKeyState` for the keys in its key table,
from its `WM_USER` input handler (`0x00408AF0`) every 50 ms, and takes its
menu keys (F2 new game, F3 pause) as `WM_KEYDOWN` through the accelerator
table. The table, as read from a running game:

| Address | Default | Action |
|---|---|---|
| `0x004606EC` | Up | forward |
| `0x004606F0` | Down | reverse |
| `0x004606F4` | Left | left |
| `0x004606F8` | Right | right |
| `0x004606FC` | Home | forward + left |
| `0x00460700` | PgUp | forward + right |
| `0x00460704` | End | reverse + left |
| `0x00460708` | PgDn | reverse + right |
| `0x004606E8` | A | jump (spring) |
| `0x004606E4` | S | wall (bricks) |
| `0x004606E0` | D | cloak |
| `0x0046070C` | 0 | joystick on (Player Controls) |

A host thread polls XInput every 8 ms. For each action it looks up the key
the game has bound to it **at that moment**, reports that key held through
the host's `GetAsyncKeyState`/`GetKeyState` shims, and posts `WM_KEYDOWN` /
`WM_KEYUP` on press and release, as a real key would. So:

- Rebinding keys in *Options > Player Controls > Set Keys* moves the pad
  with them (a key rebound while its button is held is released and the new
  one pressed).
- Diagonals need no binding of their own: stick up-left holds forward and
  left, which the game treats exactly like its forward + left key.
- The poll is on its own thread, not in the key shim, because while the game
  is paused nothing polls the keyboard, and Start still has to unpause it.
- Headless runs never start it: a recording must not be steered by a pad
  someone is holding.

The game's own joystick path (`joyGetPosEx`, *Player Controls > Joystick*)
is untouched and still works with anything Windows lists as a joystick.

## Default layout

| Pad | Action |
|---|---|
| RT, left stick up, d-pad up | forward |
| LT, left stick down, d-pad down | reverse |
| left stick / d-pad left and right | turn |
| A | jump |
| X | wall |
| B | cloak |
| Start | pause (F3) |
| Back | new game (F2) |

## hover.ini

Beside the exe (`build\hover.ini`), written with every default on first run,
so the file always lists everything there is to change:

```ini
[pad]
enabled=1
controller=0          ; 0 = first connected, 1-4 = that slot
deadzone=30           ; percent of stick travel
trigger=20            ; percent of trigger travel
forward=RT,LSTICK_UP,DPAD_UP
reverse=LT,LSTICK_DOWN,DPAD_DOWN
left=LSTICK_LEFT,DPAD_LEFT
right=LSTICK_RIGHT,DPAD_RIGHT
jump=A
wall=X
cloak=B
pause=START
new_game=BACK
```

Inputs: `A B X Y LB RB LT RT START BACK LS RS DPAD_UP DPAD_DOWN DPAD_LEFT
DPAD_RIGHT`, and `LSTICK_*` / `RSTICK_*` with `UP DOWN LEFT RIGHT`. Several
per action, comma-separated. An unknown name is reported on stderr and
ignored.

## The Recomp menu

The frame's menu bar gains **Recomp**: the controller's status, *Use Xbox
controller*, *Stick deadzone* (15/30/45%), *Which controller* (any, 1-4),
*Edit settings (hover.ini)...* and *Reload settings*. Changes are written to
`hover.ini` at once.

MFC greys every menu item it has no command handler for when a popup opens,
and drops `WM_COMMAND`s it does not know. The host's subclass of the frame
(`host_wndproc` in `host.c`) therefore takes the menu's commands before MFC,
and re-enables and re-checks the items after MFC's `WM_INITMENUPOPUP`.

## Checking it without a pad

`build\hover.exe --pad-selftest` feeds synthetic pad states through the same
code the poll thread runs (`pad_step`): triggers and stick past and inside the
deadzone, a key rebound while held, buttons, unplugging, and the ini parser.
The conformance harness runs it. Over RDP no pad reaches the session (XInput
is not redirected), so this is also how it is checked remotely.
