#!/bin/sh
#
# PROJECT:     LiberNT NTFS library tests
# LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
# PURPOSE:     Grow a fragmented directory index past the space of its base file record
# COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
#
set -eu

frontend=$1
mkntfs=$2
fusermount3=$3
ntfsls=$4
ntfsfix=$5

test -e /dev/fuse || exit 77

runtime_root=${NTFSLIB_BENCH_ROOT:-${TMPDIR:-/tmp}}
workdir=$(mktemp -d "$runtime_root/ntfslib-index-spill.XXXXXX")
image="$workdir/test.ntfs"
mountpoint="$workdir/mnt"
mounted=0
count=1300

unmount_fixture()
{
    "$fusermount3" -u "$mountpoint" 2>/dev/null ||
        umount "$mountpoint"
}

cleanup()
{
    if test "$mounted" -eq 1; then
        unmount_fixture >/dev/null 2>&1 || true
    fi
    rm -r -- "$workdir"
}
trap cleanup EXIT HUP INT TERM

# fail STEP: report where the run stopped and check the volume. With
# NTFSLIB_KEEP_FAILED set to a directory, the image as formatted and the
# image as left are copied there for a replay.
fail()
{
    echo "index spill: $1 failed" >&2
    if test "$mounted" -eq 1; then
        unmount_fixture >/dev/null 2>&1 || true
        mounted=0
    fi
    "$frontend" --check "$image" >&2 || true
    if test -n "${NTFSLIB_KEEP_FAILED:-}"; then
        mkdir -p "$NTFSLIB_KEEP_FAILED"
        cp "$workdir/formatted.ntfs" "$NTFSLIB_KEEP_FAILED/index-spill-formatted.ntfs"
        cp "$image" "$NTFSLIB_KEEP_FAILED/index-spill-failed.ntfs"
    fi
    exit 1
}

mkdir "$mountpoint"
truncate -s 256M "$image"
"$mkntfs" -F -Q -q -L INDEXSPILL "$image"
cp "$image" "$workdir/formatted.ntfs"

"$frontend" --writable "$image" "$mountpoint" || exit 77
mounted=1
attempt=0
while ! mountpoint -q "$mountpoint" && test "$attempt" -lt 50; do
    sleep 0.1
    attempt=$((attempt + 1))
done
mountpoint -q "$mountpoint" || exit 77

mkdir "$mountpoint/target" "$mountpoint/staging"

index=0
while test "$index" -lt "$count"; do
    name=$(printf 'controller_shared_button_image_%04d_knockout.png' "$index")
    head -c 5000 /dev/zero > "$mountpoint/target/$name" ||
        fail "write of target file $index"
    index=$((index + 1))
done

index=0
while test "$index" -lt "$count"; do
    name=$(printf 'controller_shared_button_image_%04d_knockout.png' "$index")
    mv "$mountpoint/target/$name" "$mountpoint/target/$name.old" ||
        fail "rename of target file $index"
    head -c 5000 /dev/zero > "$mountpoint/staging/$name" ||
        fail "write of staging file $index"
    mv "$mountpoint/staging/$name" "$mountpoint/target/$name" ||
        fail "move of staging file $index"
    index=$((index + 1))
done

entries=$(ls "$mountpoint/target" | wc -l)
test "$entries" -eq $((count * 2))
test "$(ls "$mountpoint/staging" | wc -l)" -eq 0

unmount_fixture
mounted=0

"$ntfsfix" -n "$image" >/dev/null
entries=$("$ntfsls" -f "$image" -p /target | grep -c '^controller_')
test "$entries" -eq $((count * 2))
