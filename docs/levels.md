# Level seeds

`src/runtime/levels.c`, and the `GetLocalTime` / `GetTimeZoneInformation`
shims in `host.c`.

## A level is a maze plus a seed

The walls of a level are fixed in its `.MAZ` ([maz-format.md](maz-format.md)).
Everything else is placed when the level loads, from `srand(time(NULL))`
(`0x004154DC`): the player's start, the robots' starts, all 28 pods (four of
each of seven kinds), and both teams' flags. Pin the seed and the level
comes back exactly; change it and it is a different game on the same walls.

## Pinning it

`time()` (`0x0043E9C2`, the CRT's) reads the clock through `GetLocalTime`
and nothing else. The host's `GetLocalTime` asks `levels_seed()` for a seed
and returns the moment that many seconds after 1970, and its
`GetTimeZoneInformation` says UTC, so `time()` returns the seed itself and
`srand` gets it unchanged. An unpinned game gets the real time, as the
original did, and that value is recorded as its seed, so any game can be
saved or shared after the fact.

Checked: two headless runs with `--seed 12345` give the same frame 40,
pixel for pixel; `--seed 99999` gives a different start (51% of the window
differs). The conformance run pins 12345.

The attract-mode demo (`small.maz`, level table entry 20) is seeded too, but
it is not a level to keep: the menu ignores it.

## Using it

*Recomp > Level*:

- the current level and seed, and its **share code**, e.g. `L3-1789123456`
  (level 3, seed 1789123456);
- **Pin this seed**: every level load uses it until you choose **Random seed
  every level** (the default);
- **Save this level** into **Saved levels** (up to 20, newest last);
  choosing a saved level starts a new game on it;
- **Copy share code**, and **Play share code from clipboard**.

Starting a saved level borrows the game's *Start At* setting (`[0x0046048C]`,
which a new game reads) for one F2, and puts it back as the level loads.

`hover.ini`:

```ini
[levels]
seed=random           ; or a number: pinned
[saved]
1=2,1789123456        ; level (0-based), seed
```

`--seed N` pins for one run without touching the ini.
