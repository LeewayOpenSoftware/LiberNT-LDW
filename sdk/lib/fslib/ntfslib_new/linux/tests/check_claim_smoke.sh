#!/bin/sh
#
# PROJECT:     LiberNT NTFS library tests
# LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
# PURPOSE:     The checker claims in-use clusters even while a structural
#              finding blocks every other repair, and frees lost clusters
#              only once that finding is gone
# COPYRIGHT:   Copyright 2026 Leeway Open Software
#
set -eu

driver=$1
mkntfs=$2

workdir=$(mktemp -d "${TMPDIR:-/tmp}/ntfslib-claim.XXXXXX")
trap 'rm -rf "$workdir"' EXIT HUP INT TERM
image="$workdir/claim.ntfs"

truncate -s 32M "$image"
"$mkntfs" -F -Q -q -L CLAIM "$image"

# edit <action>: damage or restore the image, print what it touched.
#   damage:  clear the $Bitmap bit of the first $MFT cluster (in use, now
#            marked free), set the bit of one free cluster (lost), and give
#            system record 8 ($BadClus) a wrong record number, which the
#            checker reports as corrupt and never frees.
#   restore: put the number of record 8 back.
#   bits:    print the two bits.
edit()
{
    python3 -I - "$image" "$1" "$workdir/lost" <<'EOF'
import struct
import sys

image, action, lost_file = sys.argv[1], sys.argv[2], sys.argv[3]
with open(image, 'r+b') as f:
    data = bytearray(f.read())
    sector = struct.unpack_from('<H', data, 0x0B)[0]
    cluster = sector * data[0x0D]
    total = struct.unpack_from('<Q', data, 0x28)[0] // data[0x0D]
    mft_lcn = struct.unpack_from('<Q', data, 0x30)[0]
    raw = struct.unpack_from('<b', data, 0x40)[0]
    size = 1 << -raw if raw < 0 else raw * cluster

    def record(number):
        off = mft_lcn * cluster + number * size
        r = bytearray(data[off:off + size])
        usa_ofs, usa_cnt = struct.unpack_from('<HH', r, 4)
        for i in range(1, usa_cnt):
            r[i * sector - 2:i * sector] = r[usa_ofs + 2 * i:usa_ofs + 2 * i + 2]
        return r

    bitmap = record(6)
    pos = struct.unpack_from('<H', bitmap, 0x14)[0]
    while struct.unpack_from('<I', bitmap, pos)[0] != 0x80:
        pos += struct.unpack_from('<I', bitmap, pos + 4)[0]
    pairs = pos + struct.unpack_from('<H', bitmap, pos + 0x20)[0]
    head = bitmap[pairs]
    length_size, offset_size = head & 15, head >> 4
    start = pairs + 1 + length_size
    bitmap_lcn = int.from_bytes(bitmap[start:start + offset_size], 'little', signed=True)
    base = bitmap_lcn * cluster

    def bit(lcn):
        return (data[base + lcn // 8] >> (lcn % 8)) & 1

    record8 = mft_lcn * cluster + 8 * size + 0x2C
    if action == 'damage':
        lost = next(lcn for lcn in range(total - 2, total // 2, -1) if not bit(lcn))
        data[base + mft_lcn // 8] &= ~(1 << (mft_lcn % 8)) & 0xFF
        data[base + lost // 8] |= 1 << (lost % 8)
        struct.pack_into('<I', data, record8, 0x7777)
        with open(lost_file, 'w') as out:
            out.write('%d\n' % lost)
    elif action == 'restore':
        struct.pack_into('<I', data, record8, 8)
    else:
        with open(lost_file) as inp:
            lost = int(inp.read())
        print('mft=%d lost=%d' % (bit(mft_lcn), bit(lost)))
    f.seek(0)
    f.write(data)
EOF
}

# check <expected exit> <args>: run the checker, keep its summary line.
check()
{
    expected=$1
    shift
    rc=0
    "$driver" --check "$image" "$@" >"$workdir/check.txt" || rc=$?
    if test "$rc" -ne "$expected"; then
        cat "$workdir/check.txt" >&2
        echo "check $*: exit $rc, expected $expected" >&2
        exit 1
    fi
    summary=$(grep '^dirty=' "$workdir/check.txt")
}

field()
{
    echo "$summary" | sed -n "s/.* $1=\([0-9]*\).*/\1/p"
}

check 0

edit damage
check 3 --fix
# The structural finding stays and blocks the index repairs and the free,
# but the in-use cluster is claimed.
test "$(field claimed)" -ge 1
test "$(field freed)" -eq 0
test "$(edit bits)" = "mft=1 lost=1"

# Nothing is left to claim, and the lost cluster is still not freed.
check 3 --fix
test "$(field claimed)" -eq 0
test "$(field freed)" -eq 0

# Once the structural finding is gone, the lost cluster is freed and the
# volume converges.
edit restore
check 0 --fix
test "$(field freed)" -ge 1
test "$(edit bits)" = "mft=1 lost=0"
check 0
