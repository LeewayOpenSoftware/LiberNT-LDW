#!/bin/sh
# PROJECT:     LiberNT NTFS library tests
# LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
# PURPOSE:     The checker rebuilds the attribute list of a directory that reads
#              as zeros while extension records hold its index extents, and
#              the directory keeps every name
# COPYRIGHT:   Copyright 2026 Leeway Open Software

set -eu

driver=$1
mkntfs=$2

workdir=$(mktemp -d "${TMPDIR:-/tmp}/ntfslib-attrlist.XXXXXX")
trap 'rm -rf "$workdir"' EXIT HUP INT TERM
image="$workdir/attrlist.ntfs"
payload="$workdir/payload.bin"
tool="$workdir/mft.py"

truncate -s 64M "$image"
"$mkntfs" -F -Q -q -L ATTRLIST "$image"
head -c 8192 /dev/zero | tr '\0' 'x' >"$payload"

cat >"$tool" <<'EOF'
import struct
import sys

image, action = sys.argv[1], sys.argv[2]
f = open(image, 'r+b')
data = bytearray(f.read())
sector = struct.unpack_from('<H', data, 0x0B)[0]
cluster = sector * data[0x0D]
mft_lcn = struct.unpack_from('<Q', data, 0x30)[0]
raw = struct.unpack_from('<b', data, 0x40)[0]
size = 1 << -raw if raw < 0 else raw * cluster


def runs(attr):
    pos = struct.unpack_from('<H', attr, 0x20)[0]
    lcn = 0
    out = []
    while attr[pos]:
        ls, os_ = attr[pos] & 15, attr[pos] >> 4
        length = int.from_bytes(attr[pos + 1:pos + 1 + ls], 'little')
        if os_:
            lcn += int.from_bytes(attr[pos + 1 + ls:pos + 1 + ls + os_], 'little', signed=True)
            out.append((lcn, length))
        else:
            out.append((None, length))
        pos += 1 + ls + os_
    return out


def unfix(r):
    usa_ofs, usa_cnt = struct.unpack_from('<HH', r, 4)
    for i in range(1, usa_cnt):
        r[i * sector - 2:i * sector] = r[usa_ofs + 2 * i:usa_ofs + 2 * i + 2]
    return r


def refix(r):
    usa_ofs, usa_cnt = struct.unpack_from('<HH', r, 4)
    usn = r[usa_ofs:usa_ofs + 2]
    for i in range(1, usa_cnt):
        r[usa_ofs + 2 * i:usa_ofs + 2 * i + 2] = r[i * sector - 2:i * sector]
        r[i * sector - 2:i * sector] = usn
    return r


def attrs(r):
    pos = struct.unpack_from('<H', r, 0x14)[0]
    while pos + 8 <= len(r):
        kind, length = struct.unpack_from('<II', r, pos)
        if kind == 0xFFFFFFFF or length == 0:
            return
        yield kind, pos, r[pos:pos + length]
        pos += length


mft0 = unfix(bytearray(data[mft_lcn * cluster:mft_lcn * cluster + size]))
mft_runs = next(runs(a) for k, p, a in attrs(mft0) if k == 0x80 and a[8])
offsets = []
for lcn, length in mft_runs:
    for c in range(length * cluster // size):
        offsets.append(lcn * cluster + c * size)


def record(n):
    return unfix(bytearray(data[offsets[n]:offsets[n] + size]))


def put(n, r):
    data[offsets[n]:offsets[n] + size] = refix(r)


def in_use(r):
    return r[0:4] == b'FILE' and struct.unpack_from('<H', r, 0x16)[0] & 1


grow = None
for n in range(len(offsets)):
    r = record(n)
    if not in_use(r) or struct.unpack_from('<Q', r, 0x20)[0] or not struct.unpack_from('<H', r, 0x16)[0] & 2:
        continue
    for k, p, a in attrs(r):
        if k == 0x30:
            v = a[struct.unpack_from('<H', a, 0x14)[0]:]
            name = v[0x42:0x42 + 2 * v[0x40]].decode('utf-16-le')
            if name == 'grow' and struct.unpack_from('<Q', v, 0)[0] & 0xFFFFFFFFFFFF == 5:
                grow = n
if grow is None:
    sys.exit('no /grow directory')
base = record(grow)
lists = [(p, a) for k, p, a in attrs(base) if k == 0x20]
spilled = False
for n in range(len(offsets)):
    r = record(n)
    if in_use(r) and struct.unpack_from('<Q', r, 0x20)[0] & 0xFFFFFFFFFFFF == grow:
        if any(k == 0xA0 for k, p, a in attrs(r)):
            spilled = True

if action == 'probe':
    print('ready' if lists and spilled else 'no')
elif action == 'record':
    print('%x' % grow)
elif action == 'damage':
    pos, a = lists[0]
    if a[8]:
        # Nonresident: the clusters of the list read as zeros, as after a cut
        # that never wrote them.
        for lcn, length in runs(a):
            if lcn is None:
                continue
            data[lcn * cluster:(lcn + length) * cluster] = bytes(length * cluster)
    else:
        vlen = struct.unpack_from('<I', a, 0x10)[0]
        start = pos + struct.unpack_from('<H', a, 0x14)[0]
        base[start:start + vlen] = bytes(vlen)
        put(grow, base)
    f.seek(0)
    f.write(data)
f.close()
EOF

check()
{
    rc=0
    "$driver" --check "$image" "$@" >"$workdir/check.txt" || rc=$?
}

check
test "$rc" -eq 0

# Grow the index of /grow while another directory takes clusters in between, until
# its $INDEX_ALLOCATION runs no longer fit the base record and an extension
# record holds part of them.
"$driver" --create-dir "$image" /grow >/dev/null
"$driver" --create-dir "$image" /frag >/dev/null
count=0
round=0
while test "$(python3 -I "$tool" "$image" probe)" != ready; do
    round=$((round + 1))
    if test "$round" -gt 200; then
        echo "fixture: /grow never spilled its index into an extension record" >&2
        exit 1
    fi
    i=0
    while test "$i" -lt 20; do
        count=$((count + 1))
        "$driver" --create-file "$image" \
            "$(printf '/grow/a-fairly-long-entry-name-to-fill-blocks-%05u.dat' "$count")" >/dev/null
        i=$((i + 1))
    done
    "$driver" --create-file "$image" "$(printf '/frag/f%04u' "$round")" >/dev/null
    "$driver" --write "$image" "$(printf '/frag/f%04u' "$round")" 0 "$payload" >/dev/null
done
check
test "$rc" -eq 0
grow=$(python3 -I "$tool" "$image" record)

python3 -I "$tool" "$image" damage
check --fix
if test "$rc" -ne 0 && test "$rc" -ne 3; then
    cat "$workdir/check.txt" >&2
    echo "check --fix: exit $rc" >&2
    exit 1
fi
grep -q "^event 29 $grow " "$workdir/check.txt"
if ! grep -q "^event 43 $grow " "$workdir/check.txt"; then
    cat "$workdir/check.txt" >&2
    echo "the attribute list of record $grow was not rebuilt" >&2
    exit 1
fi

check
if test "$rc" -ne 0; then
    cat "$workdir/check.txt" >&2
    echo "check after the rebuild: exit $rc" >&2
    exit 1
fi
test "$("$driver" --list "$image" /grow | wc -l)" -eq "$count"
