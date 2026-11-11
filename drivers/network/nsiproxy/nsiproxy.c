/*
 * PROJECT:     LiberNT Network Store Interface proxy driver
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Serves the \Device\Nsi ioctls of nsi.dll from the netio.sys network store
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "nsiproxy.h"

static PDEVICE_OBJECT NsiProxyDevice;
static UNICODE_STRING NsiProxyLinkName = RTL_CONSTANT_STRING(L"\\DosDevices\\Nsi");
static const UNICODE_STRING NsiProxySecurity = RTL_CONSTANT_STRING(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;AU)");

static NTSTATUS
NsiProxyComplete(
    _In_ PIRP Irp,
    _In_ NTSTATUS Status,
    _In_ ULONG_PTR Information)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static NTSTATUS NTAPI
NsiProxyCreateClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    return NsiProxyComplete(Irp, STATUS_SUCCESS, 0);
}

static NTSTATUS NTAPI
NsiProxyCleanup(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    NsiCancelChangeNotificationsKm(IoGetCurrentIrpStackLocation(Irp)->FileObject);
    return NsiProxyComplete(Irp, STATUS_SUCCESS, 0);
}

static NTSTATUS
NsiProxyEnumerateAll(
    _In_ PIRP Irp,
    _In_ ULONG InputLength,
    _In_ ULONG OutputLength,
    _Out_ PULONG_PTR Information)
{
    struct nsiproxy_enumerate_all In;
    NSI_ROW_SIZES Sizes, Registered;
    NSI_ENUMERATE_REQUEST Request;
    PUCHAR Out = Irp->AssociatedIrp.SystemBuffer;
    ULONG RowSize, Needed;
    NTSTATUS Status;

    *Information = 0;
    if (InputLength < sizeof(In))
        return STATUS_INVALID_PARAMETER;
    RtlCopyMemory(&In, Out, sizeof(In));
    Sizes.Key = In.key_size;
    Sizes.Rw = In.rw_size;
    Sizes.Dynamic = In.dynamic_size;
    Sizes.Static = In.static_size;
    Status = NsiQueryTableSizesKm(&In.module, In.table, &Registered);
    if (!NT_SUCCESS(Status))
        return Status;
    if ((In.key_size != 0 && In.key_size != Registered.Key) ||
        (In.rw_size != 0 && In.rw_size != Registered.Rw) ||
        (In.dynamic_size != 0 && In.dynamic_size != Registered.Dynamic) ||
        (In.static_size != 0 && In.static_size != Registered.Static))
    {
        return STATUS_INVALID_PARAMETER;
    }
    RowSize = In.key_size + In.rw_size + In.dynamic_size + In.static_size;
    if (RowSize < In.key_size || RowSize < In.rw_size || RowSize < In.dynamic_size || RowSize < In.static_size ||
        (RowSize != 0 && In.count > (MAXULONG - sizeof(ULONG)) / RowSize))
    {
        return STATUS_INVALID_PARAMETER;
    }
    Needed = sizeof(ULONG) + In.count * RowSize;
    if (OutputLength < Needed)
        return STATUS_BUFFER_TOO_SMALL;
    RtlZeroMemory(Out, Needed);

    RtlZeroMemory(&Request, sizeof(Request));
    Request.FirstArg = In.first_arg;
    Request.SecondArg = In.second_arg;
    Request.Count = In.count;
    if (In.count != 0)
    {
        PUCHAR Section = Out + sizeof(ULONG);

        if (In.key_size != 0)
            Request.KeyData = Section;
        Section += In.key_size * In.count;
        if (In.rw_size != 0)
            Request.RwData = Section;
        Section += In.rw_size * In.count;
        if (In.dynamic_size != 0)
            Request.DynamicData = Section;
        Section += In.dynamic_size * In.count;
        if (In.static_size != 0)
            Request.StaticData = Section;
    }
    Status = NsiEnumerateAllParametersKm(&In.module, In.table, &Sizes, &Request);
    if (Status == STATUS_SUCCESS || Status == STATUS_BUFFER_OVERFLOW)
    {
        *(PULONG)Out = Request.Count;
        *Information = Needed;
    }
    return Status;
}

static NTSTATUS
NsiProxyCopyKeyedInput(
    _In_ PIRP Irp,
    _In_ ULONG InputLength,
    _In_ ULONG HeaderSize,
    _In_ ULONG KeyOffset,
    _In_ ULONG KeySize,
    _Outptr_ PVOID *Copy)
{
    PVOID Buffer;

    *Copy = NULL;
    if (InputLength < HeaderSize || KeySize > NSIPROXY_MAX_KEY_SIZE || InputLength - KeyOffset < KeySize)
        return STATUS_INVALID_PARAMETER;
    Buffer = ExAllocatePoolWithTag(NonPagedPool, KeyOffset + KeySize, TAG_NSIPROXY);
    if (Buffer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlCopyMemory(Buffer, Irp->AssociatedIrp.SystemBuffer, KeyOffset + KeySize);
    *Copy = Buffer;
    return STATUS_SUCCESS;
}

static NTSTATUS
NsiProxyGetAllParameters(
    _In_ PIRP Irp,
    _In_ ULONG InputLength,
    _In_ ULONG OutputLength,
    _Out_ PULONG_PTR Information)
{
    const ULONG HeaderSize = FIELD_OFFSET(struct nsiproxy_get_all_parameters, key);
    struct nsiproxy_get_all_parameters *In;
    NSI_ROW_SIZES Sizes;
    PUCHAR Out = Irp->AssociatedIrp.SystemBuffer;
    ULONG Needed, KeySize;
    NTSTATUS Status;

    *Information = 0;
    if (InputLength < HeaderSize)
        return STATUS_INVALID_PARAMETER;
    KeySize = ((struct nsiproxy_get_all_parameters *)Out)->key_size;
    Status = NsiProxyCopyKeyedInput(Irp, InputLength, HeaderSize, HeaderSize, KeySize, (PVOID *)&In);
    if (!NT_SUCCESS(Status))
        return Status;
    Sizes.Key = In->key_size;
    Sizes.Rw = In->rw_size;
    Sizes.Dynamic = In->dynamic_size;
    Sizes.Static = In->static_size;
    Needed = In->rw_size + In->dynamic_size + In->static_size;
    if (Needed < In->rw_size || Needed < In->dynamic_size || Needed < In->static_size)
        Status = STATUS_INVALID_PARAMETER;
    else if (OutputLength < Needed)
        Status = STATUS_BUFFER_TOO_SMALL;
    if (NT_SUCCESS(Status))
    {
        RtlZeroMemory(Out, Needed);
        Status = NsiGetAllParametersKm(&In->module, In->table, &Sizes, In->first_arg, In->key, In->key_size,
                                       In->rw_size ? Out : NULL,
                                       In->dynamic_size ? Out + In->rw_size : NULL,
                                       In->static_size ? Out + In->rw_size + In->dynamic_size : NULL);
        if (NT_SUCCESS(Status))
            *Information = Needed;
    }
    ExFreePoolWithTag(In, TAG_NSIPROXY);
    return Status;
}

static NTSTATUS
NsiProxyGetParameter(
    _In_ PIRP Irp,
    _In_ ULONG InputLength,
    _In_ ULONG OutputLength,
    _Out_ PULONG_PTR Information)
{
    const ULONG HeaderSize = FIELD_OFFSET(struct nsiproxy_get_parameter, key);
    struct nsiproxy_get_parameter *In;
    PUCHAR Out = Irp->AssociatedIrp.SystemBuffer;
    NSI_ROW_SIZES Sizes;
    ULONG KeySize, SectionSize = 0, DataSize = OutputLength;
    NTSTATUS Status;

    *Information = 0;
    if (InputLength < HeaderSize)
        return STATUS_INVALID_PARAMETER;
    KeySize = ((struct nsiproxy_get_parameter *)Out)->key_size;
    Status = NsiProxyCopyKeyedInput(Irp, InputLength, HeaderSize, HeaderSize, KeySize, (PVOID *)&In);
    if (!NT_SUCCESS(Status))
        return Status;
    if (NT_SUCCESS(NsiQueryTableSizesKm(&In->module, In->table, &Sizes)))
    {
        switch (In->param_type)
        {
            case NSI_PARAM_RW: SectionSize = Sizes.Rw; break;
            case NSI_PARAM_DYNAMIC: SectionSize = Sizes.Dynamic; break;
            case NSI_PARAM_STATIC: SectionSize = Sizes.Static; break;
        }
        if (In->param_type > NSI_PARAM_STATIC || In->data_offset >= SectionSize)
        {
            ExFreePoolWithTag(In, TAG_NSIPROXY);
            return STATUS_INVALID_PARAMETER;
        }
        if (DataSize > SectionSize - In->data_offset)
            DataSize = SectionSize - In->data_offset;
    }
    RtlZeroMemory(Out, OutputLength);
    Status = NsiGetParameterKm(&In->module, In->table, In->first_arg, In->key, In->key_size,
                               In->param_type, In->data_offset, Out, DataSize);
    if (NT_SUCCESS(Status))
        *Information = DataSize;
    ExFreePoolWithTag(In, TAG_NSIPROXY);
    return Status;
}

static NTSTATUS
NsiProxyChangeNotification(
    _In_ PIRP Irp,
    _In_ ULONG InputLength)
{
    struct nsiproxy_request_notification In;

    if (InputLength < sizeof(In))
        return STATUS_INVALID_PARAMETER;
    RtlCopyMemory(&In, Irp->AssociatedIrp.SystemBuffer, sizeof(In));
    return NsiRequestChangeNotificationKm(&In.module, In.table, Irp);
}

static NTSTATUS NTAPI
NsiProxyDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG InputLength = Stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG OutputLength = Stack->Parameters.DeviceIoControl.OutputBufferLength;
    ULONG_PTR Information = 0;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(DeviceObject);

    switch (Stack->Parameters.DeviceIoControl.IoControlCode)
    {
        case IOCTL_NSIPROXY_WINE_ENUMERATE_ALL:
            Status = NsiProxyEnumerateAll(Irp, InputLength, OutputLength, &Information);
            break;
        case IOCTL_NSIPROXY_WINE_GET_ALL_PARAMETERS:
            Status = NsiProxyGetAllParameters(Irp, InputLength, OutputLength, &Information);
            break;
        case IOCTL_NSIPROXY_WINE_GET_PARAMETER:
            Status = NsiProxyGetParameter(Irp, InputLength, OutputLength, &Information);
            break;
        case IOCTL_NSIPROXY_WINE_CHANGE_NOTIFICATION:
            Status = NsiProxyChangeNotification(Irp, InputLength);
            if (Status == STATUS_PENDING)
                return Status;
            break;
        case IOCTL_NSIPROXY_WINE_ICMP_ECHO:
            Status = STATUS_NOT_SUPPORTED;
            break;
        default:
            Status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }
    return NsiProxyComplete(Irp, Status, Information);
}

static VOID NTAPI
NsiProxyUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    IoDeleteSymbolicLink(&NsiProxyLinkName);
    if (NsiProxyDevice != NULL)
    {
        IoDeleteDevice(NsiProxyDevice);
        NsiProxyDevice = NULL;
    }
}

NTSTATUS NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING DeviceName = RTL_CONSTANT_STRING(L"\\Device\\Nsi");
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(RegistryPath);

    Status = IoCreateDeviceSecure(DriverObject, 0, &DeviceName, FILE_DEVICE_NETWORK,
                                  FILE_DEVICE_SECURE_OPEN, FALSE, &NsiProxySecurity, NULL, &NsiProxyDevice);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = IoCreateSymbolicLink(&NsiProxyLinkName, &DeviceName);
    if (!NT_SUCCESS(Status))
    {
        IoDeleteDevice(NsiProxyDevice);
        NsiProxyDevice = NULL;
        return Status;
    }
    DriverObject->MajorFunction[IRP_MJ_CREATE] = NsiProxyCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = NsiProxyCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = NsiProxyCleanup;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = NsiProxyDeviceControl;
    DriverObject->DriverUnload = NsiProxyUnload;
    NsiProxyDevice->Flags |= DO_BUFFERED_IO;
    NsiProxyDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}
