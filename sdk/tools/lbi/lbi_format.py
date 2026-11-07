#!/usr/bin/env python3
"""
PROJECT:     LiberNT - Leeway Development Works
LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
PURPOSE:     Leeway Build Identity (LBI) label grammar: format, parse, digest.
COPYRIGHT:   Copyright 2026 Leeway Open Software

WS-B prototype (shared/libernt/lbi/WS-B-LBI-SPEC.md, section 3). This module is
the single definition of the label grammar; the CMake generator, the allocator,
the tests and any diagnostic parser must agree with it.

    label    = seq "." rev "." arch type "." channel "." stamp
    seq      = 1*7DIGIT             ; 0 = provisional (not an identity)
    rev      = 1*5DIGIT             ; servicing revision, 0 = fresh build
    arch     = "amd64" / "x86" / "arm64" / "riscv64" / "ppc"
    type     = "fre" / "chk"
    channel  = LOALPHA *31( LOALPHA / DIGIT / "_" )
    stamp    = 6DIGIT "-" 4DIGIT    ; yymmdd-hhmm, UTC

    buildlab = seq "." channel "." stamp        ; NT BuildLab shape
    buildlabex = label                           ; NT BuildLabEx shape
"""
import hashlib
import json
import re

ARCHES = ("amd64", "x86", "arm64", "riscv64", "ppc")
TYPES = ("fre", "chk")

_LABEL_RE = re.compile(
    r"^(?P<seq>\d{1,7})\.(?P<rev>\d{1,5})\."
    r"(?P<arch>amd64|x86|arm64|riscv64|ppc)(?P<type>fre|chk)\."
    r"(?P<channel>[a-z][a-z0-9_]{0,31})\."
    r"(?P<stamp>\d{6}-\d{4})$")
_BUILDLAB_RE = re.compile(
    r"^(?P<seq>\d{1,7})\.(?P<channel>[a-z][a-z0-9_]{0,31})\.(?P<stamp>\d{6}-\d{4})$")
_STAMP_RE = re.compile(r"^(\d{2})(\d{2})(\d{2})-(\d{2})(\d{2})$")


class LbiFormatError(ValueError):
    pass


def validate_fields(seq, rev, arch, btype, channel, stamp):
    if not (isinstance(seq, int) and 0 <= seq <= 9999999):
        raise LbiFormatError("sequence out of range: %r" % (seq,))
    if not (isinstance(rev, int) and 0 <= rev <= 99999):
        raise LbiFormatError("revision out of range: %r" % (rev,))
    if arch not in ARCHES:
        raise LbiFormatError("unknown arch: %r" % (arch,))
    if btype not in TYPES:
        raise LbiFormatError("unknown build type: %r" % (btype,))
    if not re.match(r"^[a-z][a-z0-9_]{0,31}$", channel or ""):
        raise LbiFormatError("bad channel: %r" % (channel,))
    m = _STAMP_RE.match(stamp or "")
    if not m:
        raise LbiFormatError("bad stamp: %r" % (stamp,))
    mo, d, h, mi = int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5))
    if not (1 <= mo <= 12 and 1 <= d <= 31 and h <= 23 and mi <= 59):
        raise LbiFormatError("stamp is not a date: %r" % (stamp,))


def format_label(seq, rev, arch, btype, channel, stamp):
    """BuildLabEx-shaped LBI label, e.g. 1042.1.amd64fre.ldw_dev.261107-0207"""
    validate_fields(seq, rev, arch, btype, channel, stamp)
    return "%d.%d.%s%s.%s.%s" % (seq, rev, arch, btype, channel, stamp)


def format_buildlab(seq, channel, stamp):
    """BuildLab-shaped string, e.g. 1042.ldw_dev.261107-0207"""
    validate_fields(seq, 0, ARCHES[0], TYPES[0], channel, stamp)
    return "%d.%s.%s" % (seq, channel, stamp)


def parse_label(text):
    m = _LABEL_RE.match(text or "")
    if not m:
        raise LbiFormatError("not an LBI label: %r" % (text,))
    f = m.groupdict()
    out = {"sequence": int(f["seq"]), "revision": int(f["rev"]), "arch": f["arch"],
           "type": f["type"], "channel": f["channel"], "stamp": f["stamp"]}
    validate_fields(out["sequence"], out["revision"], out["arch"], out["type"], out["channel"], out["stamp"])
    out["provisional"] = out["sequence"] == 0
    return out


def parse_buildlab(text):
    m = _BUILDLAB_RE.match(text or "")
    if not m:
        raise LbiFormatError("not an LBI BuildLab: %r" % (text,))
    return {"sequence": int(m.group("seq")), "channel": m.group("channel"), "stamp": m.group("stamp")}


def stamp_from_iso(iso_utc):
    """'2026-11-07T02:07:00Z' -> '261107-0207'"""
    m = re.match(r"^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2})(?::\d{2})?Z$", iso_utc or "")
    if not m:
        raise LbiFormatError("timestamp must be ISO-8601 UTC with Z: %r" % (iso_utc,))
    return "%s%s%s-%s%s" % (m.group(1)[2:], m.group(2), m.group(3), m.group(4), m.group(5))


def inputs_digest(source_commit, source_dirty, arch, btype, channel,
                  toolchain_id, toolchain_version, config_items, manifest_schema):
    """
    Reproducible-inputs digest (spec section 4): identical inputs give the same
    digest on every attempt; the attempt id and sequence never enter it.
    config_items: iterable of 'KEY=VALUE' strings (sorted here).
    Returns the full sha256 hex; consumers show the first 16 characters.
    """
    canon = {
        "v": 1,
        "source_commit": source_commit,
        "source_dirty": bool(source_dirty),
        "arch": arch, "type": btype, "channel": channel,
        "toolchain": [toolchain_id, toolchain_version],
        "config": sorted(set(config_items or [])),
        "manifest_schema": manifest_schema,
    }
    blob = json.dumps(canon, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(blob).hexdigest()


def canonical_record(rec):
    """Canonical JSON text of an LBI record (stable key order, LF)."""
    return json.dumps(rec, sort_keys=True, indent=2) + "\n"
