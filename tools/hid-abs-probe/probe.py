#!/usr/bin/env python3
"""hid-abs-probe: boot a LiberNT image in QEMU with an absolute USB tablet, move it to known
positions, click, take screenshots and collect the serial log (COM1).

Run with:  python -I probe.py --qemu "C:\\Program Files\\qemu\\qemu-system-x86_64.exe" --img LiberNT.img --out outdir
The image is COPIED first (the original is never modified) and the copy is deleted at the end.
Needs only the Python standard library. No driver is changed by this script: use it first on the
plain Debug image (baseline), then on an image built from leeway/fix-mouhid-absolute-diag.

Output in --out: serial.log, shotNN-<label>.png, probe.log, summary.txt (the grep hits).
"""
import argparse, json, os, re, shutil, socket, subprocess, sys, time, zlib, struct

POSITIONS = [  # label, x, y  (QEMU absolute axes run 0..32767)
    ("center", 16384, 16384),
    ("top-left", 0, 0),
    ("top-right", 32767, 0),
    ("bottom-left", 0, 32767),
    ("bottom-right", 32767, 32767),
    ("quarter", 8192, 8192),
    ("three-quarter", 24576, 24576),
]
# Start button of the default taskbar: far left, bottom edge (about x=20/1024, y=755/768)
START_CLICK = ("start-click", 640, 32300)

PATTERNS = [
    r"ABSPROBE",                      # only present in leeway/fix-mouhid-absolute-diag builds
    r"MouHid_GetButtonFlags failed",  # button parsing failed -> Flags stayed 0 on old code
    r"failed to get usages",
    r"failed to get absolute",
    r"ScaleAbsoluteCoordinate",
    r"MouHid_GetButtonMove",
    r"\[MOUHID\]",
    r"[Mm]ouhid",
    r"mouclass|MouseClass",
    r"Mouse returned an invalid byte count",
    r"Failed to read from mouse",
    r"hidusb|HIDUSB|hidclass|HIDCLASS",
    r"\(win32ss/user/ntuser/(mouse|input|rawinput)\.c",
]


def png_from_ppm(ppm, png):
    d = open(ppm, "rb").read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", d)
    if not m:
        return False
    w, h = int(m.group(1)), int(m.group(2))
    pix = d[m.end():]
    raw = b"".join(b"\x00" + pix[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(t, c):
        x = struct.pack(">I", len(c)) + t + c
        return x + struct.pack(">I", zlib.crc32(t + c) & 0xFFFFFFFF)

    open(png, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                          chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
    return True


class Qmp:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.f = self.s.makefile("rw", encoding="utf-8", newline="\n")
        self.f.readline()
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        self.f.write(json.dumps({"execute": name, "arguments": args}) + "\n")
        self.f.flush()
        while True:
            line = self.f.readline()
            if not line:
                return {}
            r = json.loads(line)
            if "return" in r or "error" in r:
                return r

    def move(self, x, y):
        return self.cmd("input-send-event", events=[
            {"type": "abs", "data": {"axis": "x", "value": x}},
            {"type": "abs", "data": {"axis": "y", "value": y}}])

    def hmp(self, line):
        return self.cmd("human-monitor-command", **{"command-line": line})

    def button(self, down):
        return self.cmd("input-send-event", events=[{"type": "btn", "data": {"down": down, "button": "left"}}])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", required=True)
    ap.add_argument("--img", required=True, help="LiberNT disk image (copied, never modified)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--mem", default="1536")
    ap.add_argument("--machine", default="q35")
    ap.add_argument("--boot-wait", type=float, default=240, help="seconds to wait for the desktop")
    ap.add_argument("--boot-down", type=int, default=0, help="boot menu: Down presses then Enter (DEBUG entry)")
    ap.add_argument("--port", type=int, default=5701)
    ap.add_argument("--keep-img", action="store_true")
    ap.add_argument("--extra", default="", help="extra QEMU arguments")
    ap.add_argument("--accel", default="tcg", help="QEMU accelerator (tcg, whpx, ...)")
    ap.add_argument("--cpu", default="qemu64")
    ap.add_argument("--display", default="gtk", help="QEMU display backend (gtk, sdl, ...)")
    ap.add_argument("--ovmf", default="", help="OVMF firmware file: added as a read-only pflash drive (UEFI boot)")
    ap.add_argument("--input", default="qmp", choices=["qmp", "hmp"],
                    help="how the tablet is driven: QMP input-send-event (absolute axes) or the monitor commands mouse_move/mouse_button")
    ap.add_argument("--usb", default="qemu-xhci", help="USB controller device (qemu-xhci, usb-ehci, piix3-usb-uhci)")
    a = ap.parse_args()

    os.makedirs(a.out, exist_ok=True)
    img = os.path.join(a.out, "vm.img")
    shutil.copyfile(a.img, img)
    serial = os.path.join(a.out, "serial.log")
    plog = open(os.path.join(a.out, "probe.log"), "w")

    def log(m):
        line = "[%5.0fs] %s" % (time.time() - t0, m)
        print(line, flush=True)
        plog.write(line + "\n")
        plog.flush()

    cmd = [a.qemu, "-m", a.mem, "-machine", a.machine, "-accel", a.accel, "-cpu", a.cpu,
           "-drive", "file=%s,format=raw,if=ide,index=0" % img,
           "-display", a.display, "-vga", "std",
           "-qmp", "tcp:127.0.0.1:%d,server,nowait" % a.port,
           "-serial", "file:" + serial,
           "-netdev", "user,id=n0", "-device", "rtl8139,netdev=n0",
           "-device", a.usb + ",id=usbc", "-device", "usb-tablet,bus=usbc.0"]
    if a.ovmf:
        cmd += ["-drive", "if=pflash,format=raw,readonly=on,file=" + a.ovmf]
    if a.extra:
        import shlex
        cmd += shlex.split(a.extra)
    t0 = time.time()
    log("QEMU: " + " ".join(cmd))
    q = subprocess.Popen(cmd, stdout=open(os.path.join(a.out, "qemu.out"), "w"), stderr=subprocess.STDOUT)
    try:
        time.sleep(5)
        if q.poll() is not None:
            log("QEMU exited early rc=%s: %s" % (q.returncode, open(os.path.join(a.out, "qemu.out")).read()))
            sys.exit(2)
        qmp = Qmp(a.port)
        n = [0]

        def move(x, y):
            if a.input == "qmp":
                qmp.move(x, y)
            else:
                qmp.hmp("mouse_move %d %d" % (x, y))

        def click(down):
            if a.input == "qmp":
                qmp.button(down)
            else:
                qmp.hmp("mouse_button %d" % (1 if down else 0))

        def shot(label):
            ppm = os.path.join(a.out, "shot%02d-%s.ppm" % (n[0], label))
            n[0] += 1
            qmp.cmd("screendump", filename=ppm)
            time.sleep(1.5)
            for _ in range(10):
                try:
                    if os.path.exists(ppm) and os.path.getsize(ppm) > 1000 and png_from_ppm(ppm, ppm[:-4] + ".png"):
                        os.remove(ppm)
                    break
                except OSError:
                    time.sleep(1)
            log("screenshot " + label)

        if a.boot_down:
            time.sleep(6)
            for _ in range(a.boot_down):
                qmp.cmd("send-key", keys=[{"type": "qcode", "data": "down"}])
                time.sleep(0.3)
            qmp.cmd("send-key", keys=[{"type": "qcode", "data": "ret"}])
            log("boot menu: down x%d + enter" % a.boot_down)

        # wait for the desktop, with a screenshot every 30 s so a stuck boot is visible
        waited = 0
        while waited < a.boot_wait and q.poll() is None:
            time.sleep(30)
            waited += 30
            shot("boot-%03ds" % waited)
        if q.poll() is not None:
            log("QEMU exited during boot rc=%s" % q.returncode)
            sys.exit(3)

        log("probe: moving the absolute tablet; every position gets a screenshot")
        for label, x, y in POSITIONS:
            move(x, y)
            time.sleep(2.0)
            move(x, y)       # a second report at the same place: a mid-flight screenshot is not a verdict
            time.sleep(1.5)
            shot("move-" + label)
        label, x, y = START_CLICK
        move(x, y)
        time.sleep(2.0)
        shot("before-" + label)
        click(True)
        time.sleep(0.15)
        click(False)
        time.sleep(3.0)
        shot("after-" + label)
        # a second click target: the middle of the screen, then back to center, to see a click is not "sticky"
        move(16384, 16384)
        time.sleep(2.0)
        shot("final-center")
    finally:
        try:
            Qmp(a.port).cmd("quit")
        except Exception:
            pass
        time.sleep(1)
        if q.poll() is None:
            q.kill()
        if not a.keep_img:
            try:
                os.remove(img)
            except OSError:
                pass

    # summary of serial hits
    hits = {}
    try:
        lines = open(serial, "r", errors="replace").read().splitlines()
    except OSError:
        lines = []
    out = ["serial lines: %d" % len(lines)]
    for p in PATTERNS:
        rx = re.compile(p)
        m = [l for l in lines if rx.search(l)]
        out.append("== %s : %d hit(s)" % (p, len(m)))
        out.extend("   " + l[:220] for l in m[:15])
    open(os.path.join(a.out, "summary.txt"), "w").write("\n".join(out) + "\n")
    print("\n".join(out))
    log("done; send the whole --out directory (serial.log, summary.txt, shots)")


if __name__ == "__main__":
    main()
