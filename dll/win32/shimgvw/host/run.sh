#!/bin/sh
# Host test of the viewer ribbon model + adapter. Run from this directory.
# Needs a MinGW gcc/g++ on Windows and the leewayui headers (sdk/include).
set -e
INC=../../../../sdk/include
OUT=${TMPDIR:-/tmp}/vribbon_host_test.exe
printf '#include <windows.h>
#include "resource.h"
#include "lang/en-US.rc"
' > ${OUT%.exe}.rc
windres -I.. -I. ${OUT%.exe}.rc -O coff -o ${OUT%.exe}_rc.o
gcc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -c ../vribbon_model.c -o ${OUT%.exe}_model.o
g++ -std=gnu++14 -Wall -Wextra -Wno-unused-parameter -DDBG=1 -I$INC \
    vribbon_host_test.cpp ../vribbon.cpp ${OUT%.exe}_model.o ${OUT%.exe}_rc.o -o $OUT -ladvapi32 -luser32 -lkernel32
$OUT ../lang/en-US.rc
