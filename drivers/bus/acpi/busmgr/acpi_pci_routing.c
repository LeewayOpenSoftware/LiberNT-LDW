/*
 * PROJECT:         LiberNT ACPI Bus Manager
 * LICENSE:         GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:         ACPI-based PCI interrupt routing provider for the HAL
 */

#include <precomp.h>
#include <ntddk.h>
#define NDEBUG
#include <debug.h>

#define ACPI_PRT_POOL_TAG 'TRPA'

typedef struct _PRT_MAP_ENTRY
{
    ULONG Segment;
    UCHAR Bus;
    UCHAR Device;
    UCHAR Pin;      /* 1..4 (INTA#..INTD#) */
    ULONG Gsi;
    UCHAR Polarity; /* HAL_ACPI_POLARITY_* */
    UCHAR Trigger;  /* HAL_ACPI_TRIGGER_* */
} PRT_MAP_ENTRY, *PPRT_MAP_ENTRY;

C_ASSERT(sizeof(PRT_MAP_ENTRY) == sizeof(HAL_ACPI_PCI_ROUTE_ENTRY));
C_ASSERT(FIELD_OFFSET(PRT_MAP_ENTRY, Gsi) == FIELD_OFFSET(HAL_ACPI_PCI_ROUTE_ENTRY, Gsi));
C_ASSERT(FIELD_OFFSET(PRT_MAP_ENTRY, Polarity) == FIELD_OFFSET(HAL_ACPI_PCI_ROUTE_ENTRY, Polarity));
C_ASSERT(FIELD_OFFSET(PRT_MAP_ENTRY, Trigger) == FIELD_OFFSET(HAL_ACPI_PCI_ROUTE_ENTRY, TriggerMode));

static PPRT_MAP_ENTRY PrtCache;
static ULONG PrtCacheCount;
static ULONG PrtCacheCapacity;

static
VOID
AcpiPrtResetCache(VOID)
{
    if (PrtCache)
    {
        ExFreePoolWithTag(PrtCache, ACPI_PRT_POOL_TAG);
        PrtCache = NULL;
    }

    PrtCacheCount = 0;
    PrtCacheCapacity = 0;
}

static
BOOLEAN
AcpiPrtEnsureCapacity(
    _In_ ULONG Additional)
{
    ULONG Required;
    ULONG NewCapacity;
    PPRT_MAP_ENTRY NewBuffer;

    if ((PrtCacheCount + Additional) <= PrtCacheCapacity)
    {
        return TRUE;
    }

    Required = PrtCacheCount + Additional;
    NewCapacity = (PrtCacheCapacity != 0) ? PrtCacheCapacity : 64;
    while (NewCapacity < Required)
    {
        NewCapacity *= 2;
    }

    NewBuffer = ExAllocatePoolWithTag(NonPagedPool,
                                      NewCapacity * sizeof(PRT_MAP_ENTRY),
                                      ACPI_PRT_POOL_TAG);
    if (!NewBuffer)
    {
        DPRINT1("ACPI: Failed to grow PCI routing cache to %lu entries\n", NewCapacity);
        return FALSE;
    }

    if (PrtCacheCount)
    {
        RtlCopyMemory(NewBuffer, PrtCache, PrtCacheCount * sizeof(PRT_MAP_ENTRY));
    }

    if (PrtCache)
    {
        ExFreePoolWithTag(PrtCache, ACPI_PRT_POOL_TAG);
    }

    PrtCache = NewBuffer;
    PrtCacheCapacity = NewCapacity;
    return TRUE;
}

static
UCHAR
AcpiPrtMapPolarity(
    _In_ UCHAR AcpiPolarity)
{
    switch (AcpiPolarity)
    {
        case ACPI_ACTIVE_HIGH:
            return HAL_ACPI_POLARITY_HIGH;
        case ACPI_ACTIVE_LOW:
            return HAL_ACPI_POLARITY_LOW;
        case ACPI_ACTIVE_BOTH:
            return HAL_ACPI_POLARITY_BOTH;
        default:
            return HAL_ACPI_POLARITY_LOW;
    }
}

static
UCHAR
AcpiPrtMapTrigger(
    _In_ UCHAR AcpiTrigger)
{
    switch (AcpiTrigger)
    {
        case ACPI_LEVEL_SENSITIVE:
            return HAL_ACPI_TRIGGER_LEVEL;
        case ACPI_EDGE_SENSITIVE:
        default:
            return HAL_ACPI_TRIGGER_EDGE;
    }
}

static
VOID
AcpiPrtInsertOrUpdate(
    _In_ const PRT_MAP_ENTRY *Entry)
{
    for (ULONG i = 0; i < PrtCacheCount; ++i)
    {
        if ((PrtCache[i].Segment == Entry->Segment) &&
            (PrtCache[i].Bus == Entry->Bus) &&
            (PrtCache[i].Device == Entry->Device) &&
            (PrtCache[i].Pin == Entry->Pin))
        {
            PrtCache[i] = *Entry;
            return;
        }
    }

    if (!AcpiPrtEnsureCapacity(1))
    {
        return;
    }

    PrtCache[PrtCacheCount++] = *Entry;
}

static
BOOLEAN
AcpiPrtResolveLink(
    _In_ ACPI_HANDLE LinkHandle,
    _In_ UINT32 SourceIndex,
    _Inout_ PPRT_MAP_ENTRY Entry)
{
    ACPI_BUFFER ResourceBuffer = { ACPI_ALLOCATE_BUFFER, NULL };
    ACPI_STATUS Status;
    BOOLEAN Resolved = FALSE;

    Status = BuspGetCurrentResources(LinkHandle, &ResourceBuffer);
    if (ACPI_FAILURE(Status) || !ResourceBuffer.Pointer)
    {
        if (Status != AE_NOT_FOUND)
        {
            DPRINT1("ACPI: _CRS for PCI link %p failed (0x%X)\n", LinkHandle, Status);
        }
        return FALSE;
    }

    for (ACPI_RESOURCE *Resource = (ACPI_RESOURCE *)ResourceBuffer.Pointer;
         Resource && Resource->Type != ACPI_RESOURCE_TYPE_END_TAG;
         Resource = ACPI_NEXT_RESOURCE(Resource))
    {
        if (Resource->Type == ACPI_RESOURCE_TYPE_EXTENDED_IRQ)
        {
            const ACPI_RESOURCE_EXTENDED_IRQ *ExtIrq = &Resource->Data.ExtendedIrq;
            if (ExtIrq->InterruptCount == 0)
            {
                continue;
            }

            UINT32 Index = (SourceIndex < ExtIrq->InterruptCount) ? SourceIndex : 0;
            Entry->Gsi = (ULONG)ExtIrq->Interrupts[Index];
            Entry->Polarity = AcpiPrtMapPolarity(ExtIrq->Polarity);
            Entry->Trigger = AcpiPrtMapTrigger(ExtIrq->Triggering);
            Resolved = TRUE;
            break;
        }
        else if (Resource->Type == ACPI_RESOURCE_TYPE_IRQ)
        {
            const ACPI_RESOURCE_IRQ *Irq = &Resource->Data.Irq;
            if (Irq->InterruptCount == 0)
            {
                continue;
            }

            UINT32 Index = (SourceIndex < Irq->InterruptCount) ? SourceIndex : 0;
            Entry->Gsi = (ULONG)Irq->Interrupts[Index];
            Entry->Polarity = AcpiPrtMapPolarity(Irq->Polarity);
            Entry->Trigger = AcpiPrtMapTrigger(Irq->Triggering);
            Resolved = TRUE;
            break;
        }
        else if (Resource->Type == ACPI_RESOURCE_TYPE_ADDRESS32)
        {
            /* ACPI resource type 4 (ADDRESS32) can encode GSIs in _CRS for root bridges. */
            const ACPI_RESOURCE_ADDRESS32 *Addr = &Resource->Data.Address32;
            if (Addr->ResourceType == ACPI_MEMORY_RANGE || Addr->ResourceType == ACPI_IO_RANGE)
                continue;

            /* Use Minimum as GSI when Source is empty. */
            Entry->Gsi = (ULONG)Addr->Address.Minimum;
            Entry->Polarity = HAL_ACPI_POLARITY_LOW;
            Entry->Trigger = HAL_ACPI_TRIGGER_LEVEL;
            Resolved = TRUE;
            break;
        }
    }

    ACPI_FREE(ResourceBuffer.Pointer);
    return Resolved;
}

static
ACPI_STATUS
AcpiPrtBuildForHandle(
    _In_ ACPI_HANDLE Handle,
    _In_ ULONG Segment,
    _In_ UCHAR RootBus)
{
    ACPI_BUFFER CrsBuffer = { ACPI_ALLOCATE_BUFFER, NULL };
    ACPI_BUFFER Buffer = { ACPI_ALLOCATE_BUFFER, NULL };
    ACPI_STATUS Status;
    ACPI_RESOURCE *CrsResource;
    ULONG MaxGsi = 0;

    /* Pre-scan _CRS for type-4 IRQ encodings so we can map them later. */
    Status = BuspGetCurrentResources(Handle, &CrsBuffer);
    if (ACPI_SUCCESS(Status) && CrsBuffer.Pointer)
    {
        for (CrsResource = (ACPI_RESOURCE *)CrsBuffer.Pointer;
             CrsResource && CrsResource->Type != ACPI_RESOURCE_TYPE_END_TAG;
             CrsResource = ACPI_NEXT_RESOURCE(CrsResource))
        {
            if (CrsResource->Type == ACPI_RESOURCE_TYPE_ADDRESS32)
            {
                const ACPI_RESOURCE_ADDRESS32 *Addr = &CrsResource->Data.Address32;
                if ((Addr->ResourceType == ACPI_MEMORY_RANGE) ||
                    (Addr->ResourceType == ACPI_IO_RANGE))
                    continue;

                if (Addr->Address.Minimum > MaxGsi)
                    MaxGsi = (ULONG)Addr->Address.Minimum;
            }
        }
    }

    Status = BuspGetIrqRoutingTable(Handle, &Buffer);
    if (ACPI_FAILURE(Status))
    {
        if (Status != AE_NOT_FOUND)
        {
            DPRINT1("ACPI: _PRT query for root %p failed (0x%X)\n", Handle, Status);
        }
        if (CrsBuffer.Pointer)
            ACPI_FREE(CrsBuffer.Pointer);
        return Status;
    }

    /* If _PRT is absent but _CRS had a type-4 IRQ, synthesize a single entry */
    if (Buffer.Pointer == NULL && MaxGsi != 0)
    {
        PRT_MAP_ENTRY Entry = {0};
        Entry.Segment = Segment;
        Entry.Bus = RootBus;
        Entry.Device = 0;
        Entry.Pin = 1;
        Entry.Gsi = MaxGsi;
        Entry.Polarity = HAL_ACPI_POLARITY_LOW;
        Entry.Trigger = HAL_ACPI_TRIGGER_LEVEL;
        AcpiPrtInsertOrUpdate(&Entry);
    }

    for (ACPI_PCI_ROUTING_TABLE *Route = (ACPI_PCI_ROUTING_TABLE *)Buffer.Pointer;
         Route && Route->Length;
         Route = (ACPI_PCI_ROUTING_TABLE *)((PUCHAR)Route + Route->Length))
    {
        PRT_MAP_ENTRY Entry = {0};
        ACPI_HANDLE LinkHandle;

        Entry.Segment = Segment;
        Entry.Bus = RootBus;
        Entry.Device = (UCHAR)((Route->Address >> 16) & 0xFF);
        Entry.Pin = (UCHAR)((Route->Pin & 0x3) + 1); /* INTA#..INTD# */
        Entry.Polarity = HAL_ACPI_POLARITY_LOW;
        Entry.Trigger = HAL_ACPI_TRIGGER_LEVEL;

        if (!Route->Source[0])
        {
            Entry.Gsi = Route->SourceIndex;
            AcpiPrtInsertOrUpdate(&Entry);
            continue;
        }

        Status = AcpiGetHandle(Handle, Route->Source, &LinkHandle);
        if (ACPI_FAILURE(Status))
        {
            DPRINT1("ACPI: Failed to resolve PCI link %s (0x%X)\n", Route->Source, Status);
            continue;
        }

        if (AcpiPrtResolveLink(LinkHandle, Route->SourceIndex, &Entry))
        {
            AcpiPrtInsertOrUpdate(&Entry);
        }
        else
        {
            DPRINT1("ACPI: Unresolved PCI link %s for device %u pin %u\n",
                    Route->Source,
                    Entry.Device,
                    Entry.Pin);
        }
    }

    if (MaxGsi != 0)
    {
        /* HalpRecordPciMaxGsi(&MaxEntry); -- HAL hook not implemented (cherry-pick stub). */
        DPRINT("ACPI: Discovered PCI max GSI %u for seg %u bus %u (HAL recording skipped)\n",
               (unsigned)MaxGsi, (unsigned)Segment, (unsigned)RootBus);
    }

    if (CrsBuffer.Pointer)
        ACPI_FREE(CrsBuffer.Pointer);
    ACPI_FREE(Buffer.Pointer);
    return AE_OK;
}

static
BOOLEAN
NTAPI
HalPciRouteProvider(
    _In_ ULONG Segment,
    _In_ UCHAR Bus,
    _In_ UCHAR Device,
    _In_ UCHAR Function,
    _In_ UCHAR Pin,
    _Out_ PULONG Gsi,
    _Out_opt_ PUCHAR Polarity,
    _Out_opt_ PUCHAR TriggerMode)
{
    UNREFERENCED_PARAMETER(Function);

    for (ULONG i = 0; i < PrtCacheCount; ++i)
    {
        if ((PrtCache[i].Segment == Segment) &&
            (PrtCache[i].Bus == Bus) &&
            (PrtCache[i].Device == Device) &&
            (PrtCache[i].Pin == Pin))
        {
            *Gsi = PrtCache[i].Gsi;
            if (Polarity) *Polarity = PrtCache[i].Polarity;
            if (TriggerMode) *TriggerMode = PrtCache[i].Trigger;
            return TRUE;
        }
    }

    return FALSE;
}

/* ----------------------------------------------------------------------------
 *  Bridge-level _PRT support.
 *
 *  ACPI allows a _PRT on any PCI-PCI bridge (including PCI Express root
 *  ports). Firmware that describes the devices behind a root port this way
 *  leaves the root bus _PRT without an entry for the endpoint, so a lookup
 *  that only knows root-bridge tables fails and pci.sys falls back to the
 *  firmware interrupt line, which in APIC mode is a PIC-mode value. Walk the
 *  device objects below each root bridge, read the secondary bus number of
 *  every bridge from configuration space, and cache its _PRT keyed by that
 *  bus so HalPciRouteProvider answers for devices behind it.
 * -------------------------------------------------------------------------- */

static ULONG PrtBridgeEntries;
static ULONG PrtBridgeTables;

static
BOOLEAN
AcpiPrtReadBridgeBuses(
    _In_ ULONG Bus,
    _In_ ULONG Device,
    _In_ ULONG Function,
    _Out_ PUCHAR Secondary,
    _Out_ PUCHAR Subordinate)
{
    PCI_SLOT_NUMBER Slot;
    UCHAR HeaderType;
    UCHAR Buses[2];
    USHORT VendorId;

    Slot.u.AsULONG = 0;
    Slot.u.bits.DeviceNumber = Device;
    Slot.u.bits.FunctionNumber = Function;

    if (HalGetBusDataByOffset(PCIConfiguration, Bus, Slot.u.AsULONG, &VendorId,
                              FIELD_OFFSET(PCI_COMMON_CONFIG, VendorID), sizeof(VendorId)) != sizeof(VendorId) ||
        VendorId == PCI_INVALID_VENDORID || VendorId == 0)
    {
        return FALSE;
    }

    if (HalGetBusDataByOffset(PCIConfiguration, Bus, Slot.u.AsULONG, &HeaderType,
                              FIELD_OFFSET(PCI_COMMON_CONFIG, HeaderType), sizeof(HeaderType)) != sizeof(HeaderType))
    {
        return FALSE;
    }

    HeaderType &= ~PCI_MULTIFUNCTION;
    if (HeaderType != PCI_BRIDGE_TYPE && HeaderType != PCI_CARDBUS_BRIDGE_TYPE)
        return FALSE;

    if (HalGetBusDataByOffset(PCIConfiguration, Bus, Slot.u.AsULONG, Buses,
                              FIELD_OFFSET(PCI_COMMON_CONFIG, u.type1.SecondaryBus), sizeof(Buses)) != sizeof(Buses))
    {
        return FALSE;
    }

    *Secondary = Buses[0];
    *Subordinate = Buses[1];
    return TRUE;
}

static
VOID
AcpiPrtWalkBridges(
    _In_ ACPI_HANDLE Parent,
    _In_ ULONG Segment,
    _In_ UCHAR ParentBus,
    _In_ ULONG Depth)
{
    ACPI_HANDLE Child = NULL;

    if (Depth > 8)
        return;

    while (ACPI_SUCCESS(AcpiGetNextObject(ACPI_TYPE_DEVICE, Parent, Child, &Child)))
    {
        ACPI_OBJECT Result;
        ACPI_BUFFER Buffer = { sizeof(Result), &Result };
        ACPI_HANDLE PrtHandle;
        ULONG Device, Function, Function0, FunctionLimit, Fn;
        UCHAR Secondary = 0, Subordinate = 0;
        BOOLEAN IsBridge = FALSE;

        if (ACPI_FAILURE(AcpiEvaluateObjectTyped(Child, METHOD_NAME__ADR, NULL, &Buffer, ACPI_TYPE_INTEGER)))
            continue;

        Device = (ULONG)(Result.Integer.Value >> 16) & 0xFFFF;
        Function = (ULONG)Result.Integer.Value & 0xFFFF;
        if (Device >= PCI_MAX_DEVICES)
            continue;

        /* _ADR function 0xFFFF means "any function"; probe them all. */
        Function0 = (Function == 0xFFFF) ? 0 : Function;
        FunctionLimit = (Function == 0xFFFF) ? PCI_MAX_FUNCTION : Function + 1;
        if (FunctionLimit > PCI_MAX_FUNCTION)
            continue;

        for (Fn = Function0; Fn < FunctionLimit; Fn++)
        {
            if (AcpiPrtReadBridgeBuses(ParentBus, Device, Fn, &Secondary, &Subordinate))
            {
                IsBridge = TRUE;
                break;
            }
        }

        if (!IsBridge || Secondary <= ParentBus)
            continue;

        if (ACPI_SUCCESS(AcpiGetHandle(Child, METHOD_NAME__PRT, &PrtHandle)))
        {
            ULONG Before = PrtCacheCount;
            ACPI_STATUS Status = AcpiPrtBuildForHandle(Child, Segment, Secondary);
            if (ACPI_SUCCESS(Status))
            {
                PrtBridgeTables++;
                PrtBridgeEntries += PrtCacheCount - Before;
                DPRINT1("ACPI: bridge %02x:%02x.%lu _PRT -> %lu route(s) for bus %u (seg %lu)\n",
                        ParentBus, Device, Fn, PrtCacheCount - Before, Secondary, Segment);
            }
            else
            {
                DPRINT1("ACPI: bridge %02x:%02x.%lu _PRT evaluation failed (0x%X)\n",
                        ParentBus, Device, Fn, Status);
            }
        }

        AcpiPrtWalkBridges(Child, Segment, Secondary, Depth + 1);
    }
}

static
ACPI_STATUS
AcpiEnumRootBridgeCallback(
    _In_ ACPI_HANDLE ObjHandle,
    _In_ UINT32 NestingLevel,
    _Inout_opt_ PVOID Context,
    _Inout_opt_ PVOID *ReturnValue)
{
    ACPI_DEVICE_INFO *Info = NULL;
    ACPI_STATUS Status;
    ACPI_OBJECT Result;
    ACPI_BUFFER Buffer = { sizeof(Result), &Result };
    ULONG Segment = 0;
    ULONG Bus = 0;

    UNREFERENCED_PARAMETER(NestingLevel);
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(ReturnValue);

    Status = AcpiGetObjectInfo(ObjHandle, &Info);
    if (ACPI_FAILURE(Status))
    {
        return AE_OK;
    }

    Status = AcpiEvaluateObjectTyped(ObjHandle, METHOD_NAME__SEG, NULL, &Buffer, ACPI_TYPE_INTEGER);
    if (ACPI_SUCCESS(Status))
    {
        Segment = (ULONG)Result.Integer.Value;
    }

    Status = AcpiEvaluateObjectTyped(ObjHandle, METHOD_NAME__BBN, NULL, &Buffer, ACPI_TYPE_INTEGER);
    if (ACPI_SUCCESS(Status))
    {
        Bus = (ULONG)Result.Integer.Value;
    }

    AcpiOsFree(Info);

    if (Bus > 0xFF)
    {
        DPRINT1("ACPI: Root bus number %lu out of range, truncating\n", Bus);
        Bus &= 0xFF;
    }

    AcpiPrtBuildForHandle(ObjHandle, Segment, (UCHAR)Bus);
    AcpiPrtWalkBridges(ObjHandle, Segment, (UCHAR)Bus, 0);
    return AE_OK;
}

int
acpi_pci_link_init(VOID)
{
    AcpiPrtResetCache();
    PrtBridgeEntries = 0;
    PrtBridgeTables = 0;

    (void)AcpiGetDevices("PNP0A03", AcpiEnumRootBridgeCallback, NULL, NULL);
    (void)AcpiGetDevices("PNP0A08", AcpiEnumRootBridgeCallback, NULL, NULL);

    HalpRegisterPciRouteQuery(HalPciRouteProvider);
    DPRINT1("ACPI: PCI routing provider registered: %lu route(s), %lu from %lu bridge-level _PRT table(s)\n",
            PrtCacheCount, PrtBridgeEntries, PrtBridgeTables);

    return 0;
}

void
acpi_pci_link_exit(VOID)
{
    HalpRegisterPciRouteQuery(NULL);
    AcpiPrtResetCache();
}
