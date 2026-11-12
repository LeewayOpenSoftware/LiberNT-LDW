#!/bin/sh
# Native Win11 standalone build of WordPad with the LeewayUI ribbon.
# MSYS2 clang64, static:
#   PATH=/c/msys64/clang64/bin:$PATH sh base/applications/wordpad/host/build.sh   (from the repo root, or anywhere)
# Output: wordpad.exe next to this script. Run: wordpad.exe [document]
#
# The resources use the tree's own PSDK headers (RC-aware, RC_INVOKED); the C
# code uses the MSYS2 headers; host/include shims wine/debug.h and
# reactos/version.rc. The ribbon links the LeewayUI framework from the tree
# (sdk/lib/leewayui, F1 on leeway/ribbon-f1).
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
CXXFLAGS="-D__REACTOS__ -std=c++14 -O1 -Wall -Wextra -Wno-unused-parameter -DUNICODE -D_UNICODE"

$RC -DRC_INVOKED -D__REACTOS__ -I"$HERE/rcinclude" $SHIM $PSDK -I"$APP" -I"$HERE" "$APP/wordpad.rc" -O coff -o "$OBJ/wordpad_rc.o"
$RC -DRC_INVOKED -D__REACTOS__ -I"$HERE/rcinclude" $SHIM $PSDK -I"$APP" "$APP/rsrc.rc" -O coff -o "$OBJ/rsrc_rc.o"

$CC $CFLAGS $SHIM -I"$APP" -c "$APP/wordpad.c"    -o "$OBJ/wordpad.o"
$CC $CFLAGS $SHIM -I"$APP" -c "$APP/print.c"      -o "$OBJ/print.o"
$CC $CFLAGS $SHIM -I"$APP" -c "$APP/registry.c"   -o "$OBJ/registry.o"
$CC $CFLAGS $SHIM -I"$APP" -c "$APP/olecallback.c" -o "$OBJ/olecallback.o"
$CC $CFLAGS $SHIM -I"$APP" -I"$ROOT/sdk/include" -c "$APP/ribbon.c" -o "$OBJ/ribbon.o"

$CXX $CXXFLAGS -I"$ROOT/sdk/include" -c "$ROOT/sdk/lib/leewayui/ribbon.cpp" -o "$OBJ/ribbonfw.o"
$CXX $CXXFLAGS -I"$ROOT/sdk/include" -c "$ROOT/sdk/lib/leewayui/theme.cpp"  -o "$OBJ/theme.o"
$CXX $CXXFLAGS -I"$ROOT/sdk/include" -c "$ROOT/sdk/lib/leewayui/dpi.cpp"    -o "$OBJ/dpi.o"
$CXX $CXXFLAGS -I"$ROOT/sdk/include" -c "$ROOT/sdk/lib/leewayui/aureline_icons.cpp" -o "$OBJ/aureline.o"

$CXX -static -mwindows -DUNICODE -D_UNICODE \
  "$OBJ/wordpad.o" "$OBJ/print.o" "$OBJ/registry.o" "$OBJ/olecallback.o" \
  "$OBJ/ribbon.o" "$OBJ/ribbonfw.o" "$OBJ/theme.o" "$OBJ/dpi.o" "$OBJ/aureline.o" \
  "$OBJ/wordpad_rc.o" "$OBJ/rsrc_rc.o" -o "$OUT" \
  -lcomctl32 -lcomdlg32 -lshell32 -lgdi32 -ladvapi32 -luser32 -lole32 -loleaut32 -lshlwapi -luuid -lmsimg32 -loleacc -lwindowscodecs -luxtheme

rm -rf "$OBJ"
echo "built $OUT"
