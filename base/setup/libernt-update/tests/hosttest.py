#!/usr/bin/env python3
"""Host test for libernt-update-bootstrap.exe (offline/image mode).

Drives the built bootstrap binary against a fake target root and a
RegSaveKey-generated SYSTEM hive. Requires administrator (SeRestoreName for
RegLoadKey/SaveKey and for setting the O:SY owner on the servicing tree).

Usage: python hosttest.py <path to libernt-update-bootstrap.exe> [devkey.pem|devkey.pfx]
"""

import ctypes
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import winreg

FAILURES = 0
CHECKS = 0


def check(cond, msg):
    global FAILURES, CHECKS
    CHECKS += 1
    if cond:
        print(f"ok:   {msg}")
    else:
        print(f"FAIL: {msg}")
        FAILURES += 1


def enable_privilege(name):
    k32 = ctypes.windll.kernel32
    adv = ctypes.windll.advapi32
    k32.GetCurrentProcess.restype = ctypes.c_void_p
    k32.GetCurrentProcess.argtypes = []
    adv.LookupPrivilegeValueW.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_longlong)]
    adv.OpenProcessToken.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_void_p)]
    luid = ctypes.c_longlong()
    if not adv.LookupPrivilegeValueW(None, name, ctypes.byref(luid)):
        return False
    class LUID_AND_ATTR(ctypes.Structure):
        _fields_ = [("Luid", ctypes.c_longlong), ("Attributes", ctypes.c_ulong)]
    class TOKEN_PRIV(ctypes.Structure):
        _fields_ = [("Count", ctypes.c_ulong), ("Priv", LUID_AND_ATTR)]
    tk = ctypes.c_void_p()
    if not adv.OpenProcessToken(k32.GetCurrentProcess(), 0x28, ctypes.byref(tk)):
        return False
    tp = TOKEN_PRIV(1, LUID_AND_ATTR(luid.value, 0x2))
    ok = adv.AdjustTokenPrivileges(tk, False, ctypes.byref(tp), 0, None, None)
    return bool(ok) and ctypes.GetLastError() == 0


def make_system_hive(path):
    """Create a minimal SYSTEM hive: Select\\Current + ControlSet001\\Control\\
    Session Manager\\BootExecute, saved with RegSaveKey."""
    root_name = r"SOFTWARE\HostTestBootstrapHive"
    key = winreg.CreateKeyEx(winreg.HKEY_LOCAL_MACHINE, root_name, 0,
                             winreg.KEY_ALL_ACCESS)
    select = winreg.CreateKeyEx(key, "Select", 0, winreg.KEY_ALL_ACCESS)
    winreg.SetValueEx(select, "Current", 0, winreg.REG_DWORD, 1)
    sm = winreg.CreateKeyEx(key, r"ControlSet001\Control\Session Manager", 0,
                            winreg.KEY_ALL_ACCESS)
    winreg.SetValueEx(sm, "BootExecute", 0, winreg.REG_MULTI_SZ,
                      ["autocheck", "autochk *"])
    winreg.CloseKey(sm)
    winreg.CloseKey(select)
    winreg.SaveKey(key, path)
    winreg.CloseKey(key)
    winreg.DeleteKey(winreg.HKEY_LOCAL_MACHINE, root_name + r"\Select")
    winreg.DeleteKey(winreg.HKEY_LOCAL_MACHINE, root_name + r"\ControlSet001\Control\Session Manager")
    winreg.DeleteKey(winreg.HKEY_LOCAL_MACHINE, root_name + r"\ControlSet001\Control")
    winreg.DeleteKey(winreg.HKEY_LOCAL_MACHINE, root_name + r"\ControlSet001")
    winreg.DeleteKey(winreg.HKEY_LOCAL_MACHINE, root_name)


def hive_bootexecute(hive_path, subkey, loader):
    winreg.LoadKey(winreg.HKEY_LOCAL_MACHINE, subkey, hive_path)
    try:
        key = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                             subkey + r"\ControlSet001\Control\Session Manager")
        try:
            value, _ = winreg.QueryValueEx(key, "BootExecute")
        finally:
            winreg.CloseKey(key)
    finally:
        winreg.UnLoadKey(winreg.HKEY_LOCAL_MACHINE, subkey)
    return list(value)


def load_key(key_path):
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.serialization import pkcs12
    with open(key_path, "rb") as f:
        blob = f.read()
    if key_path.lower().endswith(".pfx"):
        password_path = os.path.splitext(key_path)[0] + "-password.txt"
        if not os.path.exists(password_path):
            password_path = os.path.join(os.path.dirname(key_path), "pfx-password.txt")
        with open(password_path, "rb") as pf:
            password = pf.read().strip()
        key, _, _ = pkcs12.load_key_and_certificates(blob, password)
        return key
    return serialization.load_pem_private_key(blob, password=None)


def sign(data, key_path):
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import padding
    return load_key(key_path).sign(data, padding.PKCS1v15(), hashes.SHA256())


def build_bundle(bundle_dir, files, key_path):
    os.makedirs(os.path.join(bundle_dir, "files"), exist_ok=True)
    entries = []
    for name, dest, required, payload in files:
        src = os.path.join("files", name)
        with open(os.path.join(bundle_dir, src), "wb") as f:
            f.write(payload)
        entries.append({
            "name": name,
            "source": src,
            "dest": dest,
            "sha256": hashlib.sha256(payload).hexdigest(),
            "required": required,
        })
    manifest = json.dumps({"schema": 1, "binaries": entries,
                           "shortcuts": [{
                               "name": "ln-hosttest.lnk",
                               "target": r"LiberNT\Servicing\bin\libernt-update.exe",
                               "arguments": "/query",
                           }]},
                          separators=(",", ":")).encode()
    with open(os.path.join(bundle_dir, "bootstrap-manifest.json"), "wb") as f:
        f.write(manifest)
    sig = sign(manifest, key_path)
    with open(os.path.join(bundle_dir, "bootstrap-manifest.sig"), "wb") as f:
        f.write(sig)


def run(exe, *args):
    r = subprocess.run([exe, *args], capture_output=True, timeout=120)
    out = r.stdout.decode('utf-16-le', errors='replace')
    err = r.stderr.decode('utf-16-le', errors='replace')
    return r.returncode, out + err


def main():
    exe = os.path.abspath(sys.argv[1])
    devkey = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        os.environ["USERPROFILE"], ".leeway-dev-signing", "Leeway-LiberNT-Update-DEV.pfx")
    devkey = os.path.abspath(devkey)

    tmp = tempfile.mkdtemp(prefix="bs-hosttest-")
    root = os.path.join(tmp, "target")
    os.makedirs(os.path.join(root, "Windows", "System32", "config"))
    hive_path = os.path.join(root, "Windows", "System32", "config", "SYSTEM")
    open(hive_path, "wb").close()  # placeholder; dry-run never opens the hive
    print(f"fake target root: {root}")

    payload_pending = os.urandom(4096)
    payload_cli = os.urandom(2048)
    bundle = os.path.join(tmp, "bundle")
    build_bundle(bundle, [
        ("pending.exe", r"Windows\System32\LiberNTUpdatePending.exe", True, payload_pending),
        ("libernt-update.exe", r"LiberNT\Servicing\bin\libernt-update.exe", False, payload_cli),
    ], devkey)

    # 1. dry-run writes nothing (no privileges needed)
    rc, out = run(exe, "install", "--image", root, "--bundle", bundle, "--dry-run")
    check(rc == 0, f"dry-run exit 0 (rc={rc})")
    check("BootExecute +=" in out, "dry-run prints BootExecute plan")
    check("pin development public key" in out, "dry-run prints key pinning")
    check(not os.path.exists(os.path.join(root, "LiberNT")),
          "dry-run created nothing")

    # 2. unsigned manifest refused (no privileges needed)
    bundle_bad = os.path.join(tmp, "bundle-bad")
    shutil.copytree(bundle, bundle_bad)
    with open(os.path.join(bundle_bad, "bootstrap-manifest.sig"), "wb") as f:
        f.write(os.urandom(256))
    rc, out = run(exe, "install", "--image", root, "--bundle", bundle_bad)
    check(rc != 0 and "refused" in out, f"unsigned bundle refused (rc={rc})")
    check(not os.path.exists(os.path.join(root, "LiberNT")),
          "refused bundle created nothing")

    if not (enable_privilege("SeRestorePrivilege") and
            enable_privilege("SeBackupPrivilege")):
        print("SKIP: SeRestore/SeBackup privilege not held (need administrator);"
              " privileged cases (install/uninstall/hive/DACL) not run")
        print(f"{CHECKS} checks, {FAILURES} failures (partial pass)")
        shutil.rmtree(tmp, ignore_errors=True)
        return 3

    os.remove(hive_path)  # RegSaveKey fails if the file already exists
    make_system_hive(hive_path)

    # 3. real install
    rc, out = run(exe, "install", "--image", root, "--bundle", bundle)
    check(rc == 0, f"install exit 0 (rc={rc}): {out[-300:] if rc else ''}")
    pending = os.path.join(root, "Windows", "System32", "LiberNTUpdatePending.exe")
    check(open(pending, "rb").read() == payload_pending, "pending.exe installed byte-identical")
    cli = os.path.join(root, "LiberNT", "Servicing", "bin", "libernt-update.exe")
    check(open(cli, "rb").read() == payload_cli, "CLI installed byte-identical")
    check(os.path.exists(os.path.join(root, "LiberNT", "Servicing", "bootstrap", "installed.json")),
          "installed.json written")
    with open(os.path.join(root, "LiberNT", "Servicing", "bootstrap", "installed.json"),
              encoding="utf-8") as f:
        inv = json.load(f)
    check(inv.get("schema") == 1 and isinstance(inv.get("files"), list) and len(inv["files"]) >= 2,
          f"installed.json is strict JSON with a files list: schema={inv.get('schema')}, "
          f"files={len(inv.get('files', []))}")
    check(any(rec.get("path") == r"Windows\System32\LiberNTUpdatePending.exe"
              for rec in inv.get("files", [])),
          "installed.json path round-trips with its backslashes")
    key = os.path.join(root, "LiberNT", "Servicing", "store", "dev-public-key.bin")
    check(os.path.exists(key) and os.path.getsize(key) == 256, "dev public key pinned (256 bytes)")
    lnk = os.path.join(root, "ProgramData", "Microsoft", "Windows", "Start Menu",
                       "Programs", "ln-hosttest.lnk")
    check(os.path.exists(lnk), "start menu shortcut created from manifest")
    be = hive_bootexecute(hive_path, "HostTestVerify")
    check(be[:2] == ["autocheck", "autochk *"] and "LiberNTUpdatePending" in be,
          f"hive BootExecute updated preserving order: {be}")
    check(be.index("LiberNTUpdatePending") > be.index("autochk *"),
          "entry landed after autocheck")

    # 4. idempotence: second install, no duplicate BootExecute entry
    rc, out = run(exe, "install", "--image", root, "--bundle", bundle)
    check(rc == 0, f"second install exit 0 (rc={rc})")
    be = hive_bootexecute(hive_path, "HostTestVerify2")
    check(be.count("LiberNTUpdatePending") == 1, f"no duplicate BootExecute entry: {be}")

    # 5. uninstall refuses with a pending plan
    plan = os.path.join(root, "LiberNT", "Servicing", "pending", "plan.json")
    with open(plan, "w") as f:
        f.write("{}")
    rc, out = run(exe, "uninstall", "--image", root)
    check(rc != 0 and "refused" in out, f"uninstall refused with pending plan (rc={rc})")
    os.remove(plan)

    # 6. uninstall with export
    export_dir = os.path.join(tmp, "exported")
    rc, out = run(exe, "uninstall", "--image", root, "--export", export_dir)
    check(rc == 0, f"uninstall exit 0 (rc={rc})")
    check(not os.path.exists(pending), "pending.exe removed")
    check(not os.path.exists(cli), "CLI removed")
    check(not os.path.exists(lnk), "start menu shortcut removed by uninstall")
    be = hive_bootexecute(hive_path, "HostTestVerify3")
    check("LiberNTUpdatePending" not in be and be[:2] == ["autocheck", "autochk *"],
          f"BootExecute reverted: {be}")
    check(os.path.exists(os.path.join(export_dir, "bootstrap.log")),
          "bootstrap.log exported (logs kept)")

    shutil.rmtree(tmp, ignore_errors=True)
    print(f"{CHECKS} checks, {FAILURES} failures")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
