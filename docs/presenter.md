# The presenter

`src/runtime/present.c`. Hover!'s picture in a window of the host's own,
scaled on the GPU: any window size, borderless fullscreen, filters, a CRT
look and retro dithering. It is the default; `--classic` (or `presenter=0`
in `hover.ini`) shows the game in its original window instead.

## Why its own window

The obvious design, making the game's window resizable and stretching into
it, does not survive this game. Its `WM_SIZE` handler (`0x0041DCE0`) snaps
the frame back to the size it wants (516x388 client, computed with
`AdjustWindowRectEx`), and on the way it handshakes with its render thread
to rebuild the view. Even removing the menu bar from the frame, which only
changes the client height, made that handler rebuild the renderer mid-load,
and the render thread faulted in `sub_00406090`.

So the game keeps its frame exactly as it was, cloaked as in headless runs
([host.md](host.md)), and the presenter is a second top-level window on its
own thread:

- **Picture.** host.c already mirrors every blit onto a 32-bit shadow of the
  frame's client area (for `--record`). The presenter copies the shadow under
  a lock after each blit and draws it with Direct3D 11 (WARP when there is no
  GPU; one GDI `StretchDIBits` when there is no Direct3D at all).
- **Menu.** The presenter's menu bar is a new bar holding the frame's own
  popups: the game and MFC still find their menu on the frame, and
  `WM_INITMENUPOPUP` / `WM_INITMENU` are sent on to the frame so MFC enables,
  greys and checks the game's items and host.c does the Recomp items.
  `WM_COMMAND` goes to the frame too, except the Video items, which the
  presenter handles itself.
- **Keys.** `WM_KEYDOWN`/`WM_KEYUP` are posted to the frame, where the game's
  accelerator table sees them (F2, F3, F8...). Steering reads
  `GetAsyncKeyState`, which host.c answers from the real keyboard only while
  the presenter has the focus, so keys typed into another program never
  steer the hovercraft. Alt+letter opens the menus as in any window.
- **Focus.** The frame never has the focus, so host.c drops every
  deactivation it would get, and the presenter forwards the real ones (its
  own `WM_ACTIVATEAPP`, marked `PRESENT_REAL_ACTIVATION`). The game then
  does what the original did: switching away pauses it (its handler,
  `0x00415300`, calls the pause routine), and it stays paused until you
  press F3 or Start. *Recomp > Video > Pause when in the background* turns
  that off.
- **Dialogs** centre on their owner, the cloaked frame, so the presenter
  keeps the frame centred on itself.
- **Closing** the presenter closes the frame, and the game asks, saves and
  exits as it always did.

## Picture options

*Recomp > Video*, or `[video]` in `hover.ini`:

| Option | Values | |
|---|---|---|
| `filter` | `sharp` (default), `smooth`, `nearest`, `integer` | sharp-bilinear keeps texels crisp and blends only their edges (gunman's shader); integer scales by whole multiples only |
| `scale` | 1-8 | window size, times 516x388 |
| `fullscreen` | 0/1 | borderless on the window's monitor: F11 or Alt+Enter, Esc leaves |
| `crt` | 0/1 | scanlines (fading in from 1.5x to 3x, where they stop beating against the pixel grid), an aperture-grille mask, vignette |
| `curvature` | 0/1 | the CRT's barrel |
| `dither` | `off`, `16bit`, `8bit` | ordered (Bayer 4x4) dithering per game pixel: a High Color desktop, or the 216-colour web palette |
| `vivid` | 0/1 | more saturation and a little contrast |
| `pause_in_background` | 0/1 | the original's pause when the window loses the focus |
| `presenter` | 0/1 | 0 = the original window (`--classic`) |

The screenshots in `docs/img/` come from `--record`, which is the unscaled
game; the presenter's output is what you see on screen.

## Checked

Windowed runs at the console, captured with `PrintWindow`
(`PW_RENDERFULLCONTENT`): 2x sharp, CRT, 8-bit dither + vivid, F11 to
1920x1080 and Esc back to the same rectangle, the 1x/3x size commands
(516x388 and 1548x1164 client), the four menus with MFC greying only the
separators, and the pause on deactivation.
