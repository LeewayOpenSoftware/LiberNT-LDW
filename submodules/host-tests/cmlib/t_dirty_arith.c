/*
 * PROJECT:     LiberNT host-native tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Self-contained check of the HvMarkCellDirty block-range arithmetic
 *              (dx2400 STOP 0x1E at cmapi.c:1015). No cmlib link: it reproduces the
 *              pre-fix and post-fix range computations side by side and proves the
 *              pre-fix one under-marks block-aligned and multi-block cells while the
 *              post-fix one (Stanley 3fd048291d) marks every block. Always green; it
 *              guards the discriminator that t_hvmarkdirty.c exercises on the real code.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */
#include <stdio.h>
#include <string.h>

#define HBLOCK_SIZE 0x1000u
#define HCELL_BLOCK_MASK 0x7ffff000u
#define BlockOf(c) (((c) & HCELL_BLOCK_MASK) >> 12)

static unsigned char bits[0x10000];
static void setbits(unsigned start, unsigned n) { for (unsigned i = 0; i < n; i++) bits[start + i] = 1; }
static int checkbit(unsigned b) { return bits[b]; }

/* pre-fix HvMarkCellDirty: count = HvGetCellBlock(idx + HBLOCK_SIZE - 1) - HvGetCellBlock(idx) */
static void mark_pre(unsigned idx)
{
    unsigned cb = BlockOf(idx), lb = BlockOf(idx + HBLOCK_SIZE - 1);
    setbits(cb, lb - cb);
}

/* post-fix: size from the cell header, header-block..last-byte-block, +1, bounded by storage length */
static void mark_post(unsigned idx, int size, unsigned stable_len)
{
    if (size < 0) { size = -size; }
    if (size < 8) { size = 8; }
    unsigned cb = BlockOf(idx), lb = BlockOf(idx + (unsigned)size - 1);
    unsigned last = stable_len / HBLOCK_SIZE;
    if (lb >= last) { lb = last - 1; }
    setbits(cb, lb - cb + 1);
}

static int occupies_all(unsigned idx, int size)
{
    if (size < 0) { size = -size; }
    unsigned cb = BlockOf(idx), lb = BlockOf(idx + (unsigned)size - 1);
    for (unsigned b = cb; b <= lb; b++)
        if (!checkbit(b)) return 0;
    return 1;
}

int main(void)
{
    unsigned stable = 0x30000; /* 48 blocks */
    struct { const char *n; unsigned idx; int size; } cases[] = {
        { "block-aligned, 1 block", 0x3000, 0x40 },
        { "block-aligned, 3 blocks", 0x2000, 0x2800 },
        { "unaligned, 1 block", 0x2a050, 0x80 },
        { "unaligned, 2 blocks", 0x1ff0, 0x400 },
    };
    int prefail = 0, postfail = 0;
    for (int c = 0; c < 4; c++)
    {
        memset(bits, 0, sizeof bits);
        mark_pre(cases[c].idx);
        int preok = occupies_all(cases[c].idx, cases[c].size);
        memset(bits, 0, sizeof bits);
        mark_post(cases[c].idx, cases[c].size, stable);
        int postok = occupies_all(cases[c].idx, cases[c].size);
        printf("%-26s pre=%s post=%s\n", cases[c].n,
               preok ? "all-dirty" : "UNDER-MARKED", postok ? "all-dirty" : "UNDER-MARKED");
        if (!preok) { prefail++; }
        if (!postok) { postfail++; }
    }
    printf("\npre-fix under-marks %d/4 (expect >0); post-fix under-marks %d/4 (expect 0)\n",
           prefail, postfail);
    return (prefail > 0 && postfail == 0) ? 0 : 1;
}
