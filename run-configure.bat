@echo off
set "PATH=C:\Windows\System32;C:\Windows;C:\Users\Nicolas\AppData\Local\RosBE\pkgs\20261015\cmake-3.31.6\bin;C:\Users\Nicolas\AppData\Local\RosBE\pkgs\20261015\ninja-1.12.1;%PATH%"
cd /d C:\Users\Nicolas\HarnessAgents\hive\agents\pam-mvzy3ngz\scratch\ln
configure.cmd /clang /arch=amd64 -DREACTOS_TARGET_NT=0xA00
