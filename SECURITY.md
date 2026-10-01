# Security

## What the program touches

- **Files.** `hover.ini` beside the exe (the port's settings: video,
  controller, levels, multiplayer). The game reads its mazes, textures and
  sounds from `game\hover`.
- **The registry.** The game keeps its own settings under
  `HKEY_CURRENT_USER`, as the original did. `--headless` points
  `HKEY_CURRENT_USER` at a scratch key
  (`Software\hover-recomp\headless\<pid>`) and deletes it on exit.
- **The clipboard.** Level share codes and join codes are copied and pasted
  only when you pick those menu items.
- **Processes.** `--record` starts `ffmpeg` from `PATH`. *Edit settings*
  opens `hover.ini` in its associated editor.
- **The network.** Only during online play: hosting listens on one UDP port
  (7795 unless you pick another), and joining talks to the one address you
  gave. Nothing is sent anywhere else, and the game never connects on its
  own. A join code is the host's IP address and port, written in base 32. It
  is not a secret, so share it only with the people you want in the game.

It handles no credentials.

## Online play

Packets are plain UDP, with no encryption and no authentication. Anyone who
can reach a host's port can join while seats are free, so host on a LAN or
a private network such as Tailscale. If you forward a port to the internet,
anyone who finds it can take a seat. A client accepts packets only from its
host, and the host accepts inputs from each peer only for that peer's own
seats. Every tick and count read off the wire is bounds-checked before it is
used (`src/runtime/net.c`, `recv_thread`).

## How it runs code

The game's code is recompiled C running in-process, and it calls the real
Win32 API with the game's own arguments. Maze and texture files are parsed
by the game's 1995 code, so treat maze and texture files from untrusted sources
with the caution due any old program.

## Reporting

Report a problem privately through GitHub's *Report a vulnerability* on this
repository, rather than in a public issue.
