#!/usr/bin/env python3
"""
PROJECT:     LiberNT - Leeway Development Works
LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
PURPOSE:     Leeway Build Identity (LBI) attempt allocator.
COPYRIGHT:   Copyright 2026 Leeway Open Software

Allocates ONE build attempt before configure and writes the allocation file the
CMake generator (sdk/cmake/lbi.cmake) consumes. Spec: WS-B-LBI-SPEC.md section 5.

The ledger is a git repository used only for its refs:
  refs/lbi/seq/<N>      build attempt with global sequence N (revision 1)
  refs/lbi/rev/<N>/<R>  servicing re-spin R (R >= 2) of sequence N
Creation of a ref is atomic on the server (push with the lease "must not
exist"), so two workers can never share a sequence or a revision. A failed
build keeps its ref: the number is burnt, never reused.

Fail-closed (owner decision 2026-11-07): without a reachable ledger there is no
identity. The allocator exits non-zero and writes nothing; the generator then
marks the build "unallocated" and never fabricates a sequence.

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

SEQ_NS = "refs/lbi/seq/"
REV_NS = "refs/lbi/rev/"
EXIT_LEDGER = 2


class LedgerError(RuntimeError):
    pass


def git(args, cwd=None, env=None, check=True):
    r = subprocess.run(["git"] + args, cwd=cwd, env=env, capture_output=True, text=True)
    if check and r.returncode != 0:
        raise LedgerError("git %s failed (%d): %s" % (" ".join(args), r.returncode, r.stderr.strip()))
    return r


def _ls_numbers(ledger, prefix):
    r = git(["ls-remote", "--refs", ledger, prefix + "*"])
    out = []
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[1].startswith(prefix):
            tail = parts[1][len(prefix):]
            if tail.isdigit():
                out.append(int(tail))
    return out


def ledger_max_seq(ledger):
    return max(_ls_numbers(ledger, SEQ_NS), default=0)


def ledger_max_rev(ledger, seq):
    """Revision 1 is refs/lbi/seq/<N> itself; re-spins live under refs/lbi/rev/<N>/."""
    if seq not in _ls_numbers(ledger, SEQ_NS):
        raise LedgerError("sequence %d does not exist in the ledger; nothing to re-spin" % seq)
    return max(_ls_numbers(ledger, REV_NS + "%d/" % seq), default=1)


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
        raise LedgerError(p.stderr)
    blob = p.stdout.strip()
    git(["update-index", "--add", "--cacheinfo", "100644,%s,allocation.json" % blob], cwd=workdir, env=env)
    tree = git(["write-tree"], cwd=workdir, env=env).stdout.strip()
    msg = "lbi: allocate attempt %s seq %d rev %d" % (record["attempt_id"], record["sequence"], record["revision"])
    p = subprocess.run(["git", "commit-tree", tree, "-m", msg], cwd=workdir, env=env,
                       capture_output=True, text=True)
    if p.returncode != 0:
        raise LedgerError(p.stderr)
    return p.stdout.strip()


def _cas_push(ledger, workdir, record, ref):
    sha = make_allocation_commit(workdir, record)
    r = git(["push", "--quiet", "--force-with-lease=%s:" % ref, ledger, "%s:%s" % (sha, ref)],
            cwd=workdir, check=False)
    if r.returncode != 0:
        return None
    seen = git(["ls-remote", "--refs", ledger, ref]).stdout.split()
    if seen and seen[0] == sha:
        return sha
    raise LedgerError("ledger ref %s is not ours after an accepted push" % ref)


def allocate_ledger(ledger, record, workdir, respin_of=None, max_tries=50):
    """Compare-and-swap loop. The lease 'ref must not exist' makes the server (or
    the local ref lock for a path ledger) reject the push if the number was
    taken meanwhile; then re-read and retry with the next one."""
    tries = 0
    while tries < max_tries:
        tries += 1
        if respin_of is None:
            n, rev = ledger_max_seq(ledger) + 1, 1
            ref = SEQ_NS + str(n)
        else:
            n, rev = respin_of, ledger_max_rev(ledger, respin_of) + 1
            ref = REV_NS + "%d/%d" % (n, rev)
        record["sequence"], record["revision"] = n, rev
        sha = _cas_push(ledger, workdir, record, ref)
        if sha:
            record["ledger"] = {"url": ledger, "ref": ref, "commit": sha, "tries": tries}
            return record
        time.sleep(random.uniform(0.05, 0.4) * min(tries, 5))
    raise LedgerError("could not allocate after %d tries" % max_tries)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Allocate one LBI build attempt (fail-closed: no ledger, no identity)")
    ap.add_argument("--ledger", required=True, help="git URL or path of the allocation ledger")
    ap.add_argument("--out", required=True, help="directory for lbi-allocation.{json,cmake}")
    ap.add_argument("--source", default=".", help="source tree (git) for the commit and dirty flag")
    ap.add_argument("--arch", required=True, choices=lbi_format.ARCHES)
    ap.add_argument("--type", required=True, choices=lbi_format.TYPES, dest="btype")
    ap.add_argument("--channel", required=True, choices=lbi_format.CHANNELS)
    ap.add_argument("--respin", type=int, metavar="SEQ",
                    help="servicing re-spin of an existing sequence: allocates its next revision")
    ap.add_argument("--worker", default=os.environ.get("LBI_WORKER") or socket.gethostname() or "unknown",
                    help="who allocated (host name, lab id, CI runner); recorded, never identity")
    ap.add_argument("--ci-ref", default=os.environ.get("GITHUB_RUN_ID", ""), help="opaque CI run reference (recorded, never identity)")
    ap.add_argument("--manifest-schema", default="lbi-provenance-manifest/1.0",
                    help="WS-C manifest schema id (WS-C-PROVENANCE.md section 3)")
    ap.add_argument("--timestamp", help="override UTC timestamp (ISO 8601 Z), tests only")
    a = ap.parse_args(argv)

    now = a.timestamp or datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    stamp = lbi_format.stamp_from_iso(now)
    commit, dirty = "unknown", False
    if git(["-C", a.source, "rev-parse", "--git-dir"], check=False).returncode == 0:
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
        "revision": 1,
        "respin_of": a.respin,
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
    }
    lbi_format.validate_channel(a.channel)
    lbi_format.validate_fields(0, 1, record["arch"], record["type"], record["channel"], stamp)

    try:
        # ignore_cleanup_errors: on Windows a git child can still hold .git/HEAD for a
        # moment after the push returns; the allocation is already in the ledger.
        with tempfile.TemporaryDirectory(prefix="lbi-alloc-", ignore_cleanup_errors=True) as wd:
            git(["init", "-q", wd])
            git(["-C", wd, "config", "core.fsmonitor", "false"])
            git(["-C", wd, "config", "maintenance.auto", "false"])
            git(["-C", wd, "config", "gc.auto", "0"])
            record = allocate_ledger(a.ledger, record, wd, respin_of=a.respin)
    except LedgerError as e:
        print("lbi_alloc: no identity allocated (fail-closed): %s" % e, file=sys.stderr)
        return EXIT_LEDGER
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
        f.write('set(LBI_ALLOC_LEDGER_REF "%s")\n' % record["ledger"]["ref"])
    print(record["label"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
