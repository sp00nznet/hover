# QA: hover @ 4d563d2 (v0.1.0-alpha.2 build), 2026-10-21, on linuxbox (Debian 13, Wine 10) and testbox (Windows)

**Would a stranger get through it?** Online with help: two real PCs (Windows and Linux, either side hosting) stay in sync. But a Linux host who clicks another window ends the match for everyone, and can't resume.

**Claims:**
| Claim (where it's made) | What I did | Result |
|---|---|---|
| Online, lockstep over UDP, direct connection (README) | Linux (Wine) host on linuxbox, Windows client on testbox behind the lab NAT, 7 rounds | 4/4 in sync at tick 400 after the first 3 rounds failed to connect (see minor 4) |
| The same, with a forwarded port | Windows host on testbox (UDP 7795 forwarded on the lab router), Linux client, 4 rounds | PASS: 4/4 in sync at tick 400 |
| Conformance online milestones under Wine (CHANGELOG "20 of 22"; ROADMAP: "the client's Quick Help keeps it quiet") | The first online game on its own, 8 rounds sharing one wineserver and 8 with the client in its own Wine prefix | 3/8 vs **8/8**. The cause is the one in major 1, not Quick Help |
| Linux play is `Hover!.sh` (README) | Played as the host from `Hover!.sh --host`, gave the focus to another window at tick ~250 | FAIL: major 1 |

**Bugs**, worst first:
### [major] Under Wine, the host losing focus ends an online match for everyone
- Steps: linuxbox, `~/a2/root/hover`. Start the host with `./Hover!.sh --host 7795 --clients 1 --seed 12345` (or `qa-online/linuxbox/whost.sh`) and click OK on Quick Help. On testbox, join with `hover.exe --headless --run --join 192.168.100.224:7795`. About 12 s in, activate any other X window (`xmessage x & xdotool windowactivate`).
- Expected: the match goes on; online, the game's pause is turned off (docs/multiplayer.md: Pause is greyed while online).
- Actual: the host shows "GAME PAUSED / Activate Hover! and Press F3 to Continue". Its ticks stop at about 251 and it sends nothing. The client logs `waiting for the other players at tick 251`, then 8 s later `the host went quiet: the game is over`. F3 is dropped while online, so the host can't resume.
- Evidence: `qa-online/alt3-before.png`, `alt3-after.png`, `linuxbox/alt3-host.log`, `testbox/xw-alt3-join.log`.
- Why: the original's WM_ACTIVATEAPP(FALSE) path kills its 50 ms timer (`timeKillEvent` in sub_00410280, docs/host.md), and `net_timer_kill` sets `g_timer_on = 0`. The driver then sits in the `!g_timer_on` branch (net.c:590), which neither sends nor logs. `host_wndproc` (host.c:231) drops deactivation only when `g_hidden`. `--classic` isn't hidden, and the presenter, which keeps it out while online (present.c:418), doesn't run under Wine. A Linux client that loses focus presumably stalls the same way (the host gives its seat to the AI after 4 s). I didn't test that.

### [major] One-PC online conformance under Wine: same cause, not Quick Help (17/18 failing rounds)
- Steps: `DISPLAY=:0 python3 ~/qa-net/one.py out.log` on linuxbox (`qa-online/linuxbox/one.py`, the first conformance online game with timestamps). Then the same with `CPFX=<a copy of ~/.wine>`, which gives the client its own prefix.
- Expected: in sync at tick 400, as on Windows (16/16).
- Actual: with one wineserver, 3/8 clean. With a separate prefix for the client, 8/8 clean.
- First divergence (WINEDEBUG=+message on the host; failing `m2.log` vs passing `m1.log`): at 1.881 s, while the host loads level 1, its **"Please Wait" loading dialog** gets `WM_ACTIVATE(0)`, then `WM_ACTIVATEAPP(0, lp=0x130)`. 0x130 is the client's main thread, so activation moved to the other process. MFC then sends `WM_ACTIVATETOPLEVEL(0)` (0x36E) to the frame. In the passing run, the host loses activation at 1.200 s, before its level loads, and gets it back at 1.29 s.
- Why: Wine has one foreground per wineserver and no foreground lock, so the client's windows take activation from the host. The "Please Wait" dialog isn't created through the `CreateWindowExA` shim, so it isn't subclassed, and 0x36E isn't filtered, so the deactivation reaches the game's pause. The host's driver goes silent at tick 1: no "waiting for" or "not finished" lines, which matches the `!g_timer_on` branch. The client stalls at tick 4, logs "host went quiet" at +8 s, and the host logs "seat 2-2 went quiet" 4 s later. `[headless] dialog 115 answered OK unseen` is only the next line printed (the shim returns at once); it isn't the cause. Tracing with `--callbacks` or `--native-trace` slows the run enough that the race doesn't happen (16/16 clean with `--callbacks`).
- Real players on separate PCs have separate wineservers, so this hits the one-PC test and two copies on one Linux desktop, not a normal match. Both majors close if the pause can't run while online.

### [minor] A passing online round takes exactly 452 s: every game waits out a watchdog
- On testbox, `qa-online/testbox/qa-three.py` timed the three games at 151.1 + 151.1 + 150.0 s.
  - online: the host stops at 520 frames and says BYE. The client's game stops, so its frame count never reaches 520, and it ends on its 150 s watchdog (exit 4) on Windows and Wine alike.
  - desync: after DESYNC both sides stop, and neither exits until its watchdog.
  - late: the client ends at 900 frames, and the host then sits until 150 s.
- So the online games always end on `TerminateProcess(4)`, and about 350 s of every conformance run is idle.

### [minor] A windowed `--host` with Quick Help open drops the joiner after 8 s
- `Hover!.sh --host 7795 --clients 1`, leave Quick Help up, join from another PC: the client gets `game on`, then `the host went quiet: the game is over` 8 s later. The host stays in `DialogBoxParamA` with no level (`linuxbox/alt-host.log`, `testbox/xw-alt-join.log`, `alt-0.png`). The cause is the same silence: a host that isn't ticking sends nothing, so any host-side wait over 8 s ends the session.

### [minor] Cross-machine join failed 3 times, then worked 7 times
- Rounds 1-3 (Linux host, Windows client): the client sent HELLO every 250 ms for 45 s, and the host never logged `joined` (`xl-host1..3.log`). After a plain UDP probe and with tcpdump running, rounds 4-7 joined at once. I didn't find the cause, and I haven't reproduced it since.

**Confusing moments:** under Wine the host's pause screen says "Press F3 to Continue", but F3 does nothing online. A command-line host shows Quick Help first, and nothing says a joiner is waiting on it.

**Not tested, and why:**
- A Linux *client* losing focus (inferred from the code, not run).
- Windows `--classic` online with focus loss: the presenter path is the Windows default, and testbox's ssh session has no interactive desktop.
- More than 2 players, a late join across machines, Linux behind the NAT (linuxbox is on the LAN).
- No fix was attempted: report only, as asked.

Evidence (LAN only, not in the repo): `F:/projects/tools/recomp-netlab/local/qa/hover-a2/qa-online/`.
