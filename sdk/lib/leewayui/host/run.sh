#!/bin/sh
# Host test for frame.cpp: MinGW g++, stub ribbon, real comctl32. Run from sdk/lib/leewayui/host.
set -e
cd "$(dirname "$0")"
g++ -std=c++14 -DUNICODE -D_UNICODE -I../../../include -o frame_host_test.exe frame_host_test.cpp ../frame.cpp ../dpi.cpp ../theme.cpp -lcomctl32 -luxtheme -ldwmapi -ladvapi32 -luser32 -lgdi32
./frame_host_test.exe
rm -f frame_host_test.exe
