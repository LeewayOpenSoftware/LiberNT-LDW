#!/usr/bin/env python3
"""qmp-key.py PORT QCODE...  - press a key chord as ORDERED QMP input-send-event: each key down in the order
given (modifier first), then every key up in reverse order, like a real keyboard. QMP send-key presses all keys
at once, which some guest paths (terminal.exe Alt+F4) ignore. Example: qmp-key.py 5950 alt f4
Standard library only; qcodes are QEMU's (alt, ctrl, shift, meta_l, f4, up, down, tab, esc, ret)."""
import json, socket, sys, time
port, keys = int(sys.argv[1]), sys.argv[2:]
s = socket.create_connection(("127.0.0.1", port), timeout=10)
f = s.makefile("rw", encoding="utf-8", newline="\n")
f.readline()
def cmd(name, **a):
    f.write(json.dumps({"execute": name, "arguments": a}) + "\n"); f.flush()
    while True:
        l = f.readline()
        if not l: return
        r = json.loads(l)
        if "return" in r or "error" in r: return r
def ev(k, down):
    cmd("input-send-event", events=[{"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": k}}}])
cmd("qmp_capabilities")
for k in keys: ev(k, True); time.sleep(0.08)
time.sleep(0.08)
for k in reversed(keys): ev(k, False); time.sleep(0.08)
