# The toolkit change this game needed

Everything generic goes to [pcrecomp](https://github.com/sp00nznet/pcrecomp)
as its own PR (REPO_RULES section 11). Hover! needed one:

| Branch | What | How it showed up here |
|---|---|---|
| `fix/generate-jump-table-self-arm` | `generate._jump_table`: an entry never points into its own table | a fault two seconds into level 1 |

`Setup.cmd` checks the toolkit for it and names it if it is missing. Until it
merges, point `PCRECOMP` at a checkout of that branch.

## The switch that ate its own table

Pressing F2 loaded level 1, built the pods' sprite names (`POD04_05`...), and
faulted:

```
=== fault 0xC0000005 at 0x6016C82D ===
  read of 0x858904C5
  in lifted sub_00415D3C, last native call USER32.dll!TranslateAcceleratorA
```

`tools/addr2line.py` put the fault in the lifted C for `0x004164D0`, and the
C around it was nonsense: `arpl`, `bound`, `add byte ptr [ebx - 0x7a76fb3c],
al`, interleaved with real instructions, and the real `call 0x42c5e0` at
`0x004164CC` followed by a label at `0x004164CD`, the middle of its own five
bytes. The call returned into garbage.

MSVC 2 emits a switch's table straight after its jump:

```
004164A0  jmp dword ptr [eax*4 + 0x4164a7]
004164A7  dd 0041624B, 00416290, ...          ; 8 arms, then real code at 004164C7
```

`_jump_table` reads the table upward, and then *downward* for a negative
index (CRT `memcpy`'s backward copy indexes `[ecx*4 + table]` with ecx from
-3 to 0, The Movies). The dword just below this table is the jmp's own
displacement, `a7 64 41 00` = `0x004164A7`: the table's address, which is in
range, so it became an arm. The walker then decoded the table as code, the
garbage decode ran into the first real instruction from the middle, and the
body was lifted as two interleaved streams.

The first fix stopped the downward read at the jmp's own bytes. The CRT
`memcpy` in both Hover! (`0x00440489`) and SimCity 2000 (`0x0048ECB1`)
showed that was not enough: a second jmp further down uses the same table,
and for it the dword below the table is still the first jmp's displacement.
The rule that holds for both is where the entry points: **an entry never
points into its own table**, which is data.

Checked by walking every catalogued function with pcrecomp main's
`generate.py` and with the fix, and comparing the instruction sets:

| Title | Functions | Bodies that change |
|---|---|---|
| Hover! `HOVER.EXE` | 5,090 | 10, each around an inline table: 20 to 126 instructions of table-decoded-as-code dropped (`0x00415D3C` checked by hand) |
| Bunghole in One `GOLF.EXE` + `00170001.DLL` | 1,909 | 0 |
| SimCity 2000 `SIMCITY.EXE` | 9,453 | 2: the CRT `memcpy`/`memmove` (tables straight after their jmps, as in Hover!), 17 and 10 instructions dropped. Its lift on main includes that garbage |
| Civilization III `Civ3Conquests.exe` | 15,308 | 0 |
| The Movies `MoviesSE.unpacked.exe` | 82,091 | 0 |

Only the binaries that put a table directly after its jmp change; the
MSVC 5 and later titles checked do not. After the fix, Hover!'s lift has no `arpl` or
`bound` left and plays; `generate.py --selftest` has both shapes.
