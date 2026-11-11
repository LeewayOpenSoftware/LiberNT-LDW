/*
 * PROJECT:     LiberNT Cache Manager
 * FILE:        ntoskrnl/cc/api/ccinit.c
 * PURPOSE:     Cache manager and file cache initialization
 *
 * SPDX-FileCopyrightText: 2026 Ahmed ARIF
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "ccnt.h"

#define NDEBUG
#include <debug.h>

typedef struct _CC_NT_DEFERRED_WRITE
{
    LIST_ENTRY Link;
    PFILE_OBJECT FileObject;
    PCC_POST_DEFERRED_WRITE PostRoutine;
    PVOID Context1;
    PVOID Context2;
    ULONG BytesToWrite;
} CC_NT_DEFERRED_WRITE, *PCC_NT_DEFERRED_WRITE;

typedef struct _CC_NT_LAZY_WAITER
{
    LIST_ENTRY Link;
    KEVENT Event;
} CC_NT_LAZY_WAITER, *PCC_NT_LAZY_WAITER;

CC_CACHE CcNtCache;

BOOLEAN CcPfEnablePrefetcher;

ULONG CcLazyWritePages;
ULONG CcLazyWriteIos;
ULONG CcMapDataWait;
ULONG CcMapDataNoWait;
ULONG CcPinReadWait;
ULONG CcPinReadNoWait;
ULONG CcPinMappedDataCount;
ULONG CcDataPages;
ULONG CcDataFlushes;
ULONG CcFastMdlReadNotPossible;
ULONG CcFastMdlReadWait;
ULONG CcFastReadNoWait;
ULONG CcFastReadNotPossible;
ULONG CcFastReadResourceMiss;
ULONG CcFastReadWait;

static KEVENT CcNtLazyWriterEvent;
static LIST_ENTRY CcNtDeferredWrites;
static KSPIN_LOCK CcNtDeferredLock;
static LIST_ENTRY CcNtLazyWaiters;
static KSPIN_LOCK CcNtLazyWaitLock;
static BOOLEAN CcNtReady;
/* Set at shutdown: the lazy writer no longer starts passes on its own timer. */
static volatile LONG CcNtLazyTimerStopped;

NTSTATUS
CcNtFlushMap(
    _Inout_ PCC_NT_MAP NtMap,
    _In_ ULONG64 Offset,
    _In_ ULONG64 Length,
    _In_ ULONG MaximumPages,
    _Out_opt_ PULONG PagesFlushed)
{
    ULONG64 End = (Length == (ULONG64)-1 || Offset + Length < Offset) ? (ULONG64)-1 : Offset + Length;
    ULONG64 Generation = CcBcbBeginFlush(&NtMap->Map);
    ULONG Flushed = 0;
    NTSTATUS Status;

    if (NtMap->FlushToLsn != NULL && NtMap->LogHandle != NULL)
    {
        ULONG64 Oldest, Newest;
        ULONG DirtyBcbs;

        if (CcBcbDirtyLsns(&NtMap->Map, &Oldest, &Newest, &DirtyBcbs) && Newest != 0)
        {
            LARGE_INTEGER Lsn;

            Lsn.QuadPart = (LONGLONG)Newest;
            NtMap->FlushToLsn(NtMap->LogHandle, Lsn);
        }
    }

    Status = CcDirtyFlush(&NtMap->Map, Offset, Length, MaximumPages, &Flushed);

    if (NT_SUCCESS(Status) && MaximumPages == ~0u)
        CcBcbCompleteFlush(&NtMap->Map, Offset, End, Generation);

    if (Flushed != 0)
    {
        CcDataFlushes++;
        CcDataPages += Flushed;
    }

    if (PagesFlushed != NULL)
        *PagesFlushed = Flushed;

    return Status;
}

VOID
CcNtKickLazyWriter(VOID)
{
    if (CcNtReady)
        KeSetEvent(&CcNtLazyWriterEvent, IO_NO_INCREMENT, FALSE);
}

ULONG
CcUnmapIdleViews(
    _In_ SIZE_T Bytes)
{
    ULONG64 Views = ((ULONG64)Bytes >> CC_VIEW_SHIFT) + 2;

    if (!CcNtReady)
        return 0;

    return CcCacheReleaseIdle(&CcNtCache, (ULONG)min(Views, (ULONG64)MAXULONG));
}

VOID
CcNtProcessDeferredWrites(VOID)
{
    for (;;)
    {
        PCC_NT_DEFERRED_WRITE Deferred = NULL;
        KIRQL OldIrql;

        if (ReadPointerNoFence((PVOID const volatile *)&CcNtDeferredWrites.Flink) == &CcNtDeferredWrites)
            return;

        KeAcquireSpinLock(&CcNtDeferredLock, &OldIrql);

        if (!IsListEmpty(&CcNtDeferredWrites))
        {
            PCC_NT_DEFERRED_WRITE First = CONTAINING_RECORD(CcNtDeferredWrites.Flink, CC_NT_DEFERRED_WRITE, Link);

            if (CcDirtyCanWrite(&CcNtCache, NULL, First->BytesToWrite))
            {
                RemoveEntryList(&First->Link);
                Deferred = First;
            }
        }

        KeReleaseSpinLock(&CcNtDeferredLock, OldIrql);

        if (Deferred == NULL)
            return;

        Deferred->PostRoutine(Deferred->Context1, Deferred->Context2);
        ExFreePoolWithTag(Deferred, 'wDcC');
    }
}

VOID
NTAPI
CcDeferWrite(
    _In_ PFILE_OBJECT FileObject,
    _In_ PCC_POST_DEFERRED_WRITE PostRoutine,
    _In_ PVOID Context1,
    _In_ PVOID Context2,
    _In_ ULONG BytesToWrite,
    _In_ BOOLEAN Retrying)
{
    PCC_NT_DEFERRED_WRITE Deferred = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Deferred), 'wDcC');
    KIRQL OldIrql;

    if (Deferred == NULL)
    {
        PostRoutine(Context1, Context2);
        return;
    }

    Deferred->FileObject = FileObject;
    Deferred->PostRoutine = PostRoutine;
    Deferred->Context1 = Context1;
    Deferred->Context2 = Context2;
    Deferred->BytesToWrite = BytesToWrite;

    KeAcquireSpinLock(&CcNtDeferredLock, &OldIrql);
    if (Retrying)
        InsertHeadList(&CcNtDeferredWrites, &Deferred->Link);
    else
        InsertTailList(&CcNtDeferredWrites, &Deferred->Link);
    KeReleaseSpinLock(&CcNtDeferredLock, OldIrql);

    CcNtKickLazyWriter();
}

/* Engine callback, under DirtyLock: a clean map got its first dirty page. */
static
VOID
CcNtMapDirtied(
    _Inout_ PCC_MAP Map)
{
    CC_ATOMIC_WRITE64(&CONTAINING_RECORD(Map, CC_NT_MAP, Map)->DirtySince, (LONG64)KeQueryInterruptTime());
}

static
BOOLEAN
CcNtReferenceDirtyMap(
    _In_ PCC_MAP Map)
{
    PCC_NT_MAP Candidate = CONTAINING_RECORD(Map, CC_NT_MAP, Map);
    PCC_NT_MAP_BUCKET Bucket = CcNtMapBucket(Candidate->Pointers);
    BOOLEAN Live;

    KeAcquireSpinLockAtDpcLevel(&Bucket->Lock);
    Live = !IsListEmpty(&Candidate->Link);
    if (Live)
        InterlockedIncrement(&Candidate->ReferenceCount);
    KeReleaseSpinLockFromDpcLevel(&Bucket->Lock);

    return Live;
}

static
PCC_NT_MAP
CcNtNextDirtyMap(VOID)
{
    BOOLEAN Accepted;
    PCC_MAP Map;
    ULONG Tries;

    for (Tries = 0; Tries < 16; Tries++)
    {
        Map = CcDirtyNextMapAccept(&CcNtCache, CcNtReferenceDirtyMap, &Accepted);
        if (Map == NULL)
            break;

        if (Accepted)
            return CONTAINING_RECORD(Map, CC_NT_MAP, Map);
    }

    return NULL;
}

/* Writes back up to MaximumPages of one map with the file system's lazy-write
 * lock held, and returns how many pages reached the file system. */
static
ULONG
CcNtLazyFlushMap(
    _Inout_ PCC_NT_MAP NtMap,
    _In_ ULONG MaximumPages,
    _In_ BOOLEAN Wait,
    _In_ BOOLEAN CalledFromLazy)
{
    ULONG Flushed = 0;

    KeEnterCriticalRegion();
    KeWaitForSingleObject(&NtMap->LazyWriteGate, Executive, KernelMode, FALSE, NULL);
    if (NtMap->Callbacks.AcquireForLazyWrite == NULL ||
        NtMap->Callbacks.AcquireForLazyWrite(NtMap->LazyWriteContext, Wait))
    {
        CcNtFlushMap(NtMap, 0, (ULONG64)-1, MaximumPages, &Flushed);

        if (NtMap->Callbacks.ReleaseFromLazyWrite != NULL)
            NtMap->Callbacks.ReleaseFromLazyWrite(NtMap->LazyWriteContext);
    }
    KeSetEvent(&NtMap->LazyWriteGate, IO_NO_INCREMENT, FALSE);
    KeLeaveCriticalRegion();

    if (CalledFromLazy && Flushed != 0)
    {
        CcLazyWriteIos++;
        CcLazyWritePages += Flushed;
    }

    return Flushed;
}

NTSTATUS
CcRosFlushDirtyPages(
    ULONG Target,
    PULONG Count,
    BOOLEAN Wait,
    BOOLEAN CalledFromLazy)
{
    ULONG Total = 0;
    ULONG Visited = 0;

    while (Total < Target && Visited++ < 256)
    {
        PCC_NT_MAP NtMap = CcNtNextDirtyMap();

        if (NtMap == NULL)
            break;

        Total += CcNtLazyFlushMap(NtMap, Target - Total, Wait, CalledFromLazy);
        CcNtDereferenceMap(NtMap);
    }

    if (Count != NULL)
        *Count = Total;

    return STATUS_SUCCESS;
}

/*
 * The budgeted pass writes TotalDirtyPages / CC_LAZY_DIVISOR a second, which
 * is a rate limit for a busy cache, not a bound on how long a page stays
 * dirty: a 1 MiB file alone in the cache took 14 passes, and nothing wrote a
 * closed file any sooner. Before the budgeted pass, write back completely
 * every dirty map whose last handle is gone or whose oldest dirty data is
 * older than CC_LAZY_MAX_AGE_SECONDS. A map that is written to continuously
 * gets such a pass every CC_LAZY_MAX_AGE_SECONDS, so no page of it is older
 * than that plus one pass. The pass writes at most CC_LAZY_AGED_MAX_PAGES
 * and leaves the rest aged for the next one, so a large closed file cannot
 * hold the lazy writer for seconds; deferred writers are served between
 * maps. Returns the pages written.
 */
static
ULONG
CcNtFlushAgedMaps(
    _In_ ULONG64 Now)
{
    const ULONG64 MaximumAge = (ULONG64)CC_LAZY_MAX_AGE_SECONDS * 10 * 1000 * 1000;
    PCC_MAP First = NULL;
    ULONG Total = 0;
    ULONG Visited;

    /* CcDirtyNextMapAccept rotates the list, so one full turn ends where it
     * began; the bound covers a list that changes under us. */
    for (Visited = 0; Visited < 256 && Total < CC_LAZY_AGED_MAX_PAGES; Visited++)
    {
        BOOLEAN Accepted;
        PCC_MAP Map = CcDirtyNextMapAccept(&CcNtCache, CcNtReferenceDirtyMap, &Accepted);
        PCC_NT_MAP NtMap;
        BOOLEAN Aged;

        if (Map == NULL || Map == First)
        {
            if (Map != NULL && Accepted)
                CcNtDereferenceMap(CONTAINING_RECORD(Map, CC_NT_MAP, Map));
            break;
        }
        if (First == NULL)
            First = Map;
        if (!Accepted)
            continue;

        NtMap = CONTAINING_RECORD(Map, CC_NT_MAP, Map);
        Aged = (CC_ATOMIC_READ32(&NtMap->Map.OpenCount) == 0) ||
               (Now - (ULONG64)CC_ATOMIC_READ64(&NtMap->DirtySince) >= MaximumAge);
        if (Aged)
        {
            ULONG Room = CC_LAZY_AGED_MAX_PAGES - Total;
            ULONG Wanted = (CC_ATOMIC_READ64(&NtMap->Map.DirtyPages) > (LONG64)Room) ? Room : ~0u;

            /* Within the room, a full flush (CcBcbCompleteFlush included);
             * beyond it, a budgeted one, and the map stays aged. */
            Total += CcNtLazyFlushMap(NtMap, Wanted, TRUE, TRUE);
            if (Wanted == ~0u)
                CC_ATOMIC_WRITE64(&NtMap->DirtySince, (LONG64)Now);
            CcNtProcessDeferredWrites();
        }
        CcNtDereferenceMap(NtMap);
    }

    return Total;
}

static
VOID
CcNtTakeLazyWaiters(
    _Out_ PLIST_ENTRY Waiters)
{
    KIRQL OldIrql;

    InitializeListHead(Waiters);
    KeAcquireSpinLock(&CcNtLazyWaitLock, &OldIrql);
    while (!IsListEmpty(&CcNtLazyWaiters))
        InsertTailList(Waiters, RemoveHeadList(&CcNtLazyWaiters));
    KeReleaseSpinLock(&CcNtLazyWaitLock, OldIrql);
}

static
VOID
CcNtReleaseLazyWaiters(
    _Inout_ PLIST_ENTRY Waiters)
{
    while (!IsListEmpty(Waiters))
    {
        PCC_NT_LAZY_WAITER Waiter = CONTAINING_RECORD(RemoveHeadList(Waiters), CC_NT_LAZY_WAITER, Link);

        KeSetEvent(&Waiter->Event, IO_NO_INCREMENT, FALSE);
    }
}

static
VOID
NTAPI
CcNtLazyWriter(
    _In_ PVOID Context)
{
    LARGE_INTEGER Period;
#if DBG
    ULONG64 LastPass = 0;
#endif

    UNREFERENCED_PARAMETER(Context);
    Period.QuadPart = -10 * 1000 * 1000;

    for (;;)
    {
        LIST_ENTRY Waiters;
        ULONG Target;
        NTSTATUS Status;
        ULONG Aged;
        ULONG Flushed = 0;
        ULONG64 Started;
        ULONG64 Teardown;

        Status = KeWaitForSingleObject(&CcNtLazyWriterEvent, Executive, KernelMode, FALSE, &Period);

        CcNtTakeLazyWaiters(&Waiters);
        /*
         * After shutdown has begun, a pass runs only when someone asks for one
         * (CcWaitForCurrentLazyWriterActivity, a deferred or throttled write).
         * A timer pass could otherwise start between the shutdown's last lazy
         * wait and the file systems' shutdown, and its writes and map closes
         * would land on volumes already marked clean.
         */
        if (Status == STATUS_TIMEOUT && CcNtLazyTimerStopped && IsListEmpty(&Waiters))
            continue;

        Started = KeQueryInterruptTime();
#if DBG
        /* A pass that starts late was held up by the previous pass's tail
         * (teardown of closed maps, deferred writes), not by the timer. */
        if (LastPass != 0 && Started - LastPass > 15 * 1000 * 1000)
            DPRINT1("CC: lazy pass late: %I64u ms since the previous pass started\n", (Started - LastPass) / 10000);
        LastPass = Started;
#endif
        Aged = CcNtFlushAgedMaps(Started);
        Target = CcDirtyLazyTarget(&CcNtCache);
        if (Target != 0)
            CcRosFlushDirtyPages(Target, &Flushed, TRUE, TRUE);

#if DBG
        /* Lab trace: one line per pass that had work, so a power-cut probe
         * can read the write-back cadence off COM1. */
        if (Aged != 0 || Target != 0)
        {
            DPRINT1("CC: lazy pass at %I64u ms: aged %lu, target %lu, flushed %lu, dirty left %I64d, took %I64u ms\n",
                    Started / 10000, Aged, Target, Flushed, CC_ATOMIC_READ64(&CcNtCache.TotalDirtyPages),
                    (KeQueryInterruptTime() - Started) / 10000);
        }
#else
        UNREFERENCED_PARAMETER(Aged);
        UNREFERENCED_PARAMETER(Flushed);
#endif

        Teardown = KeQueryInterruptTime();
        CcNtExpireClosedMaps();
        CcNtProcessDeferredWrites();
#if DBG
        if (KeQueryInterruptTime() - Teardown >= 50 * 10 * 1000)
        {
            DPRINT1("CC: lazy pass teardown took %I64u ms\n", (KeQueryInterruptTime() - Teardown) / 10000);
        }
#else
        UNREFERENCED_PARAMETER(Teardown);
#endif
        CcNtReleaseLazyWaiters(&Waiters);
    }
}

VOID
NTAPI
CcStopLazyWriterTimer(VOID)
{
    InterlockedExchange(&CcNtLazyTimerStopped, 1);
}

NTSTATUS
NTAPI
CcWaitForCurrentLazyWriterActivity(VOID)
{
    CC_NT_LAZY_WAITER Waiter;
    KIRQL OldIrql;

    if (!CcNtReady)
        return STATUS_SUCCESS;

    KeInitializeEvent(&Waiter.Event, NotificationEvent, FALSE);
    KeAcquireSpinLock(&CcNtLazyWaitLock, &OldIrql);
    InsertTailList(&CcNtLazyWaiters, &Waiter.Link);
    KeReleaseSpinLock(&CcNtLazyWaitLock, OldIrql);

    CcNtKickLazyWriter();
    return KeWaitForSingleObject(&Waiter.Event, Executive, KernelMode, FALSE, NULL);
}

CODE_SEG("INIT")
BOOLEAN
CcInitializeCacheManager(VOID)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    ULONG64 PhysicalBytes = (ULONG64)MmNumberOfPhysicalPages << PAGE_SHIFT;
    ULONG64 ViewBytes = min(PhysicalBytes, MiSpaceFreeBytes(&MiSystem.SystemSpace)) / 2;
    ULONG Views = (ULONG)min(ViewBytes >> CC_VIEW_SHIFT, (ULONG64)MAXULONG);
    LONG64 DirtyThreshold = (LONG64)(MmNumberOfPhysicalPages / 8);
    HANDLE Handle;

    if (Views < CC_MIN_VIEWS)
        Views = CC_MIN_VIEWS;

    if (Views > CC_MAX_VIEWS)
        Views = CC_MAX_VIEWS;

    if (!NT_SUCCESS(CcCacheInitialize(&CcNtCache, Views, DirtyThreshold)))
        return FALSE;

    CcNtCache.BcbBytes = sizeof(CC_NT_BCB);
    CcNtCache.BcbCreated = CcNtBcbCreated;
    CcNtCache.BcbDeleting = CcNtBcbDeleting;
    CcNtCache.MapDirtied = CcNtMapDirtied;

    CcNtInitializeMapTable();
    InitializeListHead(&CcNtDeferredWrites);
    KeInitializeSpinLock(&CcNtDeferredLock);
    InitializeListHead(&CcNtLazyWaiters);
    KeInitializeSpinLock(&CcNtLazyWaitLock);
    KeInitializeEvent(&CcNtLazyWriterEvent, SynchronizationEvent, FALSE);

    InitializeObjectAttributes(&ObjectAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(PsCreateSystemThread(&Handle, THREAD_ALL_ACCESS, &ObjectAttributes, NULL, NULL, CcNtLazyWriter,
                                         NULL)))
    {
        return FALSE;
    }

    ZwClose(Handle);
    CcNtReady = TRUE;
    return TRUE;
}

VOID
NTAPI
CcShutdownSystem(VOID)
{
    ULONG Round;

    for (Round = 0; Round < 64; Round++)
    {
        ULONG Flushed = 0;

        CcRosFlushDirtyPages(~0u, &Flushed, TRUE, FALSE);
        if (Flushed == 0)
            break;
    }
    CcNtExpireClosedMaps();
}

CODE_SEG("INIT")
VOID
NTAPI
CcPfInitializePrefetcher(VOID)
{
}

#ifdef KDBG
#include <kdbg/kdb.h>

BOOLEAN
ExpKdbgExtFileCache(
    ULONG Argc,
    PCHAR Argv[])
{
    PLIST_ENTRY Entry;
    ULONG Index;

    UNREFERENCED_PARAMETER(Argc);
    UNREFERENCED_PARAMETER(Argv);

    KdbpPrint("views %lu hits %I64d misses %I64d reclaims %I64d dirty %I64d\n", CcNtCache.ViewCount,
              CcNtCache.ViewHits, CcNtCache.ViewMisses, CcNtCache.ViewReclaims, CcNtCache.TotalDirtyPages);

    for (Index = 0; Index < CC_NT_MAP_BUCKETS; Index++)
    {
        for (Entry = CcNtMapBuckets[Index].List.Flink; Entry != &CcNtMapBuckets[Index].List; Entry = Entry->Flink)
        {
            PCC_NT_MAP NtMap = CONTAINING_RECORD(Entry, CC_NT_MAP, Link);

            KdbpPrint("%p size %I64d views %lu dirty %I64d ", NtMap, (LONGLONG)NtMap->Map.FileSize,
                      NtMap->Map.ViewsAttached, NtMap->Map.DirtyPages);
            KdbpPrintUnicodeString(&NtMap->FileObject->FileName);
            KdbpPrint("\n");
        }
    }

    return TRUE;
}

BOOLEAN
ExpKdbgExtDefWrites(
    ULONG Argc,
    PCHAR Argv[])
{
    UNREFERENCED_PARAMETER(Argc);
    UNREFERENCED_PARAMETER(Argv);

    KdbpPrint("dirty pages %I64d threshold %I64d deferred %s\n", CcNtCache.TotalDirtyPages,
              CcNtCache.DirtyPageThreshold, IsListEmpty(&CcNtDeferredWrites) ? "none" : "pending");
    return TRUE;
}
#endif
