# Microsoft Hover! (1995): .MAZ / .TEX level formats

Sources: hexdumps of all 4 .MAZ and 4 .TEX files, plus disassembly of HOVER.EXE (VAs below; lifted C is `sub_XXXXXXXX` at the same VA).
Confidence: **H** = byte-exact parse of every shipped file and matches the loader code; **M** = matches the code or data but the meaning is inferred; **L** = a guess.

`tools/maz.py` parses all four .MAZ files and consumes every byte (it asserts that), then renders them as ASCII.
`tools/textest.py` does the same for all four .TEX files and cross-checks them against the mazes.

---
## 0. Container: MFC `CArchive` (H)

A .MAZ file is a raw MFC 4 `CArchive` stream (load side) of one `CMerlinWorld`. There is no magic number and no checksum. All integers are little-endian.

* **CString**: `u8 len` (if `0xFF`, then `u16 len` follows), then `len` bytes of ANSI text with no NUL. An empty string is the single byte `00`.
* **CObList**: `u16 count` (if `0xFFFF`, then `u32 count` follows), then `count` × ReadObject.
* **ReadObject tag**: `u16`.
  * `0xFFFF`: a new class follows. It is `u16 schema (=1)`, `u16 nameLen`, then the class name.
  * `0x8000|i`: an instance of the class already registered at map index `i`.
  * The map index starts at 1 (0 = NULL). **Each new class and each new object** takes the next index, in stream order.
  * Example from SMALL: Static class = 1, its 28 objects = 2..29, Location class = 30 (`FFFF`), its object = 31, BSP class = 32 (`FFFF`), later BSP tags are `0x8020`.
  * Object back-references never occur. Neither does the `0x7FFF`+u32 big tag (the largest index in MAZE3 is about 1500).
* **"Tail"**: every Merlin class ends its record with `u16 n` + `n` bytes that the loader reads and throws away (forward compatibility). All shipped files use `n=0`, except CMerlinStatic, which uses `n=5` (see below).

The game **can't save** worlds: the store branch of `CMerlinWorld::Serialize` (0x418DC0) is empty. The per-object store branches do exist (editor leftovers) and write exactly this layout. That is extra confirmation of the format, but we have to write our own writer.

## 1. .MAZ layout (H)

| off | type | field | notes |
|---|---|---|---|
| 0 | i16 | ymin | stored to world+0x8. The 4 header words are the bounding box of all lines (checked on all 4 files) |
| 2 | i16 | xmin | world+0x4 |
| 4 | i16 | xmax | world+0xC |
| 6 | i16 | ymax | world+0x10 |
| 8 | CObList | **statics** | `CMerlinStatic` (walls and floor edges), world+0x14. Also used as an array (world+0x18) indexed by BSP |
| … | CObList | dynamics | world+0x28. **Empty (count 0) in all files** |
| … | CObList | **locations** | `CMerlinLocation` (named points), world+0x3C |
| … | CObList | **bsp** | `CMerlinBSP` nodes, world+0x50. Node 0 is the root |
| EOF | | | nothing follows |

Counts: SMALL 28/0/1/28, MAZE1 542/0/125/544, MAZE2 377/0/118/382, MAZE3 676/0/128/676.

Coordinates are i16 world units. Walls sit on a 256-unit grid. Maps span about 512..22528.
The spatial hash is created as 0x5A00×0x5A00 with 0x100 cells (0x415CDC), so **coordinates must be in 0..23039**.
Y is "up" (math orientation) for the left/right statements below.

### Record layouts (fields in stream order; `+xx` = in-memory offset)

**CMerlinObject** (base, 0x411B10), H:
`CString name (+4)`, `tail`.

**CMerlinLine : Object** (0x414740), H:
`i16 x1 (+14), y1 (+18), x2 (+1C), y2 (+20)`, `tail`. After loading, 0x4010C0 derives the line equation/normal.

**CMerlinStatic : Line** (0x419E00), one wall or floor edge:

| field | type | +off | conf | meaning |
|---|---|---|---|---|
| tex[0] | CString | +44 | M | "top/cap" surface texture, side A (CBASE_xx, STEPS_00, pad textures) |
| tex[1] | CString | +50 | M | same for side B |
| tex[2] | CString | +5C | H | **wall face texture, side A**. Its TEX transparency flag is copied to b0 |
| tex[3] | CString | +68 | H | **wall face texture, side B**. Its flag is copied to b4 |
| tex[4] | CString | +74 | M/H | **floor texture on side A**. Side A = the right-hand side of (x1,y1)→(x2,y2). All 30 bbox-boundary walls have the interior on the right and tex[4] set |
| tex[5] | CString | +80 | M | floor texture on side B (left) |
| zlo | i16 | +A4 | M | wall bottom height (0, 64, 128, 320, 512…) |
| zhi | i16 | +A6 | M | wall top height (0, 256, 376, 512, 768). **0/0 means a zero-height line = floor-texture boundary / pad edge** |
| w2, w3 | i16 | +A8,+AA | H(value) | always 0 |
| b0 | u8 | +B0 | H | = transparency flag of tex[2] (texture word +1C). True for 100% of lines |
| b4 | u8 | +B4 | H | = transparency flag of tex[3] |
| bc | u8 | +BC | M | 1 = solid/collidable wall. It is 0 exactly for the 0-height lines |
| extLen | u16 | – | H | always 5. If ≥5, the next 5 bytes are read; any extra bytes are skipped |
| c0 | u8 | +C0 | H(value) | always 0 |
| uoffA | i16 | +B8 | L | texture u-offset for side A? (mostly 0; values like 1024, 2260) |
| uoffB | i16 | +BA | L | same for side B |

Texture names are resolved against the level's .TEX by exact name (`""` = none). A name missing from the .TEX leaves a NULL texture.
A non-empty `name` is registered in a name→static map (0x4A4578). This is how pads are found (§3).

**CMerlinLocation : Object** (0x41B940), H:
`i16 x (+14), y (+16), z (+18), a (+1A)`, `tail`.
* z (M) = height: 0, 128, 256, 384, matching raised-floor levels.
* a (L) = 96 on almost every point, sometimes 0/1/64. It is passed to the player/pod constructors (it ends up in pod+0xA4). Heading or radius, unknown.
* Named locations are registered in a map (0x4A4768).

**CMerlinBSP : Line** (0x41BC40). The line holds the (possibly split) partition segment. H for layout, M for semantics:

| field | type | +off | meaning |
|---|---|---|---|
| line | … | | segment coordinates. They equal `statics[seg]` coordinates for 540/544 nodes in MAZE1; the others are split pieces |
| seg | i16 | +44 | index into the statics list. The renderer draws `statics[seg]` (0x405FA0) |
| n1 | i16 | +46 | **L**: always −1 or a node inside this node's own subtree. The renderer ignores it. Role unknown (editor/traversal aid?) |
| right | i16 | +48 | child on the right side (−1 = none). 100% of these children are strictly right of the parent line |
| left | i16 | +4A | child on the left side **or collinear** (−1 = none) |
| k | i16 | +60 | always 0 |
| d0, d1 | f64 | +50,+58 | always 0.0 |
| tail | | | |

The render walk (0x405FA0) is back-to-front painter's order. With `side = (camX-x1)*ny + (y1-camY)*nx`, it recurses into the far child, draws `seg`, then the near child.
Every node is reachable from node 0 through right/left.

## 2. .TEX layout (H: all 4 files parse byte-exactly, and every per-level size equals the sum of its spans)

Loaded right after the .MAZ, by the same function 0x415450 (0x415A36…0x41596B).

| part | layout |
|---|---|
| palette | 256 × `{u8 R, G, B, flags}` = 1024 bytes. Passed to `CreatePalette` (LOGPALETTE ver 0x300) |
| textures | CObList of `CMerlinTexture` (0x412980), stored into the global texture list 0x4A1D08 / name map 0x4A1D30 |
| trailer | `u16 n` + n bytes (0 in all files) |

**CMerlinTexture : Object**:
`name`, `tail`, `u16 transparent (+1C)`, `u16 nLevels (+2C)` (mip levels, 6 for 128/256 px, 5 for 64 px). Then for each mip level L:

```
i16 w, wmask(w-1), h, hmask(h-1), log2(h)   ; e.g. 256,255,256,255,8 ; DECAL_09 in TEXT2/3 is 512x128
u32 nPix ; u8 pix[nPix]                     ; palette indices, only the opaque texels, packed column by column
u32 nSpans                                   ; total spans in this level
repeat w times (one per column):
    u16 k ; k × {u16 start, u16 end}         ; opaque row ranges, in LEVEL-0 rows; this level uses (v>>L)
tail
```

`nPix == Σ((end>>L)-(start>>L)+1)` and `nSpans == Σk` hold for every level of every texture.
Opaque textures have one span 0..h-1 per column, so nPix = w·h.
The engine skips the finest `[0x461CFC]` levels to save memory (low-detail option).

Texture naming (what the mazes and code reference):

| names | used for |
|---|---|
| `WBASE_nn`, `DECAL_nn` | walls. `DECAL_*` are usually transparent, e.g. the credits names in SMALL |
| `FBASE_nn` | floor |
| `CBASE_nn` | wall tops |
| `HOLD_PAD`, `FLAG_PAD`, `SLED_U/D/L/R`, `STEPS_00` | pads |
| `BACKGRND` | the sky/backdrop. Looked up by name at 0x4189EB (SMALL also uses it as a wall texture) |
| `DRONE_00..32`, `FORCE_FIELD` | sprites |
| `FLG00_nn`/`FLG01_nn` (flags), `POD00..07_nn` | animation frames. Chained by the loader (0x416043) via texture+0x38, "%s%02d" from 00 while the name exists |

Each MAZEn has a matching TEXTn. Every texture name used in a maze exists in its paired .TEX (checked).

## 3. What the loader does after parsing (0x415450, `CHoverDoc`-ish `this` = ecx)

1. `srand(time(NULL))` at 0x4154CD/0x4154DC (see §5).
2. Picks the level (§4), then opens the .TEX and the .MAZ (MFC `CFile::Open` → `CreateFileA` at 0x44F1B0, `ReadFile` 0x44F250; `CArchive` with a 0x200 buffer). Also loads music.
3. Counts locations (0x417C10). For each prefix `HUMAN_`, `ROBOT_`, `FLAG_HUMAN_`, `FLAG_ROBOT_`, `POD_RANDOM_` it counts **consecutive** `PREFIXnn` names starting at `00`, stopping at the first gap. The counts go to this+0x83D0..0x83E0.
4. Player start: `HUMAN_<rand()%nHuman>` (0x417D40). It re-rolls while another object is within 0x50 units.
5. Unless in demo mode (this+0x8498), in this order:
   * robots: `table.nRobotA` × `CRobotPlayer(1)` and `table.nRobotB` × `CRobotPlayer(0)`, each on a random free `ROBOT_nn`.
   * animated-texture chains.
   * **pads**: for each type in `PAD_HOLD_, PAD_FLAG_REMOVER_, PAD_SLED_L_, PAD_SLED_R_, PAD_SLED_T_, PAD_SLED_B_, PAD_RAZED_, PAD_LOWERED_` (table 0x4C48F0), it looks up statics named `TYPEpp_ll` ("%s%02d_%02d"). pp = pad number, ll = edge-line number, both contiguous from 00; shipped pads have 4 edges. M.
   * **pods**: for each of the 7 types in table order (SpeedPod, SlowPod, MapEraserPod, TempWallPod, SmokeBombPod, JumpPod, InvinciblePod), `count` pods (4 each). Each goes to a random free `POD_RANDOM_nn` via 0x412250. The second argument is `rand()%4==0`, which takes an alternate branch that still uses POD_RANDOM_ (probably the "mystery" variant, L).
   * **flags**: `table.nFlags` CFlag for each team, on a random free `FLAG_HUMAN_nn` / `FLAG_ROBOT_nn`.
6. `BEACON_nn` locations are read by CRobotPlayer code (0x423F0E): AI waypoints (M).

**Rules for generated mazes (derived from the code above):**
* Every prefix you use must have ≥1 entry. `rand()%0` is an integer divide-by-zero crash.
* Every prefix needs more well-spaced (>0x50 apart) points than objects placed on it: ≥28 POD_RANDOM, ≥6 FLAG_HUMAN and ≥6 FLAG_ROBOT, and enough ROBOT_ points. Otherwise the re-roll loop never ends.
* Keep names contiguous from 00.

## 4. Level selection & path substitution

* The level table is at **0x4C4000**: 21 entries × 0x58 bytes in writable `.data`.

| field | meaning |
|---|---|
| +0 | `char* music` (Sounds\Music\MUSICn.muz) |
| +4 | `char* maz` |
| +8 | `char* tex` |
| +0C..+24 | pod counts per type (all 4) |
| +28 | nRobotA |
| +2C | nRobotB |
| +30 | nFlags per team |

  Entry i uses MAZE(i%3+1)/TEXT(i%3+1). Difficulty rises through +28/+2C/+30:

| entries | +28 / +2C / +30 |
|---|---|
| 0..2 | 1/1/3 |
| 3 | 2/1/4 |
| … | … |
| 15..19 | 6/4/6 |

  Entry 20 (0x4C46E0) = `mazes\small.maz`/`small.tex`, the demo/attract level.
* The current level is `[0x46049C]`, 0-based.
  * Registry `HKCU\Software\Microsoft\Hover!` value `StartAtLevel` (clamped 1..20) → `[0x46048C]`, and level = that − 1 (0x425C08).
  * Winning does `inc [0x46049C]` (0x425AFD), capped at 19.
  * If `[0x46073C]` (random-level option in the `GameSettings` binary value) is set, level = `rand()%20`.
  * Table index = level<16 ? level : 15 + level%3.
  * In demo mode (this+0x8498) the level is forced to 20 (0x417B6B) and robots, pods, flags and pads are skipped.
* Path = `[0x460984]` (the game dir: the current dir if `mazes\small.maz` exists there via `OpenFile(OF_EXIST)` at 0x41C860, else the exe dir) + table string.
  **Substitution options, easiest first:**
  1. Overwrite the files in `MAZES\` (a generated MAZE1.MAZ/TEXT1.TEX pair).
  2. Repoint the `char*` fields at 0x4C4004/0x4C4008 + i·0x58. They live in `.data`, so a runtime poke works with no API shim.
  3. Add a `CreateFileA` shim to `GUEST_SHIMS` in `src/runtime/host.c` that rewrites paths ending in `mazes\MAZEn.maz`/`TEXTn.tex`. MAZ and TEX both go through CFile→CreateFileA (0x44F1B0). OpenFile is only the small.maz existence probe.

## 5. Randomness / "seeds" (H)

* **Level geometry is fixed**: the .MAZ is fully deterministic.
* **Randomised on every level load**:
  * player start (which HUMAN_nn)
  * robot starts (ROBOT_nn)
  * each pod's position (POD_RANDOM_nn)
  * the mystery-variant roll
  * flag positions (FLAG_HUMAN_nn/FLAG_ROBOT_nn)
  * the level itself, if the random-level option is on
* RNG = MSVC CRT `rand()` 0x43E85C: `s = s*214013 + 2531011; return (s>>16)&0x7FFF`, with per-thread state at ptd+0x14. `srand` = 0x43E84F.
* The seed is `srand(time(NULL))` at 0x4154DC, once per level load, with 1-second resolution. `time()` = 0x43E9C2 → `GetLocalTime` (only caller: 0x43E9CC).
  A second `srand(time())` at 0x433EAE is elsewhere (not in the level loader).
* **Pinning**: make the seed a constant. Two ways:
  * shim `GetLocalTime` to a fixed SYSTEMTIME, or better
  * override the `sub_0043E84F` call at 0x4154DC (or `sub_0043E9C2`) to return a chosen seed.

  Given the same seed, the same level and the same code path, placements are reproducible. So "level = MAZ file + 32-bit seed" is a valid model.
