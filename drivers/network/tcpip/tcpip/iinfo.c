/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS TCP/IP protocol driver
 * FILE:        tcpip/iinfo.c
 * PURPOSE:     Per-interface information.
 * PROGRAMMERS: Art Yerkes
 * REVISIONS:
 *   CSH 01/08-2000 Created
 */

#include "precomp.h"

#include <ipifcons.h>

/* See iptypes.h */
#define MAX_ADAPTER_DESCRIPTION_LENGTH 128

TDI_STATUS InfoTdiQueryGetIFInfo(PVOID Context,
                                 PNDIS_BUFFER Buffer,
                                 PUINT BufferSize)
{
    IP_ADDRESS Address;
    PIP_INTERFACE Interface;
    IPInterfaceInfo *InterfaceInfo;
    ULONG Size;
    TDI_STATUS Status;

    AddrInitIPv4(&Address, (ULONG)*(PULONG_PTR)Context);

    Interface = AddrLocateInterface(&Address);
    if (!Interface)
        return TDI_INVALID_PARAMETER;

    Size = FIELD_OFFSET(IPInterfaceInfo, iii_addr) + Interface->AddressLength;
    InterfaceInfo = ExAllocatePoolWithTag(NonPagedPool, Size, OUT_DATA_TAG);
    if (!InterfaceInfo)
    {
        IPDereferenceInterface(Interface);
        return TDI_NO_RESOURCES;
    }

    InterfaceInfo->iii_flags = 0;
    InterfaceInfo->iii_mtu = Interface->MTU;
    InterfaceInfo->iii_speed = Interface->Speed;
    InterfaceInfo->iii_addrlength = Interface->AddressLength;
    RtlCopyMemory(InterfaceInfo->iii_addr, Interface->Address, Interface->AddressLength);

    IPDereferenceInterface(Interface);

    Status = InfoCopyOut((PCHAR)InterfaceInfo, Size, Buffer, BufferSize);

    ExFreePoolWithTag(InterfaceInfo, OUT_DATA_TAG);

    return Status;
}

static VOID IfInfoDetectType(PLAN_ADAPTER IF, PIF_INFO_ROW Row)
{
    ULONG Medium;
    NDIS_STATUS NdisStatus;

    NdisStatus = NDISCall(IF,
                          NdisRequestQueryInformation,
                          OID_GEN_PHYSICAL_MEDIUM,
                          &Medium,
                          sizeof(ULONG));
    if (NdisStatus == NDIS_STATUS_SUCCESS)
    {
        switch (Medium)
        {
            case NdisPhysicalMediumWirelessLan:
#if (_WIN32_WINNT >= _WIN32_WINNT_VISTA)
                Row->Type = IF_TYPE_IEEE80211;
#else
                Row->Type = MIB_IF_TYPE_ETHERNET;
#endif
                break;
#if (_WIN32_WINNT >= _WIN32_WINNT_VISTA)
            case NdisPhysicalMediumNative802_11:
                Row->Type = IF_TYPE_IEEE80211;
                break;
#endif
            case NdisPhysicalMedium802_3:
                Row->Type = MIB_IF_TYPE_ETHERNET;
                break;
            case NdisPhysicalMedium802_5:
                Row->Type = MIB_IF_TYPE_TOKENRING;
                break;
        }
    }

    if (Row->Type != MIB_IF_TYPE_OTHER)
        return;

    NdisStatus = NDISCall(IF,
                          NdisRequestQueryInformation,
                          OID_GEN_MEDIA_SUPPORTED,
                          &Medium,
                          sizeof(ULONG));
    if (NdisStatus == NDIS_STATUS_SUCCESS)
    {
        switch (Medium)
        {
            case NdisMedium802_3:
                Row->Type = MIB_IF_TYPE_ETHERNET;
                break;
            case NdisMedium802_5:
                Row->Type = MIB_IF_TYPE_TOKENRING;
                break;
            case NdisMediumFddi:
                Row->Type = MIB_IF_TYPE_FDDI;
                break;
        }
    }
}

static VOID IfInfoParseGuid(PIP_INTERFACE Interface, GUID *Guid)
{
    UNICODE_STRING Text;
    GUID Parsed;
    USHORT Chars = Interface->Name.Length / sizeof(WCHAR);
    USHORT i;

    RtlZeroMemory(Guid, sizeof(*Guid));
    Guid->Data1 = Interface->Index;
    RtlCopyMemory(Guid->Data4 + 2, "NetDev", 6);

    for (i = 0; i < Chars; i++)
    {
        if (Interface->Name.Buffer[i] != L'{')
            continue;
        if (Chars - i < 38)
            return;
        Text.Buffer = &Interface->Name.Buffer[i];
        Text.Length = Text.MaximumLength = 38 * sizeof(WCHAR);
        if (NT_SUCCESS(RtlGUIDFromString(&Text, &Parsed)))
            *Guid = Parsed;
        return;
    }
}

VOID IfInfoFillRow(PIP_INTERFACE Interface, PIF_INFO_ROW Row)
{
    PLAN_ADAPTER IF = (PLAN_ADAPTER)Interface->Context;
    NDIS_STATUS NdisStatus;

    RtlZeroMemory(Row, sizeof(*Row));
    Row->Index = Interface->Index;
    Row->Type = Interface == Loopback ? MIB_IF_TYPE_LOOPBACK : MIB_IF_TYPE_OTHER;
    Row->Mtu = Interface->MTU;
    Row->PhysAddrLength = Interface->AddressLength;
    Row->AdminStatus = MIB_IF_ADMIN_STATUS_UP;
    GetInterfaceConnectionStatus(Interface, &Row->OperStatus);
    Row->PhysicalMedium = Interface->PhysicalMedium;
    IfInfoParseGuid(Interface, &Row->Guid);

    if (IF)
    {
        if (Row->Type == MIB_IF_TYPE_OTHER)
            IfInfoDetectType(IF, Row);

        GetInterfaceSpeed(Interface, &Row->Speed);
        RtlCopyMemory(Row->PhysAddr, Interface->Address,
                      min(Interface->AddressLength, sizeof(Row->PhysAddr)));
        Row->Stats = Interface->Stats;

        NdisStatus = NDISCall(IF,
                              NdisRequestQueryInformation,
                              OID_GEN_XMIT_ERROR,
                              &Row->OutErrors,
                              sizeof(ULONG));
        if (NdisStatus != NDIS_STATUS_SUCCESS)
            Row->OutErrors = 0;

        NdisStatus = NDISCall(IF,
                              NdisRequestQueryInformation,
                              OID_GEN_RCV_ERROR,
                              &Row->InErrors,
                              sizeof(ULONG));
        if (NdisStatus != NDIS_STATUS_SUCCESS)
            Row->InErrors = 0;
    }

    GetInterfaceName(Interface, Row->Description, IFINFO_MAX_DESCRIPTION);
    Row->DescriptionLength = (UINT)strlen(Row->Description);
}

NTSTATUS IfInfoSnapshot(ULONG Capacity, PIF_INFO_ROW Rows, PULONG Count)
{
    PIP_INTERFACE *Refs;
    ULONG Total = 0, Taken = 0, i;
    KIRQL OldIrql;
    IF_LIST_ITER(Interface);

    PAGED_CODE();

    TcpipAcquireSpinLock(&InterfaceListLock, &OldIrql);
    ForEachInterface(Interface) {
        Total++;
    } EndFor(Interface);
    TcpipReleaseSpinLock(&InterfaceListLock, OldIrql);

    *Count = 0;
    if (Total == 0)
        return STATUS_SUCCESS;

    Refs = ExAllocatePoolWithTag(NonPagedPool, Total * sizeof(PIP_INTERFACE), OUT_DATA_TAG);
    if (!Refs)
        return STATUS_INSUFFICIENT_RESOURCES;

    TcpipAcquireSpinLock(&InterfaceListLock, &OldIrql);
    ForEachInterface(Interface) {
        if (Taken < Total && IPReferenceInterface(Interface))
            Refs[Taken++] = Interface;
    } EndFor(Interface);
    TcpipReleaseSpinLock(&InterfaceListLock, OldIrql);

    for (i = 0; i < Taken; i++)
    {
        if (i < Capacity)
            IfInfoFillRow(Refs[i], &Rows[i]);
        IPDereferenceInterface(Refs[i]);
    }
    ExFreePoolWithTag(Refs, OUT_DATA_TAG);

    *Count = Taken;
    return Taken > Capacity ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}

NTSTATUS IfInfoSnapshotByIndex(UINT Index, PIF_INFO_ROW Row)
{
    PIP_INTERFACE Found = NULL;
    KIRQL OldIrql;
    IF_LIST_ITER(Interface);

    PAGED_CODE();

    TcpipAcquireSpinLock(&InterfaceListLock, &OldIrql);
    ForEachInterface(Interface) {
        if (Interface->Index == Index && !Found && IPReferenceInterface(Interface))
            Found = Interface;
    } EndFor(Interface);
    TcpipReleaseSpinLock(&InterfaceListLock, OldIrql);

    if (!Found)
        return STATUS_NOT_FOUND;
    IfInfoFillRow(Found, Row);
    IPDereferenceInterface(Found);
    return STATUS_SUCCESS;
}

TDI_STATUS InfoTdiQueryGetInterfaceMIB(TDIEntityID ID,
				       PIP_INTERFACE Interface,
				       PNDIS_BUFFER Buffer,
				       PUINT BufferSize) {
    TDI_STATUS Status;
    IFEntry* OutData;
    PIF_INFO_ROW Row;
    ULONG Size;

    if (!Interface)
        return TDI_INVALID_PARAMETER;

    TI_DbgPrint(DEBUG_INFO,
		("Getting IFEntry MIB (IF %08x) (%04x:%d)\n",
		 Interface, ID.tei_entity, ID.tei_instance));

    Row = ExAllocatePoolWithTag( NonPagedPool, sizeof(*Row), OUT_DATA_TAG );
    if( !Row ) return TDI_NO_RESOURCES;

    OutData = ExAllocatePoolWithTag( NonPagedPool, FIELD_OFFSET(IFEntry, if_descr[MAX_ADAPTER_DESCRIPTION_LENGTH + 1]), OUT_DATA_TAG );
    if( !OutData ) {
        ExFreePoolWithTag( Row, OUT_DATA_TAG );
        return TDI_NO_RESOURCES;
    }

    RtlZeroMemory( OutData, FIELD_OFFSET(IFEntry, if_descr[MAX_ADAPTER_DESCRIPTION_LENGTH + 1]));

    IfInfoFillRow(Interface, Row);

    OutData->if_index = Row->Index;
    OutData->if_type = Row->Type;
    OutData->if_mtu = Row->Mtu;
    OutData->if_speed = Row->Speed;
    OutData->if_physaddrlen = Row->PhysAddrLength;
    RtlCopyMemory(OutData->if_physaddr, Row->PhysAddr, sizeof(OutData->if_physaddr));
    OutData->if_adminstatus = Row->AdminStatus;
    OutData->if_operstatus = Row->OperStatus;
    OutData->if_inoctets = (ULONG)Row->Stats.InBytes;
    OutData->if_inucastpkts = (ULONG)Row->Stats.InUnicast;
    OutData->if_innucastpkts = (ULONG)Row->Stats.InNUnicast;
    OutData->if_indiscards = (ULONG)Row->Stats.InDiscarded;
    OutData->if_inerrors = Row->InErrors;
    OutData->if_inunknownprotos = (ULONG)Row->Stats.InDiscardedUnknownProto;
    OutData->if_outoctets = (ULONG)Row->Stats.OutBytes;
    OutData->if_outucastpkts = (ULONG)Row->Stats.OutUnicast;
    OutData->if_outnucastpkts = (ULONG)Row->Stats.OutNUnicast;
    OutData->if_outdiscards = (ULONG)Row->Stats.OutDiscarded;
    OutData->if_outerrors = Row->OutErrors;
    OutData->if_descrlen = Row->DescriptionLength;
    RtlCopyMemory(OutData->if_descr, Row->Description, Row->DescriptionLength + 1);
    Size = FIELD_OFFSET(IFEntry, if_descr[OutData->if_descrlen + 1]);

    TI_DbgPrint(DEBUG_INFO, ("Finished IFEntry MIB (%04x:%d) size %d\n",
			    ID.tei_entity, ID.tei_instance, Size));

    Status = InfoCopyOut( (PCHAR)OutData, Size, Buffer, BufferSize );
    ExFreePoolWithTag( OutData, OUT_DATA_TAG );
    ExFreePoolWithTag( Row, OUT_DATA_TAG );

    TI_DbgPrint(DEBUG_INFO,("Returning %x\n", Status));

    return Status;
}

TDI_STATUS InfoTdiQueryGetArptableMIB(TDIEntityID ID,
				      PIP_INTERFACE Interface,
				      PNDIS_BUFFER Buffer,
				      PUINT BufferSize) {
    NTSTATUS Status;
    ULONG Capacity = NBCopyNeighbors( Interface, NULL, 0 );
    ULONG NumNeighbors;
    PIPARP_ENTRY ArpEntries;

    if (Capacity == 0)
        return InfoCopyOut(NULL, 0, NULL, BufferSize);

    for (;;) {
        if (Capacity > MAXULONG / sizeof(IPARP_ENTRY))
            return TDI_NO_RESOURCES;

        ArpEntries = ExAllocatePoolWithTag( NonPagedPool, Capacity * sizeof(IPARP_ENTRY), ARP_ENTRY_TAG );
        if( !ArpEntries ) return STATUS_NO_MEMORY;

        RtlZeroMemory(ArpEntries, Capacity * sizeof(IPARP_ENTRY));
        NumNeighbors = NBCopyNeighbors( Interface, ArpEntries, Capacity );
        if (NumNeighbors <= Capacity)
            break;

        ExFreePoolWithTag( ArpEntries, ARP_ENTRY_TAG );
        Capacity = NumNeighbors;
    }

    if (NumNeighbors == 0)
        Status = InfoCopyOut(NULL, 0, NULL, BufferSize);
    else
        Status = InfoCopyOut( (PVOID)ArpEntries, NumNeighbors * sizeof(IPARP_ENTRY), Buffer, BufferSize );

    ExFreePoolWithTag( ArpEntries, ARP_ENTRY_TAG );

    return Status;
}

TDI_STATUS InfoTdiSetArptableMIB(PIP_INTERFACE IF, PVOID Buffer, UINT BufferSize)
{
    PIPARP_ENTRY ArpEntry = Buffer;
    IP_ADDRESS Address;
    PNEIGHBOR_CACHE_ENTRY NCE;

    if (!Buffer || BufferSize < sizeof(IPARP_ENTRY))
        return TDI_INVALID_PARAMETER;

    AddrInitIPv4(&Address, ArpEntry->LogAddr);

    if ((NCE = NBLocateNeighbor(&Address, IF)))
    {
        NBRemoveNeighbor(NCE);
        NBDereferenceNeighbor(NCE);
    }

    NCE = NBAddNeighbor(IF,
                        &Address,
                        ArpEntry->PhysAddr,
                        ArpEntry->AddrSize,
                        NUD_PERMANENT,
                        0);
    if (NCE)
    {
        NBDereferenceNeighbor(NCE);
        return TDI_SUCCESS;
    }
    else
        return TDI_INVALID_PARAMETER;
}

VOID InsertTDIInterfaceEntity( PIP_INTERFACE Interface ) {
    AddEntity(IF_ENTITY, Interface, IF_MIB);

    AddEntity(AT_ENTITY, Interface,
              (Interface != Loopback) ? AT_ARP : AT_NULL);

    /* FIXME: This is probably wrong */
    AddEntity(CL_NL_ENTITY, Interface, CL_NL_IP);
}

VOID RemoveTDIInterfaceEntity( PIP_INTERFACE Interface ) {
    /* This removes all of them */
    RemoveEntityByContext(Interface);
}
