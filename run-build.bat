@echo off
set "PATH=C:\Windows\System32;C:\Windows;C:\Users\Nicolas\AppData\Local\RosBE\pkgs\20261015\cmake-3.31.6\bin;C:\Users\Nicolas\AppData\Local\RosBE\pkgs\20261015\ninja-1.12.1;C:\Users\Nicolas\AppData\Local\RosBE\pkgs\20261015\llvm\bin;C:\Users\Nicolas\AppData\Local\RosBE\pkgs\20261015\win_flex_bison-2.5.25;C:\Users\Nicolas\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin"
cd /d C:\Users\Nicolas\HarnessAgents\hive\agents\pam-mvzy3ngz\scratch\ln\output-Clang-amd64-debug
ninja -j1 dxgkrnl softgpu legacytest
