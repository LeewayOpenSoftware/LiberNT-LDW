#!/usr/bin/env python3
"""qmp-pointer.py PORT WIDTH HEIGHT ACTION X Y   - drive the guest's absolute USB tablet through QMP.

ACTION is move | steps | near | click | dclick | down | up. steps = ~10 small moves 60 ms apart
ending on the target; near = jump 14 px left of the target, then a 1-pixel-step move onto it. X, Y are screen pixels; they are scaled to the tablet's
0..32767 axes. HMP mouse_move/mouse_button must NOT be used for this: on these images it pins the
pointer at the bottom-right corner, whereas QMP input-send-event positions it correctly (UHCI and xhci).
Do not target exactly (0,0): the guest ignores it (known win32k mouse.c bug), use (1,1) or more.
Standard library only; run with the msys python.
"""
import json, socket, sys, time


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

    def button(self, down):
        return self.cmd("input-send-event", events=[{"type": "btn", "data": {"down": down, "button": "left"}}])


def main():
    port, w, h = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
    action = sys.argv[4]
    x = int(sys.argv[5]) * 32767 // w if len(sys.argv) > 5 else 0
    y = int(sys.argv[6]) * 32767 // h if len(sys.argv) > 6 else 0
    q = Qmp(port)
    if action == "steps":
        sx, sy = max(x - 80 * 32767 // w, 1), min(y + 40 * 32767 // h, 32767)
        q.move(sx, sy); time.sleep(0.4)
        for i in range(1, 11):
            q.move(sx + (x - sx) * i // 10, sy + (y - sy) * i // 10); time.sleep(0.06)
        time.sleep(0.4)
    elif action == "near":
        q.move(max(x - 14 * 32767 // w, 1), y); time.sleep(0.6)
        q.move(max(x - 1 * 32767 // w, 1), y); time.sleep(0.2); q.move(x, y); time.sleep(0.4)
    elif action in ("move", "click", "dclick", "down", "up"):
        if action != "up":
            q.move(x, y); time.sleep(0.8); q.move(x, y); time.sleep(0.4)
    if action == "click":
        q.button(True); time.sleep(0.15); q.button(False)
    elif action == "dclick":
        for _ in range(2):
            q.button(True); time.sleep(0.08); q.button(False); time.sleep(0.08)
    elif action == "down":
        q.button(True)
    elif action == "up":
        q.button(False)
    elif action not in ("move", "steps", "near"):
        sys.exit("unknown action " + action)


main()
