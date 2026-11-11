/*
 * PROJECT:     ReactOS NTFS library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kernelmode glue
 * COPYRIGHT:   Copyright 2024 Justin Miller <justin.miller@reactos.org>
 *              Copyright 2024 Carl Bialorucki <carl.bialorucki@reactos.org>
 *              Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include <ntifs.h>
#include <ntfs_km.h>
#include <ntfslib_new_internal.h>

PDEVICE_OBJECT PartDeviceObj = NULL;
ULONG BytesPerSector = 0;

#ifdef __cplusplus
extern "C" {
#endif

void*
NtfsAllocatePoolWithTag(POOL_TYPE PoolType, size_t Size, ULONG Tag)
{
    if (PoolType == NonPagedPool)
        PoolType = NonPagedPoolNx;
    return ExAllocatePoolWithTag(PoolType, Size, Tag);
}

void NtfsFreePool(void* pObject)
{
    ExFreePool(pObject);
}

void
NtfsInitializeMutex(PFAST_MUTEX Mutex)
{
    ExInitializeFastMutex(Mutex);
}

void
NtfsAcquireMutex(PFAST_MUTEX Mutex)
{
    ExAcquireFastMutex(Mutex);
}

void
NtfsReleaseMutex(PFAST_MUTEX Mutex)
{
    ExReleaseFastMutex(Mutex);
}

unsigned long long
NtfsQueryTicks(void)
{
    return (unsigned long long)KeQueryPerformanceCounter(NULL).QuadPart;
}

NTSTATUS
NtfsQuerySystemTime(_Out_ PULONGLONG NtfsTime)
{
    LARGE_INTEGER Current;

    if (!NtfsTime)
        return STATUS_INVALID_PARAMETER;
    KeQuerySystemTime(&Current);
    if (Current.QuadPart < 0)
        return STATUS_INVALID_DEVICE_STATE;
    *NtfsTime = (ULONGLONG)Current.QuadPart;
    return STATUS_SUCCESS;
}

NTSTATUS
NtfsGenerate8dot3Name(_In_ PCUNICODE_STRING Name,
                      _Inout_ PGENERATE_NAME_CONTEXT Context,
                      _Inout_ PUNICODE_STRING ShortName)
{
    return RtlGenerate8dot3Name(Name, FALSE, Context, ShortName);
}

#ifdef __cplusplus
}
#endif


static NTSTATUS WriteDisk(_In_ PDEVICE_OBJECT DeviceObject, _In_ ULONGLONG Offset,
                          _In_ ULONG Length, _In_ PUCHAR Buffer);

/* METADATA BLOCK CACHE *****************************************************/

/*
 * Every metadata lookup otherwise costs a synchronous disk round trip, and
 * directory walks revisit the same index and MFT blocks constantly. A small
 * direct-mapped cache of fixed blocks removes the repeats. Writes go straight
 * to the disk and drop whatever they overlap, so the cache never holds data
 * the volume does not.
 */
#define NTFS_CACHE_BLOCK_SHIFT 12
#define NTFS_CACHE_BLOCK_SIZE  (1UL << NTFS_CACHE_BLOCK_SHIFT)
#define NTFS_CACHE_SLOTS       4096
#define NTFS_CACHE_WAYS        2
#define NTFS_CACHE_SETS        (NTFS_CACHE_SLOTS / NTFS_CACHE_WAYS)
#define NTFS_CACHE_SET_MASK    (NTFS_CACHE_SETS - 1)
#define NTFS_DISK_IO_CHUNK_SIZE 0x100000UL

typedef struct _NTFS_CACHE_SLOT
{
    ULONGLONG Tag;
    PUCHAR Data;
    PDEVICE_OBJECT Owner; // referenced while the slot is occupied
    ULONGLONG Generation;
    /* Write-ordering barrier (BarrierAfter). DirtySeq is the global write
     * sequence stamped when this block went from clean to dirty; a write into
     * an already dirty block keeps it, so a constraint taken against the
     * block stays valid while later writes land in it. FlushAfterSeq, when
     * non-zero, means this block must not reach the device until every dirty
     * block with DirtySeq <= FlushAfterSeq is clean. It may be set on a clean
     * block ahead of the write that dirties it; until then the block is
     * neither written back nor evicted. */
    ULONGLONG DirtySeq;
    ULONGLONG FlushAfterSeq;
    BOOLEAN Dirty;
} NTFS_CACHE_SLOT;

static NTFS_CACHE_SLOT NtfsCacheSlots[NTFS_CACHE_SLOTS];
static ULONG NtfsCacheDirtyCount = 0;
/* Monotonic write sequence; a write that makes a clean block dirty stamps the
 * block's DirtySeq. Bumped only under NtfsCacheMutex. ULONGLONG never wraps in
 * practice. */
static ULONGLONG NtfsCacheWriteSeq = 0;
/* Barrier vs CommitRange selector, read once from the registry. -1 = unread. */
static volatile LONG NtfsWriteBarrierState = -1;


/* Above this many blocks held back, write some out before taking on more.
 * Only a slice is committed at a time so no single write pays for the whole
 * backlog. */
/* High enough that a whole small-file workload defers cleanly; the flush
 * path and volume teardown write everything back outside the hot window,
 * which is exactly the bargain the FAT driver gets from the lazy writer. */
#define NTFS_CACHE_DIRTY_LIMIT 1280
#define NTFS_CACHE_DIRTY_SLICE 128
static FAST_MUTEX NtfsCacheMutex;
/* Serialize disk writebacks without holding the cache mutex at APC_LEVEL. */
static KMUTEX NtfsCacheWritebackMutex;
static volatile LONG NtfsCacheReady = 0;
static volatile LONG NtfsCacheInitializing = 0;
static KTIMER NtfsCacheLazyTimer;
static KDPC NtfsCacheLazyDpc;
static WORK_QUEUE_ITEM NtfsCacheLazyItem;
static KEVENT NtfsCacheLazyIdle;
static volatile LONG NtfsCacheLazyArmed = 0;
static volatile LONG NtfsCacheLazyStopped = 0;

#define NTFS_CACHE_LAZY_DIVISOR 8

static KDEFERRED_ROUTINE NtfsCacheLazyDpcRoutine;
static WORKER_THREAD_ROUTINE NtfsCacheLazyWorker;

#define NTFS_CACHE_EMPTY ((ULONGLONG)~0ULL)

static
VOID
NtfsCacheDiscardAll(VOID)
{
    ULONG Index;

    for (Index = 0; Index < NTFS_CACHE_SLOTS; Index++)
    {
        NtfsCacheSlots[Index].Tag = NTFS_CACHE_EMPTY;
        NtfsCacheSlots[Index].Dirty = FALSE;
        NtfsCacheSlots[Index].DirtySeq = 0;
        NtfsCacheSlots[Index].FlushAfterSeq = 0;
    }
    NtfsCacheDirtyCount = 0;
}

/* Both ways of a block's set, matching slot or NULL. Caller holds the lock. */
static
NTFS_CACHE_SLOT*
NtfsCacheProbe(_In_ PDEVICE_OBJECT Owner,
               _In_ ULONGLONG Block)
{
    NTFS_CACHE_SLOT* Set =
        &NtfsCacheSlots[(Block & NTFS_CACHE_SET_MASK) * NTFS_CACHE_WAYS];

    if (Set[0].Tag == Block && Set[0].Owner == Owner)
        return &Set[0];
    if (Set[1].Tag == Block && Set[1].Owner == Owner)
        return &Set[1];
    return NULL;
}

/* Where a new block should land: its own slot, else an empty way, else a
 * clean unconstrained way, else a dirty way (written back first), else none.
 * Preferring clean ways keeps an install from forcing a write-back when its
 * neighbour is hot and modified. A clean way that carries a FlushAfterSeq is
 * a barrier target waiting for the write that dirties it; evicting it would
 * drop the constraint, so it is never a victim. */
static
NTFS_CACHE_SLOT*
NtfsCacheVictim(_In_ PDEVICE_OBJECT Owner,
                _In_ ULONGLONG Block)
{
    NTFS_CACHE_SLOT* Set =
        &NtfsCacheSlots[(Block & NTFS_CACHE_SET_MASK) * NTFS_CACHE_WAYS];

    if (Set[0].Tag == Block && Set[0].Owner == Owner)
        return &Set[0];
    if (Set[1].Tag == Block && Set[1].Owner == Owner)
        return &Set[1];
    if (Set[0].Tag == NTFS_CACHE_EMPTY)
        return &Set[0];
    if (Set[1].Tag == NTFS_CACHE_EMPTY)
        return &Set[1];
    if (!Set[0].Dirty && Set[0].FlushAfterSeq == 0)
        return &Set[0];
    if (!Set[1].Dirty && Set[1].FlushAfterSeq == 0)
        return &Set[1];
    if (Set[0].Dirty)
        return &Set[0];
    if (Set[1].Dirty)
        return &Set[1];
    return NULL;
}

static
VOID
NtfsCacheInitialize(VOID)
{
    if (InterlockedCompareExchange(&NtfsCacheInitializing, 1, 0) == 0)
    {
        ExInitializeFastMutex(&NtfsCacheMutex);
        KeInitializeMutex(&NtfsCacheWritebackMutex, 0);
        RtlZeroMemory(NtfsCacheSlots, sizeof(NtfsCacheSlots));
        NtfsCacheDiscardAll();
        KeInitializeTimer(&NtfsCacheLazyTimer);
        KeInitializeDpc(&NtfsCacheLazyDpc, NtfsCacheLazyDpcRoutine, NULL);
        ExInitializeWorkItem(&NtfsCacheLazyItem, NtfsCacheLazyWorker, NULL);
        KeInitializeEvent(&NtfsCacheLazyIdle, NotificationEvent, TRUE);
        InterlockedExchange(&NtfsCacheReady, TRUE);
    }
    else
    {
        LARGE_INTEGER Interval;
        Interval.QuadPart = -10000; // Only contended during first initialization.
        while (!InterlockedCompareExchange(&NtfsCacheReady, 0, 0))
            KeDelayExecutionThread(KernelMode, FALSE, &Interval);
    }
}

/* Drops every block overlapping [Offset, Offset + Length). */
static
VOID
NtfsCacheInvalidateRange(_In_ PDEVICE_OBJECT Owner,
                         _In_ ULONGLONG Offset,
                         _In_ ULONG Length)
{
    ULONGLONG Block;
    ULONGLONG LastBlock;

    if (!NtfsCacheReady || !Length)
        return;

    Block = Offset >> NTFS_CACHE_BLOCK_SHIFT;
    LastBlock = (Offset + Length - 1) >> NTFS_CACHE_BLOCK_SHIFT;

    ExAcquireFastMutex(&NtfsCacheMutex);
    for (; Block <= LastBlock; Block++)
    {
        NTFS_CACHE_SLOT* Slot = NtfsCacheProbe(Owner, Block);

        if (Slot)
        {
            if (Slot->Dirty)
            {
                Slot->Dirty = FALSE;
                NtfsCacheDirtyCount--;
            }
            Slot->Tag = NTFS_CACHE_EMPTY;
            Slot->DirtySeq = 0;
            Slot->FlushAfterSeq = 0;
            Slot->Generation++;
            ObDereferenceObjectDeferDelete(Slot->Owner);
            Slot->Owner = NULL;
        }
    }
    ExReleaseFastMutex(&NtfsCacheMutex);
}

/*
 * Writes go to the volume, so the blocks we already hold can simply take the
 * same bytes instead of being dropped. Discarding them would make every
 * metadata update cost a re-read of the surrounding index and MFT blocks.
 */
static
VOID
NtfsCacheUpdateRange(_In_ PDEVICE_OBJECT Owner,
                     _In_ ULONGLONG Offset, _In_ ULONG Length,
                     _In_reads_bytes_(Length) PUCHAR Data)
{
    ULONG Remaining = Length;
    ULONGLONG Current = Offset;
    PUCHAR In = Data;

    if (!NtfsCacheReady || !Length)
        return;

    ExAcquireFastMutex(&NtfsCacheMutex);
    while (Remaining)
    {
        ULONGLONG Block = Current >> NTFS_CACHE_BLOCK_SHIFT;
        ULONG BlockOffset = (ULONG)(Current & (NTFS_CACHE_BLOCK_SIZE - 1));
        ULONG Chunk = min(Remaining, NTFS_CACHE_BLOCK_SIZE - BlockOffset);
        NTFS_CACHE_SLOT* Slot = NtfsCacheProbe(Owner, Block);

        if (Slot && Slot->Data)
        {
            RtlCopyMemory(Slot->Data + BlockOffset, In, Chunk);
            Slot->Generation++;
        }

        In += Chunk;
        Current += Chunk;
        Remaining -= Chunk;
    }
    ExReleaseFastMutex(&NtfsCacheMutex);
}

/*
 * Metadata is rewritten constantly and usually overwritten again moments
 * later, so updates are held in the cache and committed in batches. Reads are
 * served from the same blocks, so a held-back write is never invisible to the
 * code that reads it back; callers that bypass the cache flush first.
 *
 * The disk transfer must happen with the mutex dropped: it waits on an IRP,
 * and the fast mutex holds APC_LEVEL where that completion could never run.
 */
static
NTSTATUS
NtfsCacheWriteBackOne(_In_ PDEVICE_OBJECT Owner,
                      _In_ ULONGLONG Block)
{
    PUCHAR Staged;
    NTSTATUS Status = STATUS_SUCCESS;
    NTFS_CACHE_SLOT* Slot;
    PDEVICE_OBJECT Target = NULL;
    ULONGLONG Generation = 0;

    /* The writeback mutex is recursive (an NT KMUTEX), so the dependency
     * flush below can call back into this function for an earlier block. */
    KeWaitForSingleObject(&NtfsCacheWritebackMutex, Executive, KernelMode, FALSE, NULL);

    /*
     * Write-ordering barrier. A block constrained by BarrierAfter carries a
     * non-zero FlushAfterSeq: it must not reach the device until every dirty
     * block with DirtySeq <= FlushAfterSeq (its record, and anything written
     * before that) is clean. Flush those first, lowest DirtySeq first. If any
     * dependency write fails, FAIL CLOSED: return the error and leave this
     * block dirty, so the later block can never overtake the earlier one.
     */
    for (;;)
    {
        ULONGLONG FlushAfter = 0;
        PDEVICE_OBJECT DepOwner = NULL;
        ULONGLONG DepBlock = 0;
        ULONGLONG DepSeq = ~0ULL;
        ULONG i;
        NTSTATUS DepStatus;

        ExAcquireFastMutex(&NtfsCacheMutex);
        Slot = NtfsCacheProbe(Owner, Block);
        if (Slot && Slot->Dirty)
            FlushAfter = Slot->FlushAfterSeq;
        if (FlushAfter != 0)
        {
            for (i = 0; i < NTFS_CACHE_SLOTS; i++)
            {
                NTFS_CACHE_SLOT* D = &NtfsCacheSlots[i];
                if (D->Dirty && D->Owner && D->Tag != NTFS_CACHE_EMPTY &&
                    D->DirtySeq != 0 && D->DirtySeq <= FlushAfter &&
                    !(D->Owner == Owner && D->Tag == Block) &&
                    D->DirtySeq < DepSeq)
                {
                    DepSeq = D->DirtySeq;
                    DepOwner = D->Owner;
                    DepBlock = D->Tag;
                }
            }
            if (DepOwner)
                ObReferenceObject(DepOwner);
        }
        ExReleaseFastMutex(&NtfsCacheMutex);

        if (!DepOwner)
            break;

        DepStatus = NtfsCacheWriteBackOne(DepOwner, DepBlock);
        ObDereferenceObject(DepOwner);
        if (!NT_SUCCESS(DepStatus))
        {
            KeReleaseMutex(&NtfsCacheWritebackMutex, FALSE);
            return DepStatus;
        }
    }

    Staged = (PUCHAR)ExAllocatePoolUninitialized(NonPagedPoolNx,
                                                NTFS_CACHE_BLOCK_SIZE, 'CftN');
    if (!Staged)
    {
        KeReleaseMutex(&NtfsCacheWritebackMutex, FALSE);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ExAcquireFastMutex(&NtfsCacheMutex);
    Slot = NtfsCacheProbe(Owner, Block);
    if (Slot && Slot->Dirty && Slot->Data)
    {
        RtlCopyMemory(Staged, Slot->Data, NTFS_CACHE_BLOCK_SIZE);
        Target = Slot->Owner;
        ObReferenceObject(Target);
        Generation = Slot->Generation;
        // Keep dirty data unevictable until the write actually succeeds.
    }
    ExReleaseFastMutex(&NtfsCacheMutex);

    if (Target)
    {
        Status = WriteDisk(Target, Block << NTFS_CACHE_BLOCK_SHIFT,
                           NTFS_CACHE_BLOCK_SIZE, Staged);
        ExAcquireFastMutex(&NtfsCacheMutex);
        Slot = NtfsCacheProbe(Target, Block);
        if (NT_SUCCESS(Status) && Slot && Slot->Dirty &&
            Slot->Generation == Generation)
        {
            Slot->Dirty = FALSE;
            NtfsCacheDirtyCount--;
            /* Constraint satisfied: this block is now durable. */
            Slot->FlushAfterSeq = 0;
        }
        ExReleaseFastMutex(&NtfsCacheMutex);
        ObDereferenceObject(Target);
    }
    KeReleaseMutex(&NtfsCacheWritebackMutex, FALSE);
    ExFreePoolWithTag(Staged, 'CftN');
    return Status;
}

/*
 * The lazy writer writes the OLDEST dirty blocks first. A block held back
 * in the cache then waits for its age, bounded by the budget, instead of
 * for the slot hand to come round to its set: with a budget of
 * NTFS_CACHE_DIRTY_SLICE per pass and a churn that dirties more than that
 * per second, a slot-order pass could leave a block dirtied early waiting a
 * full circle of the slot array behind blocks dirtied after it (Lab04 round
 * 2, cut 1: the rewritten left halves of seven successive index splits were
 * all still unwritten 2.5 s later, while their right halves and parent,
 * committed synchronously by the split, were on the disk). DirtySeq is the
 * global write sequence stamped when a block went from clean to dirty, so
 * ascending DirtySeq is ascending age.
 *
 * One pass over the slots under the cache mutex keeps the Budget smallest
 * DirtySeq values in an insertion-sorted array (Budget is at most
 * NTFS_CACHE_SLOTS / NTFS_CACHE_LAZY_DIVISOR in practice, so the pass is
 * short). The owners are referenced under the mutex, as the slot-order pass
 * did, and NtfsCacheWriteBackOne re-probes each block, so a block evicted or
 * cleaned in between is simply skipped. A block whose FlushAfterSeq names an
 * even older dependency still has that dependency written first by
 * NtfsCacheWriteBackOne; the order here only decides which block is asked
 * for first. If the pick array cannot be allocated, the slot-order pass
 * below runs instead, so the lazy writer never stalls.
 */
typedef struct _NTFS_CACHE_DIRTY_PICK
{
    ULONGLONG Seq;
    PDEVICE_OBJECT Owner;
    ULONGLONG Block;
} NTFS_CACHE_DIRTY_PICK;

/* Where the next slot-order pass starts (fallback only). A pass resumes after
 * the last slot the previous one examined, so every slot comes round within
 * NTFS_CACHE_SLOTS / budget passes. */
static ULONG NtfsCacheFlushHand = 0;
C_ASSERT((NTFS_CACHE_SLOTS & (NTFS_CACHE_SLOTS - 1)) == 0);

static
NTSTATUS
NtfsCacheFlushSomeSlotOrder(_In_ ULONG Budget)
{
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG Start;
    ULONG Visited;
    ULONG Written = 0;

    Start = NtfsCacheFlushHand & (NTFS_CACHE_SLOTS - 1);
    for (Visited = 0; Visited < NTFS_CACHE_SLOTS && Written < Budget; Visited++)
    {
        ULONG Index = (Start + Visited) & (NTFS_CACHE_SLOTS - 1);
        ULONGLONG Block;
        BOOLEAN Dirty;
        PDEVICE_OBJECT Owner;

        ExAcquireFastMutex(&NtfsCacheMutex);
        Block = NtfsCacheSlots[Index].Tag;
        Dirty = NtfsCacheSlots[Index].Dirty;
        Owner = NtfsCacheSlots[Index].Owner;
        if (Owner)
            ObReferenceObject(Owner);
        ExReleaseFastMutex(&NtfsCacheMutex);

        if (Dirty && Owner && Block != NTFS_CACHE_EMPTY)
        {
            NTSTATUS One = NtfsCacheWriteBackOne(Owner, Block);

            if (!NT_SUCCESS(One))
                Status = One;
            Written++;
        }
        if (Owner)
            ObDereferenceObject(Owner);
    }
    /* Resume after the last slot examined; a full circle lands on Start. */
    InterlockedExchange((volatile LONG*)&NtfsCacheFlushHand,
                        (LONG)((Start + Visited) & (NTFS_CACHE_SLOTS - 1)));
    return Status;
}

static
NTSTATUS
NtfsCacheFlushSome(_In_ ULONG Budget)
{
    NTSTATUS Status = STATUS_SUCCESS;
    NTFS_CACHE_DIRTY_PICK* Picked;
    ULONG PickedCount = 0;
    ULONG Index;

    if (!NtfsCacheReady || Budget == 0)
        return STATUS_SUCCESS;
    if (Budget > NTFS_CACHE_SLOTS)
        Budget = NTFS_CACHE_SLOTS;

    Picked = (NTFS_CACHE_DIRTY_PICK*)ExAllocatePoolUninitialized(
        NonPagedPoolNx, Budget * sizeof(*Picked), 'CftN');
    if (!Picked)
        return NtfsCacheFlushSomeSlotOrder(Budget);

    ExAcquireFastMutex(&NtfsCacheMutex);
    for (Index = 0; Index < NTFS_CACHE_SLOTS; Index++)
    {
        NTFS_CACHE_SLOT* Slot = &NtfsCacheSlots[Index];
        ULONG Pos;

        if (!Slot->Dirty || !Slot->Owner || Slot->Tag == NTFS_CACHE_EMPTY)
            continue;
        /* Full, and this block is younger than every pick: skip it. */
        if (PickedCount == Budget && Slot->DirtySeq >= Picked[Budget - 1].Seq)
            continue;
        /* Insert in ascending DirtySeq; when full, the youngest pick drops. */
        Pos = (PickedCount == Budget) ? Budget - 1 : PickedCount;
        while (Pos > 0 && Picked[Pos - 1].Seq > Slot->DirtySeq)
        {
            Picked[Pos] = Picked[Pos - 1];
            Pos--;
        }
        Picked[Pos].Seq = Slot->DirtySeq;
        Picked[Pos].Owner = Slot->Owner;
        Picked[Pos].Block = Slot->Tag;
        if (PickedCount < Budget)
            PickedCount++;
    }
    for (Index = 0; Index < PickedCount; Index++)
        ObReferenceObject(Picked[Index].Owner);
    ExReleaseFastMutex(&NtfsCacheMutex);

    for (Index = 0; Index < PickedCount; Index++)
    {
        NTSTATUS One = NtfsCacheWriteBackOne(Picked[Index].Owner, Picked[Index].Block);

        if (!NT_SUCCESS(One))
            Status = One;
        ObDereferenceObject(Picked[Index].Owner);
    }
    ExFreePoolWithTag(Picked, 'CftN');
    return Status;
}

static
NTSTATUS
NtfsCacheFlushAll(_In_opt_ PDEVICE_OBJECT OnlyOwner)
{
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG Index;

    if (!NtfsCacheReady)
        return STATUS_SUCCESS;

    for (Index = 0; Index < NTFS_CACHE_SLOTS; Index++)
    {
        ULONGLONG Block;
        BOOLEAN Dirty;
        PDEVICE_OBJECT Owner;

        ExAcquireFastMutex(&NtfsCacheMutex);
        Block = NtfsCacheSlots[Index].Tag;
        Dirty = NtfsCacheSlots[Index].Dirty;
        Owner = NtfsCacheSlots[Index].Owner;
        if (Owner)
            ObReferenceObject(Owner);
        ExReleaseFastMutex(&NtfsCacheMutex);

        if (Dirty && Owner && Block != NTFS_CACHE_EMPTY &&
            (!OnlyOwner || OnlyOwner == Owner))
        {
            NTSTATUS One = NtfsCacheWriteBackOne(Owner, Block);

            if (!NT_SUCCESS(One))
                Status = One;
        }
        if (Owner)
            ObDereferenceObject(Owner);
    }
    return Status;
}

static
VOID
NtfsCacheScheduleLazyWrite(VOID)
{
    LARGE_INTEGER DueTime;

    if (NtfsCacheLazyStopped || InterlockedCompareExchange(&NtfsCacheLazyArmed, 1, 0) != 0)
        return;

    DueTime.QuadPart = -10 * 1000 * 1000;
    KeSetTimer(&NtfsCacheLazyTimer, DueTime, &NtfsCacheLazyDpc);
}

static
VOID
NTAPI
NtfsCacheLazyDpcRoutine(_In_ PKDPC Dpc,
                        _In_opt_ PVOID DeferredContext,
                        _In_opt_ PVOID SystemArgument1,
                        _In_opt_ PVOID SystemArgument2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(DeferredContext);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    KeClearEvent(&NtfsCacheLazyIdle);
    ExQueueWorkItem(&NtfsCacheLazyItem, DelayedWorkQueue);
}

static
VOID
NTAPI
NtfsCacheLazyWorker(_In_opt_ PVOID Parameter)
{
    ULONG Budget = NtfsCacheDirtyCount / NTFS_CACHE_LAZY_DIVISOR;

    UNREFERENCED_PARAMETER(Parameter);

    if (Budget < NTFS_CACHE_DIRTY_SLICE)
        Budget = NTFS_CACHE_DIRTY_SLICE;
    NtfsCacheFlushSome(Budget);

    InterlockedExchange(&NtfsCacheLazyArmed, 0);
    if (NtfsCacheDirtyCount != 0)
        NtfsCacheScheduleLazyWrite();
    KeSetEvent(&NtfsCacheLazyIdle, IO_NO_INCREMENT, FALSE);
}

/* Commits anything held for [Offset, Offset + Length) so the caller may read
 * or write those bytes on the disk directly. */
static
NTSTATUS
NtfsCacheFlushRange(_In_ PDEVICE_OBJECT Owner,
                    _In_ ULONGLONG Offset,
                    _In_ ULONG Length)
{
    NTSTATUS Status = STATUS_SUCCESS;
    ULONGLONG Block;
    ULONGLONG LastBlock;

    if (!NtfsCacheReady || !Length || NtfsCacheDirtyCount == 0)
        return STATUS_SUCCESS;


    Block = Offset >> NTFS_CACHE_BLOCK_SHIFT;
    LastBlock = (Offset + Length - 1) >> NTFS_CACHE_BLOCK_SHIFT;

    if (LastBlock - Block >= NTFS_CACHE_SLOTS)
    {
        ULONG Index;

        for (Index = 0; Index < NTFS_CACHE_SLOTS; Index++)
        {
            ULONGLONG Tag;
            BOOLEAN Dirty;

            ExAcquireFastMutex(&NtfsCacheMutex);
            Tag = NtfsCacheSlots[Index].Tag;
            Dirty = NtfsCacheSlots[Index].Dirty && NtfsCacheSlots[Index].Owner == Owner;
            ExReleaseFastMutex(&NtfsCacheMutex);
            if (!Dirty || Tag == NTFS_CACHE_EMPTY || Tag < Block || Tag > LastBlock)
                continue;

            NTSTATUS One = NtfsCacheWriteBackOne(Owner, Tag);
            if (!NT_SUCCESS(One))
                Status = One;
        }
        return Status;
    }

    for (; Block <= LastBlock; Block++)
    {
        NTSTATUS One;
        BOOLEAN Dirty;

        /* Nearly every block a large transfer walks is clean, and committing
         * one costs a staging buffer, so look before paying for it. */
        {
            NTFS_CACHE_SLOT* Slot;

            ExAcquireFastMutex(&NtfsCacheMutex);
            Slot = NtfsCacheProbe(Owner, Block);
            Dirty = (Slot != NULL && Slot->Dirty);
            ExReleaseFastMutex(&NtfsCacheMutex);
        }
        if (!Dirty)
            continue;

        One = NtfsCacheWriteBackOne(Owner, Block);
        if (!NT_SUCCESS(One))
            Status = One;
    }
    return Status;
}

/* TRUE when the whole block was served from memory. */
static
BOOLEAN
NtfsCacheCopyOut(_In_ PDEVICE_OBJECT Owner,
                 _In_ ULONGLONG Block, _In_ ULONG BlockOffset,
                 _In_ ULONG Length, _Out_writes_bytes_(Length) PUCHAR Buffer)
{
    NTFS_CACHE_SLOT* Slot;
    BOOLEAN Hit = FALSE;

    if (!NtfsCacheReady)
        return FALSE;

    ExAcquireFastMutex(&NtfsCacheMutex);
    Slot = NtfsCacheProbe(Owner, Block);
    if (Slot && Slot->Data)
    {
        RtlCopyMemory(Buffer, Slot->Data + BlockOffset, Length);
        Hit = TRUE;
    }
    ExReleaseFastMutex(&NtfsCacheMutex);
    return Hit;
}

static
NTSTATUS
NtfsCacheInstall(_In_ PDEVICE_OBJECT Owner,
                 _In_ ULONGLONG Block,
                 _In_reads_bytes_(NTFS_CACHE_BLOCK_SIZE) PUCHAR Data)
{
    NTFS_CACHE_SLOT* Slot;
    NTSTATUS Status;
    BOOLEAN SameBlock;

    if (!NtfsCacheReady)
        return STATUS_SUCCESS;

    /* Never lose an update by dropping the block that is being replaced. */
    ExAcquireFastMutex(&NtfsCacheMutex);
    Slot = NtfsCacheVictim(Owner, Block);
    if (!Slot)
    {
        ExReleaseFastMutex(&NtfsCacheMutex);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (Slot->Dirty && (Slot->Tag != Block || Slot->Owner != Owner))
    {
        ULONGLONG Victim = Slot->Tag;
        PDEVICE_OBJECT VictimOwner = Slot->Owner;
        ObReferenceObject(VictimOwner);

        ExReleaseFastMutex(&NtfsCacheMutex);
        Status = NtfsCacheWriteBackOne(VictimOwner, Victim);
        ObDereferenceObject(VictimOwner);
        if (!NT_SUCCESS(Status))
            return Status;
        ExAcquireFastMutex(&NtfsCacheMutex);
        Slot = NtfsCacheVictim(Owner, Block);
        if (!Slot)
        {
            ExReleaseFastMutex(&NtfsCacheMutex);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    // A concurrent writer may have dirtied the newly selected victim.
    // Cache fills must also preserve newer dirty data for this same block.
    if (Slot->Dirty)
    {
        ExReleaseFastMutex(&NtfsCacheMutex);
        return STATUS_SUCCESS;
    }
    SameBlock = (Slot->Tag == Block && Slot->Owner == Owner);
    if (!Slot->Data)
    {
        Slot->Data = (PUCHAR)ExAllocatePoolUninitialized(NonPagedPoolNx,
                                                         NTFS_CACHE_BLOCK_SIZE,
                                                         'CftN');
        if (!Slot->Data)
        {
            ExReleaseFastMutex(&NtfsCacheMutex);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    RtlCopyMemory(Slot->Data, Data, NTFS_CACHE_BLOCK_SIZE);
    Slot->Tag = Block;
    ObReferenceObject(Owner);
    if (Slot->Owner)
        ObDereferenceObjectDeferDelete(Slot->Owner);
    Slot->Owner = Owner;
    Slot->Generation++;
    Slot->DirtySeq = 0;
    /*
     * A write-ordering constraint (BarrierAfter) may sit on a clean block
     * ahead of the write that will dirty it. Re-filling that same block
     * from the disk, which a concurrent read miss can do, must keep the
     * constraint: resetting it would let the pointer write that follows
     * reach the device unordered. Only a slot that changes identity starts
     * without one; NtfsCacheVictim never hands out a clean constrained
     * slot for another block.
     */
    if (!SameBlock)
        Slot->FlushAfterSeq = 0;
    Slot->Dirty = FALSE;
    ExReleaseFastMutex(&NtfsCacheMutex);
    return STATUS_SUCCESS;
}

NTSTATUS
NtfsDiskInitializeKm(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ULONG SectorBytes)
{
    if (!DeviceObject ||
        (SectorBytes != 512 && SectorBytes != 4096))
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Mount probes re-point the library at other volumes constantly. Blocks
     * remember which device they came from, so switching device keeps them
     * and simply stops them matching; nothing is discarded and no write is
     * lost. */
    NtfsCacheInitialize();
    PartDeviceObj = DeviceObject;
    BytesPerSector = SectorBytes;
    return STATUS_SUCCESS;
}

static
VOID
NtfsCacheDiscardOwner(_In_ PDEVICE_OBJECT DeviceObject)
{
    ULONG Index;

    /*
     * Failed mount probes cache clean data from raw volumes. A formatter writes
     * below the filesystem, so a later probe of the same device must not reuse
     * that data. Preserve dirty blocks in case a mounted volume is still active.
     */
    ExAcquireFastMutex(&NtfsCacheMutex);
    for (Index = 0; Index < NTFS_CACHE_SLOTS; Index++)
    {
        if (NtfsCacheSlots[Index].Owner == DeviceObject &&
            !NtfsCacheSlots[Index].Dirty)
        {
            NtfsCacheSlots[Index].Tag = NTFS_CACHE_EMPTY;
            NtfsCacheSlots[Index].DirtySeq = 0;
            NtfsCacheSlots[Index].FlushAfterSeq = 0;
            NtfsCacheSlots[Index].Generation++;
            ObDereferenceObjectDeferDelete(NtfsCacheSlots[Index].Owner);
            NtfsCacheSlots[Index].Owner = NULL;
        }
    }
    ExReleaseFastMutex(&NtfsCacheMutex);
}

NTSTATUS
NtfsDiskPrepareMountKm(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ULONG SectorBytes)
{
    NTSTATUS Status;

    Status = NtfsDiskInitializeKm(DeviceObject, SectorBytes);
    if (!NT_SUCCESS(Status))
        return Status;

    NtfsCacheDiscardOwner(DeviceObject);
    return STATUS_SUCCESS;
}

VOID
NtfsDiskReleaseKm(_In_ PDEVICE_OBJECT DeviceObject)
{
    NtfsCacheDiscardOwner(DeviceObject);
}

typedef struct _NTFS_DISK_IO_CONTEXT
{
    KEVENT Event;
    IO_STATUS_BLOCK IoStatus;
    PVOID Buffer;
    ULONG Length;
} NTFS_DISK_IO_CONTEXT, *PNTFS_DISK_IO_CONTEXT;

static
NTSTATUS
NTAPI
NtfsDiskIoCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Context)
{
    PNTFS_DISK_IO_CONTEXT IoContext;

    UNREFERENCED_PARAMETER(DeviceObject);

    IoContext = static_cast<PNTFS_DISK_IO_CONTEXT>(Context);
    IoContext->IoStatus = Irp->IoStatus;
    KeSetEvent(&IoContext->Event, IO_NO_INCREMENT, FALSE);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

static
VOID
NtfsFreeDiskIoIrp(
    _In_ PIRP Irp,
    _In_ PNTFS_DISK_IO_CONTEXT IoContext)
{
    PMDL Mdl;

    if (FlagOn(Irp->Flags, IRP_BUFFERED_IO))
    {
        if (FlagOn(Irp->Flags, IRP_INPUT_OPERATION) &&
            !NT_ERROR(IoContext->IoStatus.Status) &&
            IoContext->IoStatus.Status != STATUS_VERIFY_REQUIRED)
        {
            ULONG_PTR BytesToCopy;

            BytesToCopy = min(IoContext->IoStatus.Information,
                              static_cast<ULONG_PTR>(IoContext->Length));
            RtlCopyMemory(IoContext->Buffer,
                          Irp->AssociatedIrp.SystemBuffer,
                          BytesToCopy);
        }

        if (FlagOn(Irp->Flags, IRP_DEALLOCATE_BUFFER))
            ExFreePool(Irp->AssociatedIrp.SystemBuffer);
    }

    while ((Mdl = Irp->MdlAddress))
    {
        Irp->MdlAddress = Mdl->Next;
        if (FlagOn(Mdl->MdlFlags, MDL_PAGES_LOCKED))
            MmUnlockPages(Mdl);
        IoFreeMdl(Mdl);
    }

    IoFreeIrp(Irp);
}

static
NTSTATUS
NtfsPerformDiskIo(
    _In_ UCHAR MajorFunction,
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ULONGLONG Offset,
    _In_ ULONG Length,
    _Inout_updates_bytes_(Length) PUCHAR Buffer)
{
    NTFS_DISK_IO_CONTEXT IoContext;
    PIO_STACK_LOCATION IoStack;
    PIRP Irp;
    PUCHAR IoBuffer;
    ULONG_PTR BytesToCopy;
    NTSTATUS Status;

    PAGED_CODE();

    if (!DeviceObject || (Length != 0 && !Buffer))
        return STATUS_INVALID_PARAMETER;

    IoBuffer = Buffer;
    if (Length != 0 &&
        ((ULONG_PTR)Buffer & DeviceObject->AlignmentRequirement) != 0)
    {
        IoBuffer = new(NonPagedPool, TAG_NTFS) UCHAR[Length];
        if (!IoBuffer)
            return STATUS_INSUFFICIENT_RESOURCES;
        if (MajorFunction == IRP_MJ_WRITE)
            RtlCopyMemory(IoBuffer, Buffer, Length);
    }

    KeInitializeEvent(&IoContext.Event, NotificationEvent, FALSE);
    IoContext.IoStatus.Status = STATUS_PENDING;
    IoContext.IoStatus.Information = 0;
    IoContext.Buffer = IoBuffer;
    IoContext.Length = Length;

    /*
     * IoBuildSynchronousFsdRequest completes through a kernel APC. Filesystem
     * callers can have normal kernel APCs disabled, so waiting on its event
     * would deadlock. Stop completion at our routine and own the IRP instead.
     */
    Irp = IoBuildAsynchronousFsdRequest(MajorFunction,
                                        DeviceObject,
                                        IoBuffer,
                                        Length,
                                        reinterpret_cast<PLARGE_INTEGER>(&Offset),
                                        &IoContext.IoStatus);
    if (!Irp)
    {
        if (IoBuffer != Buffer)
            delete[] IoBuffer;
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    IoStack = IoGetNextIrpStackLocation(Irp);
    SetFlag(IoStack->Flags, SL_OVERRIDE_VERIFY_VOLUME);
    IoSetCompletionRoutine(Irp,
                           NtfsDiskIoCompletion,
                           &IoContext,
                           TRUE,
                           TRUE,
                           TRUE);

    Status = IoCallDriver(DeviceObject, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&IoContext.Event,
                              Executive,
                              KernelMode,
                              FALSE,
                              NULL);
    }

    NT_ASSERT(IoContext.IoStatus.Status != STATUS_PENDING);
    Status = IoContext.IoStatus.Status;
    NtfsFreeDiskIoIrp(Irp, &IoContext);

    if (MajorFunction == IRP_MJ_READ &&
        NT_SUCCESS(Status) &&
        IoBuffer != Buffer)
    {
        BytesToCopy = min(IoContext.IoStatus.Information,
                          static_cast<ULONG_PTR>(Length));
        RtlCopyMemory(Buffer, IoBuffer, BytesToCopy);
    }
    if (IoBuffer != Buffer)
        delete[] IoBuffer;

    NT_ASSERT(Status != STATUS_VERIFY_REQUIRED);
    return Status;
}

NTSTATUS
ReadDisk(_In_ PDEVICE_OBJECT DeviceObject,
         _In_ ULONGLONG Offset,
         _In_ ULONG Length,
         _Out_ PUCHAR Buffer)
{
    NTSTATUS Status;

    do
    {
        ULONG Chunk = min(Length, NTFS_DISK_IO_CHUNK_SIZE);

        Status = NtfsPerformDiskIo(IRP_MJ_READ,
                                   DeviceObject,
                                   Offset,
                                   Chunk,
                                   Buffer);
        Offset += Chunk;
        Buffer += Chunk;
        Length -= Chunk;
    } while (NT_SUCCESS(Status) && Length);

    return Status;
}

NTSTATUS
WriteDisk(_In_ PDEVICE_OBJECT DeviceObject,
          _In_ ULONGLONG Offset,
          _In_ ULONG Length,
          _In_ PUCHAR Buffer)
{
    NTSTATUS Status;

    do
    {
        ULONG Chunk = min(Length, NTFS_DISK_IO_CHUNK_SIZE);

        Status = NtfsPerformDiskIo(IRP_MJ_WRITE,
                                   DeviceObject,
                                   Offset,
                                   Chunk,
                                   Buffer);
        Offset += Chunk;
        Buffer += Chunk;
        Length -= Chunk;
    } while (NT_SUCCESS(Status) && Length);

    return Status;
}

#ifdef __cplusplus
extern "C" {
#endif

NTSTATUS
NtfsReadVolumeContext(_In_opt_ void* Context,
                      _In_ ULONG SectorBytes,
                      _In_ ULONGLONG Offset,
                      _In_ ULONG Length,
                      _Inout_ PUCHAR Buffer)
{
    NTSTATUS Status;
    PDEVICE_OBJECT DeviceObject;
    PUCHAR ReadBuffer;
    ULONGLONG SectorAlignedOffset;
    ULONG SectorAlignedLength;

    ASSERT(Length);
    DeviceObject = Context ? static_cast<PDEVICE_OBJECT>(Context) : PartDeviceObj;
    if (!DeviceObject ||
        (SectorBytes != 512 && SectorBytes != 4096))
    {
        return STATUS_INVALID_PARAMETER;
    }

    /*
     * Metadata is read over and over from the same handful of index and MFT
     * blocks, so satisfy the request from whole cached blocks when we can and
     * otherwise fetch the covering block once, keeping it for next time.
     */
    /*
     * Only metadata-sized requests go through the cache. Splitting a bulk
     * streaming read into block-sized fetches would cost far more than the
     * repeats it saves.
     */
    if (NtfsCacheReady && Length <= NTFS_CACHE_BLOCK_SIZE)
    {
        ULONG Remaining = Length;
        ULONGLONG Current = Offset;
        PUCHAR Out = Buffer;

        while (Remaining)
        {
            ULONGLONG Block = Current >> NTFS_CACHE_BLOCK_SHIFT;
            ULONG BlockOffset = (ULONG)(Current & (NTFS_CACHE_BLOCK_SIZE - 1));
            ULONG Chunk = min(Remaining, NTFS_CACHE_BLOCK_SIZE - BlockOffset);

            if (!NtfsCacheCopyOut(DeviceObject, Block, BlockOffset, Chunk, Out))
            {
                PUCHAR BlockData = (PUCHAR)ExAllocatePoolUninitialized(
                    NonPagedPoolNx, NTFS_CACHE_BLOCK_SIZE, 'CftN');

                if (!BlockData)
                    break;

                Status = ReadDisk(DeviceObject,
                                  Block << NTFS_CACHE_BLOCK_SHIFT,
                                  NTFS_CACHE_BLOCK_SIZE,
                                  BlockData);
                if (!NT_SUCCESS(Status))
                {
                    ExFreePoolWithTag(BlockData, 'CftN');
                    break;
                }

                RtlCopyMemory(Out, BlockData + BlockOffset, Chunk);
                NtfsCacheInstall(DeviceObject, Block, BlockData);
                ExFreePoolWithTag(BlockData, 'CftN');
            }

            Out += Chunk;
            Current += Chunk;
            Remaining -= Chunk;
        }

        if (!Remaining)
            return STATUS_SUCCESS;
        /* Anything left over falls through to a plain read below. */
    }

    /* This read goes to the disk, so anything held back for it must be there. */
    NTSTATUS FlushStatus = NtfsCacheFlushRange(DeviceObject, Offset, Length);
    if (!NT_SUCCESS(FlushStatus))
        return FlushStatus;

    SectorAlignedOffset = Offset - (Offset % SectorBytes);
    SectorAlignedLength = ALIGN_UP_BY((Offset - SectorAlignedOffset) + Length,
                                      SectorBytes);

    if (SectorAlignedOffset == Offset
        && SectorAlignedLength == Length)
    {
        // Read directly to the supplied buffer.
        Status = ReadDisk(DeviceObject,
                          SectorAlignedOffset,
                          SectorAlignedLength,
                          Buffer);
    }

    else
    {
        ULONG Skip = (ULONG)(Offset - SectorAlignedOffset);
        ULONG Chunk = min(SectorAlignedLength, NTFS_DISK_IO_CHUNK_SIZE);
        ULONG Done = 0;

        // Create the read buffer
        ReadBuffer = new(NonPagedPool) UCHAR[Chunk];
        if (!ReadBuffer)
            return STATUS_INSUFFICIENT_RESOURCES;

        Status = STATUS_SUCCESS;
        while (NT_SUCCESS(Status) && Done < SectorAlignedLength)
        {
            ULONG Piece = min(Chunk, SectorAlignedLength - Done);
            ULONG From = max(Done, Skip);
            ULONG To = min(Done + Piece, Skip + Length);

            // Fill the read buffer.
            Status = ReadDisk(DeviceObject,
                              SectorAlignedOffset + Done,
                              Piece,
                              ReadBuffer);

            if (NT_SUCCESS(Status) && To > From)
            {
                // Copy the contents we need into the supplied buffer.
                RtlCopyMemory(Buffer + (From - Skip),
                              ReadBuffer + (From - Done),
                              To - From);
            }
            Done += Piece;
        }

        // Free read buffer
        delete[] ReadBuffer;
    }

    return Status;
}

NTSTATUS
NtfsCommitRangeContext(_In_opt_ void* Context,
                       _In_ ULONGLONG Offset,
                       _In_ ULONG Length)
{
    PDEVICE_OBJECT DeviceObject;

    DeviceObject = Context ? static_cast<PDEVICE_OBJECT>(Context) : PartDeviceObj;
    if (!DeviceObject)
        return STATUS_INVALID_PARAMETER;
    return NtfsDiskFlushRangeKm(DeviceObject, Offset, Length);
}

/* Barrier vs CommitRange selector: Control\FileSystem value NtfsWriteBarrier
 * (DWORD). 0 (default) keeps the synchronous CommitRange (bc86485f); 1 uses the
 * write-ordering barrier. Read once and cached, so one image runs either way. */
static BOOLEAN
NtfsWriteBarrierEnabledKm(VOID)
{
    LONG State = NtfsWriteBarrierState;

    if (State < 0)
    {
        RTL_QUERY_REGISTRY_TABLE Table[2];
        ULONG Zero = 0;
        ULONG Val = 0;

        RtlZeroMemory(Table, sizeof(Table));
        Table[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
        Table[0].Name = L"NtfsWriteBarrier";
        Table[0].EntryContext = &Val;
        Table[0].DefaultType = REG_DWORD;
        Table[0].DefaultData = &Zero;
        Table[0].DefaultLength = sizeof(ULONG);
        (void)RtlQueryRegistryValues(RTL_REGISTRY_CONTROL, L"FileSystem",
                                     Table, NULL, NULL);
        State = (Val != 0) ? 1 : 0;
        InterlockedCompareExchange(&NtfsWriteBarrierState, State, -1);
        State = NtfsWriteBarrierState;
    }
    return (State != 0);
}

BOOLEAN
NtfsWriteBarrierEnabledContext(_In_opt_ void* Context)
{
    UNREFERENCED_PARAMETER(Context);
    return NtfsWriteBarrierEnabledKm();
}

/*
 * Read the registry settings of this environment once, from DriverEntry.
 * The barrier selector is consulted while a record is allocated or an index
 * is edited, with the volume's MetadataResource held exclusively. Reading
 * the registry there takes the registry lock, while the configuration
 * manager holds that lock exclusively during NtInitializeRegistry and waits
 * for its hive-loading threads, which open the hive files on this volume
 * and need the MetadataResource. The first allocation of the boot must not
 * be the one that reads the value.
 */
extern "C"
VOID
NtfsDiskReadSettingsKm(VOID)
{
    (void)NtfsWriteBarrierEnabledKm();
}

/*
 * Write-ordering barrier: the block covering [LaterOffset, +LaterLength) must
 * not reach the device until the block(s) covering [EarlierOffset,
 * +EarlierLength) (the new MFT record) are durable. An earlier block that is
 * already clean or not cached is durable and adds no constraint.
 *
 * The caller installs the constraint BEFORE the write that dirties the later
 * block, so there is no window in which the lazy writer could find the block
 * dirty and unconstrained. For that to hold the later block must already sit
 * in the cache (a block that is not cached is not held back by anything) and
 * the write must be one the cache absorbs: a range that does not fit inside a
 * single cache block is written through, straight to the device, and no
 * constraint could stop it. Both cases, and a constraint that would close a
 * dependency cycle (the earlier range already has to wait for the later
 * block), are refused; the caller then makes the earlier range durable
 * synchronously instead. Nothing is ever ordered silently.
 */
NTSTATUS
NtfsBarrierAfterContext(_In_opt_ void* Context,
                        _In_ ULONGLONG LaterOffset, _In_ ULONG LaterLength,
                        _In_ ULONGLONG EarlierOffset, _In_ ULONG EarlierLength)
{
    PDEVICE_OBJECT DeviceObject;
    ULONGLONG LaterBlock, EarlierFirst, EarlierLast, b;
    ULONGLONG MaxEarlierSeq = 0;
    NTFS_CACHE_SLOT* L;
    NTSTATUS Status = STATUS_SUCCESS;

    if (!LaterLength || !EarlierLength ||
        LaterOffset > ~(ULONGLONG)0 - (LaterLength - 1) ||
        EarlierOffset > ~(ULONGLONG)0 - (EarlierLength - 1))
    {
        return STATUS_INVALID_PARAMETER;
    }
    /* Only a write that fits one cache block is held back by the cache. */
    if ((LaterOffset & (NTFS_CACHE_BLOCK_SIZE - 1)) + LaterLength >
        NTFS_CACHE_BLOCK_SIZE)
    {
        return STATUS_NOT_SUPPORTED;
    }
    /* No cache: every write goes through in issue order, nothing to order. */
    if (!NtfsCacheReady)
        return STATUS_SUCCESS;
    DeviceObject = Context ? static_cast<PDEVICE_OBJECT>(Context) : PartDeviceObj;
    if (!DeviceObject)
        return STATUS_INVALID_PARAMETER;

    LaterBlock = LaterOffset >> NTFS_CACHE_BLOCK_SHIFT;
    EarlierFirst = EarlierOffset >> NTFS_CACHE_BLOCK_SHIFT;
    EarlierLast = (EarlierOffset + EarlierLength - 1) >> NTFS_CACHE_BLOCK_SHIFT;

    ExAcquireFastMutex(&NtfsCacheMutex);
    for (b = EarlierFirst; b <= EarlierLast; b++)
    {
        NTFS_CACHE_SLOT* D = NtfsCacheProbe(DeviceObject, b);
        if (D && D->Dirty && D->DirtySeq > MaxEarlierSeq)
            MaxEarlierSeq = D->DirtySeq;
    }
    if (MaxEarlierSeq == 0)
    {
        /* The earlier range is already durable. */
        ExReleaseFastMutex(&NtfsCacheMutex);
        return STATUS_SUCCESS;
    }

    L = NtfsCacheProbe(DeviceObject, LaterBlock);
    if (!L || !L->Data)
    {
        Status = STATUS_NOT_FOUND;
    }
    else if (L->Dirty && MaxEarlierSeq > L->FlushAfterSeq)
    {
        /*
         * The later block is already dirty, so it may already be something
         * another constrained block has to wait for. Walk the dependency
         * closure of the earlier range: start from every dirty block the new
         * constraint would make L wait for (DirtySeq <= MaxEarlierSeq), and
         * widen by the FlushAfterSeq of each block reached, until nothing new
         * is reached. If L itself (DirtySeq <= Reach) is in that closure, the
         * new constraint would make L wait for a block that waits for L, and
         * the writeback would never finish. Refuse it. The scan is bounded by
         * the number of distinct FlushAfterSeq values in use, in practice one
         * or two passes over the slot array.
         */
        ULONGLONG Reach = 0;
        ULONGLONG Bound = MaxEarlierSeq;
        ULONG i;

        for (;;)
        {
            ULONGLONG Next = Reach;

            for (i = 0; i < NTFS_CACHE_SLOTS; i++)
            {
                NTFS_CACHE_SLOT* D = &NtfsCacheSlots[i];
                if (D != L && D->Dirty && D->Owner &&
                    D->Tag != NTFS_CACHE_EMPTY &&
                    D->DirtySeq != 0 && D->DirtySeq <= Bound &&
                    D->FlushAfterSeq > Next)
                {
                    Next = D->FlushAfterSeq;
                }
            }
            if (Next == Reach)
                break;
            Reach = Next;
            if (Reach > Bound)
                Bound = Reach;
        }
        if (Reach != 0 && L->DirtySeq <= Reach)
            Status = STATUS_NOT_SUPPORTED;
    }
    if (NT_SUCCESS(Status) && MaxEarlierSeq > L->FlushAfterSeq)
        L->FlushAfterSeq = MaxEarlierSeq;
    ExReleaseFastMutex(&NtfsCacheMutex);
    return Status;
}

/*
 * Drop a constraint installed by NtfsBarrierAfterContext on a later block the
 * caller did not write after all (the write failed, or the caller made the
 * earlier range durable synchronously instead). Only a clean block is
 * released: once the block is dirty the constraint belongs to its content.
 */
void
NtfsBarrierCancelContext(_In_opt_ void* Context,
                         _In_ ULONGLONG LaterOffset, _In_ ULONG LaterLength)
{
    PDEVICE_OBJECT DeviceObject;
    ULONGLONG First, Last, b;

    if (!NtfsCacheReady || !LaterLength ||
        LaterOffset > ~(ULONGLONG)0 - (LaterLength - 1))
    {
        return;
    }
    DeviceObject = Context ? static_cast<PDEVICE_OBJECT>(Context) : PartDeviceObj;
    if (!DeviceObject)
        return;

    First = LaterOffset >> NTFS_CACHE_BLOCK_SHIFT;
    Last = (LaterOffset + LaterLength - 1) >> NTFS_CACHE_BLOCK_SHIFT;
    ExAcquireFastMutex(&NtfsCacheMutex);
    for (b = First; b <= Last; b++)
    {
        NTFS_CACHE_SLOT* L = NtfsCacheProbe(DeviceObject, b);
        if (L && !L->Dirty)
            L->FlushAfterSeq = 0;
    }
    ExReleaseFastMutex(&NtfsCacheMutex);
}

NTSTATUS
NtfsFlushVolumeContext(_In_opt_ void* Context)
{
    PDEVICE_OBJECT DeviceObject;

    DeviceObject = Context ? static_cast<PDEVICE_OBJECT>(Context) : PartDeviceObj;
    if (!DeviceObject)
        return STATUS_INVALID_PARAMETER;

    return NtfsCacheFlushAll(DeviceObject);
}

static
NTSTATUS
NtfsWriteVolumeInternal(_In_opt_ void* Context,
                        _In_ ULONG SectorBytes,
                        _In_ ULONGLONG Offset,
                        _In_ ULONG Length,
                        _Inout_ PUCHAR Buffer,
                        _In_ BOOLEAN WriteThrough)
{
    PDEVICE_OBJECT DeviceObject;

    DeviceObject = Context ? static_cast<PDEVICE_OBJECT>(Context) : PartDeviceObj;
    if (!DeviceObject ||
        (SectorBytes != 512 && SectorBytes != 4096))
    {
        return STATUS_INVALID_PARAMETER;
    }

    /*
     * Metadata is rewritten constantly and usually superseded moments later,
     * so a write that fits inside one block is held in the cache and
     * committed in batches. Reads come from those same blocks, so a held-back
     * write is never invisible; anything reaching the volume another way
     * flushes first.
     */
    if (!WriteThrough && NtfsCacheReady && Length <= NTFS_CACHE_BLOCK_SIZE)
    {
        ULONGLONG Block = Offset >> NTFS_CACHE_BLOCK_SHIFT;
        ULONG BlockOffset = (ULONG)(Offset & (NTFS_CACHE_BLOCK_SIZE - 1));

        if (BlockOffset + Length <= NTFS_CACHE_BLOCK_SIZE)
        {
            NTFS_CACHE_SLOT* Slot;
            BOOLEAN Absorbed = FALSE;

            ExAcquireFastMutex(&NtfsCacheMutex);
            Slot = NtfsCacheProbe(DeviceObject, Block);
            if (Slot && Slot->Data)
            {
                RtlCopyMemory(Slot->Data + BlockOffset, Buffer, Length);
                Slot->Generation++;
                if (!Slot->Dirty)
                {
                    Slot->DirtySeq = ++NtfsCacheWriteSeq;
                    Slot->Dirty = TRUE;
                    NtfsCacheDirtyCount++;
                }
                Absorbed = TRUE;
            }
            ExReleaseFastMutex(&NtfsCacheMutex);

            /*
             * First touch of a metadata block: bring it in once so this
             * update and every later one to the same block stay in memory.
             * A fresh MFT record block otherwise costs a synchronous disk
             * write per record, and installs nothing, so its neighbours all
             * miss too. Writes up to one block qualify (a whole block is
             * installed without a fetch); larger writes are bulk data and
             * keep the direct path. Callers that need the bytes on the
             * disk before they return -- the zero-fill of newly allocated
             * metadata clusters -- come in through
             * NtfsWriteVolumeThroughContext, which skips this entirely.
             */
            if (!Absorbed)
            {
                PUCHAR Staged = (PUCHAR)ExAllocatePoolUninitialized(
                    NonPagedPoolNx, NTFS_CACHE_BLOCK_SIZE, 'CftN');

                if (Staged)
                {
                    NTSTATUS FillStatus;

                    if (Length == NTFS_CACHE_BLOCK_SIZE)
                    {
                        /* Fully covered: nothing on disk worth fetching. */
                        FillStatus = STATUS_SUCCESS;
                    }
                    else
                    {
                        FillStatus = ReadDisk(
                            DeviceObject,
                            Block << NTFS_CACHE_BLOCK_SHIFT,
                            NTFS_CACHE_BLOCK_SIZE,
                            Staged);
                    }

                    if (NT_SUCCESS(FillStatus))
                    {
                        RtlCopyMemory(Staged + BlockOffset, Buffer, Length);
                        NtfsCacheInstall(DeviceObject, Block, Staged);

                        /* Mark it held-back; if the slot was lost meanwhile,
                         * fall through and write through instead. */
                        ExAcquireFastMutex(&NtfsCacheMutex);
                        Slot = NtfsCacheProbe(DeviceObject, Block);
                        if (Slot && Slot->Data)
                        {
                            // Install may preserve a concurrently modified block.
                            RtlCopyMemory(Slot->Data + BlockOffset, Buffer, Length);
                            Slot->Generation++;
                            if (!Slot->Dirty)
                            {
                                Slot->DirtySeq = ++NtfsCacheWriteSeq;
                                Slot->Dirty = TRUE;
                                NtfsCacheDirtyCount++;
                            }
                            Absorbed = TRUE;
                        }
                        ExReleaseFastMutex(&NtfsCacheMutex);
                    }
                    ExFreePoolWithTag(Staged, 'CftN');
                }
            }

            if (Absorbed)
            {
                if (NtfsCacheDirtyCount > NTFS_CACHE_DIRTY_LIMIT)
                    NtfsCacheFlushSome(NTFS_CACHE_DIRTY_SLICE);
                NtfsCacheScheduleLazyWrite();
                return STATUS_SUCCESS;
            }
        }
    }

    /* Going to the disk directly: commit anything held for these bytes. */
    NTSTATUS FlushStatus = NtfsCacheFlushRange(DeviceObject, Offset, Length);
    if (!NT_SUCCESS(FlushStatus))
        return FlushStatus;

    NTSTATUS Status;
    PUCHAR WriteBuffer;
    ULONGLONG SectorAlignedOffset;
    ULONG SectorAlignedLength;

    SectorAlignedOffset = Offset - (Offset % SectorBytes);
    SectorAlignedLength = ALIGN_UP_BY((Offset - SectorAlignedOffset) + Length,
                                      SectorBytes);

    if (SectorAlignedOffset == Offset
        && SectorAlignedLength == Length)
    {
        // Write directly to the disk using the supplied buffer.
        Status = WriteDisk(DeviceObject,
                           SectorAlignedOffset,
                           SectorAlignedLength,
                           Buffer);
    }

    else
    {
        ULONG Skip = (ULONG)(Offset - SectorAlignedOffset);
        ULONG Chunk = min(SectorAlignedLength, NTFS_DISK_IO_CHUNK_SIZE);
        ULONG Done = 0;

        // Create the write buffer
        WriteBuffer = new(NonPagedPool) UCHAR[Chunk];
        if (!WriteBuffer)
            return STATUS_INSUFFICIENT_RESOURCES;

        Status = STATUS_SUCCESS;
        while (NT_SUCCESS(Status) && Done < SectorAlignedLength)
        {
            ULONG Piece = min(Chunk, SectorAlignedLength - Done);
            ULONG From = max(Done, Skip);
            ULONG To = min(Done + Piece, Skip + Length);

            // Fill the write buffer with what's on disk.
            Status = ReadDisk(DeviceObject,
                              SectorAlignedOffset + Done,
                              Piece,
                              WriteBuffer);

            if (NT_SUCCESS(Status))
            {
                // Copy the buffer contents we want to write into the write buffer.
                RtlCopyMemory(WriteBuffer + (From - Done),
                              Buffer + (From - Skip),
                              To - From);

                // Write to the disk.
                Status = WriteDisk(DeviceObject,
                                   SectorAlignedOffset + Done,
                                   Piece,
                                   WriteBuffer);
            }
            Done += Piece;
        }

        // Free write buffer
        delete[] WriteBuffer;
    }

    /* Keep whatever we still hold in step with what just went to the disk. */
    if (NT_SUCCESS(Status))
        NtfsCacheUpdateRange(DeviceObject, Offset, Length, Buffer);
    else
        NtfsCacheInvalidateRange(DeviceObject, Offset, Length);

    return Status;
}

NTSTATUS
NtfsWriteVolumeContext(_In_opt_ void* Context,
                       _In_ ULONG SectorBytes,
                       _In_ ULONGLONG Offset,
                       _In_ ULONG Length,
                       _Inout_ PUCHAR Buffer)
{
    return NtfsWriteVolumeInternal(Context, SectorBytes, Offset, Length,
                                   Buffer, FALSE);
}

/*
 * Write-through: skips the held-back path, so the bytes are on the disk when
 * this returns. A block dirty in the cache is committed first and the cached
 * copy is kept in step, exactly as for a bulk write.
 */
NTSTATUS
NtfsWriteVolumeThroughContext(_In_opt_ void* Context,
                              _In_ ULONG SectorBytes,
                              _In_ ULONGLONG Offset,
                              _In_ ULONG Length,
                              _Inout_ PUCHAR Buffer)
{
    return NtfsWriteVolumeInternal(Context, SectorBytes, Offset, Length,
                                   Buffer, TRUE);
}

NTSTATUS
NtfsFlushVolumeRangeContext(_In_opt_ void* Context,
                            _In_ ULONGLONG Offset,
                            _In_ ULONG Length)
{
    PDEVICE_OBJECT DeviceObject;

    DeviceObject = Context ? static_cast<PDEVICE_OBJECT>(Context) : PartDeviceObj;
    if (!DeviceObject || Offset > ~(ULONGLONG)0 - Length)
        return STATUS_INVALID_PARAMETER;

    return NtfsCacheFlushRange(DeviceObject, Offset, Length);
}

NTSTATUS
NtfsReadVolume(_In_ ULONGLONG Offset,
               _In_ ULONG Length,
               _Inout_ PUCHAR Buffer)
{
    return NtfsReadVolumeContext(PartDeviceObj,
                                 BytesPerSector,
                                 Offset,
                                 Length,
                                 Buffer);
}

NTSTATUS
NtfsWriteVolume(_In_ ULONGLONG Offset,
                _In_ ULONG Length,
                _Inout_ PUCHAR Buffer)
{
    return NtfsWriteVolumeContext(PartDeviceObj,
                                  BytesPerSector,
                                  Offset,
                                  Length,
                                  Buffer);
}

BOOLEAN
NtfsIsNameInExpression(_In_     PUNICODE_STRING Expression,
                       _In_     PUNICODE_STRING Name,
                       _In_     BOOLEAN IgnoreCase,
                       _In_opt_ PWCHAR UpcaseTable)
{
    return FsRtlIsNameInExpression(Expression,
                                   Name,
                                   IgnoreCase,
                                   UpcaseTable);
}

#ifdef __cplusplus
}
#endif

extern "C"
NTSTATUS
NtfsDiskFlushKm(VOID)
{
    return NtfsCacheFlushAll(NULL);
}

extern "C"
NTSTATUS
NtfsDiskUnloadKm(VOID)
{
    if (!NtfsCacheReady)
        return STATUS_SUCCESS;

    InterlockedExchange(&NtfsCacheLazyStopped, 1);
    KeCancelTimer(&NtfsCacheLazyTimer);
    KeFlushQueuedDpcs();
    KeWaitForSingleObject(&NtfsCacheLazyIdle, Executive, KernelMode, FALSE, NULL);
    return NtfsCacheFlushAll(NULL);
}

/*
 * A completed write has reached the device, which may still hold it in a
 * volatile write cache. A flush request makes it durable. A device with no
 * cache to flush may reject the request, which is not a failure.
 */
extern "C"
NTSTATUS
NtfsDiskFlushDeviceKm(_In_ PDEVICE_OBJECT DeviceObject)
{
    NTSTATUS Status;

    if (!DeviceObject)
        return STATUS_INVALID_PARAMETER;
    Status = NtfsPerformDiskIo(IRP_MJ_FLUSH_BUFFERS, DeviceObject, 0, 0, NULL);
    if (Status == STATUS_INVALID_DEVICE_REQUEST ||
        Status == STATUS_NOT_SUPPORTED ||
        Status == STATUS_NOT_IMPLEMENTED)
    {
        Status = STATUS_SUCCESS;
    }
    return Status;
}

extern "C"
NTSTATUS
NtfsDiskFlushVolumeKm(_In_ PDEVICE_OBJECT DeviceObject)
{
    NTSTATUS Status;

    if (!DeviceObject)
        return STATUS_INVALID_PARAMETER;
    Status = NtfsCacheFlushAll(DeviceObject);
    if (NT_SUCCESS(Status))
        Status = NtfsDiskFlushDeviceKm(DeviceObject);
    return Status;
}

extern "C"
NTSTATUS
NtfsDiskFlushRangeKm(_In_ PDEVICE_OBJECT DeviceObject,
                    _In_ ULONGLONG Offset,
                    _In_ ULONG Length)
{
    if (!DeviceObject || Offset > ~(ULONGLONG)0 - Length)
        return STATUS_INVALID_PARAMETER;
    return NtfsCacheFlushRange(DeviceObject, Offset, Length);
}

extern "C"
NTSTATUS
NtfsDiskReadKm(_In_ PDEVICE_OBJECT DeviceObject,
               _In_ ULONG SectorBytes,
               _In_ ULONGLONG Offset,
               _In_ ULONG Length,
               _Out_writes_bytes_(Length) PVOID Buffer)
{
    if (!DeviceObject || !Buffer || !Length || Offset > ~(ULONGLONG)0 - Length)
        return STATUS_INVALID_PARAMETER;
    return NtfsReadVolumeContext(DeviceObject,
                                 SectorBytes,
                                 Offset,
                                 Length,
                                 (PUCHAR)Buffer);
}

extern "C"
NTSTATUS
NtfsDiskWriteKm(_In_ PDEVICE_OBJECT DeviceObject,
                _In_ ULONG SectorBytes,
                _In_ ULONGLONG Offset,
                _In_ ULONG Length,
                _In_reads_bytes_(Length) const VOID* Buffer)
{
    if (!DeviceObject || !Buffer || !Length || Offset > ~(ULONGLONG)0 - Length)
        return STATUS_INVALID_PARAMETER;
    return NtfsWriteVolumeContext(DeviceObject,
                                  SectorBytes,
                                  Offset,
                                  Length,
                                  (PUCHAR)Buffer);
}
