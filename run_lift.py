#!/usr/bin/env python3
"""Hover! lift driver: HOVER.EXE -> src/recomp/gen/.

Hover! is one PE image (MSVC 2.x, static MFC 3, linker 2.55, no protection).
DASHRES.DLL and HOVERHLP.DLL beside it are resource-only as far as the game
is concerned (docs/architecture.md), so there is one module to lift, whole:
every catalogued function, no closure limit.

    py -3 tools/catalog.py      # work/functions_hover.json
    py -3 run_lift.py

docs/architecture.md has the whole pipeline.
"""
import argparse
import json
import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
# PCRECOMP picks the toolkit checkout, the same knob CMakeLists.txt has: the
# lifter and the runtime header must come from the same tree.
_TOOLS = os.path.join(os.environ.get('PCRECOMP', os.path.join(_HERE, '..', 'tools')), 'tools')
sys.path.insert(0, os.path.join(_TOOLS, 'lift'))
sys.path.insert(0, os.path.join(_TOOLS, 'pe'))

from capstone import Cs, CS_ARCH_X86, CS_MODE_32          # noqa: E402
from generate import (EXTENT_REACH, find_splits, true_extent,  # noqa: E402
                      linear_disassemble_function, lift_function_linear, write_chunk)
from lift32 import Lifter                                  # noqa: E402
from pe_analyze import analyze_pe, build_iat_map           # noqa: E402

GAME = os.path.join(_HERE, 'game', 'hover')
EXE = 'HOVER.EXE'
CATALOG = os.path.join(_HERE, 'work', 'functions_hover.json')
OUT = os.path.join(_HERE, 'src', 'recomp', 'gen')
STATS = os.path.join(_HERE, 'work', 'lift_stats.json')

# Source patches on the generated C: (VA, what the lifter wrote, what to write
# instead). Each is applied to every copy of that instruction (a body lifted
# from a mid-function entry repeats it), and a patch that matches nothing
# stops the lift, so a lifter change cannot silently drop one. These are this
# game's hooks into the host; the reasoning is in src/runtime/mp.c.
PATCHES = [
    # The frame draw's camera (0x00402250) reads the human craft at
    # doc+0x818C: ask the host whose eyes this render pass uses.
    (0x00402291, 'ecx = MEM32(edi + 0x8214);', 'ecx = MEM32(hover_cam_obj(edi) + 0x88);'),
    (0x004022AA, 'ecx = edi + 0x818C;', 'ecx = hover_cam_obj(edi);'),
    (0x004022BA, 'eax = MEM32(edi + 0x8218);', 'eax = MEM32(hover_cam_obj(edi) + 0x8C);'),
    # The render thread's frame-draw command: once per player.
    (0x0041F012, 'RECOMP_CALL(sub_00402250);', 'RECOMP_CALL(hover_render_views);'),
    # Pods for players in robot seats (mp.c). The pods' pickup check
    # (0x0040CB40) takes only CHumanPlayer, unless the pod is a flag: a seat's
    # craft passes too, AI robots still do not.
    (0x0040CB5B, 'if (TEST_NZ(_flag_a, _flag_b)) goto', 'if (TEST_NZ(_flag_a, _flag_b) || mp_is_seat(ebp)) goto'),
    # CTempWallPod::Use faces the wall along the human's heading (doc+0x832A =
    # human+0x19E); ebx is the pod's owner there, so use the owner's.
    (0x0042B52C, 'MEM16(eax + 0x832A)', 'MEM16(ebx + 0x19E)'),
    # The map eraser erases player 1's map: not when a seat picks it up.
    (0x00422589, 'if (CMP_NE(_flag_a, _flag_b)) goto', 'if (CMP_NE(_flag_a, _flag_b) || mp_is_seat(eax)) goto'),
    # The dashboard reads the human too: this pass's craft instead. Radar
    # centre and heading (0x00407160, and 0x00404BB0 per blip), the height
    # gauge in the frame draw, the speed dial (0x00405A10) and the speed bar
    # (0x00407430): CPlayer +0x19E heading, +0x1A4 height, +0x1AC/+0x1B0
    # velocity, +0x1D8 top speed.
    (0x004071A2, 'ebp = edi + 0x818C;', 'ebp = hover_cam_obj(edi);'),
    (0x004071C0, 'MEM16(edi + 0x832A)', 'MEM16(hover_cam_obj(edi) + 0x19E)'),
    (0x004052E7, 'MEM16(eax + 0x832A)', 'MEM16(hover_cam_obj(eax) + 0x19E)'),
    (0x004022E5, 'MEM32(edi + 0x8330)', 'MEM32(hover_cam_obj(edi) + 0x1A4)'),
    (0x00405A2B, 'MEM32(eax + 0x8338)', 'MEM32(hover_cam_obj(eax) + 0x1AC)'),
    (0x00405A31, 'MEM32(eax + 0x833C)', 'MEM32(hover_cam_obj(eax) + 0x1B0)'),
    (0x00408018, 'MEM32(edx + 0x8364)', 'MEM32(hover_cam_obj(edx) + 0x1D8)'),
    (0x00408047, 'MEM32(ecx + 0x8338)', 'MEM32(hover_cam_obj(ecx) + 0x1AC)'),
    (0x00408076, 'MEM32(ecx + 0x833C)', 'MEM32(hover_cam_obj(ecx) + 0x1B0)'),
    # The dashboard blit (0x00407430) draws only what changed, straight to the
    # window, unless the game's own full-dashboard option ([0x004C4CA8]) is
    # on: then it composes all of it off screen and blits it whole. Every
    # player's cell needs the whole of its own, so several views turn it on.
    (0x00407448, 'eax = MEM32(0x4C4CA8);', 'eax = hover_hud_full();'),
    (0x00408337, 'if (CMP_EQ(_flag_a, _flag_b)) goto L_004083C3;',
     'if (!hover_hud_full()) goto L_004083C3;'),
    # The renderer's visibility test for each sprite: a pass does not draw
    # the sprite of the craft it sees from.
    (0x00406343, 'RECOMP_CALL(sub_00405B90);', 'RECOMP_CALL(hover_sprite_visible);'),
    # The level teardown deletes the world, and with it the human's sprite
    # (the human, unlike the robots, outlives the level): forget it.
    (0x004149C4, 'MEM32(esi + 0x188) = edi;', 'MEM32(esi + 0x188) = edi; MEM32(esi + 0x818C + 0x78) = 0;'),
    # A robot flag taken: the flag gauge and the "all flags taken" test count
    # the taker's side, not the taker alone.
    (0x0041AFF6, 'edx = MEM32(ecx + 0xC0);', 'edx = hover_team_flags(ecx);'),
    (0x0041B05C, 'ecx = MEM32(eax + 0xC0);', 'ecx = hover_team_flags(eax);'),
    # Hunters look for, and then chase, the nearest craft on the human's side
    # (ecx/esi: the AI state asking).
    (0x0042ED09, 'ecx = eax + 0x818C;', 'ecx = hover_quarry(eax, ecx);'),
    (0x0040A810, 'eax = eax + 0x818Cu;', 'eax = hover_quarry(eax, esi);'),
] + [
    # Pod pickup/Use/Think write their gauges into the view (0x00401140):
    # a seat's go to that seat's own block instead (esi is the pod, +0x88 its owner).
    (va, 'RECOMP_CALL(sub_00401140);',
     'RECOMP_CALL(sub_00401140); eax = hover_pod_hud(eax, MEM32(esi + 0x88));')
    for va in (0x0042AB55,                                   # jump: pickup
               0x0042B297, 0x0042B422, 0x0042B0C4,           # wall: pickup, Use, Think
               0x0042A7F5, 0x0042A918, 0x0042A99E, 0x0042A6A6, 0x0042A705,  # cloak
               0x004143B6, 0x004228A5, 0x0042C476,           # speed Think; slow pickup, Think
               0x004183A3, 0x0040DEC6)                       # invincible pickup, Think
]
PATCH_DECLS = ('uint32_t hover_cam_obj(uint32_t doc);\nvoid hover_render_views(void);\n'
               'int mp_is_seat(uint32_t craft);\nuint32_t hover_pod_hud(uint32_t view, uint32_t owner);\n'
               'uint32_t hover_hud_full(void);\n'
               'void hover_sprite_visible(void);\nuint32_t hover_team_flags(uint32_t craft);\n'
               'uint32_t hover_quarry(uint32_t doc, uint32_t state);\n')


def apply_patches(out):
    hits = {va: 0 for va, _, _ in PATCHES}
    for fn in sorted(os.listdir(out)):
        if not (fn.startswith('recomp_0') and fn.endswith('.c')):
            continue
        path = os.path.join(out, fn)
        lines = open(path, encoding='utf-8').read().split('\n')
        changed = False
        for i, line in enumerate(lines):
            for va, old, new in PATCHES:
                if ('/* 0x%08X:' % va) in line and old in line:
                    lines[i] = line.replace(old, new, 1)
                    hits[va] += 1
                    changed = True
        if changed:
            open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines))
    missing = [va for va, n in hits.items() if not n]
    if missing:
        sys.exit('patches matched nothing at %s: the lifter output changed, update PATCHES'
                 % ', '.join('0x%08X' % va for va in missing))
    print('[*] %d patches applied at %d sites' % (len(PATCHES), sum(hits.values())))


def lift(path, catalog, md, out, split):
    info = analyze_pe(path)
    iat = build_iat_map(info)
    cs, ce = info.code_start, info.code_end
    cat = json.load(open(catalog))
    byaddr = {f['address']: f for f in cat['functions'] if cs <= f['address'] < ce}
    print('[*] %s: base=0x%08X code=0x%08X-0x%08X IAT=%d catalog=%d'
          % (os.path.basename(path), info.image_base, cs, ce, len(iat), len(byaddr)))

    text = [s for s in info.sections if s.name == '.text'][0]
    code = open(path, 'rb').read()[text.raw_offset:text.raw_offset + text.raw_size]
    # Known targets become RECOMP_CALL; anything else a decoded `call` names
    # becomes RECOMP_ICALL, which reports at run time instead of failing the build.
    lifter = Lifter(iat_map=iat, lifted=set(byaddr))
    starts = {a for a in byaddr if byaddr[a].get('entry_kind') != 'alias'}
    starts -= find_splits(md, code, cs, ce, starts)

    stats = {'lifted': 0, 'errors': 0, 'no_terminator': 0, 'files': 0}
    entries, chunk = [], []
    # A direct branch that leaves a body to an address nothing catalogued is a
    # tail call the catalog missed or a jump into a neighbour's shared code.
    # Either way the target has to be dispatchable, so each round's outside
    # targets become entries of the next (Bunghole's run_lift, same reason).
    todo, added = sorted(byaddr), 0
    while todo:
        outside = set()
        for addr in todo:
            name = 'sub_%08X' % addr
            reached, behind = set(), set()
            end, clean = true_extent(md, code, cs, addr, min(addr + EXTENT_REACH, ce), starts,
                                     reached=reached, behind=behind)
            outside |= {t for t in behind if t not in reached}
            stats['no_terminator'] += not clean
            try:
                lo = min(reached) if reached else addr   # a chunk can sit below the entry
                insns, leaders = (linear_disassemble_function(md, code, cs, lo, end, reached=reached)
                                  if end > addr else ([], None))
                if leaders is not None:
                    leaders.add(addr)
                body = (lift_function_linear(lifter, name, insns, leaders, addr) if insns
                        else 'void %s(void) { }\n' % name)
            except Exception as e:                  # noqa: BLE001 -- counted, not hidden
                body = '/* ERROR %s: %s */\nvoid %s(void) { }\n' % (name, e, name)
                stats['errors'] += 1
            chunk.append((body, addr, name))
            entries.append((addr, name))
            if len(chunk) >= split:
                write_chunk(out, stats['files'], chunk)
                stats['files'] += 1
                chunk = []
        todo = sorted(t for t in outside if cs <= t < ce and t not in byaddr)
        for t in todo:
            byaddr[t] = {'address': t}
        added += len(todo)
    if chunk:
        write_chunk(out, stats['files'], chunk)
        stats['files'] += 1
    stats['lifted'] = len(byaddr)
    print('[*]   %d outside branch targets added' % added)
    return entries, info.image_base + info.entry_point_rva, stats


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default=GAME, help='folder holding HOVER.EXE')
    ap.add_argument('--out', default=OUT)
    ap.add_argument('--split', type=int, default=400, help='functions per .c file')
    args = ap.parse_args()
    if not os.path.exists(CATALOG):
        sys.exit('no catalog at %s -- run tools/catalog.py first (README, Step by step)' % CATALOG)

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    os.makedirs(args.out, exist_ok=True)
    for fn in os.listdir(args.out):                 # a smaller lift must not leave stale chunks
        if fn.startswith('recomp_') and fn.endswith('.c'):
            os.remove(os.path.join(args.out, fn))

    t0 = time.time()
    entries, oep, stats = lift(os.path.join(args.game, EXE), CATALOG, md, args.out, args.split)

    with open(os.path.join(args.out, 'recomp_funcs.h'), 'w', newline='\n') as f:
        f.write('/* Hover! - AUTO-GENERATED by run_lift.py */\n#pragma once\n#include <stdint.h>\n\n')
        f.write('/* host hooks the PATCHES call (src/runtime/mp.c) */\n' + PATCH_DECLS + '\n')
        for a, n in entries:
            f.write('void %s(void);\n' % n)
    with open(os.path.join(args.out, 'recomp_dispatch.c'), 'w', newline='\n') as f:
        f.write('/* Hover! - AUTO-GENERATED by run_lift.py */\n'
                '#include "recomp_types.h"\n#include "recomp_funcs.h"\n\n'
                'const recomp_dispatch_entry_t recomp_dispatch_table[] = {\n')
        for a, n in sorted(entries):
            f.write('    { 0x%08Xu, %s },\n' % (a, n))
        f.write('};\nconst uint32_t recomp_dispatch_count = %d;\n'
                'const uint32_t hover_entry_va = 0x%08Xu;\n' % (len(entries), oep))

    apply_patches(args.out)
    stats['lines'] = sum(sum(1 for _ in open(os.path.join(args.out, fn), encoding='utf-8',
                                             errors='replace'))
                         for fn in os.listdir(args.out))
    json.dump(stats, open(STATS, 'w'), indent=1)
    print('=' * 60)
    print('  lifted %(lifted)d   errors %(errors)d   no terminator %(no_terminator)d' % stats)
    print('  %s lines of C in %d files, %.1fs' % (format(stats['lines'], ','), stats['files'],
                                                 time.time() - t0))
    print('=' * 60)


if __name__ == '__main__':
    main()
