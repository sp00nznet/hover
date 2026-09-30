#!/usr/bin/env python3
"""Function catalog for HOVER.EXE: pcrecomp disasm32.

    py -3 tools/catalog.py            # -> work/functions_hover.json

The entry point is the seed; the rest is recursive descent, prologues and a
data scan. The catalog is derived from the game binary, so it lives in work/
and is never committed.
"""
import os
import subprocess
import sys

_HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(os.environ.get('PCRECOMP', os.path.join(_HERE, '..', 'tools')), 'tools')
GAME = os.path.join(_HERE, 'game', 'hover')

os.makedirs(os.path.join(_HERE, 'work'), exist_ok=True)     # gitignored, so absent in a fresh copy
subprocess.run([sys.executable, os.path.join(TOOLS, 'disasm', 'disasm32.py'),
                os.path.join(GAME, 'HOVER.EXE'), '-o',
                os.path.join(_HERE, 'work', 'functions_hover.json')], check=True)
