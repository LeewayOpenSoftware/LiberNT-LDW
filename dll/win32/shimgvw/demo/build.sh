#!/bin/sh
# Native Win11 demo of the viewer chrome. MSYS2 clang64, static:
#   PATH=/c/msys64/clang64/bin:$PATH sh dll/win32/shimgvw/demo/build.sh   (from the repo root, or anywhere)
# Output: viewer_demo.exe next to this script. Run: viewer_demo.exe [picture]
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/../../../.."
SHIM="$HERE/.."
LWUI="$ROOT/sdk/lib/leewayui"
CXX=${CXX:-clang++}; CC=${CC:-clang}; RC=${RC:-windres}
OUT="$HERE/viewer_demo.exe"
OBJ="${TMPDIR:-/tmp}/vdemo.$$"; mkdir -p "$OBJ"
INC="-I$ROOT/sdk/include -I$SHIM"
$RC -I"$SHIM" -I"$HERE" "$HERE/demo.rc" -O coff -o "$OBJ/demo_rc.o"
$CC -std=gnu11 -O1 -DUNICODE -D_UNICODE $INC -c "$SHIM/vribbon_model.c" -o "$OBJ/model.o"
$CXX -std=gnu++17 -O1 -static -mwindows -municode -DUNICODE -D_UNICODE -Wno-deprecated-declarations $INC \
  "$HERE/viewer_demo.cpp" "$SHIM/vribbon.cpp" "$LWUI/ribbon.cpp" "$LWUI/frame.cpp" "$LWUI/dpi.cpp" "$LWUI/theme.cpp" \
  "$OBJ/model.o" "$OBJ/demo_rc.o" -o "$OUT" \
  -lgdiplus -lcomctl32 -lcomdlg32 -lshell32 -lgdi32 -ladvapi32 -luser32 -lole32 -lshlwapi -luuid
rm -rf "$OBJ"
echo "built $OUT"
