/*
 * PROJECT:     LiberNT WDDM 1.x synthetic test miniport
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     S0/M2 test vehicle: exercises dxgkrnl version negotiation,
 *              the legacy interface contract, modesetting on the QEMU
 *              bochs-display framebuffer, and init/teardown with self-owned
 *              code only (no vendor driver content).
 * COPYRIGHT:   Copyright 2026 Pam (pam-mvzy3ngz)
 */

#include <ntddk.h>
#include <dispmprt.h>
#include <d3dkmddi.h>

#define LT1X_TAG 'X1TL'
#define LT1X_FB_MAP_SIZE (4 * 1024 * 1024)
#define LT1X_CHILD_UID 0

static ULONG Lt1xDdiVersion = DXGKDDI_INTERFACE_VERSION_VISTA;
static ULONG Lt1xPoisonTail = 0;
static ULONG Lt1xChildCount = 1;
static ULONG Lt1xFailStart = 0;
static ULONG Lt1xFailCommit = 0;

typedef struct _LT1X_CONTEXT
{
    PDEVICE_OBJECT Pdo;
    HANDLE DeviceHandle;
    PHYSICAL_ADDRESS FrameBufferPhys;
    PVOID FrameBufferVa;
    ULONG ModeWidth;
    ULONG ModeHeight;
    ULONG ModeBpp;
    ULONG Stride;
    BOOLEAN DispiDetected;
    DXGK_INTERFACE DxgkInterface;
} LT1X_CONTEXT, *PLT1X_CONTEXT;

static
NTSTATUS
Lt1xReadConfig(_In_ PUNICODE_STRING RegistryPath)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING ValueName;
    UCHAR Buffer[FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION Info = (PKEY_VALUE_PARTIAL_INFORMATION)Buffer;
    HANDLE KeyHandle;
    NTSTATUS Status;
    ULONG ResultLength;

    InitializeObjectAttributes(&ObjectAttributes, RegistryPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwOpenKey(&KeyHandle, KEY_READ, &ObjectAttributes);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlInitUnicodeString(&ValueName, L"Parameters");
    {
        OBJECT_ATTRIBUTES SubAttributes;
        HANDLE SubKey = NULL;

        InitializeObjectAttributes(&SubAttributes, &ValueName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, KeyHandle, NULL);
        if (NT_SUCCESS(ZwOpenKey(&SubKey, KEY_READ, &SubAttributes)))
        {
            ZwClose(KeyHandle);
            KeyHandle = SubKey;
        }
    }

    RtlInitUnicodeString(&ValueName, L"DdiVersion");
    Status = ZwQueryValueKey(KeyHandle, &ValueName, KeyValuePartialInformation, Info, sizeof(Buffer), &ResultLength);
    if (NT_SUCCESS(Status) && Info->Type == REG_DWORD && Info->DataLength == sizeof(ULONG))
        Lt1xDdiVersion = *(PULONG)Info->Data;

    RtlInitUnicodeString(&ValueName, L"PoisonTail");
    Status = ZwQueryValueKey(KeyHandle, &ValueName, KeyValuePartialInformation, Info, sizeof(Buffer), &ResultLength);
    if (NT_SUCCESS(Status) && Info->Type == REG_DWORD && Info->DataLength == sizeof(ULONG))
        Lt1xPoisonTail = *(PULONG)Info->Data;

    RtlInitUnicodeString(&ValueName, L"ChildCount");
    Status = ZwQueryValueKey(KeyHandle, &ValueName, KeyValuePartialInformation, Info, sizeof(Buffer), &ResultLength);
    if (NT_SUCCESS(Status) && Info->Type == REG_DWORD && Info->DataLength == sizeof(ULONG))
        Lt1xChildCount = *(PULONG)Info->Data;

    RtlInitUnicodeString(&ValueName, L"FailStart");
    Status = ZwQueryValueKey(KeyHandle, &ValueName, KeyValuePartialInformation, Info, sizeof(Buffer), &ResultLength);
    if (NT_SUCCESS(Status) && Info->Type == REG_DWORD && Info->DataLength == sizeof(ULONG))
        Lt1xFailStart = *(PULONG)Info->Data;

    RtlInitUnicodeString(&ValueName, L"FailCommit");
    Status = ZwQueryValueKey(KeyHandle, &ValueName, KeyValuePartialInformation, Info, sizeof(Buffer), &ResultLength);
    if (NT_SUCCESS(Status) && Info->Type == REG_DWORD && Info->DataLength == sizeof(ULONG))
        Lt1xFailCommit = *(PULONG)Info->Data;

    ZwClose(KeyHandle);
    return STATUS_SUCCESS;
}

static
VOID
Lt1xProbeCurrentMode(_In_ PLT1X_CONTEXT Ctx)
{
    volatile USHORT *Dispi;
    ULONG Width, Height, Bpp, VirtWidth, Stride;

    Ctx->ModeWidth = 1024;
    Ctx->ModeHeight = 768;
    Ctx->ModeBpp = 32;
    Ctx->Stride = 1024 * 4;
    Ctx->DispiDetected = FALSE;

    Dispi = (volatile USHORT *)((PUCHAR)Ctx->FrameBufferVa + 0x400);
    if ((Dispi[0] & 0xFFF0) != 0xB0C0)
    {
        DbgPrint("LT1X: no dispi signature (0x%04X), defaulting to 1024x768x32\n", Dispi[0]);
        return;
    }

    Width = Dispi[1];
    Height = Dispi[2];
    Bpp = Dispi[3];
    VirtWidth = Dispi[6];
    if (Width < 640 || Width > 4096 || Height < 480 || Height > 4096 ||
        (Bpp != 16 && Bpp != 24 && Bpp != 32))
    {
        DbgPrint("LT1X: dispi mode %lux%lux%lu rejected, defaulting to 1024x768x32\n",
                 Width, Height, Bpp);
        return;
    }

    Stride = VirtWidth != 0 ? VirtWidth * Bpp / 8 : Width * Bpp / 8;
    if (Height * Stride > LT1X_FB_MAP_SIZE)
    {
        DbgPrint("LT1X: dispi mode %lux%lux%lu stride %lu does not fit the mapped window\n",
                 Width, Height, Bpp, Stride);
        return;
    }

    Ctx->ModeWidth = Width;
    Ctx->ModeHeight = Height;
    Ctx->ModeBpp = Bpp;
    Ctx->Stride = Stride;
    Ctx->DispiDetected = TRUE;
    DbgPrint("LT1X: dispi mode %lux%lux%lu stride %lu\n", Width, Height, Bpp, Stride);
}

static
VOID
Lt1xFillSourceMode(_In_ PLT1X_CONTEXT Ctx, _Out_ D3DKMDT_VIDPN_SOURCE_MODE *Mode)
{
    RtlZeroMemory(Mode, sizeof(*Mode));
    Mode->Id = 0;
    Mode->Type = D3DKMDT_RMT_GRAPHICS;
    Mode->Format.Graphics.PrimSurfSize.cx = Ctx->ModeWidth;
    Mode->Format.Graphics.PrimSurfSize.cy = Ctx->ModeHeight;
    Mode->Format.Graphics.VisibleRegionSize.cx = Ctx->ModeWidth;
    Mode->Format.Graphics.VisibleRegionSize.cy = Ctx->ModeHeight;
    Mode->Format.Graphics.Stride = Ctx->Stride;
    Mode->Format.Graphics.PixelFormat = Ctx->ModeBpp == 32 ? D3DDDIFMT_A8R8G8B8 :
                                        Ctx->ModeBpp == 24 ? D3DDDIFMT_R8G8B8 :
                                        D3DDDIFMT_X8R8G8B8;
    Mode->Format.Graphics.ColorBasis = D3DKMDT_CB_SRGB;
    Mode->Format.Graphics.PixelValueAccessMode = D3DKMDT_PVAM_DIRECT;
}

static
VOID
Lt1xFillTargetMode(_In_ PLT1X_CONTEXT Ctx, _Out_ D3DKMDT_VIDPN_TARGET_MODE *Mode)
{
    RtlZeroMemory(Mode, sizeof(*Mode));
    Mode->Id = 0;
    Mode->VideoSignalInfo.TotalSize.cx = Ctx->ModeWidth;
    Mode->VideoSignalInfo.TotalSize.cy = Ctx->ModeHeight;
    Mode->VideoSignalInfo.ActiveSize.cx = Ctx->ModeWidth;
    Mode->VideoSignalInfo.ActiveSize.cy = Ctx->ModeHeight;
    Mode->VideoSignalInfo.VSyncFreq.Numerator = 60;
    Mode->VideoSignalInfo.VSyncFreq.Denominator = 1;
    Mode->VideoSignalInfo.HSyncFreq.Numerator = (Ctx->ModeWidth + 160) * 60;
    Mode->VideoSignalInfo.HSyncFreq.Denominator = 1;
    Mode->VideoSignalInfo.PixelRate = (SIZE_T)Ctx->ModeWidth * Ctx->ModeHeight * 60;
    Mode->VideoSignalInfo.ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE;
    Mode->Preference = D3DKMDT_MP_PREFERRED;
}

static
NTSTATUS
Lt1xAssignCofuncModality(
    _In_ PLT1X_CONTEXT Ctx,
    _In_ D3DKMDT_HVIDPN hVidPn)
{
    CONST DXGK_VIDPN_INTERFACE *VidPnInterface;
    D3DKMDT_HVIDPNTOPOLOGY hTopology;
    CONST DXGK_VIDPNTOPOLOGY_INTERFACE *TopologyInterface;
    CONST D3DKMDT_VIDPN_PRESENT_PATH *Path;
    NTSTATUS Status;

    Status = Ctx->DxgkInterface.DxgkCbQueryVidPnInterface(
        hVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &VidPnInterface);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = VidPnInterface->pfnGetTopology(hVidPn, &hTopology, &TopologyInterface);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = TopologyInterface->pfnAcquireFirstPathInfo(hTopology, &Path);
    while (NT_SUCCESS(Status) && Path != NULL)
    {
        D3DKMDT_HVIDPNSOURCEMODESET hSourceModeSet;
        CONST DXGK_VIDPNSOURCEMODESET_INTERFACE *SourceModeSetInterface;
        D3DKMDT_HVIDPNTARGETMODESET hTargetModeSet;
        CONST DXGK_VIDPNTARGETMODESET_INTERFACE *TargetModeSetInterface;
        D3DKMDT_VIDPN_SOURCE_MODE *SourceMode;
        D3DKMDT_VIDPN_TARGET_MODE *TargetMode;

        Status = VidPnInterface->pfnCreateNewSourceModeSet(
            hVidPn, Path->VidPnSourceId, &hSourceModeSet, &SourceModeSetInterface);
        if (!NT_SUCCESS(Status))
            break;
        Status = SourceModeSetInterface->pfnCreateNewModeInfo(hSourceModeSet, &SourceMode);
        if (NT_SUCCESS(Status))
        {
            Lt1xFillSourceMode(Ctx, SourceMode);
            Status = SourceModeSetInterface->pfnAddMode(hSourceModeSet, SourceMode);
            if (NT_SUCCESS(Status))
                Status = SourceModeSetInterface->pfnPinMode(hSourceModeSet, SourceMode->Id);
            SourceModeSetInterface->pfnReleaseModeInfo(hSourceModeSet, SourceMode);
        }
        if (NT_SUCCESS(Status))
            Status = VidPnInterface->pfnAssignSourceModeSet(hVidPn, Path->VidPnSourceId, hSourceModeSet);
        VidPnInterface->pfnReleaseSourceModeSet(hVidPn, hSourceModeSet);
        if (!NT_SUCCESS(Status))
            break;

        Status = VidPnInterface->pfnCreateNewTargetModeSet(
            hVidPn, Path->VidPnTargetId, &hTargetModeSet, &TargetModeSetInterface);
        if (!NT_SUCCESS(Status))
            break;
        Status = TargetModeSetInterface->pfnCreateNewModeInfo(hTargetModeSet, &TargetMode);
        if (NT_SUCCESS(Status))
        {
            Lt1xFillTargetMode(Ctx, TargetMode);
            Status = TargetModeSetInterface->pfnAddMode(hTargetModeSet, TargetMode);
            if (NT_SUCCESS(Status))
                Status = TargetModeSetInterface->pfnPinMode(hTargetModeSet, TargetMode->Id);
            TargetModeSetInterface->pfnReleaseModeInfo(hTargetModeSet, TargetMode);
        }
        if (NT_SUCCESS(Status))
            Status = VidPnInterface->pfnAssignTargetModeSet(hVidPn, Path->VidPnTargetId, hTargetModeSet);
        VidPnInterface->pfnReleaseTargetModeSet(hVidPn, hTargetModeSet);
        if (!NT_SUCCESS(Status))
            break;

        Status = TopologyInterface->pfnAcquireNextPathInfo(hTopology, Path, &Path);
    }

    if (Status == STATUS_GRAPHICS_PATH_NOT_IN_TOPOLOGY || Status == STATUS_NOT_FOUND)
        Status = STATUS_SUCCESS;

    if (Path != NULL)
        TopologyInterface->pfnReleasePathInfo(hTopology, Path);
    return Status;
}

static
PVOID
Lt1xSegmentVa(_In_ PLT1X_CONTEXT Ctx, _In_ UINT SegmentId, _In_ LARGE_INTEGER SegmentAddress)
{
    ULONG64 Offset;

    if (SegmentId != 0 && SegmentId != DXGK_SEGMENT_ID_SYSTEMMEMORY)
        return NULL;
    if (SegmentId == DXGK_SEGMENT_ID_SYSTEMMEMORY)
        return NULL;
    Offset = (ULONG64)SegmentAddress.QuadPart - (ULONG64)Ctx->FrameBufferPhys.QuadPart;
    if (Offset >= LT1X_FB_MAP_SIZE)
        return NULL;
    return (PUCHAR)Ctx->FrameBufferVa + (SIZE_T)Offset;
}

static
NTSTATUS
Lt1xExecutePaging(_In_ PLT1X_CONTEXT Ctx, _In_ DXGKARG_BUILDPAGINGBUFFER *Args)
{
    switch (Args->Operation)
    {
        case DXGK_OPERATION_FILL:
        {
            PVOID Dst = Lt1xSegmentVa(Ctx, Args->Fill.Destination.SegmentId,
                                      Args->Fill.Destination.SegmentAddress);
            if (Dst == NULL || Args->Fill.FillSize > LT1X_FB_MAP_SIZE)
                return STATUS_INVALID_PARAMETER;
            RtlFillMemory(Dst, Args->Fill.FillSize, Args->Fill.FillPattern);
            return STATUS_SUCCESS;
        }
        case DXGK_OPERATION_TRANSFER:
        {
            PVOID Src = Lt1xSegmentVa(Ctx, Args->Transfer.Source.SegmentId,
                                      Args->Transfer.Source.SegmentAddress);
            PVOID Dst = Lt1xSegmentVa(Ctx, Args->Transfer.Destination.SegmentId,
                                      Args->Transfer.Destination.SegmentAddress);
            if (Dst == NULL || Src == NULL || Args->Transfer.TransferSize > LT1X_FB_MAP_SIZE)
                return STATUS_INVALID_PARAMETER;
            RtlMoveMemory(Dst, Src, Args->Transfer.TransferSize);
            return STATUS_SUCCESS;
        }
        default:
            return STATUS_SUCCESS;
    }
}

static DXGKDDI_ADD_DEVICE Lt1xDdiAddDevice;
static NTSTATUS APIENTRY
Lt1xDdiAddDevice(
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _Out_ PVOID *MiniportDeviceContext)
{
    PLT1X_CONTEXT Ctx;

    DbgPrint("LT1X: DxgkDdiAddDevice PDO=%p\n", PhysicalDeviceObject);
    Ctx = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Ctx), LT1X_TAG);
    if (Ctx == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(Ctx, sizeof(*Ctx));
    Ctx->Pdo = PhysicalDeviceObject;
    *MiniportDeviceContext = Ctx;
    return STATUS_SUCCESS;
}

static DXGKDDI_START_DEVICE Lt1xDdiStartDevice;
static NTSTATUS APIENTRY
Lt1xDdiStartDevice(
    _In_ PVOID MiniportDeviceContext,
    _In_ PDXGK_START_INFO DxgkStartInfo,
    _In_ PDXGK_INTERFACE DxgkInterface,
    _Out_ PULONG NumberOfVideoPresentSources,
    _Out_ PULONG NumberOfChildren)
{
    PLT1X_CONTEXT Ctx = (PLT1X_CONTEXT)MiniportDeviceContext;
    ULONG BarLow = 0, BytesRead = 0;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(DxgkStartInfo);

    DbgPrint("LT1X: DxgkDdiStartDevice IF Size=%u Version=0x%lx\n",
             DxgkInterface ? DxgkInterface->Size : 0,
             DxgkInterface ? DxgkInterface->Version : 0);

    if (Lt1xFailStart)
    {
        DbgPrint("LT1X: FailStart poison set, failing StartDevice\n");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    if (DxgkInterface == NULL ||
        DxgkInterface->Size < FIELD_OFFSET(DXGK_INTERFACE, DxgkCbReadDeviceSpace) + sizeof(PVOID))
    {
        DbgPrint("LT1X: DxgkInterface too small (%u)\n",
                 DxgkInterface ? DxgkInterface->Size : 0);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    RtlCopyMemory(&Ctx->DxgkInterface, DxgkInterface, DxgkInterface->Size);
    Ctx->DeviceHandle = DxgkInterface->DeviceHandle;

    Status = Ctx->DxgkInterface.DxgkCbReadDeviceSpace(
        Ctx->DeviceHandle, DXGK_WHICHSPACE_CONFIG, &BarLow, 0x10, sizeof(BarLow), &BytesRead);
    if (!NT_SUCCESS(Status) || BytesRead != sizeof(BarLow))
    {
        DbgPrint("LT1X: BAR0 read failed 0x%08lx bytes=%lu\n", Status, BytesRead);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    Ctx->FrameBufferPhys.QuadPart = (ULONGLONG)(BarLow & ~0xFUL);
    if (Ctx->FrameBufferPhys.QuadPart == 0 || (BarLow & 0x1) != 0)
    {
        DbgPrint("LT1X: BAR0 invalid (raw 0x%08lx)\n", BarLow);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    Ctx->FrameBufferVa = MmMapIoSpace(Ctx->FrameBufferPhys, LT1X_FB_MAP_SIZE, MmNonCached);
    if (Ctx->FrameBufferVa == NULL)
    {
        DbgPrint("LT1X: MmMapIoSpace failed\n");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    DbgPrint("LT1X: framebuffer phys 0x%I64x va %p\n",
             Ctx->FrameBufferPhys.QuadPart, Ctx->FrameBufferVa);
    Lt1xProbeCurrentMode(Ctx);

    *NumberOfVideoPresentSources = 1;
    *NumberOfChildren = max(Lt1xChildCount, 1);
    return STATUS_SUCCESS;
}

static DXGKDDI_STOP_DEVICE Lt1xDdiStopDevice;
static NTSTATUS APIENTRY
Lt1xDdiStopDevice(_In_ PVOID MiniportDeviceContext)
{
    PLT1X_CONTEXT Ctx = (PLT1X_CONTEXT)MiniportDeviceContext;

    DbgPrint("LT1X: DxgkDdiStopDevice ctx=%p\n", Ctx);
    if (Ctx->FrameBufferVa != NULL)
    {
        MmUnmapIoSpace(Ctx->FrameBufferVa, LT1X_FB_MAP_SIZE);
        Ctx->FrameBufferVa = NULL;
    }
    return STATUS_SUCCESS;
}

static DXGKDDI_REMOVE_DEVICE Lt1xDdiRemoveDevice;
static NTSTATUS APIENTRY
Lt1xDdiRemoveDevice(_In_ PVOID MiniportDeviceContext)
{
    DbgPrint("LT1X: DxgkDdiRemoveDevice ctx=%p\n", MiniportDeviceContext);
    if (MiniportDeviceContext != NULL)
        ExFreePoolWithTag(MiniportDeviceContext, LT1X_TAG);
    return STATUS_SUCCESS;
}

static DXGKDDI_RESET_DEVICE Lt1xDdiResetDevice;
static VOID APIENTRY
Lt1xDdiResetDevice(_In_ PVOID MiniportDeviceContext)
{
    DbgPrint("LT1X: DxgkDdiResetDevice ctx=%p\n", MiniportDeviceContext);
}

static DXGKDDI_SET_POWER_STATE Lt1xDdiSetPowerState;
static NTSTATUS APIENTRY
Lt1xDdiSetPowerState(
    _In_ PVOID MiniportDeviceContext,
    _In_ ULONG DeviceUid,
    _In_ DEVICE_POWER_STATE DevicePowerState,
    _In_ POWER_ACTION ActionType)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    DbgPrint("LT1X: DxgkDdiSetPowerState uid=%lu state=%d action=%d\n",
             DeviceUid, DevicePowerState, ActionType);
    return STATUS_SUCCESS;
}

static DXGKDDI_UNLOAD Lt1xDdiUnload;
static VOID APIENTRY
Lt1xDdiUnload(VOID)
{
    DbgPrint("LT1X: DxgkDdiUnload\n");
}

static DXGKDDI_QUERY_INTERFACE Lt1xDdiQueryInterface;
static NTSTATUS APIENTRY
Lt1xDdiQueryInterface(
    _In_ PVOID MiniportDeviceContext,
    _In_ PQUERY_INTERFACE QueryInterface)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(QueryInterface);
    DbgPrint("LT1X: DxgkDdiQueryInterface\n");
    return STATUS_NOT_SUPPORTED;
}

static DXGKDDI_NOTIFY_ACPI_EVENT Lt1xDdiNotifyAcpiEvent;
static NTSTATUS APIENTRY
Lt1xDdiNotifyAcpiEvent(
    _In_ PVOID MiniportDeviceContext,
    _In_ DXGK_EVENT_TYPE EventType,
    _In_ ULONG Event,
    _In_ PVOID Argument,
    _Out_ PULONG ArgumentStatus)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(Argument);
    DbgPrint("LT1X: DxgkDdiNotifyAcpiEvent type=%d event=%lu\n", EventType, Event);
    *ArgumentStatus = 0;
    return STATUS_NOT_SUPPORTED;
}

static DXGKDDI_CONTROL_ETW_LOGGING Lt1xDdiControlEtwLogging;
static VOID APIENTRY
Lt1xDdiControlEtwLogging(
    _In_ BOOLEAN Enable,
    _In_ ULONG Flags,
    _In_ UCHAR Level)
{
    UNREFERENCED_PARAMETER(Enable);
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(Level);
    DbgPrint("LT1X: DxgkDdiControlEtwLogging\n");
}

static DXGKDDI_DISPATCH_IO_REQUEST Lt1xDdiDispatchIoRequest;
static NTSTATUS APIENTRY
Lt1xDdiDispatchIoRequest(
    _In_ PVOID MiniportDeviceContext,
    _In_ ULONG VidPnSourceId,
    _In_ PVIDEO_REQUEST_PACKET VideoRequestPacket)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(VidPnSourceId);
    UNREFERENCED_PARAMETER(VideoRequestPacket);
    DbgPrint("LT1X: DxgkDdiDispatchIoRequest\n");
    return STATUS_NOT_SUPPORTED;
}

static DXGKDDI_INTERRUPT_ROUTINE Lt1xDdiInterruptRoutine;
static BOOLEAN APIENTRY
Lt1xDdiInterruptRoutine(
    _In_ PVOID MiniportDeviceContext,
    _In_ ULONG MessageNumber)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(MessageNumber);
    return FALSE;
}

static DXGKDDI_DPC_ROUTINE Lt1xDdiDpcRoutine;
static VOID APIENTRY
Lt1xDdiDpcRoutine(_In_ PVOID MiniportDeviceContext)
{
    PLT1X_CONTEXT Ctx = (PLT1X_CONTEXT)MiniportDeviceContext;

    if (Ctx != NULL && Ctx->DeviceHandle != NULL)
        Ctx->DxgkInterface.DxgkCbNotifyDpc(Ctx->DeviceHandle);
}

static DXGKDDI_QUERY_CHILD_RELATIONS Lt1xDdiQueryChildRelations;
static NTSTATUS APIENTRY
Lt1xDdiQueryChildRelations(
    _In_ PVOID MiniportDeviceContext,
    _Out_ PDXGK_CHILD_DESCRIPTOR ChildRelations,
    _In_ ULONG ChildRelationsSize)
{
    ULONG Count, Capacity;

    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    Count = max(Lt1xChildCount, 1);
    Capacity = ChildRelationsSize / sizeof(DXGK_CHILD_DESCRIPTOR);
    DbgPrint("LT1X: DxgkDdiQueryChildRelations size=%lu children=%lu\n",
             ChildRelationsSize, Count);

    if (Capacity == 0)
        return STATUS_BUFFER_TOO_SMALL;

    if (Capacity > 0)
    {
        RtlZeroMemory(&ChildRelations[0], sizeof(ChildRelations[0]));
        ChildRelations[0].ChildDeviceType = TypeVideoOutput;
        ChildRelations[0].ChildCapabilities.Type.VideoOutput.InterfaceTechnology = D3DKMDT_VOT_HD15;
        ChildRelations[0].ChildCapabilities.HpdAwareness = HpdAwarenessAlwaysConnected;
        ChildRelations[0].AcpiUid = 0;
        ChildRelations[0].ChildUid = LT1X_CHILD_UID;
    }
    return STATUS_SUCCESS;
}

static DXGKDDI_QUERY_CHILD_STATUS Lt1xDdiQueryChildStatus;
static NTSTATUS APIENTRY
Lt1xDdiQueryChildStatus(
    _In_ PVOID MiniportDeviceContext,
    _Inout_ PDXGK_CHILD_STATUS ChildStatus,
    _In_ BOOLEAN NonDestructiveOnly)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(NonDestructiveOnly);
    DbgPrint("LT1X: DxgkDdiQueryChildStatus\n");
    if (ChildStatus->ChildUid != LT1X_CHILD_UID)
        return STATUS_INVALID_PARAMETER;
    ChildStatus->Type = StatusConnection;
    ChildStatus->HotPlug.Connected = TRUE;
    return STATUS_SUCCESS;
}

static DXGKDDI_QUERY_DEVICE_DESCRIPTOR Lt1xDdiQueryDeviceDescriptor;
static NTSTATUS APIENTRY
Lt1xDdiQueryDeviceDescriptor(
    _In_ PVOID MiniportDeviceContext,
    _In_ ULONG ChildUid,
    _Inout_ PDXGK_DEVICE_DESCRIPTOR DeviceDescriptor)
{
    PLT1X_CONTEXT Ctx = (PLT1X_CONTEXT)MiniportDeviceContext;
    static UCHAR Edid[128];
    ULONG PixelClock, HBlank, VBlank, HSyncOffset, VSyncOffset, HSyncWidth, VSyncWidth;

    DbgPrint("LT1X: DxgkDdiQueryDeviceDescriptor uid=%lu\n", ChildUid);
    if (ChildUid != LT1X_CHILD_UID || DeviceDescriptor == NULL ||
        DeviceDescriptor->DescriptorBuffer == NULL || DeviceDescriptor->DescriptorLength < sizeof(Edid))
    {
        return STATUS_INVALID_PARAMETER;
    }

    HBlank = Ctx->ModeWidth / 5 + 32;
    VBlank = 38;
    HSyncOffset = 16;
    VSyncOffset = 4;
    HSyncWidth = 96;
    VSyncWidth = 2;
    PixelClock = (Ctx->ModeWidth + HBlank) * (Ctx->ModeHeight + VBlank) * 60 / 10000;

    RtlZeroMemory(Edid, sizeof(Edid));
    Edid[0] = 0x00;
    Edid[1] = 0xFF; Edid[2] = 0xFF; Edid[3] = 0xFF;
    Edid[4] = 0xFF; Edid[5] = 0xFF; Edid[6] = 0xFF; Edid[7] = 0x00;
    Edid[8] = 0x4C; Edid[9] = 0xD1; Edid[10] = 0x38; /* vendor "LT1", product */
    Edid[11] = 0x00; Edid[12] = 0x01;
    Edid[16] = 1; Edid[17] = 4;  /* EDID 1.4 */
    Edid[20] = 0x80;             /* digital display */
    Edid[21] = (UCHAR)(Ctx->ModeWidth > 255 ? 255 : Ctx->ModeWidth);
    Edid[22] = (UCHAR)(Ctx->ModeHeight > 255 ? 255 : Ctx->ModeHeight);
    /* detailed timing descriptor at offset 54 */
    Edid[54] = (UCHAR)(PixelClock & 0xFF);
    Edid[55] = (UCHAR)(PixelClock >> 8);
    Edid[56] = (UCHAR)(Ctx->ModeWidth & 0xFF);
    Edid[57] = (UCHAR)(HBlank & 0xFF);
    Edid[58] = (UCHAR)(((HBlank >> 4) & 0xF0) | ((Ctx->ModeWidth >> 8) & 0x0F));
    Edid[59] = (UCHAR)(Ctx->ModeHeight & 0xFF);
    Edid[60] = (UCHAR)(VBlank & 0xFF);
    Edid[61] = (UCHAR)(((VBlank >> 4) & 0xF0) | ((Ctx->ModeHeight >> 8) & 0x0F));
    Edid[62] = HSyncOffset;
    Edid[63] = HSyncWidth;
    Edid[64] = (UCHAR)(((VSyncOffset & 0x0F) << 4) | (VSyncWidth & 0x0F));
    Edid[65] = 0x15;             /* digital separate sync, -HSync -VSync */
    Edid[126] = 0; Edid[127] = 0; /* checksum intentionally 0 (checksum is optional for the consumer) */

    RtlCopyMemory(DeviceDescriptor->DescriptorBuffer, Edid, sizeof(Edid));
    return STATUS_SUCCESS;
}

static DXGKDDI_QUERYADAPTERINFO Lt1xDdiQueryAdapterInfo;
static NTSTATUS APIENTRY
Lt1xDdiQueryAdapterInfo(
    IN_CONST_HANDLE hAdapter,
    IN_CONST_PDXGKARG_QUERYADAPTERINFO pQueryAdapterInfo)
{
    PLT1X_CONTEXT Ctx = (PLT1X_CONTEXT)hAdapter;

    DbgPrint("LT1X: DxgkDdiQueryAdapterInfo type=%d outSize=%lu\n",
             pQueryAdapterInfo->Type, pQueryAdapterInfo->OutputDataSize);

    switch (pQueryAdapterInfo->Type)
    {
        case DXGKQAITYPE_DRIVERCAPS:
        {
            PDXGK_DRIVERCAPS Caps;

            if (pQueryAdapterInfo->pOutputData == NULL ||
                pQueryAdapterInfo->OutputDataSize < sizeof(DXGK_DRIVERCAPS))
                return STATUS_INVALID_PARAMETER;
            Caps = (PDXGK_DRIVERCAPS)pQueryAdapterInfo->pOutputData;
            RtlZeroMemory(Caps, sizeof(*Caps));
            Caps->GpuEngineTopology.NbAsymetricProcessingNodes = 1;
            return STATUS_SUCCESS;
        }

        case DXGKQAITYPE_QUERYSEGMENT:
        {
            PDXGK_QUERYSEGMENTOUT SegmentOut;

            if (pQueryAdapterInfo->pOutputData == NULL ||
                pQueryAdapterInfo->OutputDataSize < sizeof(DXGK_QUERYSEGMENTOUT))
                return STATUS_INVALID_PARAMETER;
            SegmentOut = (PDXGK_QUERYSEGMENTOUT)pQueryAdapterInfo->pOutputData;
            if (SegmentOut->pSegmentDescriptor == NULL)
            {
                SegmentOut->NbSegment = 1;
                return STATUS_SUCCESS;
            }
            SegmentOut->NbSegment = 1;
            SegmentOut->PagingBufferSegmentId = 0;
            SegmentOut->PagingBufferSize = 0;
            SegmentOut->PagingBufferPrivateDataSize = 0;
            RtlZeroMemory(&SegmentOut->pSegmentDescriptor[0], sizeof(DXGK_SEGMENTDESCRIPTOR));
            SegmentOut->pSegmentDescriptor[0].BaseAddress = Ctx->FrameBufferPhys;
            SegmentOut->pSegmentDescriptor[0].CpuTranslatedAddress = Ctx->FrameBufferPhys;
            SegmentOut->pSegmentDescriptor[0].Size = LT1X_FB_MAP_SIZE;
            SegmentOut->pSegmentDescriptor[0].NbOfBanks = 1;
            SegmentOut->pSegmentDescriptor[0].CommitLimit = LT1X_FB_MAP_SIZE;
            SegmentOut->pSegmentDescriptor[0].Flags.Aperture = 1;
            SegmentOut->pSegmentDescriptor[0].Flags.CpuVisible = 1;
            SegmentOut->pSegmentDescriptor[0].Flags.CacheCoherent = 1;
            return STATUS_SUCCESS;
        }

        case DXGKQAITYPE_NUMPOWERCOMPONENTS:
            if (pQueryAdapterInfo->pOutputData == NULL ||
                pQueryAdapterInfo->OutputDataSize < sizeof(ULONG))
                return STATUS_INVALID_PARAMETER;
            *(PULONG)pQueryAdapterInfo->pOutputData = 0;
            return STATUS_SUCCESS;

        default:
            return STATUS_NOT_SUPPORTED;
    }
}

static DXGKDDI_CREATEDEVICE Lt1xDdiCreateDevice;
static NTSTATUS APIENTRY
Lt1xDdiCreateDevice(
    IN_CONST_HANDLE hAdapter,
    INOUT_PDXGKARG_CREATEDEVICE pCreateDevice)
{
    UNREFERENCED_PARAMETER(hAdapter);
    DbgPrint("LT1X: DxgkDdiCreateDevice\n");
    pCreateDevice->hDevice = (HANDLE)((ULONG_PTR)0x1DADDE1CEu);
    return STATUS_SUCCESS;
}

static DXGKDDI_CREATEALLOCATION Lt1xDdiCreateAllocation;
static NTSTATUS APIENTRY
Lt1xDdiCreateAllocation(
    IN_CONST_HANDLE hAdapter,
    INOUT_PDXGKARG_CREATEALLOCATION pCreateAllocation)
{
    UINT i;

    UNREFERENCED_PARAMETER(hAdapter);
    DbgPrint("LT1X: DxgkDdiCreateAllocation count=%lu flags=0x%lx\n",
             pCreateAllocation->NumAllocations, pCreateAllocation->Flags.Value);

    for (i = 0; i < pCreateAllocation->NumAllocations; i++)
    {
        DXGK_ALLOCATIONINFO *Info = &pCreateAllocation->pAllocationInfo[i];
        PVOID Token = ExAllocatePoolWithTag(NonPagedPool, sizeof(ULONG_PTR), LT1X_TAG);

        if (Token == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;
        Info->hAllocation = Token;
        Info->SupportedReadSegmentSet = 1;
        Info->SupportedWriteSegmentSet = 1;
        Info->EvictionSegmentSet = 0;
        Info->Alignment = 0x1000;
    }
    return STATUS_SUCCESS;
}

static DXGKDDI_DESTROYALLOCATION Lt1xDdiDestroyAllocation;
static NTSTATUS APIENTRY
Lt1xDdiDestroyAllocation(
    IN_CONST_HANDLE hAdapter,
    IN_CONST_PDXGKARG_DESTROYALLOCATION pDestroyAllocation)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pDestroyAllocation);
    DbgPrint("LT1X: DxgkDdiDestroyAllocation\n");
    return STATUS_SUCCESS;
}

static DXGKDDI_DESCRIBEALLOCATION Lt1xDdiDescribeAllocation;
static NTSTATUS APIENTRY
Lt1xDdiDescribeAllocation(
    IN_CONST_HANDLE hAdapter,
    INOUT_PDXGKARG_DESCRIBEALLOCATION pDescribeAllocation)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pDescribeAllocation);
    DbgPrint("LT1X: DxgkDdiDescribeAllocation\n");
    return STATUS_NOT_SUPPORTED;
}

static DXGKDDI_GETSTANDARDALLOCATIONDRIVERDATA Lt1xDdiGetStandardAllocationDriverData;
static NTSTATUS APIENTRY
Lt1xDdiGetStandardAllocationDriverData(
    IN_CONST_HANDLE hAdapter,
    INOUT_PDXGKARG_GETSTANDARDALLOCATIONDRIVERDATA pStandardAllocationDriverData)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pStandardAllocationDriverData);
    DbgPrint("LT1X: DxgkDdiGetStandardAllocationDriverData\n");
    return STATUS_NOT_SUPPORTED;
}

static DXGKDDI_ACQUIRESWIZZLINGRANGE Lt1xDdiAcquireSwizzlingRange;
static NTSTATUS APIENTRY
Lt1xDdiAcquireSwizzlingRange(
    IN_CONST_HANDLE hAdapter,
    INOUT_PDXGKARG_ACQUIRESWIZZLINGRANGE pAcquireSwizzlingRange)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pAcquireSwizzlingRange);
    DbgPrint("LT1X: DxgkDdiAcquireSwizzlingRange\n");
    return STATUS_NOT_SUPPORTED;
}

static DXGKDDI_RELEASESWIZZLINGRANGE Lt1xDdiReleaseSwizzlingRange;
static NTSTATUS APIENTRY
Lt1xDdiReleaseSwizzlingRange(
    IN_CONST_HANDLE hAdapter,
    IN_CONST_PDXGKARG_RELEASESWIZZLINGRANGE pReleaseSwizzlingRange)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pReleaseSwizzlingRange);
    DbgPrint("LT1X: DxgkDdiReleaseSwizzlingRange\n");
    return STATUS_NOT_SUPPORTED;
}

static DXGKDDI_PATCH Lt1xDdiPatch;
static NTSTATUS APIENTRY
Lt1xDdiPatch(
    IN_CONST_HANDLE hAdapter,
    IN_CONST_PDXGKARG_PATCH pPatch)
{
    UNREFERENCED_PARAMETER(hAdapter);
    DbgPrint("LT1X: DxgkDdiPatch allocCount=%lu\n",
             pPatch ? pPatch->AllocationListSize : 0);
    return STATUS_SUCCESS;
}

static DXGKDDI_SUBMITCOMMAND Lt1xDdiSubmitCommand;
static NTSTATUS APIENTRY
Lt1xDdiSubmitCommand(
    IN_CONST_HANDLE hAdapter,
    IN_CONST_PDXGKARG_SUBMITCOMMAND pSubmitCommand)
{
    PLT1X_CONTEXT Ctx = (PLT1X_CONTEXT)hAdapter;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA NotifyData;

    DbgPrint("LT1X: DxgkDdiSubmitCommand fence=%lu size=%lu\n",
             pSubmitCommand->SubmissionFenceId, pSubmitCommand->DmaBufferSize);

    RtlZeroMemory(&NotifyData, sizeof(NotifyData));
    NotifyData.InterruptType = DXGK_INTERRUPT_DMA_COMPLETED;
    NotifyData.DmaCompleted.SubmissionFenceId = pSubmitCommand->SubmissionFenceId;
    NotifyData.DmaCompleted.NodeOrdinal = 0;
    NotifyData.DmaCompleted.EngineOrdinal = 0;
    Ctx->DxgkInterface.DxgkCbNotifyInterrupt(Ctx->DeviceHandle, &NotifyData);
    Ctx->DxgkInterface.DxgkCbQueueDpc(Ctx->DeviceHandle);
    return STATUS_SUCCESS;
}

static DXGKDDI_PREEMPTCOMMAND Lt1xDdiPreemptCommand;
static NTSTATUS APIENTRY
Lt1xDdiPreemptCommand(
    IN_CONST_HANDLE hAdapter,
    IN_CONST_PDXGKARG_PREEMPTCOMMAND pPreemptCommand)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pPreemptCommand);
    DbgPrint("LT1X: DxgkDdiPreemptCommand\n");
    return STATUS_NOT_SUPPORTED;
}

static DXGKDDI_BUILDPAGINGBUFFER Lt1xDdiBuildPagingBuffer;
static NTSTATUS APIENTRY
Lt1xDdiBuildPagingBuffer(
    IN_CONST_HANDLE hAdapter,
    IN_PDXGKARG_BUILDPAGINGBUFFER pBuildPagingBuffer)
{
    PLT1X_CONTEXT Ctx = (PLT1X_CONTEXT)hAdapter;
    NTSTATUS Status;

    DbgPrint("LT1X: DxgkDdiBuildPagingBuffer op=%d multipass=%lu dma=%p size=%lu\n",
             pBuildPagingBuffer->Operation, pBuildPagingBuffer->MultipassOffset,
             pBuildPagingBuffer->pDmaBuffer, pBuildPagingBuffer->DmaSize);

    if (pBuildPagingBuffer->pDmaBuffer == NULL)
        return STATUS_SUCCESS;

    Status = Lt1xExecutePaging(Ctx, pBuildPagingBuffer);
    pBuildPagingBuffer->MultipassOffset = 0;
    return Status;
}

static DXGKDDI_SETPALETTE Lt1xDdiSetPalette;
static NTSTATUS APIENTRY
Lt1xDdiSetPalette(
    IN_CONST_HANDLE hAdapter,
    IN_CONST_PDXGKARG_SETPALETTE pSetPalette)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pSetPalette);
    DbgPrint("LT1X: DxgkDdiSetPalette\n");
    return STATUS_NOT_SUPPORTED;
}

static DXGKDDI_SETPOINTERPOSITION Lt1xDdiSetPointerPosition;
static NTSTATUS APIENTRY
Lt1xDdiSetPointerPosition(
    IN_CONST_HANDLE hAdapter,
    IN_CONST_PDXGKARG_SETPOINTERPOSITION pSetPointerPosition)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pSetPointerPosition);
    DbgPrint("LT1X: DxgkDdiSetPointerPosition\n");
    return STATUS_SUCCESS;
}

static DXGKDDI_SETPOINTERSHAPE Lt1xDdiSetPointerShape;
static NTSTATUS APIENTRY
Lt1xDdiSetPointerShape(
    IN_CONST_HANDLE hAdapter,
    IN_CONST_PDXGKARG_SETPOINTERSHAPE pSetPointerShape)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pSetPointerShape);
    DbgPrint("LT1X: DxgkDdiSetPointerShape\n");
    return STATUS_NOT_SUPPORTED;
}

static DXGKDDI_RESETFROMTIMEOUT Lt1xDdiResetFromTimeout;
static NTSTATUS APIENTRY
Lt1xDdiResetFromTimeout(IN_CONST_HANDLE hAdapter)
{
    UNREFERENCED_PARAMETER(hAdapter);
    DbgPrint("LT1X: DxgkDdiResetFromTimeout\n");
    return STATUS_SUCCESS;
}

static DXGKDDI_RESTARTFROMTIMEOUT Lt1xDdiRestartFromTimeout;
static NTSTATUS APIENTRY
Lt1xDdiRestartFromTimeout(IN_CONST_HANDLE hAdapter)
{
    UNREFERENCED_PARAMETER(hAdapter);
    DbgPrint("LT1X: DxgkDdiRestartFromTimeout\n");
    return STATUS_SUCCESS;
}

static DXGKDDI_ESCAPE Lt1xDdiEscape;
static NTSTATUS APIENTRY
Lt1xDdiEscape(
    IN_CONST_HANDLE hAdapter,
    IN_CONST_PDXGKARG_ESCAPE pEscape)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pEscape);
    DbgPrint("LT1X: DxgkDdiEscape\n");
    return STATUS_NOT_SUPPORTED;
}

static DXGKDDI_COLLECTDBGINFO Lt1xDdiCollectDbgInfo;
static NTSTATUS APIENTRY
Lt1xDdiCollectDbgInfo(
    IN_CONST_HANDLE hAdapter,
    IN_CONST_PDXGKARG_COLLECTDBGINFO pCollectDbgInfo)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pCollectDbgInfo);
    DbgPrint("LT1X: DxgkDdiCollectDbgInfo\n");
    return STATUS_SUCCESS;
}

static DXGKDDI_QUERYCURRENTFENCE Lt1xDdiQueryCurrentFence;
static NTSTATUS APIENTRY
Lt1xDdiQueryCurrentFence(
    IN_CONST_HANDLE hAdapter,
    INOUT_PDXGKARG_QUERYCURRENTFENCE pCurrentFence)
{
    UNREFERENCED_PARAMETER(hAdapter);
    DbgPrint("LT1X: DxgkDdiQueryCurrentFence\n");
    pCurrentFence->CurrentFence = 0;
    return STATUS_SUCCESS;
}

static DXGKDDI_ISSUPPORTEDVIDPN Lt1xDdiIsSupportedVidPn;
static NTSTATUS APIENTRY
Lt1xDdiIsSupportedVidPn(
    IN_CONST_HANDLE hAdapter,
    INOUT_PDXGKARG_ISSUPPORTEDVIDPN pIsSupportedVidPn)
{
    UNREFERENCED_PARAMETER(hAdapter);
    DbgPrint("LT1X: DxgkDdiIsSupportedVidPn hVidPn=0x%Ix\n",
             (ULONG_PTR)pIsSupportedVidPn->hDesiredVidPn);
    pIsSupportedVidPn->IsVidPnSupported = TRUE;
    return STATUS_SUCCESS;
}

static DXGKDDI_RECOMMENDFUNCTIONALVIDPN Lt1xDdiRecommendFunctionalVidPn;
static NTSTATUS APIENTRY
Lt1xDdiRecommendFunctionalVidPn(
    IN_CONST_HANDLE hAdapter,
    IN_CONST_PDXGKARG_RECOMMENDFUNCTIONALVIDPN_CONST pRecommendFunctionalVidPn)
{
    PLT1X_CONTEXT Ctx = (PLT1X_CONTEXT)hAdapter;
    CONST DXGK_VIDPN_INTERFACE *VidPnInterface;
    D3DKMDT_HVIDPNTOPOLOGY hTopology;
    CONST DXGK_VIDPNTOPOLOGY_INTERFACE *TopologyInterface;
    D3DKMDT_VIDPN_PRESENT_PATH *Path;
    NTSTATUS Status;

    DbgPrint("LT1X: DxgkDdiRecommendFunctionalVidPn reason=%d targets=%lu\n",
             pRecommendFunctionalVidPn->RequestReason,
             pRecommendFunctionalVidPn->NumberOfVidPnTargets);

    Status = Ctx->DxgkInterface.DxgkCbQueryVidPnInterface(
        pRecommendFunctionalVidPn->hRecommendedFunctionalVidPn,
        DXGK_VIDPN_INTERFACE_VERSION_V1,
        &VidPnInterface);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = VidPnInterface->pfnGetTopology(
        pRecommendFunctionalVidPn->hRecommendedFunctionalVidPn,
        &hTopology, &TopologyInterface);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = TopologyInterface->pfnCreateNewPathInfo(hTopology, &Path);
    if (!NT_SUCCESS(Status))
        return Status;
    Path->VidPnSourceId = 0;
    Path->VidPnTargetId = 0;
    Path->Content = D3DKMDT_VPPC_GRAPHICS;
    Status = TopologyInterface->pfnAddPath(hTopology, Path);
    TopologyInterface->pfnReleasePathInfo(hTopology, Path);
    if (!NT_SUCCESS(Status))
        return Status;

    return Lt1xAssignCofuncModality(Ctx, pRecommendFunctionalVidPn->hRecommendedFunctionalVidPn);
}

static DXGKDDI_ENUMVIDPNCOFUNCMODALITY Lt1xDdiEnumVidPnCofuncModality;
static NTSTATUS APIENTRY
Lt1xDdiEnumVidPnCofuncModality(
    IN_CONST_HANDLE hAdapter,
    IN_CONST_PDXGKARG_ENUMVIDPNCOFUNCMODALITY_CONST pEnumCofuncModality)
{
    PLT1X_CONTEXT Ctx = (PLT1X_CONTEXT)hAdapter;

    DbgPrint("LT1X: DxgkDdiEnumVidPnCofuncModality pivot=%d\n",
             pEnumCofuncModality->EnumPivotType);
    return Lt1xAssignCofuncModality(Ctx, pEnumCofuncModality->hConstrainingVidPn);
}

static DXGKDDI_SETVIDPNSOURCEADDRESS Lt1xDdiSetVidPnSourceAddress;
static NTSTATUS APIENTRY
Lt1xDdiSetVidPnSourceAddress(
    IN_CONST_HANDLE hAdapter,
    IN_CONST_PDXGKARG_SETVIDPNSOURCEADDRESS pSetVidPnSourceAddress)
{
    UNREFERENCED_PARAMETER(hAdapter);
    DbgPrint("LT1X: DxgkDdiSetVidPnSourceAddress source=%lu segment=%lu address=0x%I64x alloc=%p\n",
             pSetVidPnSourceAddress->VidPnSourceId,
             pSetVidPnSourceAddress->PrimarySegment,
             pSetVidPnSourceAddress->PrimaryAddress.QuadPart,
             pSetVidPnSourceAddress->hAllocation);
    return STATUS_SUCCESS;
}

static DXGKDDI_SETVIDPNSOURCEVISIBILITY Lt1xDdiSetVidPnSourceVisibility;
static NTSTATUS APIENTRY
Lt1xDdiSetVidPnSourceVisibility(
    IN_CONST_HANDLE hAdapter,
    IN_CONST_PDXGKARG_SETVIDPNSOURCEVISIBILITY pSetVidPnSourceVisibility)
{
    UNREFERENCED_PARAMETER(hAdapter);
    DbgPrint("LT1X: DxgkDdiSetVidPnSourceVisibility source=%lu visible=%d\n",
             pSetVidPnSourceVisibility->VidPnSourceId,
             pSetVidPnSourceVisibility->Visible);
    return STATUS_SUCCESS;
}

static DXGKDDI_COMMITVIDPN Lt1xDdiCommitVidPn;
static NTSTATUS APIENTRY
Lt1xDdiCommitVidPn(
    IN_CONST_HANDLE hAdapter,
    IN_CONST_PDXGKARG_COMMITVIDPN_CONST pCommitVidPn)
{
    UNREFERENCED_PARAMETER(hAdapter);
    DbgPrint("LT1X: DxgkDdiCommitVidPn source=%lu primaryAlloc=%p\n",
             pCommitVidPn->AffectedVidPnSourceId,
             pCommitVidPn->hPrimaryAllocation);

    if (Lt1xFailCommit)
    {
        DbgPrint("LT1X: FailCommit poison set, failing CommitVidPn\n");
        return STATUS_UNSUCCESSFUL;
    }
    return STATUS_SUCCESS;
}

static DXGKDDI_UPDATEACTIVEVIDPNPRESENTPATH Lt1xDdiUpdateActiveVidPnPresentPath;
static NTSTATUS APIENTRY
Lt1xDdiUpdateActiveVidPnPresentPath(
    IN_CONST_HANDLE hAdapter,
    IN_CONST_PDXGKARG_UPDATEACTIVEVIDPNPRESENTPATH_CONST pUpdateActiveVidPnPresentPath)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pUpdateActiveVidPnPresentPath);
    DbgPrint("LT1X: DxgkDdiUpdateActiveVidPnPresentPath\n");
    return STATUS_SUCCESS;
}

static DXGKDDI_RECOMMENDMONITORMODES Lt1xDdiRecommendMonitorModes;
static NTSTATUS APIENTRY
Lt1xDdiRecommendMonitorModes(
    IN_CONST_HANDLE hAdapter,
    IN_CONST_PDXGKARG_RECOMMENDMONITORMODES_CONST pRecommendMonitorModes)
{
    PLT1X_CONTEXT Ctx = (PLT1X_CONTEXT)hAdapter;
    D3DKMDT_MONITOR_SOURCE_MODE *Mode;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(hAdapter);
    DbgPrint("LT1X: DxgkDdiRecommendMonitorModes target=%lu\n",
             pRecommendMonitorModes->VideoPresentTargetId);

    Status = pRecommendMonitorModes->pMonitorSourceModeSetInterface->pfnCreateNewModeInfo(
        pRecommendMonitorModes->hMonitorSourceModeSet, &Mode);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlZeroMemory(Mode, sizeof(*Mode));
    Mode->VideoSignalInfo.TotalSize.cx = Ctx->ModeWidth;
    Mode->VideoSignalInfo.TotalSize.cy = Ctx->ModeHeight;
    Mode->VideoSignalInfo.ActiveSize.cx = Ctx->ModeWidth;
    Mode->VideoSignalInfo.ActiveSize.cy = Ctx->ModeHeight;
    Mode->VideoSignalInfo.VSyncFreq.Numerator = 60;
    Mode->VideoSignalInfo.VSyncFreq.Denominator = 1;
    Mode->VideoSignalInfo.HSyncFreq.Numerator = (Ctx->ModeWidth + 160) * 60;
    Mode->VideoSignalInfo.HSyncFreq.Denominator = 1;
    Mode->VideoSignalInfo.PixelRate = (SIZE_T)Ctx->ModeWidth * Ctx->ModeHeight * 60;
    Mode->VideoSignalInfo.ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE;
    Mode->ColorBasis = D3DKMDT_CB_SRGB;
    Mode->Origin = D3DKMDT_MCO_DRIVER;
    Mode->Preference = D3DKMDT_MP_PREFERRED;

    Status = pRecommendMonitorModes->pMonitorSourceModeSetInterface->pfnAddMode(
        pRecommendMonitorModes->hMonitorSourceModeSet, Mode);
    pRecommendMonitorModes->pMonitorSourceModeSetInterface->pfnReleaseModeInfo(
        pRecommendMonitorModes->hMonitorSourceModeSet, Mode);
    return Status;
}

static DXGKDDI_RECOMMENDVIDPNTOPOLOGY Lt1xDdiRecommendVidPnTopology;
static NTSTATUS APIENTRY
Lt1xDdiRecommendVidPnTopology(
    IN_CONST_HANDLE hAdapter,
    IN_CONST_PDXGKARG_RECOMMENDVIDPNTOPOLOGY_CONST pRecommendVidPnTopology)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pRecommendVidPnTopology);
    DbgPrint("LT1X: DxgkDdiRecommendVidPnTopology\n");
    return STATUS_NOT_SUPPORTED;
}

static DXGKDDI_GETSCANLINE Lt1xDdiGetScanLine;
static NTSTATUS APIENTRY
Lt1xDdiGetScanLine(
    IN_CONST_HANDLE hAdapter,
    INOUT_PDXGKARG_GETSCANLINE pGetScanLine)
{
    UNREFERENCED_PARAMETER(hAdapter);
    pGetScanLine->InVerticalBlank = TRUE;
    pGetScanLine->ScanLine = 0;
    return STATUS_SUCCESS;
}

static DXGKDDI_STOPCAPTURE Lt1xDdiStopCapture;
static NTSTATUS APIENTRY
Lt1xDdiStopCapture(
    IN_CONST_HANDLE hAdapter,
    IN_CONST_PDXGKARG_STOPCAPTURE pStopCapture)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pStopCapture);
    DbgPrint("LT1X: DxgkDdiStopCapture\n");
    return STATUS_NOT_SUPPORTED;
}

static DXGKDDI_CONTROLINTERRUPT Lt1xDdiControlInterrupt;
static NTSTATUS APIENTRY
Lt1xDdiControlInterrupt(
    IN_CONST_HANDLE hAdapter,
    IN_CONST_DXGK_INTERRUPT_TYPE InterruptType,
    IN_BOOLEAN EnableInterrupt)
{
    UNREFERENCED_PARAMETER(hAdapter);
    DbgPrint("LT1X: DxgkDdiControlInterrupt type=%d enable=%d\n",
             InterruptType, EnableInterrupt);
    return STATUS_SUCCESS;
}

static DXGKDDI_CREATEOVERLAY Lt1xDdiCreateOverlay;
static NTSTATUS APIENTRY
Lt1xDdiCreateOverlay(
    IN_CONST_HANDLE hAdapter,
    INOUT_PDXGKARG_CREATEOVERLAY pCreateOverlay)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pCreateOverlay);
    DbgPrint("LT1X: DxgkDdiCreateOverlay\n");
    return STATUS_NOT_SUPPORTED;
}

static DXGKDDI_DESTROYDEVICE Lt1xDdiDestroyDevice;
static NTSTATUS APIENTRY
Lt1xDdiDestroyDevice(IN_CONST_HANDLE hDevice)
{
    DbgPrint("LT1X: DxgkDdiDestroyDevice h=%p\n", hDevice);
    return STATUS_SUCCESS;
}

static DXGKDDI_OPENALLOCATIONINFO Lt1xDdiOpenAllocation;
static NTSTATUS APIENTRY
Lt1xDdiOpenAllocation(
    IN_CONST_HANDLE hDevice,
    IN_CONST_PDXGKARG_OPENALLOCATION pOpenAllocation)
{
    UINT i;

    UNREFERENCED_PARAMETER(hDevice);
    for (i = 0; i < pOpenAllocation->NumAllocations; i++)
    {
        pOpenAllocation->pOpenAllocation[i].hAllocation = 0;
    }
    DbgPrint("LT1X: DxgkDdiOpenAllocation\n");
    return STATUS_SUCCESS;
}

static DXGKDDI_CLOSEALLOCATION Lt1xDdiCloseAllocation;
static NTSTATUS APIENTRY
Lt1xDdiCloseAllocation(
    IN_CONST_HANDLE hDevice,
    IN_CONST_PDXGKARG_CLOSEALLOCATION pCloseAllocation)
{
    UNREFERENCED_PARAMETER(hDevice);
    UNREFERENCED_PARAMETER(pCloseAllocation);
    DbgPrint("LT1X: DxgkDdiCloseAllocation\n");
    return STATUS_SUCCESS;
}

static DXGKDDI_RENDER Lt1xDdiRender;
static NTSTATUS APIENTRY
Lt1xDdiRender(
    IN_CONST_HANDLE hContext,
    INOUT_PDXGKARG_RENDER pRender)
{
    UNREFERENCED_PARAMETER(hContext);
    UNREFERENCED_PARAMETER(pRender);
    DbgPrint("LT1X: DxgkDdiRender\n");
    return STATUS_NOT_SUPPORTED;
}

static DXGKDDI_PRESENT Lt1xDdiPresent;
static NTSTATUS APIENTRY
Lt1xDdiPresent(
    IN_CONST_HANDLE hContext,
    INOUT_PDXGKARG_PRESENT pPresent)
{
    UNREFERENCED_PARAMETER(hContext);
    UNREFERENCED_PARAMETER(pPresent);
    DbgPrint("LT1X: DxgkDdiPresent\n");
    return STATUS_NOT_SUPPORTED;
}

static DXGKDDI_UPDATEOVERLAY Lt1xDdiUpdateOverlay;
static NTSTATUS APIENTRY
Lt1xDdiUpdateOverlay(
    IN_CONST_HANDLE hOverlay,
    IN_CONST_PDXGKARG_UPDATEOVERLAY pUpdateOverlay)
{
    UNREFERENCED_PARAMETER(hOverlay);
    UNREFERENCED_PARAMETER(pUpdateOverlay);
    DbgPrint("LT1X: DxgkDdiUpdateOverlay\n");
    return STATUS_NOT_SUPPORTED;
}

static DXGKDDI_FLIPOVERLAY Lt1xDdiFlipOverlay;
static NTSTATUS APIENTRY
Lt1xDdiFlipOverlay(
    IN_CONST_HANDLE hOverlay,
    IN_CONST_PDXGKARG_FLIPOVERLAY pFlipOverlay)
{
    UNREFERENCED_PARAMETER(hOverlay);
    UNREFERENCED_PARAMETER(pFlipOverlay);
    DbgPrint("LT1X: DxgkDdiFlipOverlay\n");
    return STATUS_NOT_SUPPORTED;
}

static DXGKDDI_DESTROYOVERLAY Lt1xDdiDestroyOverlay;
static NTSTATUS APIENTRY
Lt1xDdiDestroyOverlay(IN_CONST_HANDLE hOverlay)
{
    UNREFERENCED_PARAMETER(hOverlay);
    DbgPrint("LT1X: DxgkDdiDestroyOverlay\n");
    return STATUS_NOT_SUPPORTED;
}

static DXGKDDI_CREATECONTEXT Lt1xDdiCreateContext;
static NTSTATUS APIENTRY
Lt1xDdiCreateContext(
    IN_CONST_HANDLE hDevice,
    INOUT_PDXGKARG_CREATECONTEXT pCreateContext)
{
    UNREFERENCED_PARAMETER(hDevice);
    DbgPrint("LT1X: DxgkDdiCreateContext\n");
    pCreateContext->hContext = (HANDLE)((ULONG_PTR)0x1DADDE1CEu);
    return STATUS_SUCCESS;
}

static DXGKDDI_DESTROYCONTEXT Lt1xDdiDestroyContext;
static NTSTATUS APIENTRY
Lt1xDdiDestroyContext(IN_CONST_HANDLE hContext)
{
    DbgPrint("LT1X: DxgkDdiDestroyContext h=%p\n", hContext);
    return STATUS_SUCCESS;
}

static DXGKDDI_LINK_DEVICE Lt1xDdiLinkDevice;
static NTSTATUS APIENTRY
Lt1xDdiLinkDevice(
    _In_ CONST PDEVICE_OBJECT PhysicalDeviceObject,
    _In_ CONST PVOID MiniportDeviceContext,
    _Inout_ PLINKED_DEVICE LinkedDevice)
{
    UNREFERENCED_PARAMETER(PhysicalDeviceObject);
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(LinkedDevice);
    DbgPrint("LT1X: DxgkDdiLinkDevice\n");
    return STATUS_NOT_SUPPORTED;
}

static DXGKDDI_SETDISPLAYPRIVATEDRIVERFORMAT Lt1xDdiSetDisplayPrivateDriverFormat;
static NTSTATUS APIENTRY
Lt1xDdiSetDisplayPrivateDriverFormat(
    IN_CONST_HANDLE hAdapter,
    IN_CONST_PDXGKARG_SETDISPLAYPRIVATEDRIVERFORMAT pSetDisplayPrivateDriverFormat)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pSetDisplayPrivateDriverFormat);
    DbgPrint("LT1X: DxgkDdiSetDisplayPrivateDriverFormat\n");
    return STATUS_NOT_SUPPORTED;
}

static DXGKDDI_RENDERKM Lt1xDdiRenderKm;
static NTSTATUS APIENTRY
Lt1xDdiRenderKm(
    IN_CONST_HANDLE hContext,
    INOUT_PDXGKARG_RENDER pRender)
{
    UNREFERENCED_PARAMETER(hContext);
    UNREFERENCED_PARAMETER(pRender);
    DbgPrint("LT1X: DxgkDdiRenderKm\n");
    return STATUS_NOT_SUPPORTED;
}

static DXGKDDI_QUERYVIDPNHWCAPABILITY Lt1xDdiQueryVidPnHWCapability;
static NTSTATUS APIENTRY
Lt1xDdiQueryVidPnHWCapability(
    IN_CONST_HANDLE hAdapter,
    INOUT_PDXGKARG_QUERYVIDPNHWCAPABILITY pVidPnHWCaps)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pVidPnHWCaps);
    DbgPrint("LT1X: DxgkDdiQueryVidPnHWCapability\n");
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    DRIVER_INITIALIZATION_DATA InitData;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(DriverObject);

    Lt1xReadConfig(RegistryPath);

    DbgPrint("LT1X: DriverEntry version=0x%lx poison=%lu children=%lu failStart=%lu failCommit=%lu\n",
             Lt1xDdiVersion, Lt1xPoisonTail, Lt1xChildCount, Lt1xFailStart, Lt1xFailCommit);

    RtlZeroMemory(&InitData, sizeof(InitData));

    if (Lt1xPoisonTail)
        RtlFillMemory(&InitData, sizeof(InitData), 0xCC);

    InitData.Version = Lt1xDdiVersion;

    InitData.DxgkDdiAddDevice = Lt1xDdiAddDevice;
    InitData.DxgkDdiStartDevice = Lt1xDdiStartDevice;
    InitData.DxgkDdiStopDevice = Lt1xDdiStopDevice;
    InitData.DxgkDdiRemoveDevice = Lt1xDdiRemoveDevice;
    InitData.DxgkDdiDispatchIoRequest = Lt1xDdiDispatchIoRequest;
    InitData.DxgkDdiInterruptRoutine = Lt1xDdiInterruptRoutine;
    InitData.DxgkDdiDpcRoutine = Lt1xDdiDpcRoutine;
    InitData.DxgkDdiQueryChildRelations = Lt1xDdiQueryChildRelations;
    InitData.DxgkDdiQueryChildStatus = Lt1xDdiQueryChildStatus;
    InitData.DxgkDdiQueryDeviceDescriptor = Lt1xDdiQueryDeviceDescriptor;
    InitData.DxgkDdiSetPowerState = Lt1xDdiSetPowerState;
    InitData.DxgkDdiNotifyAcpiEvent = Lt1xDdiNotifyAcpiEvent;
    InitData.DxgkDdiResetDevice = Lt1xDdiResetDevice;
    InitData.DxgkDdiUnload = Lt1xDdiUnload;
    InitData.DxgkDdiQueryInterface = Lt1xDdiQueryInterface;
    InitData.DxgkDdiControlEtwLogging = Lt1xDdiControlEtwLogging;
    InitData.DxgkDdiQueryAdapterInfo = Lt1xDdiQueryAdapterInfo;
    InitData.DxgkDdiCreateDevice = Lt1xDdiCreateDevice;
    InitData.DxgkDdiCreateAllocation = Lt1xDdiCreateAllocation;
    InitData.DxgkDdiDestroyAllocation = Lt1xDdiDestroyAllocation;
    InitData.DxgkDdiDescribeAllocation = Lt1xDdiDescribeAllocation;
    InitData.DxgkDdiGetStandardAllocationDriverData = Lt1xDdiGetStandardAllocationDriverData;
    InitData.DxgkDdiAcquireSwizzlingRange = Lt1xDdiAcquireSwizzlingRange;
    InitData.DxgkDdiReleaseSwizzlingRange = Lt1xDdiReleaseSwizzlingRange;
    InitData.DxgkDdiPatch = Lt1xDdiPatch;
    InitData.DxgkDdiSubmitCommand = Lt1xDdiSubmitCommand;
    InitData.DxgkDdiPreemptCommand = Lt1xDdiPreemptCommand;
    InitData.DxgkDdiBuildPagingBuffer = Lt1xDdiBuildPagingBuffer;
    InitData.DxgkDdiSetPalette = Lt1xDdiSetPalette;
    InitData.DxgkDdiSetPointerPosition = Lt1xDdiSetPointerPosition;
    InitData.DxgkDdiSetPointerShape = Lt1xDdiSetPointerShape;
    InitData.DxgkDdiResetFromTimeout = Lt1xDdiResetFromTimeout;
    InitData.DxgkDdiRestartFromTimeout = Lt1xDdiRestartFromTimeout;
    InitData.DxgkDdiEscape = Lt1xDdiEscape;
    InitData.DxgkDdiCollectDbgInfo = Lt1xDdiCollectDbgInfo;
    InitData.DxgkDdiQueryCurrentFence = Lt1xDdiQueryCurrentFence;
    InitData.DxgkDdiIsSupportedVidPn = Lt1xDdiIsSupportedVidPn;
    InitData.DxgkDdiRecommendFunctionalVidPn = Lt1xDdiRecommendFunctionalVidPn;
    InitData.DxgkDdiEnumVidPnCofuncModality = Lt1xDdiEnumVidPnCofuncModality;
    InitData.DxgkDdiSetVidPnSourceAddress = Lt1xDdiSetVidPnSourceAddress;
    InitData.DxgkDdiSetVidPnSourceVisibility = Lt1xDdiSetVidPnSourceVisibility;
    InitData.DxgkDdiCommitVidPn = Lt1xDdiCommitVidPn;
    InitData.DxgkDdiUpdateActiveVidPnPresentPath = Lt1xDdiUpdateActiveVidPnPresentPath;
    InitData.DxgkDdiRecommendMonitorModes = Lt1xDdiRecommendMonitorModes;
    InitData.DxgkDdiRecommendVidPnTopology = Lt1xDdiRecommendVidPnTopology;
    InitData.DxgkDdiGetScanLine = Lt1xDdiGetScanLine;
    InitData.DxgkDdiStopCapture = Lt1xDdiStopCapture;
    InitData.DxgkDdiControlInterrupt = Lt1xDdiControlInterrupt;
    InitData.DxgkDdiCreateOverlay = Lt1xDdiCreateOverlay;
    InitData.DxgkDdiDestroyDevice = Lt1xDdiDestroyDevice;
    InitData.DxgkDdiOpenAllocation = Lt1xDdiOpenAllocation;
    InitData.DxgkDdiCloseAllocation = Lt1xDdiCloseAllocation;
    InitData.DxgkDdiRender = Lt1xDdiRender;
    InitData.DxgkDdiPresent = Lt1xDdiPresent;
    InitData.DxgkDdiUpdateOverlay = Lt1xDdiUpdateOverlay;
    InitData.DxgkDdiFlipOverlay = Lt1xDdiFlipOverlay;
    InitData.DxgkDdiDestroyOverlay = Lt1xDdiDestroyOverlay;
    InitData.DxgkDdiCreateContext = Lt1xDdiCreateContext;
    InitData.DxgkDdiDestroyContext = Lt1xDdiDestroyContext;
    InitData.DxgkDdiLinkDevice = Lt1xDdiLinkDevice;
    InitData.DxgkDdiSetDisplayPrivateDriverFormat = Lt1xDdiSetDisplayPrivateDriverFormat;

    /* 1.1-era slots past the Vista prefix; dxgkrnl must ignore them for a
     * 1.0 Version, and the PoisonTail run proves the prefix contract. */
    InitData.DxgkDdiRenderKm = Lt1xDdiRenderKm;
    InitData.DxgkDdiQueryVidPnHWCapability = Lt1xDdiQueryVidPnHWCapability;

    Status = DxgkInitialize(DriverObject, RegistryPath, &InitData);
    DbgPrint("LT1X: DxgkInitialize returned 0x%08lx\n", Status);
    return Status;
}
