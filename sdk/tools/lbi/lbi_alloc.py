#!/usr/bin/env python3
"""
PROJECT:     LiberNT - Leeway Development Works
LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
PURPOSE:     Leeway Build Identity (LBI) attempt allocator (WS-B prototype).
COPYRIGHT:   Copyright 2026 Leeway Open Software

Allocates ONE build attempt before configure and writes the allocation file the
CMake generator (sdk/cmake/lbi.cmake) consumes. Spec: WS-B-LBI-SPEC.md section 5.

Modes
  --ledger <git url or path>   git-ref compare-and-swap allocation (the chosen
                               mechanism): the attempt becomes refs/lbi/seq/<N>
                               in the ledger repository. Creation of that ref is
                               atomic on the server; two workers can never get
                               the same N. A failed build keeps its ref: N is
                               burnt, never reused.
  (no --ledger)                provisional allocation: sequence 0, unique only
                               through the attempt id (UUID). Never an identity.

Every allocation carries a random attempt id (UUID4) that is unique without any
coordination, so provisional builds are still distinguishable in diagnostics.

Output: <out>/lbi-allocation.json and <out>/lbi-allocation.cmake (same content,
the .cmake form is what the generator includes so CMake needs no JSON parser).
"""
import argparse
import datetime
import json
import os
import random
import socket
import subprocess
import sys
import tempfile
import time
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lbi_format  # noqa: E402

REF_NS = "refs/lbi/seq/"


def git(args, cwd=None, env=None, check=True):
    r = subprocess.run(["git"] + args, cwd=cwd, env=env, capture_output=True, text=True)
    if check and r.returncode != 0:
        raise RuntimeError("git %s failed (%d): %s" % (" ".join(args), r.returncode, r.stderr.strip()))
    return r


def ledger_max_seq(ledger):
    r = git(["ls-remote", "--refs", ledger, REF_NS + "*"])
    best = 0
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[1].startswith(REF_NS):
            tail = parts[1][len(REF_NS):]
            if tail.isdigit():
                best = max(best, int(tail))
    return best


def make_allocation_commit(workdir, record):
    """An orphan commit whose tree holds allocation.json; its sha is the ref target."""
    env = dict(os.environ,
               GIT_AUTHOR_NAME="Leeway Open Software",
               GIT_AUTHOR_EMAIL="281492504+LeewayOpenSoftware@users.noreply.github.com",
               GIT_COMMITTER_NAME="Leeway Open Software",
               GIT_COMMITTER_EMAIL="281492504+LeewayOpenSoftware@users.noreply.github.com",
               GIT_INDEX_FILE=os.path.join(workdir, "lbi-index"))
    p = subprocess.run(["git", "hash-object", "-w", "--stdin"], cwd=workdir, env=env,
                       input=lbi_format.canonical_record(record), capture_output=True, text=True)
    if p.returncode != 0:
        raise RuntimeError(p.stderr)
    blob = p.stdout.strip()
    git(["update-index", "--add", "--cacheinfo", "100644,%s,allocation.json" % blob], cwd=workdir, env=env)
    tree = git(["write-tree"], cwd=workdir, env=env).stdout.strip()
    msg = "lbi: allocate attempt %s seq %d" % (record["attempt_id"], record["sequence"])
    p = subprocess.run(["git", "commit-tree", tree, "-m", msg], cwd=workdir, env=env,
                       capture_output=True, text=True)
    if p.returncode != 0:
        raise RuntimeError(p.stderr)
    return p.stdout.strip()


def allocate_ledger(ledger, record, workdir, max_tries=50):
    """Compare-and-swap loop: push an orphan commit to refs/lbi/seq/<N> with the
    lease 'ref must not exist'. The server (or the local ref lock for a path
    ledger) rejects the push if N was taken meanwhile; then re-read and retry."""
    tries = 0
    while tries < max_tries:
        tries += 1
        n = ledger_max_seq(ledger) + 1
        record["sequence"] = n
        sha = make_allocation_commit(workdir, record)
        ref = REF_NS + str(n)
        r = git(["push", "--quiet", "--force-with-lease=%s:" % ref, ledger, "%s:%s" % (sha, ref)],
                cwd=workdir, check=False)
        if r.returncode == 0:
            # Prove the ref is ours: the ledger must now map N to exactly this commit.
            seen = git(["ls-remote", "--refs", ledger, ref]).stdout.split()
            if seen and seen[0] == sha:
                record["ledger"] = {"url": ledger, "ref": ref, "commit": sha, "tries": tries}
                return record
            raise RuntimeError("ledger ref %s is not ours after an accepted push" % ref)
        time.sleep(random.uniform(0.05, 0.4) * min(tries, 5))
    raise RuntimeError("could not allocate a sequence after %d tries" % max_tries)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Allocate one LBI build attempt")
    ap.add_argument("--ledger", help="git URL or path of the allocation ledger (omit for provisional)")
    ap.add_argument("--out", required=True, help="directory for lbi-allocation.{json,cmake}")
    ap.add_argument("--source", default=".", help="source tree (git) for the commit and dirty flag")
    ap.add_argument("--arch", required=True, choices=lbi_format.ARCHES)
    ap.add_argument("--type", required=True, choices=lbi_format.TYPES, dest="btype")
    ap.add_argument("--channel", default="ldw_dev")
    ap.add_argument("--revision", type=int, default=0, help="servicing revision (0 = fresh build)")
    ap.add_argument("--worker", default=os.environ.get("LBI_WORKER") or socket.gethostname() or "unknown",
                    help="who allocated (host name, lab id, CI runner); recorded, never identity")
    ap.add_argument("--ci-ref", default=os.environ.get("GITHUB_RUN_ID", ""), help="opaque CI run reference (recorded, never identity)")
    ap.add_argument("--manifest-schema", default="leeway-provenance-manifest/v1", help="WS-C manifest schema id")
    ap.add_argument("--timestamp", help="override UTC timestamp (ISO 8601 Z), tests only")
    a = ap.parse_args(argv)

    now = a.timestamp or datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    stamp = lbi_format.stamp_from_iso(now)
    commit, dirty = "unknown", False
    if os.path.isdir(os.path.join(a.source, ".git")) or git(["-C", a.source, "rev-parse", "--git-dir"], check=False).returncode == 0:
        r = git(["-C", a.source, "rev-parse", "HEAD"], check=False)
        if r.returncode == 0:
            commit = r.stdout.strip()
        r = git(["-C", a.source, "status", "--porcelain", "--untracked-files=no"], check=False)
        dirty = r.returncode == 0 and bool(r.stdout.strip())

    attempt_id = str(uuid.uuid4())
    record = {
        "lbi_version": 1,
        "attempt_id": attempt_id,
        "sequence": 0,
        "revision": a.revision,
        "arch": a.arch,
        "type": a.btype,
        "channel": a.channel,
        "timestamp_utc": now,
        "stamp": stamp,
        "source_commit": commit,
        "source_dirty": dirty,
        "manifest_ref": "%s:attempt:%s" % (a.manifest_schema, attempt_id),
        "worker": a.worker,
        "ci_ref": a.ci_ref,
        "provisional": True,
    }
    lbi_format.validate_fields(0, record["revision"], record["arch"], record["type"], record["channel"], stamp)

    if a.ledger:
        # ignore_cleanup_errors: on Windows a git child can still hold .git/HEAD for a
        # moment after the push returns; the allocation is already in the ledger.
        with tempfile.TemporaryDirectory(prefix="lbi-alloc-", ignore_cleanup_errors=True) as wd:
            git(["init", "-q", wd])
            git(["-C", wd, "config", "core.fsmonitor", "false"])
            git(["-C", wd, "config", "maintenance.auto", "false"])
            git(["-C", wd, "config", "gc.auto", "0"])
            record = allocate_ledger(a.ledger, record, wd)
        record["provisional"] = False
    record["label"] = lbi_format.format_label(record["sequence"], record["revision"], record["arch"],
                                              record["type"], record["channel"], stamp)
    record["buildlab"] = lbi_format.format_buildlab(record["sequence"], record["channel"], stamp)

    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, "lbi-allocation.json"), "w", encoding="utf-8", newline="\n") as f:
        f.write(lbi_format.canonical_record(record))
    with open(os.path.join(a.out, "lbi-allocation.cmake"), "w", encoding="utf-8", newline="\n") as f:
        f.write("# Generated by sdk/tools/lbi/lbi_alloc.py - do not edit\n")
        for k in ("attempt_id", "sequence", "revision", "arch", "type", "channel", "timestamp_utc",
                  "stamp", "source_commit", "manifest_ref", "worker", "ci_ref"):
            f.write('set(LBI_ALLOC_%s "%s")\n' % (k.upper(), str(record[k]).replace('"', '\\"')))
        f.write('set(LBI_ALLOC_SOURCE_DIRTY %s)\n' % ("1" if record["source_dirty"] else "0"))
        f.write('set(LBI_ALLOC_PROVISIONAL %s)\n' % ("1" if record["provisional"] else "0"))
    print(record["label"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
