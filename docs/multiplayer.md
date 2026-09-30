# Multiplayer

Hover! (1995) is one human against robots. This is how the port turns it
into up to four players on one PC today (`src/runtime/mp.c`), and the
research the online play builds on. Addresses are `HOVER.EXE` VAs;
the lifted function for each is `sub_XXXXXXXX`.

## What the engine is

**Objects** (names and sizes from the MFC `CRuntimeClass` records):

| Class | Size | vtable | Constructor | |
|---|---|---|---|---|
| `CGameObj` | 0x84 | 0x4BC380 | 0x40D150 | base of everything in the world |
| `CPlayer` | 0x1FC | 0x4BC910 | 0x41AC30 | a craft: physics, pods carried, flags carried |
| `CHumanPlayer` | 0x1FC | 0x4BCBE0 | 0x421FA0 | the one human, embedded in the document |
| `CRobotPlayer` | 0x688 | 0x4BCCE0 | 0x4220A0 | a robot: seven AI state objects at +0x264..+0x5FC |

The document (`CBumperDoc`) is `[0x00460970]`. The human is **inside** it at
`doc+0x818C`; nothing else points at the human, so the camera, the HUD and
the robots' chasing all compute that address. `doc+0x83A8` is a `CObList` of
everything that thinks (the human first, then hunters, then flag-runners,
then pods while their effect runs), `doc+0x83E4` the human's score and
`doc+0x83E8` the robots' flag count. All dynamic lists: 16 robots ran to a
normal ending. The ceiling is start spots: the start picker (0x417D40)
retries a random `ROBOT_nn` forever while something is within 0x50, and
MAZE1/2/3 have 17/18/17.

`CPlayer` fields that matter here:

| Offset | | Offset | |
|---|---|---|---|
| +0x44/+0x48 | x/y (16.16) | +0x19C | heading (512 a turn, 16.16) |
| +0x1AC/+0x1B0 | velocity | +0x1F0 | **turn** input, -1.0..+1.0 |
| +0x1F4 | **thrust** input | +0x1F8 | is human |
| +0x14C | spin-out timer (-1 = none) | +0xB4/+0xC0 | flags carried |

Teams go by class: `CFlag`'s pickup check (0x414470) gives robot flags only
to `CHumanPlayer` and human flags only to `CRobotPlayer`. `CRobotPlayer+0x21C`
is a personality (1 hunter, 0 flag-runner), not a team.

**The tick.** A 50 ms `timeSetEvent` (set at 0x412804, callback 0x408AA0)
posts `WM_USER` to the view. Its handler 0x408AF0 polls the keyboard into the
view's steer flags, then 0x408D00 runs the state machine on `[0x00460728]`
(2 countdown, 3 playing, 4-5 level end) and 0x427D40 the tick: every
thinker's `Think` (vtable slot 7), collisions (0x40A650), and the hand-off to
the render thread (0x40DC20). The human's Think (0x409180) turns the steer
flags into turn/thrust; a robot's (0x409300) runs its AI state, then
0x409350 steers toward its target and writes fractional turn/thrust. Both
then call `CPlayer::Think` (0x409110), the shared physics.

**Determinism.** The step is fixed: once per tick, never scaled by a clock.
The simulation, input, the loader and the AI's `rand` all run on the UI
thread; the render thread only writes the view and its DIBs. The clock reads
are the level seed (`time()`, pinned by the host), a 500 ms sound throttle
and frame-rate counters, none of which the world reads. With the seed pinned
and keys applied by tick, separate runs (two of them at the same time
included) stayed byte-identical in world and entity state through 610 ticks;
moving one key by one tick diverged by tick 90. Slow machines drop ticks
(flag `[0x004C4CE4]`) rather than catch up, and every peer needs the same exe
(floating-point division goes through the CRT's FDIV check, 0x43EC07).

**The renderer** runs on its own thread: command 2 of its loop (0x41EE51) is
the frame draw, 0x00402250, a thiscall with no stack arguments. It reads the
camera from the human (heading `+0x88`, the craft itself as the position
source, eye height `+0x8C`), sets camera `[view+0xC8]` with 0x406C00 and
renders with 0x406090. The rear-view mirror is a second camera
(`[view+0xCC]`) drawn in the same call, so the renderer is already called
twice a frame; its state is statics (`0x4A44xx`), so passes must stay on the
one thread, one after another. The radar (0x407160), speed (0x405A10),
powerup gauges and score read the human and the one view. Six built-in
layouts (320x240 to 700x525, each with its own dashboard art in DASHRES,
table at 0x4609A0) are chosen by the window size.

## Split screen (in)

*Recomp > Multiplayer*, `[mp] players=` in `hover.ini`, or `--players N`:

- **Seats.** Player 1 is the game's human (keyboard, and controller 1).
  Players 2-4 each get a robot of their own: every level's table entry
  (0x004C4000, 0x58 a level, flag-runners at +0x2C) gains one flag-runner per
  extra player, so the level keeps the AI it was designed with, and the seats
  are the last robots the loader creates. `CRobotPlayer::Think` is reached
  only through its virtual call, so `recomp_lookup_manual` hands it to
  `seat_think`, which writes the seat's controls into turn/thrust and runs the
  physics; every other robot runs its AI.
- **Controls.** Controller 2-4 for players 2-4 (analogue: stick or triggers
  for thrust, stick or d-pad to turn); player 2 also has I/J/K/L on the
  keyboard.
- **Views.** `run_lift.py` patches the frame draw's three camera reads to ask
  `hover_cam_obj()`, and its call on the render thread (0x0041F012) to
  `hover_render_views()`, which draws once per player, player 1 last, from
  the same registers each time. host.c puts each pass's blits in that
  player's cell of the capture buffer (2 side by side, 3-4 two by two), and
  blits outside a pass (the dashboard's background, painted in `WM_PAINT`)
  in every cell. The presenter fits the bigger picture to the monitor.
- Players in robot seats play for the robots, so two players is one against
  the other, with each side's AI.

**Not yet:** the HUD in players 2-4's views is player 1's (radar, flags,
score, powerups: they all read the one human and the one view); robots
cannot pick up powerups, so players 2-4 have none; the human craft has no
sprite, so players 2-4 cannot see player 1 (robots carry sprite objects at
`+0x220/+0x264/+0x2F4`); sounds play only for the human. A change of player
count takes effect from the next level load.

Checked headless: 2 and 4 players on a pinned seed, each view distinct and
following its own craft (docs: the conformance run has a 2-player run), and
windowed at the console (1824x685 for two players on a 1920x1080 monitor).

## Online play (in)

`src/runtime/net.c`. Lockstep: every PC runs the same world from the same
inputs, and only the inputs cross the network.

**Starting one.** The host chooses *Recomp > Multiplayer > Host an online
game* (or `--host [PORT]`, default UDP 7795, `--clients N` for how many PCs
to wait for); each other PC copies the host's address and chooses *Join*
(or `--join HOST[:PORT]`). A PC brings as many local players as its split
screen is set to (`--local N`), up to 4; the host's first player is seat 1
(the game's own hovercraft), every other player a robot seat. When the last
PC joins, every PC starts the same new game: the host's *Start At* level on
the host's seed.

**How it stays in step.**

- *The gate.* Online, the game's 50 ms timer is the host's: `timeSetEvent`
  for the tick callback (0x00408AA0) is taken over, and a driver thread
  calls the game's callback for tick N only once tick N-1 has been handled
  (the flag at `[0x004C4CE4]` is clear again) and every seat's input for
  tick N has arrived. It stalls rather than skips, and never bursts to catch
  up. Tick 0 is the first tick of the session's first real level: the
  attract demo, which may still be running when the session starts, keeps
  its own timer.
- *Inputs.* Between ticks each PC samples its players for tick N+3 (turn,
  thrust, jump/wall/cloak) and sends them; every packet repeats the last 8
  ticks, so a lost packet costs nothing. Clients send to the host, and the
  host relays every seat to every client, also while it waits. Seat 1's
  steering reaches the game through the key shims from the tick's record,
  its jump/wall/cloak are posted to the view on the tick they are pressed,
  and robot seats read the record in `seat_think`. While online, nothing
  else of the keyboard or pads reaches the game: the presenter, the pad
  thread and scripted keys only feed the sampler. The joystick flag
  (`[0x0046070C]`) is cleared each tick, because the tick handler would
  otherwise poll a local joystick.
- *The desync check.* Every 20 ticks each PC hashes every thinker (vtable,
  position, heading, turn, thrust); the host compares against every client
  and forwards its hash so every client compares too. A mismatch stops the
  game on every PC and names the tick.

**Checked:** two instances on one PC over localhost, both driving: in sync
through tick 400, headless and with two presenter windows. A deliberate
one-unit nudge of one PC's craft at tick 100 (`--net-desync-test 100`) is
reported at tick 120 on both. Both are conformance milestones.

**Not yet:**

- Leaving: a PC that quits stalls everyone at "waiting for the other
  players"; restart to leave. Drop-in/out is the next step (a seat nobody
  holds reverts to a robot at an agreed tick; a joiner replays the inputs).
- Addresses: direct IP only (a LAN, Tailscale, or UDP port 7795 forwarded to
  the host). A relay with join codes would remove the port forwarding.
- The game's own menu (*Start Game*, *Pause*) still works with the mouse on
  one PC and would desync the session: leave it alone while online.
- Everything the split screen lacks (players 2+'s HUD, powerups, player 1's
  missing sprite) applies online too.
- Every PC needs the same build and the same game files.
