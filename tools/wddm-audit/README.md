# WDDM audit tools

swatchtest.c - reference swatch/ramp/alpha test. Build (host, mingw):

    gcc -O2 -Wall -mwindows -municode -o swatchtest.exe swatchtest.c

Run in the guest, screenshot at ~2 s and ~7 s, then collect
C:\swatch-readback.txt.
