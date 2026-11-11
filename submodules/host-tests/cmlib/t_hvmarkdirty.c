/*
 * PROJECT:     LiberNT host-native tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     HvMarkCellDirty marks every block a cell occupies (regression for the
 *              dx2400 STOP 0x1E at cmapi.c:1015 / CmDeleteValueKey).
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Builds the real cmlib against its host shim (CMLIB_HOST). Creates a fresh hive,
 * forces two cells and checks the dirty bitmap:
 *   - a cell whose header is block-aligned (index % HBLOCK_SIZE == 0): the buggy
 *     HvMarkCellDirty set a zero-length range and left its block clean, which is
 *     exactly what tripped ASSERT(HvIsCellDirty(Hive, Cell)) on the checked kernel.
 *   - a cell that spans several blocks: the buggy code marked only its first block.
 * Fails on 25c67ffe3b4 (pre-fix), passes on 014bc3d0dab (Stanley 3fd048291d).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmlib.h>

static unsigned long Checks, Failures;
#define CHECK(c) do { Checks++; if (!(c)) { Failures++; \
    fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static PVOID NTAPI TestAllocate(SIZE_T Size, BOOLEAN Paged, ULONG Tag)
{
    (void)Paged; (void)Tag;
#ifdef _WIN32
    return _aligned_malloc((size_t)Size, 16);
#else
    void *p = NULL;
    if (posix_memalign(&p, 16, (size_t)Size)) return NULL;
    return p;
#endif
}

static VOID NTAPI TestFree(PVOID Ptr, ULONG Quota)
{
    (void)Quota;
#ifdef _WIN32
    _aligned_free(Ptr);
#else
    free(Ptr);
#endif
}

static BOOLEAN NTAPI TestRead(PHHIVE H, ULONG T, PULONG O, PVOID B, SIZE_T L)
{ (void)H;(void)T;(void)O;(void)B;(void)L; return FALSE; }
static BOOLEAN NTAPI TestWrite(PHHIVE H, ULONG T, PULONG O, PVOID B, SIZE_T L)
{ (void)H;(void)T;(void)O;(void)B;(void)L; return TRUE; }

/* Block that a stable cell index lives in, independent of the code under test. */
static ULONG BlockOf(HCELL_INDEX Cell)
{
    return (ULONG)((Cell & HCELL_BLOCK_MASK) >> HCELL_BLOCK_SHIFT);
}

int main(void)
{
    CMHIVE CmHive;
    PHHIVE Hive = &CmHive.Hive;
    NTSTATUS Status;
    HCELL_INDEX Big, Cell, Aligned = HCELL_NIL;
    LONG Size;
    ULONG i, Blocks, b;

    RtlZeroMemory(&CmHive, sizeof(CmHive));
    Status = HvInitialize(Hive, HINIT_CREATE, HIVE_NOLAZYFLUSH, HFILE_TYPE_PRIMARY,
                          0, TestAllocate, TestFree, TestWrite, TestRead, 1, NULL);
    if (!NT_SUCCESS(Status))
    {
        fprintf(stderr, "FAIL HvInitialize 0x%lx\n", (unsigned long)Status);
        return 2;
    }

    /* A cell that spans several blocks: a size well over HBLOCK_SIZE. */
    Big = HvAllocateCell(Hive, 3 * HBLOCK_SIZE - 0x40, Stable, HCELL_NIL);
    if (Big == HCELL_NIL) { fprintf(stderr, "FAIL alloc big\n"); HvFree(Hive); return 2; }

    /* A cell whose header lands on a block boundary. A bin begins with its header, so
     * only a cell inside a multi-block bin can start on an interior block boundary.
     * Make a multi-block bin and free it, then carve small cells from its start: they
     * pack in 8-byte steps from <bin>+0x20, so one lands exactly on <bin>+HBLOCK_SIZE.
     * Bounded; fail the setup if never hit. */
    Cell = HvAllocateCell(Hive, 3 * HBLOCK_SIZE - 0x40, Stable, HCELL_NIL);
    if (Cell == HCELL_NIL) { fprintf(stderr, "FAIL alloc scratch\n"); HvFree(Hive); return 2; }
    HvFreeCell(Hive, Cell);
    for (i = 0; i < 8192 && Aligned == HCELL_NIL; i++)
    {
        Cell = HvAllocateCell(Hive, 1, Stable, HCELL_NIL);
        if (Cell == HCELL_NIL) break;
        if (Cell != 0 && (Cell & HCELL_OFFSET_MASK) == 0)
            Aligned = Cell;
    }
    if (Aligned == HCELL_NIL)
    {
        fprintf(stderr, "FAIL setup: no block-aligned cell produced\n");
        HvFree(Hive);
        return 2;
    }

    /* Allocation marks cells dirty, so clear the vector before each case to test
     * HvMarkCellDirty in isolation (otherwise a neighbour masks the bug). */

    /* --- block-aligned cell: its own block must come back dirty --- */
    RtlClearAllBits(&Hive->DirtyVector);
    HvMarkCellDirty(Hive, Aligned, FALSE);
    CHECK(HvIsCellDirty(Hive, Aligned) == TRUE);

    /* --- multi-block cell: every block it occupies must come back dirty --- */
    Size = HvGetCellSize(Hive, HvGetCell(Hive, Big));
    HvReleaseCell(Hive, Big);
    CHECK(Size > (LONG)HBLOCK_SIZE);
    RtlClearAllBits(&Hive->DirtyVector);
    HvMarkCellDirty(Hive, Big, FALSE);
    Blocks = BlockOf(Big + (ULONG)Size - 1) - BlockOf(Big) + 1;
    for (b = 0; b < Blocks; b++)
        CHECK(HvIsCellDirty(Hive, Big + b * HBLOCK_SIZE) == TRUE);

    printf("cmlib HvMarkCellDirty: %lu checks, %lu failures "
           "(aligned cell %#lx, big cell %#lx spans %lu blocks)\n",
           Checks, Failures, (unsigned long)Aligned, (unsigned long)Big,
           (unsigned long)Blocks);

    /* Release the hive through TestFree: bins, map, free display, the dirty and
     * unreconciled vectors and the base block (the sanitizer build runs LSan). */
    HvFree(Hive);
    return Failures ? 1 : 0;
}
