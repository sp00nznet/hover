#!/usr/bin/env python3
r"""Put your copy of Hover!'s files in game/hover/, from wherever they are.

    py -3 tools/gamefiles.py E:\                   # a CD in a drive: the Windows 95 CD
                                                   #   (FUNSTUFF\HOVER) or the Hover! CD
    py -3 tools/gamefiles.py C:\Games\HOVER!       # any folder holding HOVER.EXE
    py -3 tools/gamefiles.py hover.zip             # a zip of it (zips inside zips too)
    py -3 tools/gamefiles.py win95.iso             # a 2048-byte ISO, or a BIN/CUE dump
    py -3 tools/gamefiles.py                       # look in every drive

Whatever it is given, it finds the folder that holds HOVER.EXE (searching a
few levels down) and copies that folder: HOVER.EXE, DASHRES.DLL, MAZES\ and
SOUNDS\ are what the game reads. Disc images are read with pcrecomp's ISO9660
walker (tools/assets/iso_peek.py), so nothing is mounted.

It supplies your own copy to the local build; nothing is downloaded, and none
of it is ever committed (README, "What is not in this repo").
"""
import io
import os
import shutil
import string
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'game', 'hover')
NEEDED = ('HOVER.EXE', 'DASHRES.DLL', 'MAZES/MAZE1.MAZ')


def complete(files):
    """Does this set of relative paths (any case, '/' separated) hold the game?"""
    have = {f.upper() for f in files}
    return all(n in have for n in NEEDED)


def pick(paths):
    """The prefix ('' or 'a/b/') under which the game sits, or None."""
    for p in sorted(paths, key=len):
        if p.upper().endswith('HOVER.EXE'):
            pre = p[:-len('HOVER.EXE')]
            if complete(q[len(pre):] for q in paths if q.upper().startswith(pre.upper())):
                return pre
    return None


def write(rel, data):
    dst = os.path.join(OUT, *rel.split('/'))
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst, 'wb') as f:
        f.write(data)


def from_zip(z, label):
    names = [n for n in z.namelist() if not n.endswith('/')]
    pre = pick(names)
    if pre is None:                         # archive.org wraps the game zip in another zip
        for n in names:
            if n.lower().endswith('.zip'):
                with zipfile.ZipFile(io.BytesIO(z.read(n))) as inner:
                    if from_zip(inner, '%s!%s' % (label, n)):
                        return True
        return False
    take = [n for n in names if n.startswith(pre)]
    print('reading %d files from %s%s' % (len(take), label, '!' + pre if pre else ''))
    for n in take:
        write(n[len(pre):], z.read(n))
    return True


def from_image(path):
    sys.path.insert(0, os.path.join(os.environ.get('PCRECOMP', os.path.join(ROOT, '..', 'tools')),
                                    'tools', 'assets'))
    import iso_peek

    class RawReader(iso_peek.Reader):
        """ISO offsets over a raw BIN: sector n's 2048 bytes sit 16 bytes into
        raw sector n (12 sync + 4 header), 2352 bytes apart."""

        def read(self, off, size):
            out = bytearray()
            while size > 0:
                sec, within = divmod(off, 2048)
                take = min(size, 2048 - within)
                self.fh.seek(sec * 2352 + 16 + within)
                out += self.fh.read(take)
                off += take
                size -= take
            return bytes(out)

    if path.lower().endswith('.cue'):
        for line in open(path, errors='replace'):     # the data track is the first FILE line
            if line.strip().upper().startswith('FILE'):
                path = os.path.join(os.path.dirname(path), line.split('"')[1])
                break
    size = os.path.getsize(path)
    raw = size % 2352 == 0 and size % 2048 != 0
    reader = (RawReader if raw or path.lower().endswith(('.bin', '.img')) else iso_peek.Reader)(path)
    files = {p: (off, n) for p, off, n in iso_peek.walk(reader)}
    pre = pick(list(files))
    if pre is None:
        return False
    take = [p for p in files if p.startswith(pre)]
    print('reading %d files from %s at %s' % (len(take), path, pre or '/'))
    for p in take:
        off, n = files[p]
        write(p[len(pre):], reader.read(off, n))
    return True


def from_folder(src):
    for top, dirs, files in os.walk(src):
        if top[len(src):].count(os.sep) > 3:   # a few levels down is enough (FUNSTUFF\HOVER)
            dirs[:] = []
            continue
        if any(f.upper() == 'HOVER.EXE' for f in files):
            rel = [os.path.relpath(os.path.join(t, f), top).replace(os.sep, '/')
                   for t, _, fs in os.walk(top) for f in fs]
            if complete(rel):
                print('copying %s' % top)
                shutil.copytree(top, OUT, dirs_exist_ok=True)
                return True
    return False


def main():
    src = sys.argv[1].strip('"') if len(sys.argv) > 1 else ''
    if not src:
        drives = ['%s:\\' % c for c in string.ascii_uppercase if os.path.exists('%s:\\' % c)]
        for d in drives:
            if from_folder(d):
                break
        else:
            sys.exit('no drive holds Hover!; pass a drive, folder, zip or disc image')
    elif os.path.isdir(src):
        if not from_folder(src):
            sys.exit('%s does not hold HOVER.EXE with DASHRES.DLL and MAZES\\ beside it' % src)
    elif zipfile.is_zipfile(src):
        with zipfile.ZipFile(src) as z:
            if not from_zip(z, src):
                sys.exit('%s is a zip, but Hover! is not in it' % src)
    elif os.path.isfile(src):
        if not from_image(src):
            sys.exit('%s is not a disc image holding Hover!' % src)
    else:
        sys.exit('no such drive, folder or file: %s' % src)
    if not complete(os.path.relpath(os.path.join(t, f), OUT).replace(os.sep, '/')
                    for t, _, fs in os.walk(OUT) for f in fs):
        sys.exit('copied, but %s is still missing a file the game needs' % OUT)
    print('done: %s' % OUT)


if __name__ == '__main__':
    main()
