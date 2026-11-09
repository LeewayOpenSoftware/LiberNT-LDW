#!/usr/bin/env python3
"""Host test of the boot-class swap through the REAL applier (pending.c behind the host shim).

Builds a fake volume with a kernel/HAL/bootvid trio, a dev-signed manifest and a swap-file plan, then runs
host_applier.exe in several scenarios and checks the result by content hash:
  A  clean volume        : trio replaced, X.old = originals, journal pending-first-boot, bootstate ARMED, and the
                           dirty query happened after 0 mutating operations (B1 review M4, proven by execution)
  B  dirty volume        : S1_FAKE_DIRTY=1 -> refused, files untouched, nothing written before the query
  C  second boot         : the set is in place -> no-op
  D  torn set            : one X replaced by garbage after A -> restored from X.old and swapped again (N1/M2)
  E  missing X           : X deleted -> restored by copying X.old, then swapped (M2)
  F  mixed generation    : one X back to the original, two new -> whole set restored then swapped (N1)
Usage: test-swap-host.py <host_applier.exe> <work-dir>   (build: see README.md in this directory)
No private key or password is printed or stored; the dev key is read from %USERPROFILE%\\.leeway-dev-signing.
"""
import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))


class mk:  # the dev signer (same key and scheme as the update tools); the key stays outside the repository
    KEYDIR = os.path.realpath(os.path.expandvars(os.environ.get("LN_DEV_SIGNING_DIR", r"%USERPROFILE%\.leeway-dev-signing")))

    @staticmethod
    def load_key():
        from cryptography.hazmat.primitives.serialization import pkcs12
        pw = open(os.path.join(mk.KEYDIR, "pfx-password.txt"), "rb").read().strip()
        return pkcs12.load_key_and_certificates(
            open(os.path.join(mk.KEYDIR, "Leeway-LiberNT-Update-DEV.pfx"), "rb").read(), pw)[0]

    @staticmethod
    def sign(data, key):
        from cryptography.hazmat.primitives import hashes
        from cryptography.hazmat.primitives.asymmetric import padding
        return key.sign(data, padding.PKCS1v15(), hashes.SHA256())

NAMES = ["bootvid.dll", "hal.dll", "ntkrnlmp.exe"]
PKG, VER, MACHINE = "ln.kernel-trio", "9.9", "host-test-machine"


def sha(b):
    return hashlib.sha256(b).hexdigest()


def blob(tag, n=70000):
    seed = (tag * 64).encode()
    return (seed * (n // len(seed) + 1))[:n]


def build(root):
    if os.path.exists(root):
        for _ in range(20):
            try:
                shutil.rmtree(root)
                break
            except OSError:
                time.sleep(0.5)       # antivirus or a just-exited process may still hold a handle
        else:
            raise SystemExit("cannot clean " + root)
    sysdir = os.path.join(root, "Windows", "System32")
    sv = os.path.join(root, "LiberNT", "Servicing")
    for d in (sysdir, os.path.join(sv, "pending"), os.path.join(sv, "journal"), os.path.join(sv, "backup"),
              os.path.join(sv, "logs"), os.path.join(sv, "store", "objects"),
              os.path.join(sv, "store", "manifests", PKG, VER)):
        os.makedirs(d)
    orig, new = {}, {}
    for n in NAMES:
        orig[n] = blob("old-" + n)
        new[n] = blob("new-" + n, 90000)
        open(os.path.join(sysdir, n), "wb").write(orig[n])
        open(os.path.join(sv, "store", "objects", sha(new[n])), "wb").write(new[n])
    manifest = json.dumps({
        "schemaVersion": 1, "packageId": PKG, "releaseVersion": VER, "type": "system-component",
        "targetFamily": "LiberNT", "architecture": "amd64",
        "payloads": [{"path": n, "target": "Windows\\System32\\" + n, "sha256": sha(new[n]), "class": "boot"}
                     for n in NAMES]}, indent=2).encode()
    mdir = os.path.join(sv, "store", "manifests", PKG, VER)
    open(os.path.join(mdir, "manifest.json"), "wb").write(manifest)
    open(os.path.join(mdir, "manifest.sig"), "wb").write(mk.sign(manifest, mk.load_key()))
    digest = sha(manifest)
    txid = sha(("\n".join([MACHINE, PKG, VER, digest, "install"])).encode())
    plan = {"schema": 1, "transactionId": txid, "target": {"machineId": MACHINE, "arch": "amd64"},
            "package": {"id": PKG, "version": VER, "digest": digest}, "operation": "install",
            "requiresReboot": True,
            "operations": [{"seq": i + 1, "type": "swap-file", "component": PKG, "version": VER,
                            "source": "store/objects/" + sha(new[n]), "target": "\\Windows\\System32\\" + n,
                            "sha256": sha(new[n]), "originalSha256": sha(orig[n]), "backup": "backup/x/%04d" % (i + 1)}
                           for i, n in enumerate(NAMES)]}
    open(os.path.join(sv, "pending", "plan.json"), "w").write(json.dumps(plan))
    return sysdir, sv, orig, new


def run(exe, root, env=None):
    e = dict(os.environ)
    e.pop("S1_FAKE_DIRTY", None)
    if env:
        e.update(env)
    p = subprocess.run([exe, root], capture_output=True, text=True, env=e, timeout=120)
    global last_rc
    last_rc = p.returncode
    if p.returncode == 3:
        print(p.stdout)
        print("FAIL the applier wrote to the volume before the dirty check (exit 3)")
        raise SystemExit(1)
    return p.stdout + p.stderr


def h(path):
    return sha(open(path, "rb").read()) if os.path.exists(path) else None


def old_of(path):
    return os.path.splitext(path)[0] + ".old"


fails = []
last_rc = 0


def retry(fn):
    # a freshly written executable can be held for a moment by a real-time scanner on the host
    for _ in range(60):
        try:
            return fn()
        except OSError:
            time.sleep(0.5)
    return fn()


def put(path, data):
    def w():
        with open(path, "wb") as f:
            f.write(data)
    retry(w)


def check(name, cond):
    print(("PASS " if cond else "FAIL ") + name)
    if not cond:
        fails.append(name)


def trio_is(sysdir, table):
    return all(h(os.path.join(sysdir, n)) == sha(table[n]) for n in NAMES)


def main():
    exe, work = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    root = os.path.join(work, "root")

    # A: clean
    sysdir, sv, orig, new = build(root)
    out = run(exe, root)
    check("A dirty query happened after 0 mutating operations", "dirty query after 0 mutating operations" in out)
    check("A no write-before-check FAIL line and exit code 0", "FAIL" not in out and last_rc == 0)
    check("A trio replaced by the new files", trio_is(sysdir, new))
    check("A X.old == the originals (coherent set)",
          all(h(old_of(os.path.join(sysdir, n))) == sha(orig[n]) for n in NAMES))
    check("A no X.new / X.pre left", not any(os.path.exists(os.path.join(sysdir, n + s))
                                              for n in NAMES for s in (".new", ".pre")))
    journal = open(os.path.join(sv, "journal", "transactions.jsonl")).read()
    check("A journal has pending-first-boot", "pending-first-boot" in journal)
    attempt = open(os.path.join(sv, "pending", "attempt.json")).read()
    check("A attempt reset with lastResult swapped", "swapped" in attempt)
    bs = os.path.join(sv, "pending", "bootstate")
    check("A bootstate ARMED", os.path.exists(bs) and open(bs).read().startswith("ARMED"))

    # C: second boot -> no-op
    before = {n: h(os.path.join(sysdir, n)) for n in NAMES}
    out = run(exe, root)
    check("C second boot leaves the set unchanged", before == {n: h(os.path.join(sysdir, n)) for n in NAMES})
    check("C dirty query not required when nothing to swap", "dirty query" not in out)

    # D: torn set
    sysdir, sv, orig, new = build(root)
    run(exe, root)
    put(os.path.join(sysdir, "hal.dll"), b"torn" * 100)
    run(exe, root)
    check("D torn hal.dll: set restored and swapped again", trio_is(sysdir, new))
    check("D .old set still the originals", all(h(old_of(os.path.join(sysdir, n))) == sha(orig[n]) for n in NAMES))

    # E: X missing
    sysdir, sv, orig, new = build(root)
    run(exe, root)
    retry(lambda: os.remove(os.path.join(sysdir, "ntkrnlmp.exe")))
    run(exe, root)
    check("E missing ntkrnlmp.exe: restored from X.old and swapped", trio_is(sysdir, new))

    # F: mixed generation
    sysdir, sv, orig, new = build(root)
    run(exe, root)
    put(os.path.join(sysdir, "bootvid.dll"), orig["bootvid.dll"])
    run(exe, root)
    check("F mixed generation: whole set ends new", trio_is(sysdir, new))

    # B: dirty volume
    sysdir, sv, orig, new = build(root)
    out = run(exe, root, {"S1_FAKE_DIRTY": "1"})
    check("B dirty query after 0 mutating operations (boot.log append counted too)",
          "dirty query after 0 mutating operations" in out and last_rc == 0)
    check("B files untouched", trio_is(sysdir, orig))
    check("B nothing staged", not any(os.path.exists(os.path.join(sysdir, n + s))
                                       for n in NAMES for s in (".new", ".pre", ".old")))
    journal = open(os.path.join(sv, "journal", "transactions.jsonl")).read() if os.path.exists(
        os.path.join(sv, "journal", "transactions.jsonl")) else ""
    check("B refusal journaled as failed", "failed" in journal and "applying" not in journal)

    print("\n%d failure(s)" % len(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
