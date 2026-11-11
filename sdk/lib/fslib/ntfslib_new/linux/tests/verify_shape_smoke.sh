#!/bin/sh
# PROJECT:     LiberNT NTFS library tests
# LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
# PURPOSE:     The shape verifier passes a sound volume and names the broken
#              invariant on damaged ones: an index block in use but marked
#              free, a mapping that does not cover its allocation, and an
#              extension record with no attribute list to name it
# COPYRIGHT:   Copyright 2026 Leeway Open Software

set -eu

driver=$1
mkntfs=$2

workdir=$(mktemp -d "${TMPDIR:-/tmp}/ntfslib-shape.XXXXXX")
trap 'rm -rf "$workdir"' EXIT HUP INT TERM
image="$workdir/shape.ntfs"
tool="$workdir/damage.py"

truncate -s 64M "$image"
"$mkntfs" -F -Q -q -L SHAPE "$image"

# damage.py IMAGE ACTION: edit the base record of /big.
#   bitmap:     clear the first bit of its $I30 $BITMAP (a block in use)
#   allocation: grow the allocated size of its $I30 $INDEX_ALLOCATION
#   extension:  write an in-use record whose base is /big, which has no
#               attribute list
cat >"$tool" <<'EOF'
import struct
import sys

image, action = sys.argv[1], sys.argv[2]
with open(image, 'r+b') as f:
    data = bytearray(f.read())
    sector = struct.unpack_from('<H', data, 0x0B)[0]
    cluster = sector * data[0x0D]
    mft = struct.unpack_from('<Q', data, 0x30)[0] * cluster
    raw = struct.unpack_from('<b', data, 0x40)[0]
    size = 1 << -raw if raw < 0 else raw * cluster

    def load(n):
        r = bytearray(data[mft + n * size:mft + (n + 1) * size])
        if r[0:4] != b'FILE':
            return None
        usa, count = struct.unpack_from('<HH', r, 4)
        for i in range(1, count):
            r[i * sector - 2:i * sector] = r[usa + 2 * i:usa + 2 * i + 2]
        return r

    def store(n, r):
        usa, count = struct.unpack_from('<HH', r, 4)
        for i in range(1, count):
            r[usa + 2 * i:usa + 2 * i + 2] = r[i * sector - 2:i * sector]
            r[i * sector - 2:i * sector] = r[usa:usa + 2]
        data[mft + n * size:mft + (n + 1) * size] = r

    def attrs(r):
        pos = struct.unpack_from('<H', r, 0x14)[0]
        while True:
            kind, length = struct.unpack_from('<II', r, pos)
            if kind == 0xFFFFFFFF or length == 0:
                return
            yield kind, pos
            pos += length

    big = None
    for n in range(16, 64):
        r = load(n)
        if r is None or not struct.unpack_from('<H', r, 0x16)[0] & 1 or struct.unpack_from('<Q', r, 0x20)[0]:
            continue
        for kind, pos in attrs(r):
            if kind == 0x30:
                value = r[pos + struct.unpack_from('<H', r, pos + 0x14)[0]:]
                if value[0x42:0x42 + 2 * value[0x40]].decode('utf-16-le') == 'big':
                    big = n
    if big is None:
        sys.exit('no /big')
    r = load(big)
    if action == 'bitmap':
        pos = next(p for k, p in attrs(r) if k == 0xB0)
        assert not r[pos + 8]
        value = pos + struct.unpack_from('<H', r, pos + 0x14)[0]
        assert r[value] & 1
        r[value] &= 0xFE
        store(big, r)
    elif action == 'allocation':
        pos = next(p for k, p in attrs(r) if k == 0xA0)
        assert r[pos + 8]
        struct.pack_into('<Q', r, pos + 0x28, struct.unpack_from('<Q', r, pos + 0x28)[0] + cluster)
        store(big, r)
    elif action == 'extension':
        m = load(0)
        pos = next(p for k, p in attrs(m) if k == 0x80 and not m[p + 9])
        records = struct.unpack_from('<Q', m, pos + 0x38)[0] // size
        free = next(n for n in range(records - 1, 23, -1) if load(n) is None or
                    not struct.unpack_from('<H', load(n), 0x16)[0] & 1)
        e = bytearray(r)
        start = struct.unpack_from('<H', e, 0x14)[0]
        struct.pack_into('<I', e, start, 0xFFFFFFFF)
        struct.pack_into('<I', e, 0x18, start + 8)
        struct.pack_into('<H', e, 0x16, 1)
        struct.pack_into('<Q', e, 0x20, big | struct.unpack_from('<H', r, 0x10)[0] << 48)
        struct.pack_into('<I', e, 0x2C, free)
        store(free, e)
    f.seek(0)
    f.write(data)
EOF

expect()
{
    out=$("$driver" --verify-shape "$image") && rc=0 || rc=$?
    case "$1:$rc:$out" in
        ok:0:"shape: ok "*|"$1":3:"shape: invariant $1 "*)
            ;;
        *)
            echo "expected $1, got exit $rc: $out" >&2
            exit 1
            ;;
    esac
}

long=$(printf '%0190d' 0 | tr 0 n)
"$driver" --create-dir "$image" /big >/dev/null
count=0
while test "$count" -lt 60; do
    count=$((count + 1))
    "$driver" --create-file "$image" "$(printf '/big/%s%05u' "$long" "$count")" >/dev/null
done
expect ok
cp "$image" "$workdir/sound"

# 14: NtfsShapeIndexBlockFree
python3 -I "$tool" "$image" bitmap
expect 14
cp "$workdir/sound" "$image"

# 10: NtfsShapeMappingGap
python3 -I "$tool" "$image" allocation
expect 10
cp "$workdir/sound" "$image"

# 8: NtfsShapeExtensionWithoutList
python3 -I "$tool" "$image" extension
expect 8
