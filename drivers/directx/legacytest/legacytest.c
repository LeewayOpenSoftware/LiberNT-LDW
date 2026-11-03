/*
 * PROJECT:     LiberNT WDDM 1.x synthetic test miniport
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     S0 test vehicle: exercises dxgkrnl version negotiation,
 *              the legacy interface contract, and init/teardown with
 *              self-owned code only (no vendor driver content).
 * COPYRIGHT:   Copyright 2026 Pam (pam-mvzy3ngz)
 */

#include <ntddk.h>
#include <dispmprt.h>
#include <d3dkmddi.h>

#define LT1X_TAG 'X1TL'

static ULONG Lt1xDdiVersion = DXGKDDI_INTERFACE_VERSION_VISTA;
static ULONG Lt1xPoisonTail = 0;
static ULONG Lt1xChildCount = 0;
static ULONG Lt1xDeviceCounter = 0;

static
NTSTATUS
Lt1xReadConfig(_In_ PUNICODE_STRING RegistryPath)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING ValueName;
    KEY_VALUE_PARTIAL_INFORMATION Info;
    HANDLE KeyHandle;
    NTSTATUS Status;
    ULONG ResultLength;

    InitializeObjectAttributes(&ObjectAttributes, RegistryPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwOpenKey(&KeyHandle, KEY_READ, &ObjectAttributes);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlInitUnicodeString(&ValueName, L"Parameters");
    /* Subkey Parameters: open it when present; missing means defaults. */
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
    Status = ZwQueryValueKey(KeyHandle, &ValueName, KeyValuePartialInformation, &Info, sizeof(Info), &ResultLength);
    if (NT_SUCCESS(Status) && Info.Type == REG_DWORD && Info.DataLength == sizeof(ULONG))
        Lt1xDdiVersion = *(PULONG)Info.Data;

    RtlInitUnicodeString(&ValueName, L"PoisonTail");
    Status = ZwQueryValueKey(KeyHandle, &ValueName, KeyValuePartialInformation, &Info, sizeof(Info), &ResultLength);
    if (NT_SUCCESS(Status) && Info.Type == REG_DWORD && Info.DataLength == sizeof(ULONG))
        Lt1xPoisonTail = *(PULONG)Info.Data;

    RtlInitUnicodeString(&ValueName, L"ChildCount");
    Status = ZwQueryValueKey(KeyHandle, &ValueName, KeyValuePartialInformation, &Info, sizeof(Info), &ResultLength);
    if (NT_SUCCESS(Status) && Info.Type == REG_DWORD && Info.DataLength == sizeof(ULONG))
        Lt1xChildCount = *(PULONG)Info.Data;

    ZwClose(KeyHandle);
    return STATUS_SUCCESS;
}

static
NTSTATUS
APIENTRY
Lt1xDdiAddDevice(
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _Out_ PVOID *MiniportDeviceContext)
{
    PVOID Context;

    UNREFERENCED_PARAMETER(PhysicalDeviceObject);

    DbgPrint("LT1X: DxgkDdiAddDevice PDO=%p\n", PhysicalDeviceObject);
    Context = ExAllocatePoolWithTag(NonPagedPool, sizeof(ULONG_PTR), LT1X_TAG);
    if (Context == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    *(PULONG)Context = 0x1DAC0DE;
    *MiniportDeviceContext = Context;
    return STATUS_SUCCESS;
}

static
NTSTATUS
APIENTRY
Lt1xDdiStartDevice(
    _In_ PVOID MiniportDeviceContext,
    _In_ PDXGK_START_INFO DxgkStartInfo,
    _In_ PDXGK_INTERFACE DxgkInterface,
    _Out_ PULONG NumberOfVideoPresentSources,
    _Out_ PULONG NumberOfChildren)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(DxgkStartInfo);

    DbgPrint("LT1X: DxgkDdiStartDevice IF Size=%u Version=0x%lx\n",
             DxgkInterface ? DxgkInterface->Size : 0,
             DxgkInterface ? DxgkInterface->Version : 0);
    *NumberOfVideoPresentSources = 1;
    *NumberOfChildren = Lt1xChildCount;
    return STATUS_SUCCESS;
}

static
NTSTATUS
APIENTRY
Lt1xDdiStopDevice(_In_ PVOID MiniportDeviceContext)
{
    DbgPrint("LT1X: DxgkDdiStopDevice ctx=%p\n", MiniportDeviceContext);
    return STATUS_SUCCESS;
}

static
NTSTATUS
APIENTRY
Lt1xDdiRemoveDevice(_In_ PVOID MiniportDeviceContext)
{
    DbgPrint("LT1X: DxgkDdiRemoveDevice ctx=%p\n", MiniportDeviceContext);
    if (MiniportDeviceContext != NULL)
        ExFreePoolWithTag(MiniportDeviceContext, LT1X_TAG);
    return STATUS_SUCCESS;
}

static
NTSTATUS
APIENTRY
Lt1xDdiResetDevice(_In_ PVOID MiniportDeviceContext)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    DbgPrint("LT1X: DxgkDdiResetDevice\n");
    return STATUS_SUCCESS;
}

static
NTSTATUS
APIENTRY
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

static
VOID
APIENTRY
Lt1xDdiUnload(VOID)
{
    DbgPrint("LT1X: DxgkDdiUnload\n");
}

static
NTSTATUS
APIENTRY
Lt1xDdiQueryInterface(
    _In_ PVOID MiniportDeviceContext,
    _In_ PQUERY_INTERFACE QueryInterface)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(QueryInterface);
    DbgPrint("LT1X: DxgkDdiQueryInterface\n");
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
Lt1xDdiNotifyAcpiEvent(
    _In_ PVOID MiniportDeviceContext,
    _In_ DXGK_EVENT_TYPE EventType,
    _In_ PUCHAR EventData,
    _In_ ULONG DataSize)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(EventData);
    UNREFERENCED_PARAMETER(DataSize);
    DbgPrint("LT1X: DxgkDdiNotifyAcpiEvent type=%d\n", EventType);
    return STATUS_NOT_SUPPORTED;
}

static
VOID
APIENTRY
Lt1xDdiControlEtwLogging(
    _In_ BOOLEAN Enable,
    _In_ ULONG Flags)
{
    UNREFERENCED_PARAMETER(Enable);
    UNREFERENCED_PARAMETER(Flags);
    DbgPrint("LT1X: DxgkDdiControlEtwLogging\n");
}

static
NTSTATUS
APIENTRY
Lt1xDdiDispatchIoRequest(
    _In_ PVOID MiniportDeviceContext,
    _In_ ULONG VidPnSourceId,
    _In_ PDXGK_IO_REQUEST IoRequest)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(VidPnSourceId);
    UNREFERENCED_PARAMETER(IoRequest);
    DbgPrint("LT1X: DxgkDdiDispatchIoRequest\n");
    return STATUS_NOT_SUPPORTED;
}

static
BOOLEAN
APIENTRY
Lt1xDdiInterruptRoutine(_In_ PVOID MiniportDeviceContext)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    return FALSE;
}

static
VOID
APIENTRY
Lt1xDdiDpcRoutine(_In_ PVOID MiniportDeviceContext)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
}

static
NTSTATUS
APIENTRY
Lt1xDdiQueryChildRelations(
    _In_ PVOID MiniportDeviceContext,
    _In_ PDXGK_CHILD_DESCRIPTOR ChildRelations,
    _In_ ULONG ChildRelationsSize)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(ChildRelations);
    UNREFERENCED_PARAMETER(ChildRelationsSize);
    DbgPrint("LT1X: DxgkDdiQueryChildRelations size=%lu children=%lu\n",
             ChildRelationsSize, Lt1xChildCount);
    return STATUS_SUCCESS;
}

static
NTSTATUS
APIENTRY
Lt1xDdiQueryChildStatus(
    _In_ PVOID MiniportDeviceContext,
    _In_ PDXGK_CHILD_STATUS ChildStatus)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(ChildStatus);
    DbgPrint("LT1X: DxgkDdiQueryChildStatus\n");
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
Lt1xDdiQueryDeviceDescriptor(
    _In_ PVOID MiniportDeviceContext,
    _In_ ULONG ChildUid,
    _In_ PDXGK_CHILD_DESCRIPTOR ChildDescriptor)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(ChildUid);
    UNREFERENCED_PARAMETER(ChildDescriptor);
    DbgPrint("LT1X: DxgkDdiQueryDeviceDescriptor uid=%lu\n", ChildUid);
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
Lt1xDdiQueryAdapterInfo(
    _In_ PVOID MiniportDeviceContext,
    _In_ PDXGKARG_QUERYADAPTERINFO QueryAdapterInfo)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);

    DbgPrint("LT1X: DxgkDdiQueryAdapterInfo type=%d outSize=%lu\n",
             QueryAdapterInfo->Type, QueryAdapterInfo->OutputDataSize);

    switch (QueryAdapterInfo->Type)
    {
        case DXGKQAITYPE_DRIVERCAPS:
            /* The caller zeroes the buffer; all-zero caps are a valid
             * minimal answer for every WDDM revision. */
            return STATUS_SUCCESS;

        case DXGKQAITYPE_QUERYSEGMENT:
        {
            PDXGK_QUERYSEGMENTOUT SegmentOut;

            if (QueryAdapterInfo->pOutputData == NULL ||
                QueryAdapterInfo->OutputDataSize < sizeof(DXGK_QUERYSEGMENTOUT))
                return STATUS_INVALID_PARAMETER;
            SegmentOut = (PDXGK_QUERYSEGMENTOUT)QueryAdapterInfo->pOutputData;
            SegmentOut->NbSegment = 0;
            return STATUS_SUCCESS;
        }

        case DXGKQAITYPE_NUMPOWERCOMPONENTS:
            if (QueryAdapterInfo->pOutputData == NULL ||
                QueryAdapterInfo->OutputDataSize < sizeof(ULONG))
                return STATUS_INVALID_PARAMETER;
            *(PULONG)QueryAdapterInfo->pOutputData = 0;
            return STATUS_SUCCESS;

        default:
            return STATUS_NOT_SUPPORTED;
    }
}

static
NTSTATUS
APIENTRY
Lt1xDdiCreateDevice(
    _In_ PVOID MiniportDeviceContext,
    _In_ PDXGKARG_CREATEDEVICE CreateDevice)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    DbgPrint("LT1X: DxgkDdiCreateDevice flags=0x%x\n", CreateDevice ? CreateDevice->Flags : 0);
    return STATUS_SUCCESS;
}

static
NTSTATUS
APIENTRY
Lt1xDdiCreateAllocation(
    _In_ PVOID MiniportDeviceContext,
    _In_ PDXGKARG_CREATEALLOCATION CreateAllocation)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(CreateAllocation);
    DbgPrint("LT1X: DxgkDdiCreateAllocation\n");
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
Lt1xDdiDestroyAllocation(
    _In_ PVOID MiniportDeviceContext,
    _In_ PVOID DeviceContext,
    _In_ PDXGKARG_DESTROYALLOCATION DestroyAllocation)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(DeviceContext);
    UNREFERENCED_PARAMETER(DestroyAllocation);
    DbgPrint("LT1X: DxgkDdiDestroyAllocation\n");
    return STATUS_SUCCESS;
}

static
NTSTATUS
APIENTRY
Lt1xDdiDescribeAllocation(
    _In_ PVOID MiniportDeviceContext,
    _In_ PDXGKARG_DESCRIBEALLOCATION DescribeAllocation)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(DescribeAllocation);
    DbgPrint("LT1X: DxgkDdiDescribeAllocation\n");
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
Lt1xDdiGetStandardAllocationDriverData(
    _In_ PVOID MiniportDeviceContext,
    _In_ PDXGKARG_GETSTANDARDALLOCATIONDRIVERDATA GetStandardAllocationDriverData)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(GetStandardAllocationDriverData);
    DbgPrint("LT1X: DxgkDdiGetStandardAllocationDriverData\n");
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
Lt1xDdiAcquireSwizzlingRange(
    _In_ PVOID MiniportDeviceContext,
    _In_ PDXGKARG_ACQUIRESWIZZLINGRANGE AcquireSwizzlingRange)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(AcquireSwizzlingRange);
    DbgPrint("LT1X: DxgkDdiAcquireSwizzlingRange\n");
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
Lt1xDdiReleaseSwizzlingRange(
    _In_ PVOID MiniportDeviceContext,
    _In_ PDXGKARG_RELEASESWIZZLINGRANGE ReleaseSwizzlingRange)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(ReleaseSwizzlingRange);
    DbgPrint("LT1X: DxgkDdiReleaseSwizzlingRange\n");
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
Lt1xDdiPatch(
    _In_ PVOID MiniportDeviceContext,
    _In_ PDXGKARG_PATCH Patch)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    DbgPrint("LT1X: DxgkDdiPatch allocCount=%lu\n",
             Patch ? Patch->AllocationListSize : 0);
    return STATUS_SUCCESS;
}

static
NTSTATUS
APIENTRY
Lt1xDdiSubmitCommand(
    _In_ PVOID MiniportDeviceContext,
    _In_ PDXGKARG_SUBMITCOMMAND SubmitCommand)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(SubmitCommand);
    DbgPrint("LT1X: DxgkDdiSubmitCommand\n");
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
Lt1xDdiPreemptCommand(
    _In_ PVOID MiniportDeviceContext,
    _In_ PDXGKARG_PREEMPTCOMMAND PreemptCommand)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(PreemptCommand);
    DbgPrint("LT1X: DxgkDdiPreemptCommand\n");
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
Lt1xDdiBuildPagingBuffer(
    _In_ PVOID MiniportDeviceContext,
    _In_ PDXGKARG_BUILDPAGINGBUFFER BuildPagingBuffer)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(BuildPagingBuffer);
    DbgPrint("LT1X: DxgkDdiBuildPagingBuffer\n");
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
Lt1xDdiSetPalette(
    _In_ PVOID MiniportDeviceContext,
    _In_ PDXGKARG_SETPALETTE SetPalette)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(SetPalette);
    DbgPrint("LT1X: DxgkDdiSetPalette\n");
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
Lt1xDdiSetPointerPosition(
    _In_ PVOID MiniportDeviceContext,
    _In_ CONST DXGKARG_SETPOINTERPOSITION *SetPointerPosition)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(SetPointerPosition);
    DbgPrint("LT1X: DxgkDdiSetPointerPosition\n");
    return STATUS_SUCCESS;
}

static
NTSTATUS
APIENTRY
Lt1xDdiSetPointerShape(
    _In_ PVOID MiniportDeviceContext,
    _In_ PDXGKARG_SETPOINTERSHAPE SetPointerShape)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(SetPointerShape);
    DbgPrint("LT1X: DxgkDdiSetPointerShape\n");
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
Lt1xDdiResetFromTimeout(_In_ PVOID MiniportDeviceContext)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    DbgPrint("LT1X: DxgkDdiResetFromTimeout\n");
    return STATUS_SUCCESS;
}

static
NTSTATUS
APIENTRY
Lt1xDdiRestartFromTimeout(_In_ PVOID MiniportDeviceContext)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    DbgPrint("LT1X: DxgkDdiRestartFromTimeout\n");
    return STATUS_SUCCESS;
}

static
NTSTATUS
APIENTRY
Lt1xDdiEscape(
    _In_ PVOID MiniportDeviceContext,
    _In_ PDXGKARG_ESCAPE Escape)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(Escape);
    DbgPrint("LT1X: DxgkDdiEscape\n");
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
Lt1xDdiCollectDbgInfo(
    _In_ PVOID MiniportDeviceContext,
    _In_ CONST DXGKARG_COLLECTDBGINFO *CollectDbgInfo)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(CollectDbgInfo);
    DbgPrint("LT1X: DxgkDdiCollectDbgInfo\n");
    return STATUS_SUCCESS;
}

static
NTSTATUS
APIENTRY
Lt1xDdiQueryCurrentFence(
    _In_ PVOID MiniportDeviceContext,
    _Inout_ DXGKARG_QUERYCURRENTFENCE *QueryCurrentFence)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    DbgPrint("LT1X: DxgkDdiQueryCurrentFence\n");
    QueryCurrentFence->CurrentFence = 0;
    return STATUS_SUCCESS;
}

static
NTSTATUS
APIENTRY
Lt1xDdiIsSupportedVidPn(
    _In_ PVOID MiniportDeviceContext,
    _Inout_ PDXGKARG_ISSUPPORTEDVIDPN IsSupportedVidPn)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(IsSupportedVidPn);
    DbgPrint("LT1X: DxgkDdiIsSupportedVidPn\n");
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
Lt1xDdiRecommendFunctionalVidPn(
    _In_ PVOID MiniportDeviceContext,
    _In_ PDXGKARG_RECOMMENDFUNCTIONALVIDPN RecommendFunctionalVidPn)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(RecommendFunctionalVidPn);
    DbgPrint("LT1X: DxgkDdiRecommendFunctionalVidPn\n");
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
Lt1xDdiEnumVidPnCofuncModality(
    _In_ PVOID MiniportDeviceContext,
    _In_ CONST DXGKARG_ENUMVIDPNCOFUNCMODALITY *EnumCofuncModality)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(EnumCofuncModality);
    DbgPrint("LT1X: DxgkDdiEnumVidPnCofuncModality\n");
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
Lt1xDdiSetVidPnSourceAddress(
    _In_ PVOID MiniportDeviceContext,
    _In_ CONST DXGKARG_SETVIDPNSOURCEADDRESS *SetVidPnSourceAddress)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(SetVidPnSourceAddress);
    DbgPrint("LT1X: DxgkDdiSetVidPnSourceAddress\n");
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
Lt1xDdiSetVidPnSourceVisibility(
    _In_ PVOID MiniportDeviceContext,
    _In_ CONST DXGKARG_SETVIDPNSOURCEVISIBILITY *SetVidPnSourceVisibility)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(SetVidPnSourceVisibility);
    DbgPrint("LT1X: DxgkDdiSetVidPnSourceVisibility\n");
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
Lt1xDdiCommitVidPn(
    _In_ PVOID MiniportDeviceContext,
    _In_ CONST DXGKARG_COMMITVIDPN *CommitVidPn)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(CommitVidPn);
    DbgPrint("LT1X: DxgkDdiCommitVidPn\n");
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
Lt1xDdiUpdateActiveVidPnPresentPath(
    _In_ PVOID MiniportDeviceContext,
    _In_ CONST DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *UpdateActiveVidPnPresentPath)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(UpdateActiveVidPnPresentPath);
    DbgPrint("LT1X: DxgkDdiUpdateActiveVidPnPresentPath\n");
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
Lt1xDdiRecommendMonitorModes(
    _In_ PVOID MiniportDeviceContext,
    _In_ CONST DXGKARG_RECOMMENDMONITORMODES *RecommendMonitorModes)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(RecommendMonitorModes);
    DbgPrint("LT1X: DxgkDdiRecommendMonitorModes\n");
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
Lt1xDdiRecommendVidPnTopology(
    _In_ PVOID MiniportDeviceContext,
    _In_ CONST DXGKARG_RECOMMENDVIDPNTOPOLOGY *RecommendVidPnTopology)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(RecommendVidPnTopology);
    DbgPrint("LT1X: DxgkDdiRecommendVidPnTopology\n");
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
Lt1xDdiGetScanLine(
    _In_ PVOID MiniportDeviceContext,
    _Inout_ DXGKARG_GETSCANLINE *GetScanLine)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    GetScanLine->InVerticalBlank = TRUE;
    GetScanLine->ScanLine = 0;
    return STATUS_SUCCESS;
}

static
NTSTATUS
APIENTRY
Lt1xDdiStopCapture(
    _In_ PVOID MiniportDeviceContext,
    _In_ CONST DXGKARG_STOPCAPTURE *StopCapture)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(StopCapture);
    DbgPrint("LT1X: DxgkDdiStopCapture\n");
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
Lt1xDdiControlInterrupt(
    _In_ PVOID MiniportDeviceContext,
    _In_ CONST DXGKARG_CONTROL_INTERRUPT *ControlInterrupt)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(ControlInterrupt);
    DbgPrint("LT1X: DxgkDdiControlInterrupt\n");
    return STATUS_SUCCESS;
}

static
NTSTATUS
APIENTRY
Lt1xDdiCreateOverlay(
    _In_ PVOID MiniportDeviceContext,
    _In_ PDXGKARG_CREATEOVERLAY CreateOverlay)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(CreateOverlay);
    DbgPrint("LT1X: DxgkDdiCreateOverlay\n");
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
Lt1xDdiDestroyDevice(
    _In_ PVOID MiniportDeviceContext,
    _In_ PVOID DeviceContext)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(DeviceContext);
    DbgPrint("LT1X: DxgkDdiDestroyDevice\n");
    return STATUS_SUCCESS;
}

static
NTSTATUS
APIENTRY
Lt1xDdiOpenAllocation(
    _In_ PVOID MiniportDeviceContext,
    _In_ PVOID DeviceContext,
    _In_ PDXGKARG_OPENALLOCATION OpenAllocation)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(DeviceContext);
    UNREFERENCED_PARAMETER(OpenAllocation);
    DbgPrint("LT1X: DxgkDdiOpenAllocation\n");
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
Lt1xDdiCloseAllocation(
    _In_ PVOID MiniportDeviceContext,
    _In_ PVOID DeviceContext,
    _In_ PVOID AllocationContext)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(DeviceContext);
    UNREFERENCED_PARAMETER(AllocationContext);
    DbgPrint("LT1X: DxgkDdiCloseAllocation\n");
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
Lt1xDdiRender(
    _In_ PVOID MiniportDeviceContext,
    _In_ PVOID DeviceContext,
    _In_ PDXGKARG_RENDER Render)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(DeviceContext);
    UNREFERENCED_PARAMETER(Render);
    DbgPrint("LT1X: DxgkDdiRender\n");
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
Lt1xDdiPresent(
    _In_ PVOID MiniportDeviceContext,
    _In_ PVOID DeviceContext,
    _In_ PDXGKARG_PRESENT Present)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(DeviceContext);
    UNREFERENCED_PARAMETER(Present);
    DbgPrint("LT1X: DxgkDdiPresent\n");
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
Lt1xDdiUpdateOverlay(
    _In_ PVOID MiniportDeviceContext,
    _In_ PVOID DeviceContext,
    _In_ PVOID AllocationContext,
    _In_ PDXGKARG_UPDATEOVERLAY UpdateOverlay)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(DeviceContext);
    UNREFERENCED_PARAMETER(AllocationContext);
    UNREFERENCED_PARAMETER(UpdateOverlay);
    DbgPrint("LT1X: DxgkDdiUpdateOverlay\n");
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
Lt1xDdiFlipOverlay(
    _In_ PVOID MiniportDeviceContext,
    _In_ PVOID DeviceContext,
    _In_ PVOID AllocationContext,
    _In_ PDXGKARG_FLIPOVERLAY FlipOverlay)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(DeviceContext);
    UNREFERENCED_PARAMETER(AllocationContext);
    UNREFERENCED_PARAMETER(FlipOverlay);
    DbgPrint("LT1X: DxgkDdiFlipOverlay\n");
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
Lt1xDdiDestroyOverlay(
    _In_ PVOID MiniportDeviceContext,
    _In_ PVOID DeviceContext,
    _In_ PVOID AllocationContext)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(DeviceContext);
    UNREFERENCED_PARAMETER(AllocationContext);
    DbgPrint("LT1X: DxgkDdiDestroyOverlay\n");
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
Lt1xDdiCreateContext(
    _In_ PVOID MiniportDeviceContext,
    _In_ PDXGKARG_CREATECONTEXT CreateContext)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(CreateContext);
    DbgPrint("LT1X: DxgkDdiCreateContext\n");
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
Lt1xDdiDestroyContext(
    _In_ PVOID MiniportDeviceContext,
    _In_ PVOID DeviceContext,
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(DeviceContext);
    UNREFERENCED_PARAMETER(Context);
    DbgPrint("LT1X: DxgkDdiDestroyContext\n");
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
Lt1xDdiLinkDevice(
    _In_ PVOID MiniportDeviceContext,
    _In_ PDXGKARG_LINK_DEVICE LinkDevice)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(LinkDevice);
    DbgPrint("LT1X: DxgkDdiLinkDevice\n");
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
Lt1xDdiSetDisplayPrivateDriverFormat(
    _In_ PVOID MiniportDeviceContext,
    _In_ CONST DXGKARG_SETDISPLAYPRIVATEDRIVERFORMAT *SetDisplayPrivateDriverFormat)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(SetDisplayPrivateDriverFormat);
    DbgPrint("LT1X: DxgkDdiSetDisplayPrivateDriverFormat\n");
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
Lt1xDdiRenderKm(
    _In_ PVOID MiniportDeviceContext,
    _In_ PVOID DeviceContext,
    _In_ PVOID Context,
    _In_ PDXGKARG_RENDER Render)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(DeviceContext);
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Render);
    DbgPrint("LT1X: DxgkDdiRenderKm\n");
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
APIENTRY
Lt1xDdiQueryVidPnHWCapability(
    _In_ PVOID MiniportDeviceContext,
    _In_ CONST DXGKARG_QUERYVIDPNHWCAPABILITY *QueryVidPnHWCapability)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(QueryVidPnHWCapability);
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

    DbgPrint("LT1X: DriverEntry version=0x%lx poison=%lu children=%lu\n",
             Lt1xDdiVersion, Lt1xPoisonTail, Lt1xChildCount);

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

    /* WDDM 1.1-era slots past the Vista prefix; dxgkrnl ignores them for a
     * 1.0 Version, but populating them exercises the prefix contract. */
    InitData.DxgkDdiRenderKm = Lt1xDdiRenderKm;
    InitData.DxgkDdiQueryVidPnHWCapability = Lt1xDdiQueryVidPnHWCapability;

    Status = DxgkInitialize(DriverObject, RegistryPath, &InitData);
    DbgPrint("LT1X: DxgkInitialize returned 0x%08lx\n", Status);
    return Status;
}
