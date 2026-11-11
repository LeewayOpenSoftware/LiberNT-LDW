#!/usr/bin/env python3
# PROJECT:     LiberNT Source Provenance (LBI WS-C)
# LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
# PURPOSE:     Generate and validate the LBI per-build provenance manifest
# COPYRIGHT:   Copyright 2026 Leeway Open Software
"""
lbi-provenance.py - LBI WS-C manifest generator and validator (schema 1.0).

Implements shared/libernt/lbi/WS-C-PROVENANCE.md section 5. Every field is
produced by a recorded method; fields without evidence are emitted as null
with an explicit confidence - this tool never fabricates ancestry.

Provenance claims require a full mirror: when the repository is shallow,
every proven-ancestry confidence is downgraded to unverified and the
manifest records the mirror state. --require-full-mirror turns that into a
hard failure (CI gate).

Usage:
  lbi-provenance.py --repo PATH [--source-ref REF] [--upstream-ref REF]
      [--source-remote NAME] [--upstream-url URL] [--ledger DIR]
      [--cmake-cache FILE] [--arch ARCH] [--build-type TYPE]
      [--patch-id-sample N] [--lbi-json FILE] [--artifact FILE]...
      [--generator-ref SHA] [--require-full-mirror] [--output FILE]
  lbi-provenance.py --check FILE

Defaults follow the WS-C example: source refs/heads/leeway/main, upstream
up/main-nt10, ledger shared/libernt/upstream-review (relative to CWD).
The --lbi-json switch binds the manifest to one WS-B allocated attempt: the
build's lbi.json supplies the label, the attempt UUID, the source commit
cross-check and the inputs digest; manifest_ref is derived as
"schema_id:attempt:<uuid>" and must equal what WS-B embedded. Per the WS-B
contract an unallocated build (allocated=false, empty attempt_id and
manifest_ref, label 'unallocated') has no manifest of record: this tool
refuses to mint one. ledger_ref is recorded verbatim
("refs/lbi/seq/<N>" fresh, "refs/lbi/rev/<N>/<R>" for a servicing re-spin).
"""

import argparse
import hashlib
import json
import re
import subprocess
import sys
from datetime import datetime, timezone

SCHEMA_ID = "lbi-provenance-manifest/1.0"
SCHEMA_VERSION = "1.0"
GENERATOR_VERSION = "0.2.0"
CONFIDENCE_VALUES = {
    "proven-ancestry",
    "content-equal",
    "patch-equal",
    "recorded",
    "unverified",
    "none-recorded",
}
ATTEMPT_RE = re.compile(
    r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")
SHA_RE = re.compile(r"^[0-9a-f]{40}$")


def fail(msg):
    sys.stderr.write("lbi-provenance: error: %s\n" % msg)
    sys.exit(2)


def git(repo, *args, check=True):
    r = subprocess.run(["git", "-C", repo] + list(args),
                       capture_output=True, text=True)
    if check and r.returncode != 0:
        fail("git %s failed: %s" % (" ".join(args), r.stderr.strip()))
    return r


def git_out(repo, *args, **kw):
    return git(repo, *args, **kw).stdout.strip()


def tree_blobs(repo, ref):
    """path -> blob sha for every file in ref's tree (shallow-safe)."""
    out = git_out(repo, "ls-tree", "-r", ref)
    blobs = {}
    for line in out.splitlines():
        meta, _, path = line.partition("\t")
        mode, typ, sha = meta.split()
        if typ == "blob":
            blobs[path] = sha
    return blobs


def merge_base(repo, a, b):
    r = git(repo, "merge-base", a, b, check=False)
    return r.stdout.strip() if r.returncode == 0 else None


def patch_id(repo, sha):
    show = git(repo, "show", sha, check=False)
    if show.returncode != 0:
        return None
    pid = subprocess.run(["git", "patch-id", "--stable"],
                         input=show.stdout, capture_output=True, text=True)
    if pid.returncode != 0 or not pid.stdout.strip():
        return None
    return pid.stdout.split()[0]


def picked_table_stats(repo, ledger):
    """Per-batch stats from PICKED-<id>.txt files, with a patch-id sample."""
    import glob, os
    batches = []
    for path in sorted(glob.glob(os.path.join(ledger, "PICKED-*.txt"))):
        batch_id = re.search(r"PICKED-(\w+)\.txt$", path).group(1)
        pairs = []
        with open(path, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                m = re.match(r"([0-9a-f]+)\s*<-\s*([0-9a-f]+)\s*::", line.strip())
                if m:
                    pairs.append((m.group(1), m.group(2)))
        batches.append((batch_id, path, pairs))
    return batches


def repo_is_shallow(repo):
    return git_out(repo, "rev-parse", "--is-shallow-repository") == "true"


def downgrade(conf, shallow):
    return "unverified" if (shallow and conf == "proven-ancestry") else conf


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_lbi_json(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            data = json.load(fh)
    except (OSError, ValueError) as exc:
        fail("cannot read %s: %s" % (path, exc))
    if not data.get("allocated"):
        fail("%s is an unallocated build (allocated=false, empty "
             "attempt_id/manifest_ref, label 'unallocated'): no manifest of "
             "record; refusing to mint one" % path)
    attempt = data.get("attempt_id")
    if not attempt or not ATTEMPT_RE.match(attempt):
        fail("%s has no valid attempt_id (got %r)" % (path, attempt))
    return data


def build_artifact_entries(paths):
    entries = []
    for path in paths:
        import os
        if not os.path.isfile(path):
            fail("artifact not found: %s" % path)
        entries.append({
            "path": path,
            "file_name": os.path.basename(path),
            "size": os.path.getsize(path),
            "sha256": sha256_file(path),
            "method": "sha256 over the file as built",
        })
    return entries


def canonical_manifest_id(manifest):
    clone = dict(manifest)
    inner = dict(clone["manifest"])
    inner["manifest_id"] = None
    clone["manifest"] = inner
    canonical = json.dumps(clone, sort_keys=True, ensure_ascii=False,
                           separators=(",", ":")).encode()
    return hashlib.sha256(canonical).hexdigest()


def walk_confidences(node, path="$"):
    if isinstance(node, dict):
        if "confidence" in node:
            yield path, node["confidence"]
        for key, value in node.items():
            yield from walk_confidences(value, "%s.%s" % (path, key))
    elif isinstance(node, list):
        for index, value in enumerate(node):
            yield from walk_confidences(value, "%s[%d]" % (path, index))


def check_manifest(data):
    problems = []

    def require(cond, msg):
        if not cond:
            problems.append(msg)

    manifest = data.get("manifest")
    require(isinstance(manifest, dict), "missing manifest block")
    if not isinstance(manifest, dict):
        return problems
    require(manifest.get("schema_id") == SCHEMA_ID,
            "schema_id must be %s" % SCHEMA_ID)
    require(manifest.get("schema_version") == SCHEMA_VERSION,
            "schema_version must be %s" % SCHEMA_VERSION)
    recorded_id = manifest.get("manifest_id")
    require(bool(recorded_id) and re.match(r"^[0-9a-f]{64}$", recorded_id),
            "manifest_id must be 64 lowercase hex")
    if recorded_id and re.match(r"^[0-9a-f]{64}$", recorded_id):
        require(canonical_manifest_id(data) == recorded_id,
                "manifest_id does not match the canonical content hash")
    generated_at = manifest.get("generated_at", "")
    require(re.match(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$",
                     generated_at) is not None,
            "generated_at must be UTC ISO-8601 Zulu")
    generator = manifest.get("generator", {})
    require(generator.get("name") == "lbi-provenance",
            "generator.name must be lbi-provenance")

    build = data.get("build", {})
    source = build.get("source", {})
    require(SHA_RE.match(source.get("commit", "")) is not None,
            "build.source.commit must be a full sha")
    label = build.get("lbi")
    attempt = build.get("lbi_attempt_id")
    ref = build.get("manifest_ref")
    if attempt is None:
        require(label is None and ref is None,
                "pre-B manifest must leave lbi/lbi_attempt_id/manifest_ref null")
    else:
        require(ATTEMPT_RE.match(attempt) is not None,
                "lbi_attempt_id must be lowercase uuid4")
        require(isinstance(label, str) and label,
                "lbi label must be present with an attempt id")
        expected_ref = "%s:attempt:%s" % (SCHEMA_ID, attempt)
        require(ref == expected_ref,
                "manifest_ref must be %s" % expected_ref)

    shallow = manifest.get("mirror", {}).get("shallow") is True
    for where, value in walk_confidences(data):
        require(value in CONFIDENCE_VALUES,
                "confidence at %s is not a taxonomy value: %r" % (where, value))
        if shallow:
            require(value != "proven-ancestry",
                    "proven-ancestry at %s in a shallow-mirror manifest"
                    % where)

    for index, artifact in enumerate(data.get("artifacts", [])):
        require(re.match(r"^[0-9a-f]{64}$", artifact.get("sha256", ""))
                is not None,
                "artifacts[%d].sha256 must be 64 lowercase hex" % index)
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", metavar="FILE",
                    help="validate a manifest instead of generating one")
    ap.add_argument("--repo")
    ap.add_argument("--source-ref", default="refs/heads/leeway/main")
    ap.add_argument("--upstream-ref", default="up/main-nt10")
    ap.add_argument("--source-remote", default="origin")
    ap.add_argument("--upstream-url",
                    default="https://github.com/LiberNT/LiberNT.git")
    ap.add_argument("--ledger", default="shared/libernt/upstream-review")
    ap.add_argument("--cmake-cache")
    ap.add_argument("--arch")
    ap.add_argument("--build-type")
    ap.add_argument("--patch-id-sample", type=int, default=5)
    ap.add_argument("--lbi-json",
                    help="WS-B build lbi.json binding this manifest to an "
                         "allocated attempt")
    ap.add_argument("--artifact", action="append", default=[],
                    help="build output to hash; repeatable")
    ap.add_argument("--generator-ref",
                    help="git sha of this tool in the source tree")
    ap.add_argument("--require-full-mirror", action="store_true",
                    help="exit 3 when the repository is shallow")
    ap.add_argument("--output")
    args = ap.parse_args()

    if args.check:
        with open(args.check, encoding="utf-8", errors="replace") as fh:
            data = json.load(fh)
        problems = check_manifest(data)
        if problems:
            for problem in problems:
                sys.stderr.write("lbi-provenance: check: %s\n" % problem)
            sys.exit(1)
        sys.stdout.write("CHECK PASS %s\n" % data["manifest"]["manifest_id"])
        return

    if not args.repo:
        fail("--repo is required to generate")

    src = args.source_ref
    up = args.upstream_ref
    head = git_out(args.repo, "rev-parse", "--verify", src)
    tree = git_out(args.repo, "rev-parse", head + "^{tree}")
    cdate = git_out(args.repo, "log", "-1", "--format=%cI", head)
    origin_url = git(args.repo, "config", "remote.%s.url" % args.source_remote,
                     check=False).stdout.strip() or None

    shallow = repo_is_shallow(args.repo)
    if shallow and args.require_full_mirror:
        sys.stderr.write("lbi-provenance: %s is a shallow mirror; "
                         "provenance requires a full clone\n" % args.repo)
        sys.exit(3)
    mirror = {"shallow": shallow}
    if shallow:
        mirror["note"] = ("proven-ancestry claims downgraded to unverified; "
                          "re-run against a full mirror to upgrade them")

    lbi = None
    if args.lbi_json:
        lbi = read_lbi_json(args.lbi_json)
        built_from = lbi.get("source_commit")
        if built_from and built_from != head:
            fail("lbi.json source_commit %s != %s %s; build tree mismatch"
                 % (built_from, src, head))

    mb = merge_base(args.repo, up, head)
    up_tip = git(args.repo, "rev-parse", "--verify", up,
                 check=False).stdout.strip() or None

    # content equality + local delta (skip gracefully if upstream absent)
    shared = identical = 0
    differing = src_only = up_only = 0
    diff_by_area = {}
    if up_tip:
        a = tree_blobs(args.repo, up)
        b = tree_blobs(args.repo, head)
        common = set(a) & set(b)
        shared = len(common)
        identical = sum(1 for p in common if a[p] == b[p])
        differing = shared - identical
        src_only = len(set(b) - set(a))
        up_only = len(set(a) - set(b))
        from collections import Counter
        diff_by_area = dict(Counter(p.split("/")[0]
                                    for p in common if a[p] != b[p]).most_common(12))

    # sync batches: table presence + patch-id sample verification
    batches = []
    for batch_id, path, pairs in picked_table_stats(args.repo, args.ledger):
        checked = matched = missing = 0
        step = max(1, len(pairs) // max(1, args.patch_id_sample))
        for new, old in pairs[::step][:args.patch_id_sample]:
            pn, po = patch_id(args.repo, new), patch_id(args.repo, old)
            if pn is None or po is None:
                missing += 1
                continue
            checked += 1
            matched += (pn == po)
        batches.append({
            "id": batch_id,
            "picked_table": path,
            "picked_count": len(pairs),
            "patch_id_sample": {"checked": checked, "matched": matched,
                                "missing_objects": missing},
            "confidence": "patch-equal" if checked and matched == checked
                          else "recorded",
        })

    # reactos-era lineage probe: candidate upstream refs, deepest wins; a
    # small count means the local ref is shallow-clipped and we say so
    lineage = {"in_repo_evidence": None, "confidence": "unverified",
               "exact_fork_revision": {"value": None,
                                       "confidence": "unverified",
                                       "external_check":
                                           "compare LiberNT/LiberNT against "
                                           "reactos/reactos via GitHub fork "
                                           "network or tree match"}}
    if up_tip:
        best = None
        for cand in [up, "up/main"]:
            count = git_out(args.repo, "rev-list", "--count", cand,
                            check=False)
            if not count:
                continue
            count = int(count)
            if best is None or count > best[1]:
                best = (cand, count)
        if best:
            cand, count = best
            oldest = git_out(args.repo, "log", "--format=%H %cI", cand,
                             check=False)
            first = oldest.splitlines()[-1].split() if oldest else []
            lineage["in_repo_evidence"] = {
                "ref_used": cand,
                "oldest_commit": first[0] if first else None,
                "oldest_commit_date": first[1] if len(first) > 1 else None,
                "commit_count": count,
                "shallow_clipped": count < 10000,
                "method": "git log/rev-list on " + cand,
            }
            lineage["confidence"] = downgrade("proven-ancestry", shallow)

    toolchain = {"compiler": None, "generator": None, "build_type": None,
                 "arch": args.arch, "cmake_defines_digest": None}
    if args.cmake_cache:
        kv = {}
        with open(args.cmake_cache, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                m = re.match(r"(\w+):(\w+)=(.*)", line.strip())
                if m:
                    kv[m.group(1)] = m.group(3)
        toolchain["compiler"] = {
            "id": kv.get("CMAKE_C_COMPILER_ID"),
            "version": kv.get("CMAKE_C_COMPILER_ID") and
                       kv.get("CMAKE_C_COMPILER_VERSION"),
        }
        toolchain["generator"] = kv.get("CMAKE_GENERATOR")
        toolchain["build_type"] = kv.get("CMAKE_BUILD_TYPE") or args.build_type
        defines = sorted(k for k in kv if k.startswith("ENABLE_"))
        toolchain["cmake_defines_digest"] = hashlib.sha256(
            "\n".join(defines).encode()).hexdigest()

    attempt_id = lbi.get("attempt_id") if lbi else None
    manifest = {
        "manifest": {
            "schema_id": SCHEMA_ID,
            "schema_version": SCHEMA_VERSION,
            "manifest_id": None,
            "generated_at": datetime.now(timezone.utc).strftime(
                "%Y-%m-%dT%H:%M:%SZ"),
            "generator": {"name": "lbi-provenance",
                          "version": GENERATOR_VERSION,
                          "schema_ref": "WS-C-PROVENANCE.md#3",
                          "ref": args.generator_ref},
            "mirror": mirror,
        },
        "build": {
            "lbi": lbi.get("label") if lbi else None,
            "lbi_attempt_id": attempt_id,
            "manifest_ref": ("%s:attempt:%s" % (SCHEMA_ID, attempt_id))
                            if attempt_id else None,
            "inputs_digest": lbi.get("inputs_digest") if lbi else None,
            "ledger_ref": lbi.get("ledger_ref") if lbi else None,
            "source": {
                "remote": origin_url,
                "ref": src,
                "commit": head,
                "tree": tree,
                "committer_date": cdate,
            },
        },
        "upstream": {
            "repository": args.upstream_url,
            "branch": up,
            "tip_at_manifest_time": up_tip,
            "ancestry": {
                "merge_base_with_source": mb,
                "merge_base_method": "git merge-base %s %s" % (up, head),
                "confidence": downgrade("proven-ancestry", shallow) if mb
                              else "none-recorded",
            },
            "sync": {"batches": batches,
                     "ledger": args.ledger},
            "content_equality": {
                "vs_upstream_commit": up_tip,
                "shared_paths": shared, "identical_blobs": identical,
                "differing_blobs": differing,
                "source_only_paths": src_only,
                "upstream_only_paths": up_only,
                "method": "git ls-tree -r blob-sha compare",
                "confidence": "content-equal" if up_tip else "none-recorded",
            },
        },
        "reactos_lineage": lineage,
        "local_delta": {"differing_blobs_by_area": diff_by_area,
                        "note": "informational; vs upstream ref at generation"},
        "toolchain": toolchain,
        "artifacts": build_artifact_entries(args.artifact),
        "notes": ("pre-B manifests leave lbi fields null; artifact hashes "
                  "bind the manifest to the build outputs it is given"),
    }

    manifest["manifest"]["manifest_id"] = canonical_manifest_id(manifest)
    out = json.dumps(manifest, indent=2, ensure_ascii=False) + "\n"
    if args.output:
        with open(args.output, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(out)
    else:
        sys.stdout.write(out)


if __name__ == "__main__":
    main()
