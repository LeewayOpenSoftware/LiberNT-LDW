#!/bin/sh
# PROJECT:     LiberNT NTFS library tests
# LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
# PURPOSE:     The check runs the shape verifier on what it and its repairs
#              left: a damaged volume whose shape is still broken after the
#              check reports the broken invariant as a problem, never as a
#              clean volume, and a damage the check repairs leaves a sound
#              shape
# COPYRIGHT:   Copyright 2026 Leeway Open Software

set -eu

driver=$1
mkntfs=$2

workdir=$(mktemp -d "${TMPDIR:-/tmp}/ntfslib-checkshape.XXXXXX")
trap 'rm -rf "$workdir"' EXIT HUP INT TERM
tool="$workdir/damage.py"

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

# NtfsCheckShapeFault: Argument1 holds the invariant in bits 48-63 and the
# file record below them.
shape_event=44

# check IMAGE MODE: run the check (MODE ro or fix), keep its output in
# IMAGE.out and its exit status in rc.
check()
{
    rc=0
    if test "$2" = fix; then
        "$driver" --check "$1" --fix >"$1.out" || rc=$?
    else
        "$driver" --check "$1" >"$1.out" || rc=$?
    fi
}

# reported IMAGE MODE INVARIANT: the check fails and names the invariant.
reported()
{
    check "$1" "$2"
    hex=$(printf '%x' "$3")
    if test "$rc" -ne 3 ||
        ! grep -Eq "^event $shape_event $hex[0-9a-f]{12} " "$1.out" ||
        grep -q ' errors=0$' "$1.out"; then
        cat "$1.out" >&2
        echo "$1 $2: expected invariant $3 reported, got exit $rc" >&2
        exit 1
    fi
}

# clean IMAGE: the check passes without a shape event and the verifier
# agrees.
clean()
{
    check "$1" ro
    if test "$rc" -ne 0 || grep -q "^event $shape_event " "$1.out"; then
        cat "$1.out" >&2
        echo "$1: expected a clean check, got exit $rc" >&2
        exit 1
    fi
    "$driver" --verify-shape "$1" >/dev/null
}

sound="$workdir/sound.ntfs"
truncate -s 64M "$sound"
"$mkntfs" -F -Q -q -L CHKSHAPE "$sound"
long=$(printf '%0190d' 0 | tr 0 n)
"$driver" --create-dir "$sound" /big >/dev/null
count=0
while test "$count" -lt 60; do
    count=$((count + 1))
    "$driver" --create-file "$sound" "$(printf '/big/%s%05u' "$long" "$count")" >/dev/null
done
clean "$sound"

# An extension record whose base has no attribute list: the check used to
# end with no problem left and the volume clean. Each case has its own
# image, since a file just closed can stay busy on some hosts.
image="$workdir/extension.ntfs"
cp "$sound" "$image"
python3 -I "$tool" "$image" extension
reported "$image" ro 8
reported "$image" fix 8
reported "$image" ro 8

# A mapping that does not cover its allocation: reported, next to the
# finding of the check itself.
image="$workdir/allocation.ntfs"
cp "$sound" "$image"
python3 -I "$tool" "$image" allocation
reported "$image" fix 10

# An index block in use but marked free: reported before the repair, and
# the repair leaves a sound shape.
image="$workdir/bitmap.ntfs"
cp "$sound" "$image"
python3 -I "$tool" "$image" bitmap
reported "$image" ro 14
check "$image" fix
if test "$rc" -ne 0 || grep -q "^event $shape_event " "$image.out"; then
    cat "$image.out" >&2
    echo "bitmap fix: expected the repair to leave a sound shape, got exit $rc" >&2
    exit 1
fi
clean "$image"
