#!/usr/bin/env python3
"""Flake rate of conformance.py's online milestones: the same three
one-PC online runs, scored by the same patterns, N times.

    py -3 tools/online_flake.py [N]       # default 10; WINE picks a wine off Windows

Prints one line per run (which online milestones failed) and a pass count per
milestone. Exit 0 only if every run passed every online milestone. Logs of
failing runs go to work/online_flake/.
"""
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import conformance as c  # noqa: E402

ONLINE = [(n, p) for n, p in c.MILESTONES if n.startswith('online:')]


def one():
    try:
        h, cl = c.online(7795, ['--clients', '1', '--seed', '12345', '--key', 'UP@12000+6000'],
                         ['--key', 'UP@12000+6000', '--key', 'RIGHT@13000+1500'], 520)
        out = '\n[online-host] ' + h + '\n[online-client] ' + cl + '\n'
        h, cl = c.online(7796, ['--clients', '1', '--seed', '12345'], ['--net-desync-test', '100'], 300)
        out += '\n[online-desync] ' + h + ' ' + cl + '\n'
        h, cl = c.online(7797, ['--seats', '4', '--seed', '31337', '--key', 'UP@9000+60000'],
                         ['--key', 'UP@25000+20000'], 1900, join_after=15, client_frames=900)
        out += '\n[online-late] ' + cl + ' ' + h + '\n'
    except subprocess.TimeoutExpired as e:  # conformance.py doesn't catch this: it would crash
        return [], 'TIMEOUT %s' % e
    return [n for n, p in ONLINE if re.search(p, out)], out


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 10
    if not (os.path.exists(c.HOST) and os.path.isfile(os.path.join(c.ROOT, 'game', 'hover', 'HOVER.EXE'))):
        print('online_flake: SKIPPED -- needs game/hover and build/hover.exe')
        return 3
    logs = os.path.join(c.ROOT, 'work', 'online_flake')
    os.makedirs(logs, exist_ok=True)
    tally = {name: 0 for name, _ in ONLINE}
    clean = 0
    for i in range(1, n + 1):
        t = time.time()
        passed, out = one()
        for name in passed:
            tally[name] += 1
        missed = [name for name, _ in ONLINE if name not in passed]
        clean += not missed
        if missed:
            open(os.path.join(logs, 'run%02d.log' % i), 'w').write(out)
        print('run %d/%d %3.0fs  %s' % (i, n, time.time() - t, 'ok' if not missed else 'FAIL ' + '; '.join(missed)),
              flush=True)
    print('online: %d/%d runs passed every online milestone' % (clean, n))
    for name, _ in ONLINE:
        print('  %2d/%d  %s' % (tally[name], n, name))
    return 0 if clean == n else 1


if __name__ == '__main__':
    sys.exit(main())
