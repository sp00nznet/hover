#!/bin/bash
# Linux setup: the same six steps as Setup.cmd. The Windows host is
# cross-compiled here (clang-cl, with xwin's MSVC CRT and Windows SDK) and
# played under Wine, so the game runs as it does on Windows.
#
#   ./setup.sh                   # asks before installing anything
#   ./setup.sh --yes             # answers yes to every question (recomp-launcher)
#   ./setup.sh --game-dir DIR    # a folder, mounted CD, .zip, .iso, .cue or .bin
#   ./setup.sh --force           # redo every step
#
# It leaves Hover!.sh here: run it to play. A rerun skips finished steps, and
# what each step prints goes to setup.log.
set -e
cd "$(dirname "$0")"
ROOT=$PWD
FORCE=0; YES=0; GAME=""
while [ $# -gt 0 ]; do
  case "$1" in
    --force) FORCE=1 ;;
    --yes) YES=1 ;;
    --game-dir|--game) GAME=$2; shift ;;
    *) echo "usage: ./setup.sh [--yes] [--force] [--game-dir DIR]" >&2; exit 2 ;;
  esac
  shift
done
LOG=$ROOT/setup.log
echo "==== setup $(date -Is)" >> "$LOG"

say()  { printf '%s\n' "$*" | tee -a "$LOG"; }
# "[n/6] what": recomp-launcher's progress bar reads these, as Setup.cmd's.
STEP=0
step() { STEP=$((STEP + 1)); printf '\n\033[36m[%d/6] %s\033[0m\n' "$STEP" "$*"; echo "[$STEP/6] $*" >> "$LOG"; }
fail() { printf '\n\033[31mSetup stopped: %s\033[0m\n' "$*" >&2; echo "Setup stopped: $*" >> "$LOG"; exit 1; }
ask()  { [ "$YES" = 1 ] && { say "$1 yes"; return 0; }; read -r -p "$1 [Y/n] " r; [[ ! $r =~ ^[nN] ]]; }
done_already() { [ "$FORCE" = 0 ] && [ -e "$1" ]; }
# A program on PATH or, as Debian and Ubuntu keep clang-cl, in /usr/lib/llvm-*/bin.
have() { command -v "$1" >/dev/null || ls /usr/lib/llvm-*/bin/"$1" >/dev/null 2>&1; }
# Run a step with its output in the log.
run()  { say "  $1..."; shift; "$@" >> "$LOG" 2>&1 || fail "that failed: the details are in $LOG."; }

say "Hover!: static recompilation setup (Linux, under Wine)"
say "You need your own copy of Hover! (the Windows 95 CD, the Hover! CD, an image of either, or a zip of the game folder) and about 2 GB free."

# ---------------------------------------------------------------- tools
step "Checking the tools"
need=()
for t in clang-cl lld-link llvm-lib cmake ninja wine python3 curl; do have "$t" || need+=("$t"); done
if [ ${#need[@]} -gt 0 ]; then
  say "  Missing: ${need[*]}. From your package manager, for example:"
  say "    Debian, Ubuntu: sudo apt install clang clang-tools lld llvm cmake ninja-build wine python3-venv curl git"
  say "                    (and for 32-bit Wine: sudo dpkg --add-architecture i386 && sudo apt update && sudo apt install wine32:i386)"
  say "    Fedora:         sudo dnf install clang lld llvm cmake ninja-build wine python3 curl git"
  say "    Arch:           sudo pacman -S clang lld llvm cmake ninja wine python curl git"
  fail "install ${need[*]}, then run ./setup.sh again."
fi
python3 -c 'import sys; sys.exit(sys.version_info < (3, 10))' || fail "Python 3.10 or newer is needed."

if ! command -v xwin >/dev/null && [ ! -x "$HOME/.local/bin/xwin" ]; then
  say "  xwin (https://github.com/Jake-Shadle/xwin) fetches the MSVC C runtime and Windows SDK."
  ask "  Download its release binary into ~/.local/bin (about 5 MB)?" || fail "xwin is required (or: cargo install xwin)."
  mkdir -p "$HOME/.local/bin"
  v=0.10.0
  curl -fsSL "https://github.com/Jake-Shadle/xwin/releases/download/$v/xwin-$v-x86_64-unknown-linux-musl.tar.gz" |
    tar -xz -C "$HOME/.local/bin" --strip-components=1 "xwin-$v-x86_64-unknown-linux-musl/xwin" ||
    fail "xwin did not download."
fi
PATH=$PATH:$HOME/.local/bin
XWIN_DIR=${XWIN_DIR:-$HOME/.xwin}
if [ ! -d "$XWIN_DIR/crt/lib/x86" ]; then
  say "  The x86 MSVC C runtime and Windows SDK are needed to build hover.exe (about 1 GB, 10 to 15 minutes)."
  say "  xwin downloads them from Microsoft, under Microsoft's licence: https://go.microsoft.com/fwlink/?LinkId=2086102"
  ask "  Accept that licence and download them into $XWIN_DIR?" || fail "the MSVC C runtime and Windows SDK are required."
  # The cache beside the output: moving out of /tmp fails where it is tmpfs.
  run "Downloading the C runtime and SDK" \
    xwin --accept-license --cache-dir "$XWIN_DIR.cache" --arch x86 splat --output "$XWIN_DIR"
  rm -rf "$XWIN_DIR.cache"
fi
export XWIN_DIR
say "  C compiler: clang-cl, with $XWIN_DIR"

if python3 -c 'import pefile, capstone' 2>/dev/null; then
  PY=$(command -v python3)
else
  if [ ! -x .venv/bin/python ] || ! .venv/bin/python -c 'import pefile, capstone' 2>/dev/null; then
    say "  The Python packages pefile and capstone are missing (about 20 MB)."
    ask "  Install them into .venv here?" || fail "pefile and capstone are required."
    say "  Installing pefile and capstone..."
    { python3 -m venv .venv && .venv/bin/pip install -q pefile capstone; } >> "$LOG" 2>&1 ||
      fail "Python could not make its venv: install python3-venv (Debian, Ubuntu), then run ./setup.sh again."
  fi
  PY=$ROOT/.venv/bin/python
fi
say "  Python: $PY"

# The toolkit: PCRECOMP, else ../pcrecomp, else ../tools (older setups).
if [ -z "$PCRECOMP" ]; then
  if [ ! -d ../pcrecomp ] && [ -f ../tools/tools/lift/lift32.py ]; then PCRECOMP=$(cd ../tools && pwd)
  else PCRECOMP=$(cd .. && pwd)/pcrecomp; fi
fi
if [ ! -f "$PCRECOMP/tools/lift/lift32.py" ]; then
  have git || fail "git is needed to fetch pcrecomp: install it from your package manager."
  ask "  Download the pcrecomp toolkit into $PCRECOMP (git, about 20 MB)?" || fail "pcrecomp is required at $PCRECOMP."
  run "Cloning pcrecomp" git clone --depth 1 https://github.com/sp00nznet/pcrecomp "$PCRECOMP"
fi
# The toolkit changes this game needs (docs/toolkit.md), each by a line it
# adds, and native32's Wine fix (DEP on, so callbacks into lifted code work).
toolkit_ok() {
  grep -q native32_export "$PCRECOMP/runtime/native32/native32.c" &&
    grep -q SetProcessDEPPolicy "$PCRECOMP/runtime/native32/native32.c" &&
    grep -qF 'table_end = at + code_start' "$PCRECOMP/tools/lift/generate.py"
}
if ! toolkit_ok && [ "$(cat "$PCRECOMP/.git/HEAD" 2>/dev/null)" = "ref: refs/heads/main" ]; then
  if ask "  Your pcrecomp is older than this game needs. Update it (git pull)?"; then
    run "Updating pcrecomp" git -C "$PCRECOMP" pull --ff-only
  fi
fi
toolkit_ok || fail "your pcrecomp at $PCRECOMP is too old: update it (git -C \"$PCRECOMP\" pull), or point PCRECOMP at a newer checkout."
export PCRECOMP
say "  pcrecomp: $PCRECOMP"

# ---------------------------------------------------------------- the game
step "Copying your copy of Hover! into game/hover"
if done_already game/hover/HOVER.EXE; then say "  Already there (skipping)."; else
  if [ -z "$GAME" ]; then
    # A mounted CD (the Windows 95 CD keeps it in FUNSTUFF/HOVER), then a
    # download nearby: archive.org's is hoverwindows95.zip.
    for m in /media/"$USER" /run/media/"$USER"; do
      [ -d "$m" ] || continue
      exe=$(find "$m" -maxdepth 4 -iname hover.exe -print -quit 2>/dev/null)
      [ -n "$exe" ] && { GAME=$(dirname "$exe"); break; }
    done
  fi
  if [ -z "$GAME" ]; then
    GAME=$(find "$ROOT" "$ROOT/.." "$HOME/Downloads" -maxdepth 1 -type f \
             \( -iname 'hover*.zip' -o -iname 'hover*.iso' -o -iname 'hover*.cue' \) -print -quit 2>/dev/null || true)
  fi
  [ -n "$GAME" ] && say "  Found $GAME"
  [ -z "$GAME" ] && [ "$YES" = 1 ] && fail "Hover! wasn't found. Give its folder, mounted CD or .zip/.iso with --game-dir."
  while [ -z "$GAME" ]; do
    read -r -p "  Where is Hover!? A folder with HOVER.EXE, a mounted CD, or a .zip/.iso/.cue/.bin file: " GAME
  done
  run "Copying the game (17 MB)" "$PY" tools/gamefiles.py "$GAME"
fi

# ---------------------------------------------------------------- catalog
step "Finding every function (under a minute)"
if done_already work/functions_hover.json; then say "  Already done (skipping)."
else run "Disassembling HOVER.EXE" "$PY" tools/catalog.py; fi

# ---------------------------------------------------------------- lift
step "Recompiling HOVER.EXE to C (about 20 seconds)"
if done_already src/recomp/gen/recomp_dispatch.c; then say "  Already done (skipping)."
else run "Lifting" "$PY" run_lift.py; fi

# ---------------------------------------------------------------- build
step "Building build/hover.exe (a few minutes)"
if done_already build/hover.exe; then say "  Already built (skipping)."; else
  [ "$FORCE" = 1 ] && rm -rf build
  run "Compiling" sh build.sh
fi

# ---------------------------------------------------------------- launcher
step "Making the launcher"
cat > 'Hover!.sh' <<'SH'
#!/bin/sh
# Plays the recompiled Hover! under Wine, in the game's own window (--classic):
# Wine can't cloak a window, so with the presenter the game's window would
# show beside it. The Recomp menu has the port's settings; WINE picks another
# wine. mscoree and mshtml are off so a new Wine prefix doesn't stop to offer
# Mono and Gecko.
cd "$(dirname "$0")" || exit 1
WINEDLLOVERRIDES=${WINEDLLOVERRIDES:-mscoree,mshtml=} exec "${WINE:-wine}" build/hover.exe --run --classic "$@"
SH
chmod +x 'Hover!.sh'
say "  $ROOT/Hover!.sh"
printf '\n\033[32mDone.\033[0m Run Hover!.sh here to play.\n'
