#!/usr/bin/env python3
"""Hover! conformance harness (REPO_RULES section 9).

Two fixed corpora, one pass/fail count each, compared against the committed
baseline in conformance.json; a regression fails the run:

* **Milestones**: one headless run of build/hover.exe with scripted keys,
  scored by the lines the host prints at each stage. Boot: what the original
  does on every start, in this order. Play: F2 starts level 1, UP drives, and
  the hovercraft has to move: the host compares the window at frame 150 with
  frame 210 while UP is held. Standing still, that is 0%; driving, 30-40%.
* **Lift health**: from the generated tree. Lift errors, bodies with no
  terminator, RECOMP_ITAIL targets that cannot resolve at run time (not in the
  dispatch table), and UNIMPLEMENTED instructions.

The game is not in the repo. Without game/hover and build/hover.exe this
skips with a message and exits 0, so it can sit in CI without the corpus.

    py -3 tools/conformance.py              # run, compare, print the table
    py -3 tools/conformance.py --update     # ...and accept the result as the baseline
"""
import argparse
import glob
import json
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = os.path.join(ROOT, 'build', 'hover.exe')
GEN = os.path.join(ROOT, 'src', 'recomp', 'gen')
BASELINE = os.path.join(ROOT, 'conformance.json')

# (name, what the host prints when it is reached). Order is boot order.
MILESTONES = [
    ('image mapped, every import bound', r'\[bind\] 0x00400000: .* 0 unresolved'),
    ('display check passes, no 256-colour warning', r'\[display\] the 256-colour check is answered'),
    ('frame window created', r'CreateWindowExA\("Hover!", 536x431\)'),
    ('dashboard art loaded (DASHRES.DLL)', r'LoadLibraryA\("dashres.dll"\) -> data file [0-9A-F]*[1-9A-F]'),
    ('Quick Help dialog at startup', r'\[headless\] dialog 115 '),
    ('first frame presented', r'\[capture\] frame 1 presented'),
    ('F2: Start Game', r'\[input\] key 0x71 down'),
    ('100 frames', r'\[capture\] frame 100 presented'),
    ('UP: drive forward', r'\[input\] key 0x26 down'),
    ('the hovercraft moved', r'\[diff\] ([2-9]\d|100)% of the window changed'),
    ('1000 frames with no fault', r'\[capture\] 1000 frames: stopping'),
    ('controller mapping self-test', r'\[pad\] selftest OK'),
    ('level 1 on the pinned seed', r'\[level\] level 1, seed 12345 \(pinned\)'),
    ('share codes self-test', r'\[level\] selftest OK'),
    ('Recomp menu on the frame', r'\[menu\] Recomp menu added'),
    ('split screen: 2 players, 150 frames', r'\[split\] .*2x1 views.*\[capture\] 150 frames: stopping'),
    ('online: host and client start one game', r'\[online-host\] .*game on: 2 seats'),
    ('online: in sync at tick 400 on both PCs',
     r'(?s)\[online-host\] [^\n]*in sync at tick 400.*\[online-client\] [^\n]*in sync at tick 400'),
    ('online: a one-unit nudge is caught', r'\[online-desync\] .*DESYNC at tick 120'),
    ('online: a late joiner replays, catches up and takes its seat',
     r'\[online-late\] .*caught up: .*our seats are ours from tick'),
    ('online: the late joiner stays in sync', r'\[online-late\] .*caught up.*in sync at tick 800'),
    ('online: its seat goes back to the AI when it leaves', r'\[online-late\] .*seat 2-2 left: the AI drives it now'),
]

# Split screen: player 2 in a robot seat, driven by I/J/K/L, its own view.
SPLIT = ['--players', '2', '--frames', '150', '--seed', '12345', '--key', 'F2@2000',
         '--key', 'UP@7000+5000', '--key', 'I@7000+5000']

# Milliseconds from entry: F2 once the menu is up, then UP held long enough
# that frames 150-210 are driving (level 1 loads in about 5 s), with a turn.
# The seed is pinned, so level 1 places everything the same way every run.
PLAY = ['--frames', '1000', '--diff', '150,210', '--seed', '12345',
        '--key', 'F2@2000', '--key', 'UP@8000+12000', '--key', 'LEFT@14000+800']


def online(port, host_extra, client_extra, frames, join_after=1, client_frames=None):
    """A host and a client on this PC, over localhost: both logs."""
    common = ['--headless', '--run', '--watchdog', '150']
    h = subprocess.Popen([HOST] + common + ['--frames', str(frames), '--host', str(port)] + host_extra,
                         cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
    time.sleep(join_after)
    c = subprocess.run([HOST] + common + ['--frames', str(client_frames or frames),
                                          '--join', '127.0.0.1:%d' % port] + client_extra,
                       cwd=ROOT, capture_output=True, text=True, errors='replace', timeout=200)
    try:
        hout = h.communicate(timeout=150)[0]
    except subprocess.TimeoutExpired:
        h.kill()
        hout = h.communicate()[0]
    return hout.replace('\n', ' '), (c.stdout + c.stderr).replace('\n', ' ')


def boot(seconds):
    try:
        p = subprocess.run([HOST, '--headless', '--run', '--watchdog', str(seconds)] + PLAY,
                           cwd=ROOT, capture_output=True, text=True, errors='replace',
                           timeout=seconds + 60)
        out, code = p.stdout + p.stderr, p.returncode
    except subprocess.TimeoutExpired as e:
        out, code = (e.stdout or '') + (e.stderr or ''), 'timeout'
        out = out if isinstance(out, str) else out.decode(errors='replace')
    # The controller mapping has no controller to test against, so the host
    # tests it on synthetic pad states (pad.c, pad_selftest).
    t = subprocess.run([HOST, '--headless', '--run', '--watchdog', '90'] + SPLIT, cwd=ROOT,
                       capture_output=True, text=True, errors='replace', timeout=150)
    out += '\n[split] ' + (t.stdout + t.stderr).replace('\n', ' ') + '\n'
    # Online: lockstep over localhost, both driving; then a deliberate desync.
    hout, cout = online(7795, ['--clients', '1', '--seed', '12345', '--key', 'UP@12000+6000'],
                        ['--key', 'UP@12000+6000', '--key', 'RIGHT@13000+1500'], 520)
    out += '\n[online-host] ' + hout + '\n[online-client] ' + cout + '\n'
    hout, cout = online(7796, ['--clients', '1', '--seed', '12345'], ['--net-desync-test', '100'], 300)
    out += '\n[online-desync] ' + hout + ' ' + cout + '\n'
    # Drop in, drop out: an open 4-seat game; a client joins 15 s in, replays
    # the game so far, takes its seat, plays, and quits (saying goodbye).
    # Pinned seed: with four seats the robots can take every flag inside a
    # minute on some levels, and a game over ends the session before the
    # client leaves. 31337 lasts the whole run.
    hout, cout = online(7797, ['--seats', '4', '--seed', '31337', '--key', 'UP@9000+60000'], ['--key', 'UP@25000+20000'],
                        1900, join_after=15, client_frames=900)
    out += '\n[online-late] ' + cout + ' ' + hout + '\n'
    for test in ('--pad-selftest', '--levels-selftest'):
        t = subprocess.run([HOST, test], cwd=ROOT, capture_output=True, text=True,
                           errors='replace', timeout=60)
        out += t.stdout + t.stderr
    passed = [name for name, pat in MILESTONES if re.search(pat, out)]
    last = [l for l in out.splitlines() if l.startswith(('===', '[watchdog]', 'ICALL', 'ITAIL', '[diff]'))]
    return passed, code, last[:3]


def lift_health():
    stats = json.load(open(os.path.join(ROOT, 'work', 'lift_stats.json')))
    disp = set(re.findall(r'\{ 0x([0-9A-F]{8})u,', open(os.path.join(GEN, 'recomp_dispatch.c')).read()))
    unresolved = unimplemented = 0
    for fn in glob.glob(os.path.join(GEN, 'recomp_0*.c')):
        text = open(fn).read()
        unresolved += sum(1 for t in re.findall(r'RECOMP_ITAIL\(0x([0-9A-F]{8})u\)', text) if t not in disp)
        unimplemented += text.count('UNIMPLEMENTED:')
    return {'lifted': stats['lifted'], 'errors': stats['errors'],
            'no_terminator': stats['no_terminator'], 'unresolved_itail': unresolved,
            'unimplemented': unimplemented}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--update', action='store_true', help='accept this run as the baseline')
    ap.add_argument('--seconds', type=int, default=180, help='headless run limit')
    args = ap.parse_args()
    if not (os.path.exists(HOST) and os.path.isfile(os.path.join(ROOT, 'game', 'hover', 'HOVER.EXE'))):
        print('conformance: skipped -- needs game/hover (your copy of Hover!) and build/hover.exe '
              '(README, Building from source)')
        return 0

    passed, code, last = boot(args.seconds)
    health = lift_health()
    now = {'milestones': len(passed), 'of': len(MILESTONES), **health}
    base = json.load(open(BASELINE)) if os.path.exists(BASELINE) else None

    print('milestones: %d/%d  (exit %s)' % (len(passed), len(MILESTONES), code))
    for name, _ in MILESTONES:
        print('  [%s] %s' % ('x' if name in passed else ' ', name))
    for l in last:
        print('  ' + l)
    print('lift: %(lifted)d functions, %(errors)d errors, %(no_terminator)d with no terminator, '
          '%(unresolved_itail)d unresolvable ITAIL targets, %(unimplemented)d UNIMPLEMENTED '
          'instructions' % health)

    worse = []
    if base:
        if now['milestones'] < base['milestones']:
            worse.append('milestones %d -> %d' % (base['milestones'], now['milestones']))
        for k in ('errors', 'no_terminator', 'unresolved_itail', 'unimplemented'):
            if now[k] > base.get(k, now[k]):
                worse.append('%s %d -> %d' % (k, base[k], now[k]))
    if args.update or not base:
        json.dump(now, open(BASELINE, 'w'), indent=1)
        print('baseline written to conformance.json')
    if worse:
        print('REGRESSION: ' + '; '.join(worse))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
