# hid-abs-probe

Question: when QEMU's `usb-tablet` (an absolute pointer) is attached to LiberNT, why do clicks land at
the bottom right? The host-side check says the HID parser and the scaling in `mouhid` are correct for
the tablet's descriptor, so the cause has to be read from a running system.

## Run 1 - baseline (plain Nov-1 or main Debug image, no change)

```
python -I probe.py --qemu "C:\Program Files\qemu\qemu-system-x86_64.exe" ^
    --img <LiberNT.img> --out run1 [--boot-down N]
```

The script copies the image, starts a **visible** QEMU window (`-display gtk`; use `--extra "-display sdl"`
or edit the script if gtk is missing in your build) with
`-device qemu-xhci,id=usbc -device usb-tablet,bus=usbc.0` added to the usual
`-machine q35 -accel tcg -cpu qemu64 -drive ...,if=ide -vga std -serial file:serial.log`, waits
`--boot-wait` seconds (screenshot every 30 s), then through QMP `input-send-event` moves the tablet to
center, the four corners, a quarter and three quarters, clicks the Start button (about 640,32300 of 32767)
and takes a screenshot after every step. Only the standard library of Python 3 is needed. The copy of the
image is deleted at the end. If your boot needs the boot-menu keys, use `--boot-down N` (Down x N + Enter).
Other USB controllers: `--usb usb-ehci` or `--usb piix3-usb-uhci`.

Expected on the broken build: the cursor ends up bottom right in every screenshot.

## What to send back

The whole `--out` directory, or at least `serial.log`, `summary.txt` and the `shot*.png` files.

`summary.txt` already greps `serial.log` for these patterns (grep them by hand if you prefer):

| pattern | meaning |
|---|---|
| `MouHid_GetButtonFlags failed` / `failed to get usages` | button parsing failed -> on old code `Flags` stayed 0 = relative (the main hypothesis) |
| `failed to get absolute` | X/Y value read failed |
| `\[MOUHID\]` and `mouhid` (case-insensitive) | anything from the driver |
| `Mouse returned an invalid byte count`, `Failed to read from mouse` | win32k side of the mouse queue |
| `hidusb`, `hidclass`, `mouclass` | transport / class driver errors |
| `win32ss/user/ntuser/(mouse|input|rawinput).c` | win32k messages from the input path |
| `ABSPROBE` | only in the diag build, see below |

## Run 2 - diag + fix build

Build (CI) `leeway/fix-mouhid-absolute-diag`. It contains the candidate fix
(`leeway/fix-mouhid-absolute-flag2`: always report `MOUSE_MOVE_ABSOLUTE` for an absolute device) plus
trace lines:

```
[MOUHID] ABSPROBE init: absolute=1 X(abs=1 logical 0..32767) Y(abs=1 logical 0..32767) buttons=3 wheel=1
[MOUHID] ABSPROBE report #12 len=7 bytes 00 00 20 00 60 00 00 -> LastX=16384 LastY=49152 Flags=1 ButtonFlags=0
```

Run the same command on that image (`--out run2`). Compare the screenshots with run 1 and read the
ABSPROBE lines: `absolute=0` means the device was not classified absolute; `Flags=0` with `absolute=1` means the
flag was lost; LastX/LastY far above 65536 or all equal to the maximum would point to the scaling.

## More options

`--accel` (tcg, whpx...), `--cpu`, `--display` (gtk, sdl...), `--ovmf <OVMF.fd>` (UEFI: added as a read-only
pflash drive), `--machine`, `--usb` (qemu-xhci | usb-ehci | piix3-usb-uhci), `--extra "<more qemu args>"`, and
`--input qmp|hmp`: `qmp` drives the tablet with absolute axes (0..32767); `hmp` uses the monitor commands
`mouse_move X Y` / `mouse_button 1|0` for comparison. Absolute (0,0) is a valid position (top-left corner).
