#!/bin/sh
# Configure and build the 32-bit host on Linux: clang-cl and lld-link against
# xwin's MSVC CRT and Windows SDK (cmake/clang-cl-x86.cmake). The same
# hover.exe build.cmd makes, to run under Wine.
#
#   ./build.sh                  # -> build/hover.exe
#
# Needs clang, lld, llvm, cmake and ninja, and once,
#   xwin --accept-license --arch x86 splat --output ~/.xwin
# (setup.sh does all of it).
set -e
cd "$(dirname "$0")"
BUILD_DIR=${BUILD_DIR:-build}
BUILD_TYPE=${BUILD_TYPE:-RelWithDebInfo}
if [ -z "$PCRECOMP" ]; then
  if [ ! -d ../pcrecomp ] && [ -d ../tools/runtime/native32 ]; then PCRECOMP=$(cd ../tools && pwd)
  else PCRECOMP=$(cd .. && pwd)/pcrecomp; fi
fi
if [ ! -f "$BUILD_DIR/build.ninja" ]; then
  # shellcheck disable=SC2086  # CMAKE_ARGS is a list of flags
  cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DCMAKE_TOOLCHAIN_FILE=cmake/clang-cl-x86.cmake -DPCRECOMP="$PCRECOMP" $CMAKE_ARGS
fi
cmake --build "$BUILD_DIR" "$@"
