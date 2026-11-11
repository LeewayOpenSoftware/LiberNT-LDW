#!/bin/sh
# PROJECT:     LiberNT NTFS library tests
# LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
# PURPOSE:     The checker writes an unreadable attribute list again from the
#              base record and every extension record: a non-resident list
#              that reads as zeros and a resident list of garbage, each with
#              several extensions; the new list names every attribute and
#              every extension, the file keeps its names and data, and the
#              volume then checks clean and keeps its shape
# COPYRIGHT:   Copyright 2026 Leeway Open Software

set -eu

driver=$1
mkntfs=$2

workdir=$(mktemp -d "${TMPDIR:-/tmp}/ntfslib-attrlist-rebuild.XXXXXX")
trap 'rm -rf "$workdir"' EXIT HUP INT TERM
payload="$workdir/payload.bin"
tool="$workdir/list.py"

# list.py IMAGE ACTION NAME, for the file NAME in the root directory:
#   probe:  "resident|nonresident|none EXTENSIONS"
#   record: "RECORD EXTENSIONS", the record in hex
#   damage: zero the clusters of a non-resident list, or fill a resident
#           list with garbage
#   verify: the list holds exactly one entry for each attribute of the base
#           record and of every extension record, sorted, so it names every
#           extension
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

# Events of the check (NtfsCheckEvent in ntfslib_new.h)
unreadable=29
rebuilt=43

long=$(printf '%0200d' 0 | tr 0 n)
head -c 12288 /dev/zero | tr '\0' 'd' >"$payload"

# rebuild RESIDENCY LINKS DATA: a file with LINKS long hard links, one per
# extension record, and with DATA 1 a non-resident data stream; its list is
# then damaged. The data stream takes room in the base record, so a list
# with several extensions next to it is no longer resident.
rebuild()
{
    image="$workdir/$1.ntfs"
    truncate -s 64M "$image"
    "$mkntfs" -F -Q -q -L REBUILD "$image"
    "$driver" --create-file "$image" /h >/dev/null
    if test "$3" -eq 1; then
        "$driver" --write "$image" /h 0 "$payload" >/dev/null
    fi
    count=0
    while test "$count" -lt "$2"; do
        count=$((count + 1))
        "$driver" --link "$image" /h "/$long$count" >/dev/null
    done

    shape=$(python3 -I "$tool" "$image" probe h)
    case "$shape" in
        "$1 "*)
            ;;
        *)
            echo "fixture: expected a $1 list, got $shape" >&2
            exit 1
            ;;
    esac
    set -- "$1" "$2" "$3" $(python3 -I "$tool" "$image" record h)
    record=$4
    extensions=$5
    if test "$extensions" -lt 2; then
        echo "fixture: $1 list with $extensions extension records" >&2
        exit 1
    fi
    python3 -I "$tool" "$image" verify h >/dev/null
    "$driver" --verify-shape "$image" >/dev/null

    python3 -I "$tool" "$image" damage h
    rc=0
    "$driver" --check "$image" --fix >"$workdir/fix.txt" || rc=$?
    if test "$rc" -ne 0 && test "$rc" -ne 3; then
        cat "$workdir/fix.txt" >&2
        echo "$1: check --fix: exit $rc" >&2
        exit 1
    fi
    if ! grep -q "^event $unreadable $record " "$workdir/fix.txt" ||
       ! grep -q "^event $rebuilt $record $(printf '%x' "$extensions")\$" "$workdir/fix.txt"; then
        cat "$workdir/fix.txt" >&2
        echo "$1: the list of record $record was not rebuilt from $extensions extensions" >&2
        exit 1
    fi

    python3 -I "$tool" "$image" verify h >/dev/null
    rc=0
    "$driver" --check "$image" >"$workdir/check.txt" || rc=$?
    if test "$rc" -ne 0; then
        cat "$workdir/check.txt" >&2
        echo "$1: check after the rebuild: exit $rc" >&2
        exit 1
    fi
    "$driver" --verify-shape "$image" >/dev/null
    test "$("$driver" --list "$image" / | grep -c "$long")" -eq "$2"
    if test "$3" -eq 1; then
        "$driver" --cat "$image" "/${long}1" | cmp -s - "$payload"
    fi
}

rebuild nonresident 8 1
rebuild resident 4 0
