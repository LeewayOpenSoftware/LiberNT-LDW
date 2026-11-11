#!/bin/sh
# Host test for the G0 game shell: MinGW g++, stub ribbon, real registry/user32. Run from sdk/lib/leewayui/host.
set -e
cd "$(dirname "$0")"
g++ -std=c++14 -DUNICODE -D_UNICODE -I../../../include -o game_host_test.exe game_host_test.cpp ../gamestats.cpp ../gamesave.cpp ../gameanim.cpp ../gameshell.cpp ../frame.cpp ../dpi.cpp ../theme.cpp -lcomctl32 -luxtheme -ldwmapi -ladvapi32 -luser32 -lgdi32
./game_host_test.exe
rm -f game_host_test.exe
