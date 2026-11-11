/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Network store (NSI) provider and table access test, kernel half
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <kmt_test.h>
#include <netiodef.h>
#include <nsikm.h>

#include "nsi_store.h"

static const GUID StoreTestGuid = NSISTORE_MODULE_GUID;
static NPI_MODULEID TestModule;
static PNSI_PROVIDER MainProvider;
static KMT_MESSAGE_HANDLER TestMessageHandler;

static VOID
FillRow(
    _In_ ULONG Key,
    _Out_opt_ NSISTORE_RW *Rw,
    _Out_opt_ NSISTORE_DYNAMIC *Dynamic,
    _Out_opt_ NSISTORE_STATIC *Static)
{
    if (Rw != NULL)
    {
        Rw->A = NSISTORE_RW_A(Key);
        Rw->B = NSISTORE_RW_B(Key);
    }
    if (Dynamic != NULL)
    {
        Dynamic->Counter = NSISTORE_COUNTER(Key);
        Dynamic->X = NSISTORE_X(Key);
        Dynamic->Y = NSISTORE_Y(Key);
    }
    if (Static != NULL)
    {
        Static->S[0] = NSISTORE_S0(Key);
        Static->S[1] = NSISTORE_S1(Key);
    }
}

static NTSTATUS NTAPI
TestEnumerate(
    _In_opt_ PVOID Context,
    _Inout_ PNSI_ENUMERATE_REQUEST Request)
{
    ULONG Capacity = Request->Count;
    ULONG i;

    ok_eq_pointer(Context, &TestModule);
    for (i = 0; i < NSISTORE_ROWS && i < Capacity; i++)
    {
        ULONG Key = i + 1;

        if (Request->KeyData != NULL)
            ((PULONG)Request->KeyData)[i] = Key;
        FillRow(Key,
                Request->RwData ? (NSISTORE_RW *)Request->RwData + i : NULL,
                Request->DynamicData ? (NSISTORE_DYNAMIC *)Request->DynamicData + i : NULL,
                Request->StaticData ? (NSISTORE_STATIC *)Request->StaticData + i : NULL);
    }
    Request->Count = NSISTORE_ROWS;
    return Capacity >= NSISTORE_ROWS ? STATUS_SUCCESS : STATUS_BUFFER_OVERFLOW;
}

static NTSTATUS NTAPI
TestGetAll(
    _In_opt_ PVOID Context,
    _In_ ULONG FirstArg,
    _In_reads_bytes_(KeySize) const VOID *Key,
    _In_ ULONG KeySize,
    _Out_writes_bytes_opt_(sizeof(NSISTORE_RW)) PVOID Rw,
    _Out_writes_bytes_opt_(sizeof(NSISTORE_DYNAMIC)) PVOID Dynamic,
    _Out_writes_bytes_opt_(sizeof(NSISTORE_STATIC)) PVOID Static)
{
    ULONG Value;

    ok_eq_pointer(Context, &TestModule);
    ok_eq_ulong(FirstArg, 0UL);
    ok_eq_ulong(KeySize, (ULONG)sizeof(ULONG));
    if (KeySize != sizeof(ULONG))
        return STATUS_INVALID_PARAMETER;
    Value = *(const ULONG *)Key;
    if (Value == 0 || Value > NSISTORE_ROWS)
        return STATUS_NOT_FOUND;
    FillRow(Value, Rw, Dynamic, Static);
    return STATUS_SUCCESS;
}

static const NSI_PROVIDER_DISPATCH TestDispatch =
{
    sizeof(NSI_PROVIDER_DISPATCH),
    { sizeof(ULONG), sizeof(NSISTORE_RW), sizeof(NSISTORE_DYNAMIC), sizeof(NSISTORE_STATIC) },
    TestEnumerate,
    TestGetAll,
    NULL
};

static VOID
TestRegistration(VOID)
{
    PNSI_PROVIDER Provider = NULL;
    NSI_PROVIDER_DISPATCH Bad;
    NTSTATUS Status;

    Status = NsiRegisterProvider(&TestModule, NSISTORE_TABLE, &TestDispatch, &TestModule, &Provider);
    ok_eq_hex(Status, STATUS_OBJECT_NAME_COLLISION);
    ok_eq_pointer(Provider, NULL);

    Bad = TestDispatch;
    Bad.GetAll = NULL;
    Status = NsiRegisterProvider(&TestModule, NSISTORE_SCRATCH_TABLE, &Bad, &TestModule, &Provider);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);

    Bad = TestDispatch;
    Bad.RowSizes.Key = 0;
    Status = NsiRegisterProvider(&TestModule, NSISTORE_SCRATCH_TABLE, &Bad, &TestModule, &Provider);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);

    Status = NsiRegisterProvider(&TestModule, NSISTORE_SCRATCH_TABLE, &TestDispatch, &TestModule, &Provider);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        NSI_ROW_SIZES Sizes;

        Status = NsiQueryTableSizesKm(&TestModule, NSISTORE_SCRATCH_TABLE, &Sizes);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulong(Sizes.Dynamic, (ULONG)sizeof(NSISTORE_DYNAMIC));
        NsiDeregisterProvider(Provider);
        Status = NsiQueryTableSizesKm(&TestModule, NSISTORE_SCRATCH_TABLE, &Sizes);
        ok_eq_hex(Status, STATUS_NOT_FOUND);
        ok_eq_ulong(Sizes.Key, 0UL);
    }
}

static VOID
TestEnumeration(VOID)
{
    NSI_ROW_SIZES Sizes = { sizeof(ULONG), sizeof(NSISTORE_RW), sizeof(NSISTORE_DYNAMIC), sizeof(NSISTORE_STATIC) };
    NSI_ROW_SIZES Wrong = Sizes;
    NSI_ROW_SIZES Partial = { sizeof(ULONG), 0, sizeof(NSISTORE_DYNAMIC), 0 };
    NSI_ENUMERATE_REQUEST Request;
    ULONG Keys[4];
    NSISTORE_RW Rw[4];
    NSISTORE_DYNAMIC Dynamic[4];
    NSISTORE_STATIC Static[4];
    NTSTATUS Status;

    RtlZeroMemory(&Request, sizeof(Request));
    Status = NsiEnumerateAllParametersKm(&TestModule, NSISTORE_TABLE, &Sizes, &Request);
    ok_eq_hex(Status, STATUS_BUFFER_OVERFLOW);
    ok_eq_ulong(Request.Count, (ULONG)NSISTORE_ROWS);

    RtlZeroMemory(Keys, sizeof(Keys));
    RtlZeroMemory(Dynamic, sizeof(Dynamic));
    Request.KeyData = Keys;
    Request.DynamicData = Dynamic;
    Request.Count = 1;
    Status = NsiEnumerateAllParametersKm(&TestModule, NSISTORE_TABLE, &Partial, &Request);
    ok_eq_hex(Status, STATUS_BUFFER_OVERFLOW);
    ok_eq_ulong(Request.Count, (ULONG)NSISTORE_ROWS);
    ok_eq_ulong(Keys[0], 1UL);
    ok_eq_ulong(Keys[1], 0UL);
    ok_eq_ulong(Dynamic[0].X, (ULONG)NSISTORE_X(1));

    RtlZeroMemory(Keys, sizeof(Keys));
    Request.KeyData = Keys;
    Request.RwData = Rw;
    Request.DynamicData = Dynamic;
    Request.StaticData = Static;
    Request.Count = 4;
    Status = NsiEnumerateAllParametersKm(&TestModule, NSISTORE_TABLE, &Sizes, &Request);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(Request.Count, (ULONG)NSISTORE_ROWS);
    ok_eq_ulong(Keys[0], 1UL);
    ok_eq_ulong(Keys[1], 2UL);
    ok_eq_ulong(Rw[1].B, (ULONG)NSISTORE_RW_B(2));
    ok_eq_ulonglong(Dynamic[1].Counter, NSISTORE_COUNTER(2));
    ok_eq_ulong(Static[0].S[1], (ULONG)NSISTORE_S1(1));

    Wrong.Rw = sizeof(NSISTORE_RW) + 4;
    Request.Count = 4;
    Status = NsiEnumerateAllParametersKm(&TestModule, NSISTORE_TABLE, &Wrong, &Request);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);

    Request.Count = 4;
    Status = NsiEnumerateAllParametersKm(&TestModule, NSISTORE_SCRATCH_TABLE, &Sizes, &Request);
    ok_eq_hex(Status, STATUS_NOT_FOUND);

    Status = NsiEnumerateAllParametersKm(&NPI_MS_NDIS_MODULEID, NSISTORE_TABLE, &Sizes, &Request);
    ok(Status == STATUS_NOT_FOUND || Status == STATUS_INVALID_PARAMETER,
       "enumerate of a foreign module with our sizes returned 0x%08lx\n", Status);
}

static VOID
TestRows(VOID)
{
    NSI_ROW_SIZES Sizes = { sizeof(ULONG), sizeof(NSISTORE_RW), sizeof(NSISTORE_DYNAMIC), sizeof(NSISTORE_STATIC) };
    NSI_ROW_SIZES Partial = { sizeof(ULONG), sizeof(NSISTORE_RW), 0, 0 };
    NSISTORE_RW Rw;
    NSISTORE_DYNAMIC Dynamic;
    NSISTORE_STATIC Static;
    ULONG Key, Value;
    USHORT ShortKey = 1;
    NTSTATUS Status;

    Key = 2;
    RtlZeroMemory(&Rw, sizeof(Rw));
    RtlZeroMemory(&Dynamic, sizeof(Dynamic));
    RtlZeroMemory(&Static, sizeof(Static));
    Status = NsiGetAllParametersKm(&TestModule, NSISTORE_TABLE, &Sizes, 0, &Key, sizeof(Key), &Rw, &Dynamic, &Static);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(Rw.A, (ULONG)NSISTORE_RW_A(2));
    ok_eq_ulong(Dynamic.Y, (ULONG)NSISTORE_Y(2));
    ok_eq_ulong(Static.S[0], (ULONG)NSISTORE_S0(2));

    Key = 1;
    RtlZeroMemory(&Rw, sizeof(Rw));
    Dynamic.X = 0xFFFFFFFF;
    Status = NsiGetAllParametersKm(&TestModule, NSISTORE_TABLE, &Partial, 0, &Key, sizeof(Key), &Rw, &Dynamic, &Static);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(Rw.A, (ULONG)NSISTORE_RW_A(1));
    ok_eq_ulong(Dynamic.X, 0xFFFFFFFFUL);

    Key = 3;
    Status = NsiGetAllParametersKm(&TestModule, NSISTORE_TABLE, &Sizes, 0, &Key, sizeof(Key), &Rw, &Dynamic, &Static);
    ok_eq_hex(Status, STATUS_NOT_FOUND);

    Status = NsiGetAllParametersKm(&TestModule, NSISTORE_TABLE, &Sizes, 0, &ShortKey, sizeof(ShortKey), &Rw, &Dynamic, &Static);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);

    Key = 1;
    Value = 0;
    Status = NsiGetParameterKm(&TestModule, NSISTORE_TABLE, 0, &Key, sizeof(Key), NSI_PARAM_DYNAMIC,
                               FIELD_OFFSET(NSISTORE_DYNAMIC, X), &Value, sizeof(Value));
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(Value, (ULONG)NSISTORE_X(1));

    Key = 2;
    Value = 0;
    Status = NsiGetParameterKm(&TestModule, NSISTORE_TABLE, 0, &Key, sizeof(Key), NSI_PARAM_STATIC,
                               FIELD_OFFSET(NSISTORE_STATIC, S[1]), &Value, sizeof(Value));
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(Value, (ULONG)NSISTORE_S1(2));

    Status = NsiGetParameterKm(&TestModule, NSISTORE_TABLE, 0, &Key, sizeof(Key), NSI_PARAM_RW,
                               sizeof(NSISTORE_RW), &Value, sizeof(Value));
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);

    Status = NsiGetParameterKm(&TestModule, NSISTORE_TABLE, 0, &Key, sizeof(Key), NSI_PARAM_STATIC + 1,
                               0, &Value, sizeof(Value));
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);

    Key = 3;
    Status = NsiGetParameterKm(&TestModule, NSISTORE_TABLE, 0, &Key, sizeof(Key), NSI_PARAM_RW, 0, &Value, sizeof(Value));
    ok_eq_hex(Status, STATUS_NOT_FOUND);
}

static NTSTATUS NTAPI
NotifyComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);
    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static PIRP
RequestChange(
    _In_ const NPI_MODULEID *Module,
    _In_ ULONG Table,
    _Out_ PKEVENT Event,
    _In_opt_ PFILE_OBJECT FileObject,
    _Out_ NTSTATUS *Status)
{
    PIRP Irp = IoAllocateIrp(1, FALSE);

    KeInitializeEvent(Event, NotificationEvent, FALSE);
    if (Irp == NULL)
    {
        *Status = STATUS_INSUFFICIENT_RESOURCES;
        return NULL;
    }
    IoSetCompletionRoutine(Irp, NotifyComplete, Event, TRUE, TRUE, TRUE);
    IoSetNextIrpStackLocation(Irp);
    IoGetCurrentIrpStackLocation(Irp)->FileObject = FileObject;
    *Status = NsiRequestChangeNotificationKm(Module, Table, Irp);
    return Irp;
}

static BOOLEAN
WaitCompletion(
    _In_ PKEVENT Event)
{
    LARGE_INTEGER Timeout;

    Timeout.QuadPart = -5LL * 10000000;
    return KeWaitForSingleObject(Event, Executive, KernelMode, FALSE, &Timeout) == STATUS_SUCCESS;
}

static VOID
TestNotifications(VOID)
{
    KEVENT EventA, EventB, EventC, EventD;
    PIRP IrpA, IrpB, IrpC, IrpD;
    FILE_OBJECT FileA, FileB;
    PNSI_PROVIDER Scratch = NULL;
    ULONG Key = 1, FirstSequence;
    NTSTATUS Status;

    RtlZeroMemory(&FileA, sizeof(FileA));
    RtlZeroMemory(&FileB, sizeof(FileB));

    IrpA = RequestChange(&TestModule, NSISTORE_TABLE, &EventA, &FileA, &Status);
    ok_eq_hex(Status, STATUS_PENDING);
    if (IrpA == NULL)
        return;
    ok(KeReadStateEvent(&EventA) == 0, "request completed before any change\n");
    NsiNotifyChange(MainProvider, NsiChangeParameter, &Key, sizeof(Key));
    ok(WaitCompletion(&EventA), "request not completed by a change\n");
    ok_eq_hex(IrpA->IoStatus.Status, STATUS_SUCCESS);
    FirstSequence = (ULONG)IrpA->IoStatus.Information;
    ok(FirstSequence != 0, "sequence is 0 after a change\n");
    IoFreeIrp(IrpA);

    IrpA = RequestChange(&TestModule, NSISTORE_TABLE, &EventA, &FileA, &Status);
    ok_eq_hex(Status, STATUS_PENDING);
    if (IrpA == NULL)
        return;
    NsiNotifyChange(MainProvider, NsiChangeAdd, NULL, 0);
    ok(WaitCompletion(&EventA), "second request not completed by a change\n");
    ok_eq_ulong((ULONG)IrpA->IoStatus.Information, FirstSequence + 1);
    IoFreeIrp(IrpA);

    IrpB = RequestChange(&TestModule, NSISTORE_TABLE, &EventB, &FileA, &Status);
    ok_eq_hex(Status, STATUS_PENDING);
    if (IrpB == NULL)
        return;
    ok(IoCancelIrp(IrpB), "IoCancelIrp found no cancel routine\n");
    ok(WaitCompletion(&EventB), "cancelled request not completed\n");
    ok_eq_hex(IrpB->IoStatus.Status, STATUS_CANCELLED);
    IoFreeIrp(IrpB);

    IrpC = RequestChange(&TestModule, NSISTORE_TABLE, &EventC, &FileA, &Status);
    ok_eq_hex(Status, STATUS_PENDING);
    IrpD = RequestChange(&TestModule, NSISTORE_TABLE, &EventD, &FileB, &Status);
    ok_eq_hex(Status, STATUS_PENDING);
    if (IrpC == NULL || IrpD == NULL)
    {
        NsiCancelChangeNotificationsKm(&FileA);
        NsiCancelChangeNotificationsKm(&FileB);
        if (IrpC != NULL)
            IoFreeIrp(IrpC);
        if (IrpD != NULL)
            IoFreeIrp(IrpD);
        return;
    }
    NsiCancelChangeNotificationsKm(&FileA);
    ok(WaitCompletion(&EventC), "request of the closed file object not completed\n");
    ok_eq_hex(IrpC->IoStatus.Status, STATUS_CANCELLED);
    ok(KeReadStateEvent(&EventD) == 0, "request of another file object was cancelled too\n");
    NsiNotifyChange(MainProvider, NsiChangeDelete, &Key, sizeof(Key));
    ok(WaitCompletion(&EventD), "surviving request not completed by a change\n");
    ok_eq_hex(IrpD->IoStatus.Status, STATUS_SUCCESS);
    ok_eq_ulong((ULONG)IrpD->IoStatus.Information, FirstSequence + 2);
    IoFreeIrp(IrpC);
    IoFreeIrp(IrpD);

    Status = NsiRegisterProvider(&TestModule, NSISTORE_SCRATCH_TABLE, &TestDispatch, &TestModule, &Scratch);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (Scratch != NULL)
    {
        IrpA = RequestChange(&TestModule, NSISTORE_SCRATCH_TABLE, &EventA, &FileA, &Status);
        ok_eq_hex(Status, STATUS_PENDING);
        NsiDeregisterProvider(Scratch);
        if (IrpA != NULL)
        {
            ok(WaitCompletion(&EventA), "request not completed by deregistration\n");
            ok_eq_hex(IrpA->IoStatus.Status, STATUS_CANCELLED);
            IoFreeIrp(IrpA);
        }
    }

    IrpA = RequestChange(&TestModule, NSISTORE_SCRATCH_TABLE, &EventA, &FileA, &Status);
    ok_eq_hex(Status, STATUS_NOT_FOUND);
    if (IrpA != NULL)
    {
        ok(KeReadStateEvent(&EventA) == 0, "failed request was completed\n");
        IoFreeIrp(IrpA);
    }
}

typedef struct _NOTIFY_THREAD
{
    PNSI_PROVIDER Provider;
    volatile LONG Stop;
    volatile LONG Started;
    LONG Iterations;
    KEVENT Done;
} NOTIFY_THREAD, *PNOTIFY_THREAD;

static VOID NTAPI
NotifyThread(
    _In_ PVOID Parameter)
{
    PNOTIFY_THREAD Thread = Parameter;
    ULONG Key = 1;

    InterlockedExchange(&Thread->Started, 1);
    do
    {
        NsiNotifyChange(Thread->Provider, NsiChangeParameter, &Key, sizeof(Key));
        Thread->Iterations++;
    } while (!Thread->Stop);
    KeSetEvent(&Thread->Done, IO_NO_INCREMENT, FALSE);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static BOOLEAN
StartNotifyThread(
    _In_ PNOTIFY_THREAD Thread,
    _In_ PNSI_PROVIDER Provider,
    _In_ BOOLEAN Once,
    _Out_ PHANDLE Handle)
{
    OBJECT_ATTRIBUTES Attributes;
    NTSTATUS Status;

    RtlZeroMemory(Thread, sizeof(*Thread));
    Thread->Provider = Provider;
    Thread->Stop = Once;
    KeInitializeEvent(&Thread->Done, NotificationEvent, FALSE);
    InitializeObjectAttributes(&Attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = PsCreateSystemThread(Handle, THREAD_ALL_ACCESS, &Attributes, NULL, NULL, NotifyThread, Thread);
    ok_eq_hex(Status, STATUS_SUCCESS);
    return NT_SUCCESS(Status);
}

static VOID
JoinNotifyThread(
    _In_ PNOTIFY_THREAD Thread,
    _In_ HANDLE Handle)
{
    PVOID Object;

    ok(WaitCompletion(&Thread->Done), "notify thread did not finish\n");
    if (NT_SUCCESS(ObReferenceObjectByHandle(Handle, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, &Object, NULL)))
    {
        KeWaitForSingleObject(Object, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(Object);
    }
    ZwClose(Handle);
}

static VOID
TestNotifyRaces(VOID)
{
    NOTIFY_THREAD Thread;
    HANDLE Handle;
    KEVENT Event;
    PIRP Irp;
    FILE_OBJECT File;
    PNSI_PROVIDER Scratch = NULL;
    LARGE_INTEGER Delay;
    NTSTATUS Status;

    RtlZeroMemory(&File, sizeof(File));

    Irp = RequestChange(&TestModule, NSISTORE_TABLE, &Event, &File, &Status);
    ok_eq_hex(Status, STATUS_PENDING);
    if (Irp == NULL)
        return;
    if (StartNotifyThread(&Thread, MainProvider, TRUE, &Handle))
    {
        ok(WaitCompletion(&Event), "request not completed by a change from another thread\n");
        ok_eq_hex(Irp->IoStatus.Status, STATUS_SUCCESS);
        ok(Irp->IoStatus.Information != 0, "sequence is 0 after a change from another thread\n");
        JoinNotifyThread(&Thread, Handle);
    }
    else
    {
        IoCancelIrp(Irp);
        WaitCompletion(&Event);
    }
    IoFreeIrp(Irp);

    Status = NsiRegisterProvider(&TestModule, NSISTORE_SCRATCH_TABLE, &TestDispatch, &TestModule, &Scratch);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (Scratch == NULL)
        return;
    if (!StartNotifyThread(&Thread, Scratch, FALSE, &Handle))
    {
        NsiDeregisterProvider(Scratch);
        return;
    }
    while (!Thread.Started)
    {
        Delay.QuadPart = -10000;
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }
    Delay.QuadPart = -20 * 10000;
    KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    NsiDeregisterProvider(Scratch);
    InterlockedExchange(&Thread.Stop, 1);
    JoinNotifyThread(&Thread, Handle);
    ok(Thread.Iterations > 0, "racing thread never signalled a change\n");
    trace("%ld changes signalled while the provider was deregistered\n", Thread.Iterations);
}

NTSTATUS
TestEntry(
    IN PDRIVER_OBJECT DriverObject,
    IN PCUNICODE_STRING RegistryPath,
    OUT PCWSTR *DeviceName,
    IN OUT INT *Flags)
{
    NTSTATUS Status;

    PAGED_CODE();

    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);
    UNREFERENCED_PARAMETER(Flags);

    *DeviceName = L"NsiStore";

    TestModule.Length = sizeof(TestModule);
    TestModule.Type = MIT_GUID;
    TestModule.Guid = StoreTestGuid;
    Status = NsiRegisterProvider(&TestModule, NSISTORE_TABLE, &TestDispatch, &TestModule, &MainProvider);
    ok_eq_hex(Status, STATUS_SUCCESS);

    KmtRegisterMessageHandler(IOCTL_NSISTORE_RUN, NULL, TestMessageHandler);
    KmtRegisterMessageHandler(IOCTL_NSISTORE_NOTIFY, NULL, TestMessageHandler);
    return STATUS_SUCCESS;
}

VOID
TestUnload(
    IN PDRIVER_OBJECT DriverObject)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(DriverObject);

    KmtUnregisterMessageHandler(IOCTL_NSISTORE_RUN, NULL, TestMessageHandler);
    KmtUnregisterMessageHandler(IOCTL_NSISTORE_NOTIFY, NULL, TestMessageHandler);
    if (MainProvider != NULL)
    {
        NsiDeregisterProvider(MainProvider);
        MainProvider = NULL;
    }
}

static NTSTATUS
TestMessageHandler(
    IN PDEVICE_OBJECT DeviceObject,
    IN ULONG ControlCode,
    IN PVOID Buffer OPTIONAL,
    IN SIZE_T InLength,
    IN OUT PSIZE_T OutLength)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(InLength);
    UNREFERENCED_PARAMETER(OutLength);

    ok_irql(PASSIVE_LEVEL);
    if (skip(MainProvider != NULL, "provider registration failed at load\n"))
        return STATUS_SUCCESS;
    if (ControlCode == IOCTL_NSISTORE_NOTIFY)
    {
        ULONG Key = 2;

        NsiNotifyChange(MainProvider, NsiChangeParameter, &Key, sizeof(Key));
        return STATUS_SUCCESS;
    }
    ok_eq_ulong(ControlCode, (ULONG)IOCTL_NSISTORE_RUN);
    TestRegistration();
    TestEnumeration();
    TestRows();
    TestNotifications();
    TestNotifyRaces();
    return STATUS_SUCCESS;
}
