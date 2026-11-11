#!/bin/sh
# PROJECT:     LiberNT NTFS library tests
# LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
# PURPOSE:     A directory index whose allocation is fragmented keeps growing
#              once its mapping no longer fits the base record: an attribute
#              list then names an extension record, and no create fails
# COPYRIGHT:   Copyright 2026 Leeway Open Software

set -eu

driver=$1
mkntfs=$2

workdir=$(mktemp -d "${TMPDIR:-/tmp}/ntfslib-fragindex.XXXXXX")
trap 'rm -rf "$workdir"' EXIT HUP INT TERM
image="$workdir/fragindex.ntfs"
payload="$workdir/payload.bin"
tool="$workdir/shape.py"

truncate -s 128M "$image"
"$mkntfs" -F -Q -q -L FRAGINDEX "$image"
head -c 4096 /dev/zero | tr '\0' 'p' >"$payload"

# Prints "list=L extensions=E free=F attrs=..." for /big: L is 1 when its
# base record has an $ATTRIBUTE_LIST, E counts the in-use extension records
# that name it, F is the free space of the base record, and attrs lists its
# attributes as type:length.
cat >"$tool" <<'EOF'
import struct
import sys

data = open(sys.argv[1], 'rb').read()
sector = struct.unpack_from('<H', data, 0x0B)[0]
cluster = sector * data[0x0D]
mft_lcn = struct.unpack_from('<Q', data, 0x30)[0]
raw = struct.unpack_from('<b', data, 0x40)[0]
size = 1 << -raw if raw < 0 else raw * cluster


def unfix(r):
    usa_ofs, usa_cnt = struct.unpack_from('<HH', r, 4)
    for i in range(1, usa_cnt):
        r[i * sector - 2:i * sector] = r[usa_ofs + 2 * i:usa_ofs + 2 * i + 2]
    return r


def attrs(r):
    pos = struct.unpack_from('<H', r, 0x14)[0]
    while pos + 8 <= len(r):
        kind, length = struct.unpack_from('<II', r, pos)
        if kind == 0xFFFFFFFF or length == 0:
            return
        yield kind, r[pos:pos + length]
        pos += length


def runs(attr):
    pos = struct.unpack_from('<H', attr, 0x20)[0]
    lcn = 0
    while attr[pos]:
        ls, os_ = attr[pos] & 15, attr[pos] >> 4
        length = int.from_bytes(attr[pos + 1:pos + 1 + ls], 'little')
        if os_:
            lcn += int.from_bytes(attr[pos + 1 + ls:pos + 1 + ls + os_], 'little', signed=True)
        yield lcn, length
        pos += 1 + ls + os_


mft0 = unfix(bytearray(data[mft_lcn * cluster:mft_lcn * cluster + size]))
offsets = []
for lcn, length in runs(next(a for k, a in attrs(mft0) if k == 0x80 and a[8])):
    offsets.extend(lcn * cluster + c * size for c in range(length * cluster // size))


def record(n):
    r = unfix(bytearray(data[offsets[n]:offsets[n] + size]))
    if r[0:4] != b'FILE' or not struct.unpack_from('<H', r, 0x16)[0] & 1:
        return None
    return r


big = None
for n in range(16, len(offsets)):
    r = record(n)
    if r is None or struct.unpack_from('<Q', r, 0x20)[0] or not struct.unpack_from('<H', r, 0x16)[0] & 2:
        continue
    for k, a in attrs(r):
        if k == 0x30:
            v = a[struct.unpack_from('<H', a, 0x14)[0]:]
            if v[0x42:0x42 + 2 * v[0x40]].decode('utf-16-le') == 'big' and \
               struct.unpack_from('<Q', v, 0)[0] & 0xFFFFFFFFFFFF == 5:
                big = n
if big is None:
    sys.exit('no /big directory')
has_list = any(k == 0x20 for k, a in attrs(record(big)))
extensions = 0
for n in range(16, len(offsets)):
    r = record(n)
    if r is not None and struct.unpack_from('<Q', r, 0x20)[0] & 0xFFFFFFFFFFFF == big:
        extensions += 1
base = record(big)
used, alloc = struct.unpack_from('<II', base, 0x18)
layout = ','.join('%x:%x' % (k, len(a)) for k, a in attrs(base))
print('list=%d extensions=%d free=0x%x attrs=%s' % (has_list, extensions, alloc - used, layout))
EOF

long=$(printf '%0190d' 0 | tr 0 n)
"$driver" --create-dir "$image" /big >/dev/null
"$driver" --create-dir "$image" /pad >/dev/null
count=0
while test "$count" -lt 3000; do
    count=$((count + 1))
    if ! "$driver" --create-file "$image" "$(printf '/big/%s%05u.dat' "$long" "$count")" >/dev/null; then
        echo "create $count in /big failed: $(python3 -I "$tool" "$image")" >&2
        exit 1
    fi
    # A data block between index blocks keeps the allocation fragmented.
    if test $((count % 8)) -eq 0; then
        "$driver" --create-file "$image" "$(printf '/pad/p%05u' "$count")" >/dev/null
        "$driver" --write "$image" "$(printf '/pad/p%05u' "$count")" 0 "$payload" >/dev/null
    fi
done

test "$("$driver" --list "$image" /big | wc -l)" -eq 3000
shape=$(python3 -I "$tool" "$image")
case "$shape" in
    "list=1 extensions="[1-9]*)
        ;;
    *)
        echo "/big did not move its index mapping to an extension record: $shape" >&2
        exit 1
        ;;
esac

rc=0
"$driver" --check "$image" >"$workdir/check.txt" || rc=$?
if test "$rc" -ne 0; then
    cat "$workdir/check.txt" >&2
    echo "check: exit $rc" >&2
    exit 1
fi

# Every list names its extensions, every mapping covers its allocation, and
# every index block in use is reachable.
"$driver" --verify-shape "$image" >/dev/null
