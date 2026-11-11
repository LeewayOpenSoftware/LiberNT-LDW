#!/usr/bin/env python3
"""
PROJECT:     LiberNT - Leeway Development Works
LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
PURPOSE:     Consumer compatibility checks for the BuildLab / BuildLabEx shapes.
COPYRIGHT:   Copyright 2026 Leeway Open Software

Run:  python3 -I sdk/tools/lbi/tests/test_consumers.py

Owner condition for the coordinated BuildLab + BuildLabEx modernization: every
in-tree reader of the registry BuildLab value must parse or degrade without
error on the new shape. The two readers that transform the string
(win32ss/user/ntuser/desktop.c watermark, dll/cpl/sysdm/general.c
GetSystemRelease) share one algorithm; it is transliterated here step by step
so the host can run it over the old shapes, the new shapes and the unallocated
marker. The raw readers (drwtsn32, systemsettings, diagdata, kd info dump,
debugger vertarget, lab launchers) only need the strings to be bounded
printable ASCII, which the grammar guarantees and this file checks.
"""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import lbi_format  # noqa: E402

OLD_TAGGED = "20261106-nt10.0-044-2812-g4fadd7f.Clang_24.0.0"
OLD_BARE_BASE = "20261104-36a2889.Clang_24.0.0"
OLD_BARE_MAIN = "20261106-4fadd7f.Clang_24.0.0"
NEW_BUILDLAB = "1042.ldw_dev.261107-0207"
NEW_BUILDLABEX = "1042.1.amd64fre.ldw_dev.261107-0207"
NEW_RESPIN = "1042.ldw_rel.261110-1200"
UNALLOCATED = lbi_format.UNALLOCATED
KERNEL_VERSION_STR = "0.4.17-amd64-dev"
BUFFER_WCHARS = 256  # desktop.c BuildLabBuffer[256], sysdm szLab[256]


def hide_hash_and_date(lab):
    """desktop.c lines 403-439 / general.c lines 644-665, one for one."""
    assert len(lab) + 1 <= BUFFER_WCHARS, "string does not fit the consumer buffer"
    s = list(lab)
    # Hide the git commit count and hash, preserving the tag and toolchain.
    hash_pos = None
    scan = 0
    while True:
        i = "".join(s).find("-g", scan)
        if i < 0:
            break
        hash_pos = i
        scan = i + 2
    if hash_pos is not None:
        revision = hash_pos
        while revision > 0 and s[revision - 1].isdigit():
            revision -= 1
        j = hash_pos + 2
        while j < len(s) and s[j] in "0123456789abcdefABCDEF":
            j += 1
        compiler = j
        if revision > 0 and revision < hash_pos and s[revision - 1] == "-" and compiler > hash_pos + 2 \
                and (compiler == len(s) or s[compiler] == "."):
            revision -= 1
            s = s[:revision] + s[compiler:]
    # strip a leading all-digit date
    k = 0
    while k < len(s) and s[k].isdigit():
        k += 1
    if k > 0 and k < len(s) and s[k] == "-":
        s = s[k + 1:]
    # map the last '_' to '-'
    text = "".join(s)
    u = text.rfind("_")
    if u >= 0:
        text = text[:u] + "-" + text[u + 1:]
    arch = KERNEL_VERSION_STR.split("-")[1] if "-" in KERNEL_VERSION_STR else KERNEL_VERSION_STR
    return text, arch


def sysdm_build_line(lab):
    text, arch = hide_hash_and_date(lab)
    return " %s-%s" % (text, arch)


class TestTransformingReaders(unittest.TestCase):
    def test_old_shapes_unchanged_behaviour(self):
        self.assertEqual(sysdm_build_line(OLD_TAGGED), " nt10.0-044.Clang-24.0.0-amd64")
        self.assertEqual(sysdm_build_line(OLD_BARE_BASE), " 36a2889.Clang-24.0.0-amd64")   # V1 rev2 gui-2.png
        self.assertEqual(sysdm_build_line(OLD_BARE_MAIN), " 4fadd7f.Clang-24.0.0-amd64")

    def test_new_buildlab_degrades_to_readable_text(self):
        self.assertEqual(sysdm_build_line(NEW_BUILDLAB), " 1042.ldw-dev.261107-0207-amd64")
        self.assertEqual(sysdm_build_line(NEW_RESPIN), " 1042.ldw-rel.261110-1200-amd64")
        self.assertEqual(sysdm_build_line(NEW_BUILDLABEX), " 1042.1.amd64fre.ldw-dev.261107-0207-amd64")

    def test_unallocated_and_empty(self):
        self.assertEqual(sysdm_build_line(UNALLOCATED), " unallocated-amd64")
        self.assertEqual(sysdm_build_line(""), " -amd64")

    def test_every_grammar_label_fits_and_survives(self):
        import random
        rnd = random.Random(29)
        for _ in range(2000):
            ch = rnd.choice(lbi_format.CHANNELS) if rnd.random() < 0.5 else rnd.choice("abcdefghijklmnopqrstuvwxyz") + "".join(
                rnd.choice("abcdefghijklmnopqrstuvwxyz0123456789_") for _ in range(rnd.randint(0, 31)))
            stamp = "%02d%02d%02d-%02d%02d" % (rnd.randint(0, 99), rnd.randint(1, 12), rnd.randint(1, 28),
                                               rnd.randint(0, 23), rnd.randint(0, 59))
            bl = lbi_format.format_buildlab(rnd.randint(1, 9999999), ch, stamp)
            text, _ = hide_hash_and_date(bl)
            # no "-g<hex>" can occur in a grammar string, so nothing is cut; only '_' -> '-'
            self.assertEqual(text, bl[:bl.rfind("_")] + "-" + bl[bl.rfind("_") + 1:] if "_" in bl else bl)
            lx = lbi_format.format_label(rnd.randint(1, 9999999), rnd.randint(1, 99999), rnd.choice(lbi_format.ARCHES),
                                         rnd.choice(lbi_format.TYPES), ch, stamp)
            hide_hash_and_date(lx)


class TestRawReaders(unittest.TestCase):
    """drwtsn32 sysinfo.cpp, systemsettings page_system.cpp, diagdata.c, kd/kdinfo.c,
    the debugger's vertarget (KdDebuggerDataBlock.NtBuildLab) and the lab launchers
    (reg query) print the value as it is."""

    def test_bounded_printable_ascii(self):
        longest_label = lbi_format.format_label(9999999, 99999, "riscv64", "chk", "z" * 32, "991231-2359")
        longest_buildlab = lbi_format.format_buildlab(9999999, "z" * 32, "991231-2359")
        for s in (longest_label, longest_buildlab, NEW_BUILDLAB, NEW_BUILDLABEX, UNALLOCATED):
            self.assertLess(len(s), 128)
            self.assertTrue(all(32 < ord(c) < 127 for c in s), s)
            self.assertNotIn(" ", s)
        self.assertEqual(len(longest_label), 69)

    def test_kdinfo_line_shape(self):
        # ntoskrnl/kd/kdinfo.c prints "build=%s\nbuildlabex=%s\n" into a bounded text buffer;
        # neither value can contain a newline or a '=' that would break the key=value dump.
        for s in (NEW_BUILDLAB, NEW_BUILDLABEX, UNALLOCATED):
            self.assertNotIn("\n", s)
            self.assertNotIn("=", s)


if __name__ == "__main__":
    unittest.main(verbosity=2)
