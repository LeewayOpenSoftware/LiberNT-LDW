#!/usr/bin/env python3
# mon.py NAME CMD...   send HMP commands to a VM started by run-vm.sh (monitor on 127.0.0.1:<vm/NAME/monitor.port>)
#   python3 mon.py creed-p1 "screendump E:/LiberNT/results/x.ppm"   (then: magick x.ppm x.png)
#   python3 mon.py creed-p1 "sendkey ctrl-alt-delete"
#   python3 mon.py creed-p1 --type "notepad"     types text with sendkey (us layout), one key at a time
import socket, sys, time, os

name = sys.argv[1]
# per-VM dirs live under OVERLAY_DIR (same variable as run-vm.sh and libernt-smoke.sh)
overlay = os.environ.get("OVERLAY_DIR", "C:/LiberNT/vm/overlays")
vmdir = next((d for d in (os.path.join(overlay, name),) if os.path.exists(os.path.join(d, "monitor.port"))), None)
if vmdir is None: sys.exit("mon.py: no monitor.port for " + name)
port = int(open(os.path.join(vmdir, "monitor.port")).read().strip())
KEYS = {" ": "spc", "\n": "ret", "\\": "backslash", "/": "slash", ".": "dot", ",": "comma", "-": "minus",
        "=": "equal", ";": "semicolon", "'": "apostrophe", "`": "grave_accent", "[": "bracket_left",
        "]": "bracket_right", "\t": "tab"}
SHIFT = {":": "semicolon", "_": "minus", "%": "5", "\"": "apostrophe", "&": "7", ">": "dot", "<": "comma",
         "|": "backslash", "?": "slash", "(": "9", ")": "0", "!": "1", "@": "2", "#": "3", "$": "4", "*": "8", "+": "equal"}


def key(ch):
    if ch in KEYS: return KEYS[ch]
    if ch in SHIFT: return "shift-" + SHIFT[ch]
    if ch.isupper(): return "shift-" + ch.lower()
    return ch


def run(cmds):
    s = socket.create_connection(("127.0.0.1", port), timeout=10)
    out = b""
    def drain(t=0.4):
        nonlocal out
        s.settimeout(t)
        try:
            while True:
                d = s.recv(65536)
                if not d: break
                out += d
        except socket.timeout:
            pass
    drain()
    for c in cmds:
        s.sendall((c + "\n").encode())
        drain(0.15 if c.startswith("sendkey") else 1.5)
    s.close()
    # drop the monitor's line-editing echo, keep the replies
    lines = [l for l in out.decode(errors="replace").replace("\r", "").split("\n") if "\x1b" not in l]
    return "\n".join(l for l in lines if l.strip() and l.strip() != "(qemu)")


args = sys.argv[2:]
if args and args[0] == "--type":
    print(run(["sendkey " + key(ch) for ch in " ".join(args[1:])])[-200:])
else:
    print(run(args))
