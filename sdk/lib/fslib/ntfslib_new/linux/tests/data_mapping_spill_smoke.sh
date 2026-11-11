#!/bin/sh
# PROJECT:     LiberNT NTFS library tests
# LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
# PURPOSE:     A data stream whose mapping fills the base record keeps
#              growing through sparse writes and cluster moves: an attribute
#              list names the extension records that take the mapping, no
#              write or move fails, the data reads back, and the volume
#              checks clean and keeps its shape
# COPYRIGHT:   Copyright 2026 Leeway Open Software

set -eu

driver=$1
mkntfs=$2

workdir=$(mktemp -d "${TMPDIR:-/tmp}/ntfslib-dataspill.XXXXXX")
trap 'rm -rf "$workdir"' EXIT HUP INT TERM
tool="$workdir/list.py"
build="$workdir/build.py"

# list.py IMAGE ACTION NAME, for the file NAME in the root directory:
#   probe:  "resident|nonresident|none EXTENSIONS"
#   verify: the list holds exactly one entry for each attribute of the base
#           record and of every extension record, sorted
cat >"$tool" <<'EOF'
import struct
import sys

image, action, name = sys.argv[1], sys.argv[2], sys.argv[3]
f = open(image, 'r+b')
data = bytearray(f.read())
sector = struct.unpack_from('<H', data, 0x0B)[0]
cluster = sector * data[0x0D]
raw = struct.unpack_from('<b', data, 0x40)[0]
size = 1 << -raw if raw < 0 else raw * cluster
start = struct.unpack_from('<Q', data, 0x30)[0] * cluster


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
    usa, count = struct.unpack_from('<HH', r, 4)
    for i in range(1, count):
        r[i * sector - 2:i * sector] = r[usa + 2 * i:usa + 2 * i + 2]
    return r


def refix(r):
    usa, count = struct.unpack_from('<HH', r, 4)
    for i in range(1, count):
        r[usa + 2 * i:usa + 2 * i + 2] = r[i * sector - 2:i * sector]
        r[i * sector - 2:i * sector] = r[usa:usa + 2]
    return r


def attrs(r):
    pos = struct.unpack_from('<H', r, 0x14)[0]
    while pos + 8 <= len(r):
        kind, length = struct.unpack_from('<II', r, pos)
        if kind == 0xFFFFFFFF or length == 0:
            return
        yield kind, pos, r[pos:pos + length]
        pos += length


offsets = []
for lcn, length in next(runs(a) for k, p, a in attrs(unfix(bytearray(data[start:start + size])))
                        if k == 0x80 and a[8]):
    offsets += [lcn * cluster + c * size for c in range(length * cluster // size)]


def record(n):
    return unfix(bytearray(data[offsets[n]:offsets[n] + size]))


def flags(r):
    return struct.unpack_from('<H', r, 0x16)[0] if r[0:4] == b'FILE' else 0


def owner(r):
    return struct.unpack_from('<Q', r, 0x20)[0]


def attr_name(a):
    return a[struct.unpack_from('<H', a, 0x0A)[0]:][:2 * a[9]].decode('utf-16-le')


base = None
for n in range(len(offsets)):
    r = record(n)
    if not flags(r) & 1 or owner(r):
        continue
    for k, p, a in attrs(r):
        if k == 0x30:
            v = a[struct.unpack_from('<H', a, 0x14)[0]:]
            if (v[0x42:0x42 + 2 * v[0x40]].decode('utf-16-le') == name and
                    struct.unpack_from('<Q', v, 0)[0] & 0xFFFFFFFFFFFF == 5):
                base = n
if base is None:
    sys.exit('no /%s' % name)
b = record(base)
seq = struct.unpack_from('<H', b, 0x10)[0]
extensions = [n for n in range(len(offsets))
              if flags(record(n)) & 1 and owner(record(n)) == base | seq << 48]
lists = [(p, a) for k, p, a in attrs(b) if k == 0x20]


def expected():
    out = set()
    for n in [base] + extensions:
        r = record(n)
        ref = n | struct.unpack_from('<H', r, 0x10)[0] << 48
        for k, p, a in attrs(r):
            if k != 0x20:
                vcn = struct.unpack_from('<Q', a, 0x10)[0] if a[8] else 0
                out.add((k, attr_name(a), vcn, ref, struct.unpack_from('<H', a, 0x0E)[0]))
    return out


def entries():
    p, a = lists[0]
    if a[8]:
        value = b''.join(bytes(data[lcn * cluster:(lcn + length) * cluster]) if lcn is not None
                         else bytes(length * cluster) for lcn, length in runs(a))
        value = value[:struct.unpack_from('<Q', a, 0x30)[0]]
    else:
        value = a[struct.unpack_from('<H', a, 0x14)[0]:][:struct.unpack_from('<I', a, 0x10)[0]]
    out = []
    pos = 0
    while pos < len(value):
        kind, length, nlen, nofs, vcn, ref, ident = struct.unpack_from('<IHBBQQH', value, pos)
        if length < 0x1A or pos + length > len(value):
            sys.exit('list entry at 0x%x is malformed' % pos)
        out.append((kind, value[pos + nofs:pos + nofs + 2 * nlen].decode('utf-16-le'), vcn, ref, ident))
        pos += length
    return out


if action == 'probe':
    # "resident|nonresident EXTENSIONS", or "none 0" with no list
    print('%s %d' % (('nonresident' if lists[0][1][8] else 'resident') if lists else 'none', len(extensions)))
elif action == 'record':
    print('%x %d' % (base, len(extensions)))
elif action == 'damage':
    p, a = lists[0]
    if a[8]:
        # The clusters of the list read as zeros, as after a cut that never
        # wrote them.
        for lcn, length in runs(a):
            if lcn is not None:
                data[lcn * cluster:(lcn + length) * cluster] = bytes(length * cluster)
    else:
        # A resident list of garbage: its first entry claims a length
        # beyond the list.
        vlen = struct.unpack_from('<I', a, 0x10)[0]
        value = p + struct.unpack_from('<H', a, 0x14)[0]
        b[value:value + vlen] = b'\xa5' * vlen
        data[offsets[base]:offsets[base] + size] = refix(b)
    f.seek(0)
    f.write(data)
elif action == 'verify':
    # The list names every attribute of the base and of every extension,
    # once, sorted by type, name and first VCN, and every extension.
    have = entries()
    want = expected()
    if sorted(have) != sorted(want) or len(have) != len(set(have)):
        sys.exit('list %s\nwant %s' % (sorted(have), sorted(want)))
    keys = [(k, n.upper(), v) for k, n, v, r, i in have]
    if keys != sorted(keys):
        sys.exit('list out of order: %s' % keys)
    named = set(r & 0xFFFFFFFFFFFF for k, n, v, r, i in have)
    if not set(extensions) <= named:
        sys.exit('extensions %s, list names %s' % (extensions, sorted(named)))
    print('list ok: %d entries, %d extensions' % (len(have), len(extensions)))
f.close()
EOF

# build.py sparse OUT COUNT: the expected content of the sparse file, 4 KiB
# of chunk K at K * 8 KiB and zeros between, and each chunk in OUT.K.
# build.py move OUT CLUSTERS: every cluster tagged with its number.
cat >"$build" <<'EOF'
import sys

kind, out, count = sys.argv[1], sys.argv[2], int(sys.argv[3])
with open(out, 'wb') as f:
    if kind == 'sparse':
        for k in range(count):
            if k:
                f.write(bytes(4096))
            f.write(b'%04096d' % k)
            with open('%s.%d' % (out, k), 'wb') as chunk:
                chunk.write(b'%04096d' % k)
    else:
        for k in range(count):
            f.write(b'%04096d' % k)
EOF

# settle NAME IMAGE: the list exists and names extension records, the
# volume checks clean and the verifier finds every shape sound
settle()
{
    shape=$(python3 -I "$tool" "$2" probe "$1")
    case "$shape" in
        resident\ 0|nonresident\ 0|none\ *)
            echo "$1: the mapping never left the base record ($shape)" >&2
            exit 1
            ;;
    esac
    python3 -I "$tool" "$2" verify "$1" >/dev/null
    rc=0
    "$driver" --check "$2" >"$workdir/check.txt" || rc=$?
    if test "$rc" -ne 0; then
        cat "$workdir/check.txt" >&2
        echo "$1: check: exit $rc" >&2
        exit 1
    fi
    "$driver" --verify-shape "$2" >/dev/null
}

# A sparse write past the end adds a data run and a hole for each chunk.
# The base record used to fill up at chunk 139 and every further write
# failed STATUS_BUFFER_TOO_SMALL.
image="$workdir/sparse.ntfs"
truncate -s 64M "$image"
"$mkntfs" -F -Q -q -L SPARSE "$image"
"$driver" --create-file "$image" /s >/dev/null
"$driver" --set-sparse "$image" /s 1 >/dev/null
python3 -I "$build" sparse "$workdir/expect" 300
count=0
while test "$count" -lt 300; do
    "$driver" --write "$image" /s $((count * 8192)) "$workdir/expect.$count" >/dev/null
    count=$((count + 1))
done
settle s "$image"
"$driver" --cat "$image" /s | cmp - "$workdir/expect"

# Moving every other cluster of a contiguous stream splits it into a run
# per cluster. The base record used to fill up at move 87 and every further
# move failed STATUS_BUFFER_TOO_SMALL.
image="$workdir/move.ntfs"
truncate -s 64M "$image"
"$mkntfs" -F -Q -q -L MOVE "$image"
python3 -I "$build" move "$workdir/expect.move" 1200
"$driver" --create-file "$image" /m >/dev/null
"$driver" --write "$image" /m 0 "$workdir/expect.move" >/dev/null
count=0
while test "$count" -lt 300; do
    "$driver" --move "$image" /m $((count * 2)) $((9000 + count * 2)) 1 >/dev/null
    count=$((count + 1))
done
settle m "$image"
"$driver" --cat "$image" /m | cmp - "$workdir/expect.move"
