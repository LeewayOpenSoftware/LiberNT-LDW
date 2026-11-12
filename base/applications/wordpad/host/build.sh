#!/bin/sh
# Native Win11 standalone build of WordPad (audit + iteration). MSYS2 clang64, static:
#   PATH=/c/msys64/clang64/bin:$PATH sh base/applications/wordpad/host/build.sh   (from the repo root, or anywhere)
# Output: wordpad.exe next to this script. Run: wordpad.exe [document]
#
# The resources use the tree's own PSDK headers (RC-aware, RC_INVOKED); the C
# code uses the MSYS2 headers; host/include shims wine/debug.h and
# reactos/version.rc.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
APP="$HERE/.."
ROOT="$HERE/../../../.."
CC=${CC:-clang}
CXX=${CXX:-clang++}
RC=${RC:-windres}
OUT="$HERE/wordpad.exe"
OBJ="${TMPDIR:-/tmp}/wp.$$"; mkdir -p "$OBJ"
SHIM="-I$HERE/include"
PSDK="-I$ROOT/sdk/include/psdk -I$ROOT/sdk/include/crt -I$ROOT/sdk/include"
CFLAGS="-include $HERE/include/host_shim.h -D__REACTOS__ -std=gnu11 -O1 -Wall -Wextra -Wno-unused-parameter -DUNICODE -D_UNICODE"

$RC -DRC_INVOKED -D__REACTOS__ -I"$HERE/rcinclude" $SHIM $PSDK -I"$APP" -I"$HERE" "$APP/wordpad.rc" -O coff -o "$OBJ/wordpad_rc.o"
$RC -DRC_INVOKED -D__REACTOS__ -I"$HERE/rcinclude" $SHIM $PSDK -I"$APP" "$APP/rsrc.rc" -O coff -o "$OBJ/rsrc_rc.o"

$CC $CFLAGS $SHIM -I"$APP" -c "$APP/wordpad.c"    -o "$OBJ/wordpad.o"
$CC $CFLAGS $SHIM -I"$APP" -c "$APP/print.c"      -o "$OBJ/print.o"
$CC $CFLAGS $SHIM -I"$APP" -c "$APP/registry.c"   -o "$OBJ/registry.o"
$CC $CFLAGS $SHIM -I"$APP" -c "$APP/olecallback.c" -o "$OBJ/olecallback.o"

$CXX -static -mwindows -DUNICODE -D_UNICODE \
  "$OBJ/wordpad.o" "$OBJ/print.o" "$OBJ/registry.o" "$OBJ/olecallback.o" \
  "$OBJ/wordpad_rc.o" "$OBJ/rsrc_rc.o" -o "$OUT" \
  -lcomctl32 -lcomdlg32 -lshell32 -lgdi32 -ladvapi32 -luser32 -lole32 -loleaut32 -lshlwapi -luuid -lmsimg32

rm -rf "$OBJ"
echo "built $OUT"
