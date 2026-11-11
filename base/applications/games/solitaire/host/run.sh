#!/bin/sh
# Host test for the pure Klondike model. Run from base/applications/games/solitaire/host.
set -e
cd "$(dirname "$0")"
g++ -std=c++14 -Wall -Wextra -o solmodel_test.exe solmodel_test.cpp ../solmodel.cpp
./solmodel_test.exe
rm -f solmodel_test.exe
