/*
 * PROJECT:     NetIO driver
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Network store (NSI): provider registry, table access and change requests
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <ntifs.h>
#include <netiodef.h>
#include <nsikm.h>

#define TAG_NSI 'tSsN'

typedef struct _NSI_PROVIDER
{
    LIST_ENTRY ListEntry;
    NPI_MODULEID Module;
    ULONG Table;
    NSI_PROVIDER_DISPATCH Dispatch;
    PVOID Context;
    volatile LONG References;
    volatile LONG Sequence;
    BOOLEAN Deregistering;
    KEVENT Released;
    IO_CSQ Csq;
    LIST_ENTRY PendingIrps;
    KSPIN_LOCK PendingLock;
} NSI_PROVIDER;

static LIST_ENTRY NsiProviderList;
static KSPIN_LOCK NsiProviderLock;
static BOOLEAN NsiReady;

static IO_CSQ_INSERT_IRP NsiCsqInsertIrp;
static IO_CSQ_REMOVE_IRP NsiCsqRemoveIrp;
static IO_CSQ_PEEK_NEXT_IRP NsiCsqPeekNextIrp;
static IO_CSQ_ACQUIRE_LOCK NsiCsqAcquireLock;
static IO_CSQ_RELEASE_LOCK NsiCsqReleaseLock;
static IO_CSQ_COMPLETE_CANCELED_IRP NsiCsqCompleteCanceledIrp;

VOID
NsiInitialize(VOID)
{
    InitializeListHead(&NsiProviderList);
    KeInitializeSpinLock(&NsiProviderLock);
    NsiReady = TRUE;
}

static BOOLEAN
NsiModuleEqual(
    _In_ const NPI_MODULEID *First,
    _In_ const NPI_MODULEID *Second)
{
    if (First->Type != Second->Type)
        return FALSE;
    if (First->Type == MIT_GUID)
        return RtlEqualMemory(&First->Guid, &Second->Guid, sizeof(GUID));
    if (First->Type == MIT_IF_LUID)
        return First->IfLuid.LowPart == Second->IfLuid.LowPart &&
               First->IfLuid.HighPart == Second->IfLuid.HighPart;
    return FALSE;
}

static PNSI_PROVIDER
NsiFindProviderLocked(
    _In_ const NPI_MODULEID *Module,
    _In_ ULONG Table)
{
    PLIST_ENTRY Entry;

    for (Entry = NsiProviderList.Flink; Entry != &NsiProviderList; Entry = Entry->Flink)
    {
        PNSI_PROVIDER Provider = CONTAINING_RECORD(Entry, NSI_PROVIDER, ListEntry);

        if (Provider->Table == Table && !Provider->Deregistering &&
            NsiModuleEqual(&Provider->Module, Module))
        {
            return Provider;
        }
    }
    return NULL;
}

static PNSI_PROVIDER
NsiReferenceProvider(
    _In_ const NPI_MODULEID *Module,
    _In_ ULONG Table)
{
    PNSI_PROVIDER Provider;
    KIRQL OldIrql;

    if (!NsiReady || Module == NULL)
        return NULL;
    KeAcquireSpinLock(&NsiProviderLock, &OldIrql);
    Provider = NsiFindProviderLocked(Module, Table);
    if (Provider != NULL)
        InterlockedIncrement(&Provider->References);
    KeReleaseSpinLock(&NsiProviderLock, OldIrql);
    return Provider;
}

static VOID
NsiDereferenceProvider(
    _In_ PNSI_PROVIDER Provider)
{
    if (InterlockedDecrement(&Provider->References) == 0)
        KeSetEvent(&Provider->Released, IO_NO_INCREMENT, FALSE);
}

static BOOLEAN
NsiReferenceRegisteredProvider(
    _In_ PNSI_PROVIDER Provider)
{
    PLIST_ENTRY Entry;
    KIRQL OldIrql;
    BOOLEAN Found = FALSE;

    if (!NsiReady)
        return FALSE;
    KeAcquireSpinLock(&NsiProviderLock, &OldIrql);
    for (Entry = NsiProviderList.Flink; Entry != &NsiProviderList; Entry = Entry->Flink)
    {
        if (CONTAINING_RECORD(Entry, NSI_PROVIDER, ListEntry) != Provider)
            continue;
        if (!Provider->Deregistering)
        {
            InterlockedIncrement(&Provider->References);
            Found = TRUE;
        }
        break;
    }
    KeReleaseSpinLock(&NsiProviderLock, OldIrql);
    return Found;
}

static NTSTATUS
NsiCheckSizes(
    _In_ const NSI_PROVIDER *Provider,
    _In_ const NSI_ROW_SIZES *Sizes)
{
    const NSI_ROW_SIZES *Own = &Provider->Dispatch.RowSizes;

    if ((Sizes->Key != 0 && Sizes->Key != Own->Key) ||
        (Sizes->Rw != 0 && Sizes->Rw != Own->Rw) ||
        (Sizes->Dynamic != 0 && Sizes->Dynamic != Own->Dynamic) ||
        (Sizes->Static != 0 && Sizes->Static != Own->Static))
    {
        return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}

static VOID NTAPI
NsiCsqInsertIrp(
    _In_ PIO_CSQ Csq,
    _In_ PIRP Irp)
{
    PNSI_PROVIDER Provider = CONTAINING_RECORD(Csq, NSI_PROVIDER, Csq);

    InsertTailList(&Provider->PendingIrps, &Irp->Tail.Overlay.ListEntry);
}

static VOID NTAPI
NsiCsqRemoveIrp(
    _In_ PIO_CSQ Csq,
    _In_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(Csq);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
}

static PIRP NTAPI
NsiCsqPeekNextIrp(
    _In_ PIO_CSQ Csq,
    _In_opt_ PIRP Irp,
    _In_opt_ PVOID PeekContext)
{
    PNSI_PROVIDER Provider = CONTAINING_RECORD(Csq, NSI_PROVIDER, Csq);
    PLIST_ENTRY Entry;

    Entry = (Irp == NULL) ? Provider->PendingIrps.Flink : Irp->Tail.Overlay.ListEntry.Flink;
    for (; Entry != &Provider->PendingIrps; Entry = Entry->Flink)
    {
        PIRP Candidate = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);

        if (PeekContext == NULL ||
            IoGetCurrentIrpStackLocation(Candidate)->FileObject == (PFILE_OBJECT)PeekContext)
        {
            return Candidate;
        }
    }
    return NULL;
}

_IRQL_raises_(DISPATCH_LEVEL)
static VOID NTAPI
NsiCsqAcquireLock(
    _In_ PIO_CSQ Csq,
    _Out_ PKIRQL Irql)
{
    PNSI_PROVIDER Provider = CONTAINING_RECORD(Csq, NSI_PROVIDER, Csq);

    KeAcquireSpinLock(&Provider->PendingLock, Irql);
}

static VOID NTAPI
NsiCsqReleaseLock(
    _In_ PIO_CSQ Csq,
    _In_ KIRQL Irql)
{
    PNSI_PROVIDER Provider = CONTAINING_RECORD(Csq, NSI_PROVIDER, Csq);

    KeReleaseSpinLock(&Provider->PendingLock, Irql);
}

static VOID NTAPI
NsiCsqCompleteCanceledIrp(
    _In_ PIO_CSQ Csq,
    _In_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(Csq);
    Irp->IoStatus.Status = STATUS_CANCELLED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

static VOID
NsiDrainPendingIrps(
    _In_ PNSI_PROVIDER Provider,
    _In_opt_ PFILE_OBJECT FileObject,
    _In_ NTSTATUS Status,
    _In_ ULONG_PTR Information)
{
    PIRP Irp;

    while ((Irp = IoCsqRemoveNextIrp(&Provider->Csq, FileObject)) != NULL)
    {
        Irp->IoStatus.Status = Status;
        Irp->IoStatus.Information = Information;
        IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);
    }
}

NTSTATUS
NTAPI
NsiRegisterProvider(
    _In_ const NPI_MODULEID *Module,
    _In_ ULONG Table,
    _In_ const NSI_PROVIDER_DISPATCH *Dispatch,
    _In_opt_ PVOID Context,
    _Out_ PNSI_PROVIDER *Provider)
{
    PNSI_PROVIDER New;
    KIRQL OldIrql;
    NTSTATUS Status;

    *Provider = NULL;
    if (!NsiReady)
        return STATUS_DEVICE_NOT_READY;
    if (Module == NULL || Dispatch == NULL || Dispatch->Size < sizeof(*Dispatch) ||
        Dispatch->GetAll == NULL || Dispatch->RowSizes.Key == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }
    New = ExAllocatePoolWithTag(NonPagedPool, sizeof(*New), TAG_NSI);
    if (New == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(New, sizeof(*New));
    New->Module = *Module;
    New->Table = Table;
    RtlCopyMemory(&New->Dispatch, Dispatch, sizeof(New->Dispatch));
    New->Context = Context;
    New->References = 1;
    KeInitializeEvent(&New->Released, NotificationEvent, FALSE);
    InitializeListHead(&New->PendingIrps);
    KeInitializeSpinLock(&New->PendingLock);
    Status = IoCsqInitialize(&New->Csq, NsiCsqInsertIrp, NsiCsqRemoveIrp, NsiCsqPeekNextIrp,
                             NsiCsqAcquireLock, NsiCsqReleaseLock, NsiCsqCompleteCanceledIrp);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(New, TAG_NSI);
        return Status;
    }

    KeAcquireSpinLock(&NsiProviderLock, &OldIrql);
    if (NsiFindProviderLocked(Module, Table) != NULL)
    {
        KeReleaseSpinLock(&NsiProviderLock, OldIrql);
        ExFreePoolWithTag(New, TAG_NSI);
        return STATUS_OBJECT_NAME_COLLISION;
    }
    InsertTailList(&NsiProviderList, &New->ListEntry);
    KeReleaseSpinLock(&NsiProviderLock, OldIrql);

    *Provider = New;
    return STATUS_SUCCESS;
}

VOID
NTAPI
NsiDeregisterProvider(
    _In_ PNSI_PROVIDER Provider)
{
    KIRQL OldIrql;

    PAGED_CODE();

    KeAcquireSpinLock(&NsiProviderLock, &OldIrql);
    Provider->Deregistering = TRUE;
    RemoveEntryList(&Provider->ListEntry);
    KeReleaseSpinLock(&NsiProviderLock, OldIrql);

    NsiDereferenceProvider(Provider);
    KeWaitForSingleObject(&Provider->Released, Executive, KernelMode, FALSE, NULL);
    NsiDrainPendingIrps(Provider, NULL, STATUS_CANCELLED, 0);
    ExFreePoolWithTag(Provider, TAG_NSI);
}

VOID
NTAPI
NsiNotifyChange(
    _In_ PNSI_PROVIDER Provider,
    _In_ NSI_CHANGE_TYPE Type,
    _In_reads_bytes_opt_(KeySize) const VOID *Key,
    _In_ ULONG KeySize)
{
    LONG Sequence;

    UNREFERENCED_PARAMETER(Type);
    UNREFERENCED_PARAMETER(Key);
    UNREFERENCED_PARAMETER(KeySize);

    if (!NsiReferenceRegisteredProvider(Provider))
        return;
    Sequence = InterlockedIncrement(&Provider->Sequence);
    NsiDrainPendingIrps(Provider, NULL, STATUS_SUCCESS, (ULONG_PTR)(ULONG)Sequence);
    NsiDereferenceProvider(Provider);
}

NTSTATUS
NTAPI
NsiRequestChangeNotificationKm(
    _In_ const NPI_MODULEID *Module,
    _In_ ULONG Table,
    _In_ PIRP Irp)
{
    PNSI_PROVIDER Provider = NsiReferenceProvider(Module, Table);

    if (Provider == NULL)
        return STATUS_NOT_FOUND;
    IoMarkIrpPending(Irp);
    IoCsqInsertIrp(&Provider->Csq, Irp, NULL);
    NsiDereferenceProvider(Provider);
    return STATUS_PENDING;
}

VOID
NTAPI
NsiCancelChangeNotificationsKm(
    _In_ PFILE_OBJECT FileObject)
{
    LIST_ENTRY Cancelled;
    PLIST_ENTRY Entry;
    PIRP Irp;
    KIRQL OldIrql;

    if (!NsiReady)
        return;
    InitializeListHead(&Cancelled);
    KeAcquireSpinLock(&NsiProviderLock, &OldIrql);
    for (Entry = NsiProviderList.Flink; Entry != &NsiProviderList; Entry = Entry->Flink)
    {
        PNSI_PROVIDER Provider = CONTAINING_RECORD(Entry, NSI_PROVIDER, ListEntry);

        while ((Irp = IoCsqRemoveNextIrp(&Provider->Csq, FileObject)) != NULL)
            InsertTailList(&Cancelled, &Irp->Tail.Overlay.ListEntry);
    }
    KeReleaseSpinLock(&NsiProviderLock, OldIrql);

    while (!IsListEmpty(&Cancelled))
    {
        Entry = RemoveHeadList(&Cancelled);
        Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
        Irp->IoStatus.Status = STATUS_CANCELLED;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }
}

NTSTATUS
NTAPI
NsiQueryTableSizesKm(
    _In_ const NPI_MODULEID *Module,
    _In_ ULONG Table,
    _Out_ PNSI_ROW_SIZES Sizes)
{
    PNSI_PROVIDER Provider = NsiReferenceProvider(Module, Table);

    RtlZeroMemory(Sizes, sizeof(*Sizes));
    if (Provider == NULL)
        return STATUS_NOT_FOUND;
    *Sizes = Provider->Dispatch.RowSizes;
    NsiDereferenceProvider(Provider);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
NsiEnumerateAllParametersKm(
    _In_ const NPI_MODULEID *Module,
    _In_ ULONG Table,
    _In_ const NSI_ROW_SIZES *Sizes,
    _Inout_ PNSI_ENUMERATE_REQUEST Request)
{
    PNSI_PROVIDER Provider = NsiReferenceProvider(Module, Table);
    NTSTATUS Status;

    if (Provider == NULL)
        return STATUS_NOT_FOUND;
    Status = NsiCheckSizes(Provider, Sizes);
    if (NT_SUCCESS(Status))
    {
        if (Provider->Dispatch.Enumerate == NULL)
            Status = STATUS_NOT_SUPPORTED;
        else
            Status = Provider->Dispatch.Enumerate(Provider->Context, Request);
    }
    NsiDereferenceProvider(Provider);
    return Status;
}

NTSTATUS
NTAPI
NsiGetAllParametersKm(
    _In_ const NPI_MODULEID *Module,
    _In_ ULONG Table,
    _In_ const NSI_ROW_SIZES *Sizes,
    _In_ ULONG FirstArg,
    _In_reads_bytes_(KeySize) const VOID *Key,
    _In_ ULONG KeySize,
    _Out_writes_bytes_opt_(Sizes->Rw) PVOID Rw,
    _Out_writes_bytes_opt_(Sizes->Dynamic) PVOID Dynamic,
    _Out_writes_bytes_opt_(Sizes->Static) PVOID Static)
{
    PNSI_PROVIDER Provider = NsiReferenceProvider(Module, Table);
    NTSTATUS Status;

    if (Provider == NULL)
        return STATUS_NOT_FOUND;
    Status = NsiCheckSizes(Provider, Sizes);
    if (NT_SUCCESS(Status) && KeySize != Provider->Dispatch.RowSizes.Key)
        Status = STATUS_INVALID_PARAMETER;
    if (NT_SUCCESS(Status))
    {
        Status = Provider->Dispatch.GetAll(Provider->Context, FirstArg, Key, KeySize,
                                           Sizes->Rw ? Rw : NULL,
                                           Sizes->Dynamic ? Dynamic : NULL,
                                           Sizes->Static ? Static : NULL);
    }
    NsiDereferenceProvider(Provider);
    return Status;
}

static NTSTATUS
NsiGetParameterFromRow(
    _In_ PNSI_PROVIDER Provider,
    _In_ ULONG FirstArg,
    _In_reads_bytes_(KeySize) const VOID *Key,
    _In_ ULONG KeySize,
    _In_ ULONG ParamType,
    _In_ ULONG DataOffset,
    _Out_writes_bytes_(DataSize) PVOID Data,
    _In_ ULONG DataSize)
{
    const NSI_ROW_SIZES *Own = &Provider->Dispatch.RowSizes;
    ULONG SectionSize;
    PVOID Row;
    NTSTATUS Status;

    switch (ParamType)
    {
        case NSI_PARAM_RW: SectionSize = Own->Rw; break;
        case NSI_PARAM_DYNAMIC: SectionSize = Own->Dynamic; break;
        case NSI_PARAM_STATIC: SectionSize = Own->Static; break;
        default: return STATUS_INVALID_PARAMETER;
    }
    if (SectionSize == 0 || DataOffset > SectionSize || DataSize > SectionSize - DataOffset)
        return STATUS_INVALID_PARAMETER;
    Row = ExAllocatePoolWithTag(NonPagedPool, SectionSize, TAG_NSI);
    if (Row == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(Row, SectionSize);
    Status = Provider->Dispatch.GetAll(Provider->Context, FirstArg, Key, KeySize,
                                       ParamType == NSI_PARAM_RW ? Row : NULL,
                                       ParamType == NSI_PARAM_DYNAMIC ? Row : NULL,
                                       ParamType == NSI_PARAM_STATIC ? Row : NULL);
    if (NT_SUCCESS(Status))
        RtlCopyMemory(Data, (PUCHAR)Row + DataOffset, DataSize);
    ExFreePoolWithTag(Row, TAG_NSI);
    return Status;
}

NTSTATUS
NTAPI
NsiGetParameterKm(
    _In_ const NPI_MODULEID *Module,
    _In_ ULONG Table,
    _In_ ULONG FirstArg,
    _In_reads_bytes_(KeySize) const VOID *Key,
    _In_ ULONG KeySize,
    _In_ ULONG ParamType,
    _In_ ULONG DataOffset,
    _Out_writes_bytes_(DataSize) PVOID Data,
    _In_ ULONG DataSize)
{
    PNSI_PROVIDER Provider = NsiReferenceProvider(Module, Table);
    NTSTATUS Status;

    if (Provider == NULL)
        return STATUS_NOT_FOUND;
    if (KeySize != Provider->Dispatch.RowSizes.Key)
    {
        Status = STATUS_INVALID_PARAMETER;
    }
    else if (Provider->Dispatch.GetParameter != NULL)
    {
        Status = Provider->Dispatch.GetParameter(Provider->Context, FirstArg, Key, KeySize,
                                                 ParamType, DataOffset, Data, DataSize);
    }
    else
    {
        Status = NsiGetParameterFromRow(Provider, FirstArg, Key, KeySize,
                                        ParamType, DataOffset, Data, DataSize);
    }
    NsiDereferenceProvider(Provider);
    return Status;
}
