#!/bin/sh
# Host test for the pure Minesweeper board logic. Run from base/applications/games/winmine/host.
set -e
cd "$(dirname "$0")"
gcc -std=c99 -Wall -Wextra -o mbcore_test.exe mbcore_test.c ../mbcore.c
./mbcore_test.exe
rm -f mbcore_test.exe
