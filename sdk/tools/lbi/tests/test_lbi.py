#!/usr/bin/env python3
"""
PROJECT:     LiberNT - Leeway Development Works
LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
PURPOSE:     Tests for the LBI grammar, allocator and generator (WS-B prototype).
COPYRIGHT:   Copyright 2026 Leeway Open Software

Run:  python3 -I sdk/tools/lbi/tests/test_lbi.py        (host, no build needed)
Needs git; the generator test also needs cmake (skipped when absent).

1. parse round-trip of the label grammar (fixed vectors + random fields + rejects)
2. parallel allocation uniqueness against a local bare ledger (N processes x M
   allocations, strictly unique sequences, burnt numbers never reused)
3. identity persistence: the allocation record, the generated lbi.h, the generated
   lbi.json and the BuildLab/BuildLabEx strings all carry the same identity
"""
import concurrent.futures
import json
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import unittest


def _load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def _read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def _write(path, text):
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
SDK = os.path.dirname(os.path.dirname(TOOLS))
ROOT = os.path.dirname(SDK)
sys.path.insert(0, TOOLS)
import lbi_format  # noqa: E402

ALLOC = os.path.join(TOOLS, "lbi_alloc.py")
GEN = os.path.join(SDK, "cmake", "lbi.cmake")


def run(args, **kw):
    return subprocess.run(args, capture_output=True, text=True, **kw)


class TestGrammar(unittest.TestCase):
    def test_vectors(self):
        for label, want in [
            ("1042.1.amd64fre.ldw_dev.261107-0207",
             dict(sequence=1042, revision=1, arch="amd64", type="fre", channel="ldw_dev", stamp="261107-0207", provisional=False)),
            ("0.0.x86chk.ldw_lab.260101-0000",
             dict(sequence=0, revision=0, arch="x86", type="chk", channel="ldw_lab", stamp="260101-0000", provisional=True)),
            ("9999999.99999.riscv64fre.a.991231-2359",
             dict(sequence=9999999, revision=99999, arch="riscv64", type="fre", channel="a", stamp="991231-2359", provisional=False)),
        ]:
            self.assertEqual(lbi_format.parse_label(label), want)
            self.assertEqual(lbi_format.format_label(want["sequence"], want["revision"], want["arch"],
                                                     want["type"], want["channel"], want["stamp"]), label)

    def test_rejects(self):
        for bad in ["", "1042.1.amd64.ldw_dev.261107-0207", "1042.1.amd64fre.Ldw_dev.261107-0207",
                    "1042.1.amd64fre.ldw-dev.261107-0207", "1042.1.amd64fre.ldw_dev.2611070207",
                    "1042.1.amd64fre.ldw_dev.261307-0207", "1042.1.amd64fre.ldw_dev.261107-2507",
                    "12345678.1.amd64fre.ldw_dev.261107-0207", "1042.1.amd64fre.ldw_dev.261107-0207 ",
                    "1042.1.arm64fre.%s.261107-0207" % ("c" * 33), "20261106-4fadd7f.GCC_13.2.0"]:
            with self.assertRaises(lbi_format.LbiFormatError, msg=bad):
                lbi_format.parse_label(bad)

    def test_random_roundtrip(self):
        rnd = random.Random(1107)
        for _ in range(2000):
            seq = rnd.choice([0, rnd.randint(1, 9999999)])
            rev = rnd.choice([0, rnd.randint(0, 99999)])
            arch = rnd.choice(lbi_format.ARCHES)
            bt = rnd.choice(lbi_format.TYPES)
            ch = rnd.choice("abcdefghijklmnopqrstuvwxyz") + "".join(
                rnd.choice("abcdefghijklmnopqrstuvwxyz0123456789_") for _ in range(rnd.randint(0, 31)))
            stamp = "%02d%02d%02d-%02d%02d" % (rnd.randint(0, 99), rnd.randint(1, 12), rnd.randint(1, 28),
                                               rnd.randint(0, 23), rnd.randint(0, 59))
            label = lbi_format.format_label(seq, rev, arch, bt, ch, stamp)
            p = lbi_format.parse_label(label)
            self.assertEqual((p["sequence"], p["revision"], p["arch"], p["type"], p["channel"], p["stamp"]),
                             (seq, rev, arch, bt, ch, stamp))
            bl = lbi_format.format_buildlab(seq, ch, stamp)
            self.assertEqual(lbi_format.parse_buildlab(bl), {"sequence": seq, "channel": ch, "stamp": stamp})

    def test_stamp_and_digest(self):
        self.assertEqual(lbi_format.stamp_from_iso("2026-11-07T02:07:00Z"), "261107-0207")
        d1 = lbi_format.inputs_digest("a" * 40, False, "amd64", "fre", "ldw_dev", "Clang", "18.1.8",
                                      ["DBG=0", "ENABLE_X=ON"], "leeway-provenance-manifest/v1")
        d2 = lbi_format.inputs_digest("a" * 40, False, "amd64", "fre", "ldw_dev", "Clang", "18.1.8",
                                      ["ENABLE_X=ON", "DBG=0", "DBG=0"], "leeway-provenance-manifest/v1")
        d3 = lbi_format.inputs_digest("a" * 40, True, "amd64", "fre", "ldw_dev", "Clang", "18.1.8",
                                      ["ENABLE_X=ON", "DBG=0"], "leeway-provenance-manifest/v1")
        self.assertEqual(d1, d2)          # order and duplicates of config items do not matter
        self.assertNotEqual(d1, d3)       # a dirty tree is a different input
        self.assertEqual(len(d1), 64)


def _alloc_one(args):
    ledger, out, idx, source = args
    r = run([sys.executable, "-I", ALLOC, "--ledger", ledger, "--out", os.path.join(out, "w%03d" % idx),
             "--source", source, "--arch", "amd64", "--type", "fre", "--channel", "ldw_test",
             "--worker", "w%03d" % idx])
    return idx, r.returncode, r.stdout.strip(), r.stderr.strip()


class TestAllocation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="lbi-test-")
        cls.ledger = os.path.join(cls.tmp, "ledger.git")
        run(["git", "init", "-q", "--bare", cls.ledger]).check_returncode()
        cls.source = os.path.join(cls.tmp, "src")
        run(["git", "init", "-q", cls.source]).check_returncode()
        with open(os.path.join(cls.source, "f"), "w") as f:
            f.write("x\n")
        env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t", GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t")
        run(["git", "-C", cls.source, "add", "f"], env=env).check_returncode()
        run(["git", "-C", cls.source, "commit", "-q", "-m", "init"], env=env).check_returncode()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_parallel_uniqueness(self):
        n = 24
        out = os.path.join(self.tmp, "par")
        base = len(run(["git", "--git-dir", self.ledger, "for-each-ref", "refs/lbi/seq/"]).stdout.split("\n")) - 1
        with concurrent.futures.ThreadPoolExecutor(max_workers=12) as ex:
            results = list(ex.map(_alloc_one, [(self.ledger, out, i, self.source) for i in range(n)]))
        fails = [r for r in results if r[1] != 0]
        self.assertEqual(fails, [], "allocations failed: %r" % fails[:3])
        labels = [r[2] for r in results]
        seqs = sorted(lbi_format.parse_label(l)["sequence"] for l in labels)
        self.assertEqual(seqs, list(range(base + 1, base + n + 1)),
                         "sequences must be exactly base+1..base+N, each once, no gaps, no burns")
        refs = run(["git", "--git-dir", self.ledger, "for-each-ref", "--format=%(refname)", "refs/lbi/seq/"]).stdout.split()
        self.assertEqual(sorted(int(r.rsplit("/", 1)[1]) for r in refs), list(range(1, base + n + 1)))
        ids = set()
        for i in range(n):
            rec = _load(os.path.join(out, "w%03d" % i, "lbi-allocation.json"))
            self.assertFalse(rec["provisional"])
            self.assertEqual(rec["ledger"]["ref"], "refs/lbi/seq/%d" % rec["sequence"])
            ids.add(rec["attempt_id"])
        self.assertEqual(len(ids), n)

    def test_burnt_numbers_never_reused(self):
        # "a failed build" = an allocation whose build never completes: its ref stays.
        before = run(["git", "--git-dir", self.ledger, "for-each-ref", "refs/lbi/seq/"]).stdout.count("\n")
        a = _alloc_one((self.ledger, os.path.join(self.tmp, "burn"), 900, self.source))
        self.assertEqual(a[1], 0, a[3])
        burnt = lbi_format.parse_label(a[2])["sequence"]
        b = _alloc_one((self.ledger, os.path.join(self.tmp, "burn"), 901, self.source))
        self.assertEqual(b[1], 0, b[3])
        self.assertEqual(lbi_format.parse_label(b[2])["sequence"], burnt + 1)
        after = run(["git", "--git-dir", self.ledger, "for-each-ref", "refs/lbi/seq/"]).stdout.count("\n")
        self.assertEqual(after, before + 2)

    def test_provisional_without_ledger(self):
        out = os.path.join(self.tmp, "prov")
        r = run([sys.executable, "-I", ALLOC, "--out", out, "--source", self.source, "--arch", "x86",
                 "--type", "chk", "--channel", "ldw_dev", "--timestamp", "2026-11-07T02:07:00Z"])
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.strip(), "0.0.x86chk.ldw_dev.261107-0207")
        rec = _load(os.path.join(out, "lbi-allocation.json"))
        self.assertTrue(rec["provisional"])
        self.assertEqual(rec["sequence"], 0)
        self.assertEqual(rec["buildlab"], "0.ldw_dev.261107-0207")
        self.assertTrue(rec["manifest_ref"].endswith(":attempt:" + rec["attempt_id"]))


@unittest.skipUnless(shutil.which("cmake"), "cmake not on PATH")
class TestGenerator(unittest.TestCase):
    """Runs sdk/cmake/lbi.cmake in script mode (-P) on an allocation and checks
    that lbi.h, lbi.json and the allocation agree: identity persistence across
    the artifacts the generator produces."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="lbi-gen-")
        self.source = os.path.join(self.tmp, "src")
        run(["git", "init", "-q", self.source]).check_returncode()
        env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t", GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t")
        _write(os.path.join(self.source, "f"), "x\n")
        run(["git", "-C", self.source, "add", "f"], env=env).check_returncode()
        run(["git", "-C", self.source, "commit", "-q", "-m", "init"], env=env).check_returncode()
        self.commit = run(["git", "-C", self.source, "rev-parse", "HEAD"]).stdout.strip()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _generate(self, alloc_dir, binary_dir, extra=()):
        r = run(["cmake", "-DLBI_SCRIPT_MODE=1",
                 "-DREACTOS_SOURCE_DIR=" + ROOT, "-DREACTOS_BINARY_DIR=" + binary_dir,
                 "-DLBI_SOURCE_DIR=" + self.source,
                 "-DLBI_ALLOCATION_FILE=" + os.path.join(alloc_dir, "lbi-allocation.cmake"),
                 "-DWINARCH=amd64", "-DDBG=0", "-DCMAKE_C_COMPILER_ID=Clang", "-DCMAKE_C_COMPILER_VERSION=18.1.8",
                 *extra, "-P", GEN])
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        h = _read(os.path.join(binary_dir, "sdk", "include", "reactos", "lbi.h"))
        j = _load(os.path.join(binary_dir, "lbi.json"))
        return h, j

    @staticmethod
    def _macro(h, name):
        m = re.search(r'^#define\s+%s\s+(.*)$' % re.escape(name), h, re.M)
        assert m, name
        v = m.group(1).strip()
        return v[1:-1] if v.startswith('"') else v

    def test_persistence_allocated(self):
        alloc = os.path.join(self.tmp, "alloc")
        r = run([sys.executable, "-I", ALLOC, "--out", alloc, "--source", self.source, "--arch", "amd64",
                 "--type", "fre", "--channel", "ldw_lab", "--revision", "3", "--timestamp", "2026-11-07T02:07:00Z"])
        self.assertEqual(r.returncode, 0, r.stderr)
        rec = _load(os.path.join(alloc, "lbi-allocation.json"))
        h, j = self._generate(alloc, os.path.join(self.tmp, "bin"))
        # header <-> allocation
        self.assertEqual(self._macro(h, "LBI_LABEL"), rec["label"])
        self.assertEqual(self._macro(h, "LBI_BUILDLABEX"), rec["label"])
        self.assertEqual(self._macro(h, "LBI_BUILDLAB"), rec["buildlab"])
        self.assertEqual(int(self._macro(h, "LBI_SEQUENCE")), rec["sequence"])
        self.assertEqual(int(self._macro(h, "LBI_REVISION")), 3)
        self.assertEqual(self._macro(h, "LBI_ATTEMPT_ID"), rec["attempt_id"])
        self.assertEqual(self._macro(h, "LBI_SOURCE_COMMIT"), self.commit)
        self.assertEqual(self._macro(h, "LBI_MANIFEST_REF"), rec["manifest_ref"])
        self.assertEqual(self._macro(h, "LBI_TIMESTAMP_UTC"), "2026-11-07T02:07:00Z")
        self.assertEqual(self._macro(h, "LBI_LABEL_RC"), rec["label"] + "\\0")
        # json <-> header
        for k, mac in [("label", "LBI_LABEL"), ("attempt_id", "LBI_ATTEMPT_ID"), ("source_commit", "LBI_SOURCE_COMMIT"),
                       ("inputs_digest", "LBI_INPUTS_DIGEST"), ("buildlab", "LBI_BUILDLAB"), ("manifest_ref", "LBI_MANIFEST_REF")]:
            self.assertEqual(str(j[k]), self._macro(h, mac), k)
        self.assertEqual(j["sequence"], rec["sequence"])
        self.assertEqual(j["toolchain"], {"id": "Clang", "version": "18.1.8"})
        # the label parses back to the same fields
        p = lbi_format.parse_label(j["label"])
        self.assertEqual((p["sequence"], p["revision"], p["arch"], p["type"], p["channel"]),
                         (rec["sequence"], 3, "amd64", "fre", "ldw_lab"))
        # the inputs digest is reproducible by the python reference implementation
        want = lbi_format.inputs_digest(self.commit, False, "amd64", "fre", "ldw_lab", "Clang", "18.1.8",
                                        j["config"], j["manifest_schema"])
        self.assertEqual(j["inputs_digest"], want)
        # a second attempt with the same inputs: different attempt id, same digest
        alloc2 = os.path.join(self.tmp, "alloc2")
        run([sys.executable, "-I", ALLOC, "--out", alloc2, "--source", self.source, "--arch", "amd64",
             "--type", "fre", "--channel", "ldw_lab", "--revision", "3", "--timestamp", "2026-11-07T02:08:00Z"]).check_returncode()
        h2, j2 = self._generate(alloc2, os.path.join(self.tmp, "bin2"))
        self.assertNotEqual(j2["attempt_id"], j["attempt_id"])
        self.assertEqual(j2["inputs_digest"], j["inputs_digest"])

    def test_generator_without_allocation_is_provisional(self):
        h, j = self._generate(os.path.join(self.tmp, "nowhere"), os.path.join(self.tmp, "bin3"))
        self.assertEqual(int(self._macro(h, "LBI_SEQUENCE")), 0)
        self.assertEqual(self._macro(h, "LBI_PROVISIONAL"), "1")
        p = lbi_format.parse_label(j["label"])
        self.assertTrue(p["provisional"])
        self.assertEqual(p["arch"], "amd64")
        self.assertEqual(p["type"], "fre")
        self.assertEqual(self._macro(h, "LBI_SOURCE_COMMIT"), self.commit)
        self.assertEqual(len(j["attempt_id"]), 36)

    def test_chk_from_dbg(self):
        h, j = self._generate(os.path.join(self.tmp, "nowhere"), os.path.join(self.tmp, "bin4"), ["-DDBG=1"])
        self.assertEqual(lbi_format.parse_label(j["label"])["type"], "chk")


if __name__ == "__main__":
    unittest.main(verbosity=2)
