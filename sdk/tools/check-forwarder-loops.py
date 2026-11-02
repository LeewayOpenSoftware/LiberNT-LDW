#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 Leeway Open Software
"""check-forwarder-loops: fail when the built system DLLs contain an export loop.

usage: python -I check-forwarder-loops.py <build-or-system32-dir> [--quiet]   (searched recursively)

Two kinds of loop make a thread spin or fault at the first call, with nothing in the log:

  forwarder cycle   kernel32!A forwards to kernelbase.A, which forwards back to kernel32.A
  stub cycle        a DLL exports A as the 6-byte stub `jmp [rip+x]` through its own import
                    of other!A, and other!A resolves (directly or through forwarders) to the
                    stub again: the import slot then holds the stub's own address and the
                    stub jumps to itself (kernelbase!IsDebuggerPresent on the Nov-1 build)

Only DLLs found in the directory are followed; a forwarder to a DLL that is not there (an API
set, for instance) ends the chain without an error. Exit status: 0 no loop, 1 loop(s) found,
2 usage or read error. No third-party module is needed.
"""
import os
import struct
import sys


class Pe:
    def __init__(self, path):
        d = open(path, "rb").read()
        self.data = d
        if d[:2] != b"MZ":
            raise ValueError("not a PE")
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        if d[pe:pe + 4] != b"PE\0\0":
            raise ValueError("not a PE")
        nsec, = struct.unpack_from("<H", d, pe + 6)
        optsz, = struct.unpack_from("<H", d, pe + 20)
        opt = pe + 24
        magic, = struct.unpack_from("<H", d, opt)
        self.is64 = magic == 0x20B
        dd = opt + (112 if self.is64 else 96)
        self.exp_rva, self.exp_size = struct.unpack_from("<II", d, dd)
        self.imp_rva, _ = struct.unpack_from("<II", d, dd + 8)
        self.sections = []
        sec = opt + optsz
        for i in range(nsec):
            vsz, va, rawsz, raw = struct.unpack_from("<IIII", d, sec + 40 * i + 8)
            self.sections.append((va, max(vsz, rawsz), raw, rawsz))
        self.exports = {}       # name -> ("code", rva) | ("fwd", "DLL.Name")
        self.imports = {}       # iat slot rva -> (dll lowercase, name)
        self._read_exports()
        self._read_imports()

    def off(self, rva):
        for va, sz, raw, rawsz in self.sections:
            if va <= rva < va + sz:
                o = raw + (rva - va)
                return o if o < len(self.data) else None
        return None

    def cstr(self, rva):
        o = self.off(rva)
        if o is None:
            return None
        e = self.data.find(b"\0", o)
        return self.data[o:e].decode("latin-1") if e >= 0 else None

    def _read_exports(self):
        if not self.exp_rva:
            return
        o = self.off(self.exp_rva)
        if o is None:
            return
        nfun, nnames, afn, anm, aord = struct.unpack_from("<IIIII", self.data, o + 20)
        for i in range(nnames):
            no = self.off(anm + 4 * i)
            oo = self.off(aord + 2 * i)
            if no is None or oo is None:
                continue
            name = self.cstr(struct.unpack_from("<I", self.data, no)[0])
            idx, = struct.unpack_from("<H", self.data, oo)
            fo = self.off(afn + 4 * idx)
            if name is None or fo is None:
                continue
            rva, = struct.unpack_from("<I", self.data, fo)
            if self.exp_rva <= rva < self.exp_rva + self.exp_size:
                self.exports[name] = ("fwd", self.cstr(rva))
            else:
                self.exports[name] = ("code", rva)

    def _read_imports(self):
        if not self.imp_rva:
            return
        step = 8 if self.is64 else 4
        o = self.off(self.imp_rva)
        while o is not None:
            oft, _, _, name, ft = struct.unpack_from("<IIIII", self.data, o)
            if not name and not ft:
                break
            dll = (self.cstr(name) or "").lower()
            lookup = oft or ft
            k = 0
            while True:
                lo = self.off(lookup + step * k)
                if lo is None:
                    break
                val = int.from_bytes(self.data[lo:lo + step], "little")
                if val == 0:
                    break
                if not val >> (step * 8 - 1):          # by name (not by ordinal)
                    nm = self.cstr((val & 0x7FFFFFFF) + 2)
                    if nm:
                        self.imports[ft + step * k] = (dll, nm)
                k += 1
            o += 20


def load(directory):
    dlls = {}
    # a build tree keeps each module in its own directory: search below `directory`, first one wins
    for root, dirs, files in os.walk(directory):
        dirs.sort()
        for f in sorted(files):
            if f.lower().endswith((".dll", ".drv", ".cpl", ".ocx")) and f.lower() not in dlls:
                try:
                    dlls[f.lower()] = Pe(os.path.join(root, f))
                except (ValueError, struct.error, OSError):
                    pass
    return dlls


def stub_target(pe, rva):
    """If the export at rva is `jmp [rip+disp32]` return the import it goes through, else None."""
    o = pe.off(rva)
    if o is None or pe.data[o:o + 2] != b"\xff\x25":
        return None
    disp, = struct.unpack_from("<i", pe.data, o + 2)
    return pe.imports.get(rva + 6 + disp) if pe.is64 else None


def resolve(dlls, dll, name, seen):
    """Follow dll!name; return the chain as a list, ending in 'LOOP at X' when it cycles."""
    key = (dll, name)
    if key in seen:
        return ["LOOP back to %s!%s" % key]
    seen = seen | {key}
    pe = dlls.get(dll)
    if pe is None or name not in pe.exports:
        return []
    kind, val = pe.exports[name]
    if kind == "fwd":
        if not val or "." not in val:
            return []
        target_dll, target_name = val.split(".", 1)
        target_dll = target_dll.lower() + ".dll"
        if target_name.startswith("#"):
            return []
        return ["%s!%s forwards to %s" % (dll, name, val)] + resolve(dlls, target_dll, target_name, seen)
    imp = stub_target(pe, val)
    if imp is not None:
        return ["%s!%s is a jump stub through its import %s!%s" % (dll, name, imp[0], imp[1])] + \
            resolve(dlls, imp[0], imp[1], seen)
    return []


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    quiet = "--quiet" in argv
    try:
        dlls = load(argv[1])
    except OSError as e:
        print("check-forwarder-loops: %s" % e)
        return 2
    loops = []
    reported = set()
    for dll, pe in dlls.items():
        for name in pe.exports:
            chain = resolve(dlls, dll, name, frozenset())
            if chain and chain[-1].startswith("LOOP"):
                members = frozenset(step.split(" ")[0] for step in chain[:-1])   # report each cycle once
                if members in reported:
                    continue
                reported.add(members)
                loops.append((dll, name, chain))
    if not quiet or loops:
        print("checked %d DLLs, %d export loop(s)" % (len(dlls), len(loops)))
    for dll, name, chain in loops:
        print("LOOP %s!%s" % (dll, name))
        for step in chain:
            print("    " + step)
    return 1 if loops else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
