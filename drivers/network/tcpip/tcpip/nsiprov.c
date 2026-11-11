/*
 * PROJECT:     LiberNT TCP/IP protocol driver
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Network store producer for the NDIS interface tables
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Built outside the driver's precompiled header because the row layouts
 * come from sdk/include/wine/nsi.h, a user-mode header whose socket types
 * clash with the driver's own IP definitions. The typedefs below give it
 * the Win32 names it uses and _WINIOCTL_ keeps the user-mode ioctl header
 * out. Everything the rows need from the interface list arrives through
 * the IF_INFO_ROW snapshot of include/ifinfo.h.
 */

#include <ntifs.h>
#include <ndis.h>
#define NDEBUG
#include <reactos/debug.h>
#include <netiodef.h>
#include <ws2def.h>
#include <ws2ipdef.h>
#include <ipifcons.h>
#include <nsikm.h>
#include <tcpip_undoc.h>

typedef UCHAR BYTE;
typedef ULONG DWORD;
typedef struct _OVERLAPPED OVERLAPPED;
#include <tcpmib.h>
#define WINAPI NTAPI

#define _WINIOCTL_
#include <wine/nsi.h>

#include "ifinfo.h"

#define NSI_PROVIDER_TAG 'pSiT'
#define TI_NSI_MAX_ROWS 4096
#define TI_NSI_LOOPBACK_SPEED 1073741824

static PNSI_PROVIDER TiNsiIfInfo;
static PNSI_PROVIDER TiNsiIndexLuid;

static NET_LUID
TiNsiLuid(
    _In_ const IF_INFO_ROW *Row)
{
    NET_LUID Luid;

    Luid.Value = 0;
    Luid.Info.NetLuidIndex = Row->Index;
    Luid.Info.IfType = Row->Type;
    return Luid;
}

static VOID
TiNsiCountedString(
    _Out_ IF_COUNTED_STRING *String,
    _In_ const IF_INFO_ROW *Row)
{
    ULONG Bytes = 0;

    RtlMultiByteToUnicodeN(String->String, sizeof(String->String) - sizeof(WCHAR), &Bytes,
                           Row->Description, Row->DescriptionLength);
    String->Length = (USHORT)Bytes;
    String->String[Bytes / sizeof(WCHAR)] = 0;
}

static VOID
TiNsiPhysicalAddress(
    _Out_ IF_PHYSICAL_ADDRESS *Address,
    _In_ const IF_INFO_ROW *Row)
{
    Address->Length = (USHORT)min(Row->PhysAddrLength, IFINFO_MAX_PHYSADDR);
    RtlCopyMemory(Address->Address, Row->PhysAddr, Address->Length);
}

static VOID
TiNsiFillRw(
    _In_ const IF_INFO_ROW *Row,
    _Out_ struct nsi_ndis_ifinfo_rw *Rw)
{
    RtlZeroMemory(Rw, sizeof(*Rw));
    Rw->network_guid = Row->Guid;
    Rw->admin_status = NET_IF_ADMIN_STATUS_UP;
    TiNsiCountedString(&Rw->alias, Row);
    TiNsiPhysicalAddress(&Rw->phys_addr, Row);
    TiNsiCountedString(&Rw->name2, Row);
}

static VOID
TiNsiFillDynamic(
    _In_ const IF_INFO_ROW *Row,
    _Out_ struct nsi_ndis_ifinfo_dynamic *Dynamic)
{
    BOOLEAN Up = Row->OperStatus == MIB_IF_OPER_STATUS_OPERATIONAL;

    RtlZeroMemory(Dynamic, sizeof(*Dynamic));
    Dynamic->oper_status = Up ? NET_IF_OPER_STATUS_UP : NET_IF_OPER_STATUS_DOWN;
    Dynamic->media_conn_state = Up ? MediaConnectStateConnected : MediaConnectStateDisconnected;
    Dynamic->mtu = Row->Mtu;
    Dynamic->xmit_speed = (ULONG64)Row->Speed * 100;
    if (Dynamic->xmit_speed == 0 && Row->Type == MIB_IF_TYPE_LOOPBACK)
        Dynamic->xmit_speed = TI_NSI_LOOPBACK_SPEED;
    Dynamic->rcv_speed = Dynamic->xmit_speed;
    Dynamic->in_discards = Row->Stats.InDiscarded;
    Dynamic->in_errors = Row->InErrors;
    Dynamic->in_octets = Row->Stats.InBytes;
    Dynamic->in_ucast_pkts = Row->Stats.InUnicast;
    Dynamic->in_mcast_pkts = Row->Stats.InNUnicast;
    Dynamic->out_octets = Row->Stats.OutBytes;
    Dynamic->out_ucast_pkts = Row->Stats.OutUnicast;
    Dynamic->out_mcast_pkts = Row->Stats.OutNUnicast;
    Dynamic->out_errors = Row->OutErrors;
    Dynamic->out_discards = Row->Stats.OutDiscarded;
    Dynamic->in_ucast_octs = Row->Stats.InBytes;
    Dynamic->out_ucast_octs = Row->Stats.OutBytes;
}

static VOID
TiNsiFillStatic(
    _In_ const IF_INFO_ROW *Row,
    _Out_ struct nsi_ndis_ifinfo_static *Static)
{
    BOOLEAN Loopback = Row->Type == MIB_IF_TYPE_LOOPBACK;

    RtlZeroMemory(Static, sizeof(*Static));
    Static->if_index = Row->Index;
    TiNsiCountedString(&Static->descr, Row);
    Static->type = Row->Type;
    Static->access_type = Loopback ? NET_IF_ACCESS_LOOPBACK : NET_IF_ACCESS_BROADCAST;
    Static->conn_type = NET_IF_CONNECTION_DEDICATED;
    Static->if_guid = Row->Guid;
    Static->conn_present = !Loopback;
    TiNsiPhysicalAddress(&Static->perm_phys_addr, Row);
    Static->flags.hw = !Loopback;
    Static->phys_medium_type = Row->PhysicalMedium;
}

static NTSTATUS NTAPI
TiNsiIfInfoEnumerate(
    _In_opt_ PVOID Context,
    _Inout_ PNSI_ENUMERATE_REQUEST Request)
{
    ULONG Capacity = Request->Count, Count = 0, i;
    PIF_INFO_ROW Rows = NULL;
    NTSTATUS Status;

    PAGED_CODE();
    UNREFERENCED_PARAMETER(Context);

    if (Capacity > TI_NSI_MAX_ROWS)
        Capacity = TI_NSI_MAX_ROWS;
    if (Capacity != 0)
    {
        Rows = ExAllocatePoolWithTag(PagedPool, Capacity * sizeof(IF_INFO_ROW), NSI_PROVIDER_TAG);
        if (Rows == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;
    }
    Status = IfInfoSnapshot(Capacity, Rows, &Count);
    if (Status == STATUS_SUCCESS || Status == STATUS_BUFFER_OVERFLOW)
    {
        for (i = 0; i < Count && i < Capacity; i++)
        {
            if (Request->KeyData != NULL)
                ((NET_LUID *)Request->KeyData)[i] = TiNsiLuid(&Rows[i]);
            if (Request->RwData != NULL)
                TiNsiFillRw(&Rows[i], (struct nsi_ndis_ifinfo_rw *)Request->RwData + i);
            if (Request->DynamicData != NULL)
                TiNsiFillDynamic(&Rows[i], (struct nsi_ndis_ifinfo_dynamic *)Request->DynamicData + i);
            if (Request->StaticData != NULL)
                TiNsiFillStatic(&Rows[i], (struct nsi_ndis_ifinfo_static *)Request->StaticData + i);
        }
        Request->Count = Count;
    }
    if (Rows != NULL)
        ExFreePoolWithTag(Rows, NSI_PROVIDER_TAG);
    return Status;
}

static NTSTATUS NTAPI
TiNsiIfInfoGetAll(
    _In_opt_ PVOID Context,
    _In_ ULONG FirstArg,
    _In_reads_bytes_(KeySize) const VOID *Key,
    _In_ ULONG KeySize,
    _Out_writes_bytes_opt_(sizeof(struct nsi_ndis_ifinfo_rw)) PVOID Rw,
    _Out_writes_bytes_opt_(sizeof(struct nsi_ndis_ifinfo_dynamic)) PVOID Dynamic,
    _Out_writes_bytes_opt_(sizeof(struct nsi_ndis_ifinfo_static)) PVOID Static)
{
    const NET_LUID *Luid = Key;
    IF_INFO_ROW Row;
    NTSTATUS Status;

    PAGED_CODE();
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(FirstArg);

    if (KeySize != sizeof(NET_LUID))
        return STATUS_INVALID_PARAMETER;
    Status = IfInfoSnapshotByIndex(Luid->Info.NetLuidIndex, &Row);
    if (!NT_SUCCESS(Status))
        return Status;
    if (TiNsiLuid(&Row).Value != Luid->Value)
        return STATUS_NOT_FOUND;
    if (Rw != NULL)
        TiNsiFillRw(&Row, Rw);
    if (Dynamic != NULL)
        TiNsiFillDynamic(&Row, Dynamic);
    if (Static != NULL)
        TiNsiFillStatic(&Row, Static);
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI
TiNsiIndexLuidGetAll(
    _In_opt_ PVOID Context,
    _In_ ULONG FirstArg,
    _In_reads_bytes_(KeySize) const VOID *Key,
    _In_ ULONG KeySize,
    _Out_writes_bytes_opt_(0) PVOID Rw,
    _Out_writes_bytes_opt_(0) PVOID Dynamic,
    _Out_writes_bytes_opt_(sizeof(NET_LUID)) PVOID Static)
{
    IF_INFO_ROW Row;
    NTSTATUS Status;

    PAGED_CODE();
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(FirstArg);
    UNREFERENCED_PARAMETER(Rw);
    UNREFERENCED_PARAMETER(Dynamic);

    if (KeySize != sizeof(UINT))
        return STATUS_INVALID_PARAMETER;
    Status = IfInfoSnapshotByIndex(*(const UINT *)Key, &Row);
    if (!NT_SUCCESS(Status))
        return Status;
    if (Static != NULL)
        *(NET_LUID *)Static = TiNsiLuid(&Row);
    return STATUS_SUCCESS;
}

static const NSI_PROVIDER_DISPATCH TiNsiIfInfoDispatch =
{
    sizeof(NSI_PROVIDER_DISPATCH),
    { sizeof(NET_LUID), sizeof(struct nsi_ndis_ifinfo_rw), sizeof(struct nsi_ndis_ifinfo_dynamic), sizeof(struct nsi_ndis_ifinfo_static) },
    TiNsiIfInfoEnumerate,
    TiNsiIfInfoGetAll,
    NULL
};

static const NSI_PROVIDER_DISPATCH TiNsiIndexLuidDispatch =
{
    sizeof(NSI_PROVIDER_DISPATCH),
    { sizeof(UINT), 0, 0, sizeof(NET_LUID) },
    NULL,
    TiNsiIndexLuidGetAll,
    NULL
};


typedef int (*TI_NSI_COMPARE)(const VOID *Left, const VOID *Right);

static VOID
TiNsiSort(
    _Inout_updates_bytes_(Count * Size) PVOID Base,
    _In_ ULONG Count,
    _In_ ULONG Size,
    _In_ TI_NSI_COMPARE Compare,
    _In_reads_bytes_(Size) PVOID Scratch)
{
    PUCHAR Rows = Base;
    ULONG i, j;

    for (i = 1; i < Count; i++)
    {
        RtlCopyMemory(Scratch, Rows + i * Size, Size);
        for (j = i; j > 0 && Compare(Rows + (j - 1) * Size, Scratch) > 0; j--)
            RtlCopyMemory(Rows + j * Size, Rows + (j - 1) * Size, Size);
        RtlCopyMemory(Rows + j * Size, Scratch, Size);
    }
}

static NTSTATUS
TiNsiInterfaceKeys(
    _Outptr_result_maybenull_ PIF_INFO_KEY *Keys,
    _Out_ PULONG Count)
{
    PIF_INFO_KEY Buffer = NULL;
    ULONG Capacity = 0, Total = 0, Attempt;
    NTSTATUS Status;

    *Keys = NULL;
    *Count = 0;
    for (Attempt = 0; Attempt < 4; Attempt++)
    {
        Status = IfInfoInterfaceKeys(Buffer, Capacity, &Total);
        if (!NT_SUCCESS(Status) && Status != STATUS_BUFFER_OVERFLOW)
        {
            IfInfoFree(Buffer);
            return Status;
        }
        if (Total <= Capacity)
        {
            *Keys = Buffer;
            *Count = Total;
            return STATUS_SUCCESS;
        }
        IfInfoFree(Buffer);
        Capacity = Total + 4;
        Buffer = ExAllocatePoolWithTag(PagedPool, Capacity * sizeof(IF_INFO_KEY), NSI_PROVIDER_TAG);
        if (Buffer == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;
    }
    ExFreePoolWithTag(Buffer, NSI_PROVIDER_TAG);
    return STATUS_BUFFER_OVERFLOW;
}

static NET_LUID
TiNsiLuidForIndex(
    _In_reads_(Count) const IF_INFO_KEY *Keys,
    _In_ ULONG Count,
    _In_ UINT Index)
{
    NET_LUID Luid;
    ULONG i;

    Luid.Value = 0;
    Luid.Info.NetLuidIndex = Index;
    Luid.Info.IfType = MIB_IF_TYPE_OTHER;
    for (i = 0; i < Count; i++)
    {
        if (Keys[i].Index == Index)
        {
            Luid.Info.IfType = Keys[i].Type;
            break;
        }
    }
    return Luid;
}

static VOID
TiNsiZoneIndices(
    _Out_writes_(ScopeLevelCount) ULONG *Zones,
    _In_ UINT Index,
    _In_ BOOLEAN Loopback,
    _In_reads_(ScopeLevelCount) const ULONG *Overrides)
{
    UINT Level;

    for (Level = 0; Level < ScopeLevelCount; Level++)
        Zones[Level] = (Level < ScopeLevelAdmin && !Loopback) ? Index : 1;
    Zones[ScopeLevelGlobal] = 0;
    for (Level = 0; Level < ScopeLevelCount; Level++)
    {
        if (Overrides[Level] != 0)
            Zones[Level] = Overrides[Level];
    }
}

static VOID
TiNsiFillIp6Interface(
    _In_ const IP6_INTERFACE_ROW *Row,
    _In_ NET_LUID Luid,
    _Out_opt_ struct nsi_ip_interface_key *Key,
    _Out_opt_ struct nsi_ip_interface_rw *Rw,
    _Out_opt_ struct nsi_ip_interface_dynamic *Dynamic,
    _Out_opt_ struct nsi_ip_interface_static *Static)
{
    if (Key != NULL)
        Key->luid = Luid;
    if (Rw != NULL)
    {
        RtlZeroMemory(Rw, sizeof(*Rw));
        Rw->metric = Row->Metric;
        Rw->base_reachable_time = Row->BaseReachableTime;
        Rw->retransmit_time = Row->RetransmitTime;
        Rw->path_mtu_discovery_timeout = Row->PathMtuDiscoveryTimeout;
        Rw->dad_transmits = Row->DadTransmits;
        Rw->link_local_address_behavior = LinkLocalAlwaysOn;
        Rw->link_local_address_timeout = Row->LinkLocalAddressTimeout;
        Rw->mtu = Row->Loopback ? ~0u : Row->Mtu;
        Rw->site_prefix_len = Row->SitePrefixLength;
        Rw->router_discovery_behaviour = Row->RouterDiscoveryBehavior;
        TiNsiZoneIndices(Rw->zone_indices, Row->Index, Row->Loopback, Row->ZoneIndices);
    }
    if (Dynamic != NULL)
    {
        RtlZeroMemory(Dynamic, sizeof(*Dynamic));
        Dynamic->if_index = Row->Index;
        Dynamic->reachable_time = Row->ReachableTime;
        Dynamic->connected = Row->Connected;
    }
    if (Static != NULL)
        RtlZeroMemory(Static, sizeof(*Static));
}

static int
TiNsiCompareIp6Address(
    _In_ const VOID *Left,
    _In_ const VOID *Right)
{
    const IP6_ADDRESS_ROW *A = Left, *B = Right;

    if (A->Index != B->Index)
        return A->Index < B->Index ? -1 : 1;
    return memcmp(A->Address, B->Address, sizeof(A->Address));
}

static int
TiNsiCompareIp6Route(
    _In_ const VOID *Left,
    _In_ const VOID *Right)
{
    const IP6_ROUTE_ROW *A = Left, *B = Right;
    int Diff = memcmp(A->Prefix, B->Prefix, sizeof(A->Prefix));

    if (Diff != 0)
        return Diff;
    if (A->PrefixLength != B->PrefixLength)
        return A->PrefixLength < B->PrefixLength ? -1 : 1;
    if (A->Index != B->Index)
        return A->Index < B->Index ? -1 : 1;
    return memcmp(A->NextHop, B->NextHop, sizeof(A->NextHop));
}

static NTSTATUS
TiNsiIp6Enumerate(
    _In_ ULONG TableId,
    _Inout_ PNSI_ENUMERATE_REQUEST Request)
{
    PIF_INFO_KEY Keys = NULL;
    PVOID Table = NULL;
    PIP6_TABLE_HEADER Header;
    ULONG KeyCount = 0, Count = 0, Capacity = Request->Count, i;
    NTSTATUS Status;

    Status = TiNsiInterfaceKeys(&Keys, &KeyCount);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = IfInfoIp6Query(TableId, &Table, &Count);
    if (!NT_SUCCESS(Status))
    {
        IfInfoFree(Keys);
        return Status;
    }
    Header = Table;

    switch (TableId)
    {
        case Ip6TableInterfaces:
        {
            IP6_INTERFACE_ROW *Rows = (IP6_INTERFACE_ROW *)(Header + 1);

            for (i = 0; i < Count && i < Capacity; i++)
            {
                TiNsiFillIp6Interface(&Rows[i], TiNsiLuidForIndex(Keys, KeyCount, Rows[i].Index),
                                      Request->KeyData ? (struct nsi_ip_interface_key *)Request->KeyData + i : NULL,
                                      Request->RwData ? (struct nsi_ip_interface_rw *)Request->RwData + i : NULL,
                                      Request->DynamicData ? (struct nsi_ip_interface_dynamic *)Request->DynamicData + i : NULL,
                                      Request->StaticData ? (struct nsi_ip_interface_static *)Request->StaticData + i : NULL);
            }
            break;
        }
        case Ip6TableAddresses:
        {
            IP6_ADDRESS_ROW *Rows = (IP6_ADDRESS_ROW *)(Header + 1), Scratch;

            TiNsiSort(Rows, Count, sizeof(*Rows), TiNsiCompareIp6Address, &Scratch);
            for (i = 0; i < Count && i < Capacity; i++)
            {
                const IP6_ADDRESS_ROW *Row = &Rows[i];

                if (Request->KeyData != NULL)
                {
                    struct nsi_ipv6_unicast_key *Key = (struct nsi_ipv6_unicast_key *)Request->KeyData + i;

                    RtlZeroMemory(Key, sizeof(*Key));
                    Key->luid = TiNsiLuidForIndex(Keys, KeyCount, Row->Index);
                    RtlCopyMemory(&Key->addr, Row->Address, sizeof(Key->addr));
                }
                if (Request->RwData != NULL)
                {
                    struct nsi_ip_unicast_rw *Rw = (struct nsi_ip_unicast_rw *)Request->RwData + i;

                    RtlZeroMemory(Rw, sizeof(*Rw));
                    Rw->preferred_lifetime = Row->PreferredLifetime;
                    Rw->valid_lifetime = Row->ValidLifetime;
                    Rw->prefix_origin = Row->PrefixOrigin;
                    Rw->suffix_origin = Row->SuffixOrigin;
                    Rw->on_link_prefix = Row->PrefixLength;
                }
                if (Request->DynamicData != NULL)
                {
                    struct nsi_ip_unicast_dynamic *Dynamic = (struct nsi_ip_unicast_dynamic *)Request->DynamicData + i;

                    RtlZeroMemory(Dynamic, sizeof(*Dynamic));
                    Dynamic->scope_id = IN6_IS_ADDR_LINKLOCAL((const IN6_ADDR *)Row->Address) ? Row->Index : 0;
                    Dynamic->dad_state = Row->DadState;
                }
                if (Request->StaticData != NULL)
                {
                    struct nsi_ip_unicast_static *Static = (struct nsi_ip_unicast_static *)Request->StaticData + i;

                    RtlZeroMemory(Static, sizeof(*Static));
                    Static->creation_time = Row->CreationTime.QuadPart;
                }
            }
            break;
        }
        case Ip6TableNeighbors:
        {
            IP6_NEIGHBOR_ROW *Rows = (IP6_NEIGHBOR_ROW *)(Header + 1);

            for (i = 0; i < Count && i < Capacity; i++)
            {
                const IP6_NEIGHBOR_ROW *Row = &Rows[i];
                ULONG Length = min(Row->PhysicalAddressLength, IF_MAX_PHYS_ADDRESS_LENGTH);

                if (Request->KeyData != NULL)
                {
                    struct nsi_ipv6_neighbour_key *Key = (struct nsi_ipv6_neighbour_key *)Request->KeyData + i;

                    RtlZeroMemory(Key, sizeof(*Key));
                    Key->luid = Key->luid2 = TiNsiLuidForIndex(Keys, KeyCount, Row->Index);
                    RtlCopyMemory(&Key->addr, Row->Address, sizeof(Key->addr));
                }
                if (Request->RwData != NULL)
                {
                    struct nsi_ip_neighbour_rw *Rw = (struct nsi_ip_neighbour_rw *)Request->RwData + i;

                    RtlZeroMemory(Rw, sizeof(*Rw));
                    RtlCopyMemory(Rw->phys_addr, Row->PhysicalAddress, Length);
                }
                if (Request->DynamicData != NULL)
                {
                    struct nsi_ip_neighbour_dynamic *Dynamic = (struct nsi_ip_neighbour_dynamic *)Request->DynamicData + i;

                    RtlZeroMemory(Dynamic, sizeof(*Dynamic));
                    Dynamic->state = Row->State;
                    Dynamic->flags.is_router = Row->IsRouter;
                    Dynamic->flags.is_unreachable = Row->IsUnreachable;
                    Dynamic->phys_addr_len = (USHORT)Length;
                }
            }
            break;
        }
        case Ip6TableRoutes:
        {
            IP6_ROUTE_ROW *Rows = (IP6_ROUTE_ROW *)(Header + 1), Scratch;

            TiNsiSort(Rows, Count, sizeof(*Rows), TiNsiCompareIp6Route, &Scratch);
            for (i = 0; i < Count && i < Capacity; i++)
            {
                const IP6_ROUTE_ROW *Row = &Rows[i];

                if (Request->KeyData != NULL)
                {
                    struct nsi_ipv6_forward_key *Key = (struct nsi_ipv6_forward_key *)Request->KeyData + i;

                    RtlZeroMemory(Key, sizeof(*Key));
                    RtlCopyMemory(&Key->prefix, Row->Prefix, sizeof(Key->prefix));
                    Key->prefix_len = (BYTE)Row->PrefixLength;
                    Key->luid = Key->luid2 = TiNsiLuidForIndex(Keys, KeyCount, Row->Index);
                    RtlCopyMemory(&Key->next_hop, Row->NextHop, sizeof(Key->next_hop));
                }
                if (Request->RwData != NULL)
                {
                    struct nsi_ip_forward_rw *Rw = (struct nsi_ip_forward_rw *)Request->RwData + i;

                    RtlZeroMemory(Rw, sizeof(*Rw));
                    Rw->valid_lifetime = Row->ValidLifetime;
                    Rw->preferred_lifetime = Row->PreferredLifetime;
                    Rw->metric = Row->Metric;
                    Rw->protocol = Row->Protocol;
                    Rw->loopback = Row->Loopback;
                    Rw->autoconf = Row->Autoconfigure;
                    Rw->publish = Row->Publish;
                    Rw->immortal = Row->Immortal;
                }
                if (Request->DynamicData != NULL)
                {
                    struct nsi_ipv6_forward_dynamic *Dynamic = (struct nsi_ipv6_forward_dynamic *)Request->DynamicData + i;

                    RtlZeroMemory(Dynamic, sizeof(*Dynamic));
                    RtlCopyMemory(&Dynamic->addr2, Row->Prefix, sizeof(Dynamic->addr2));
                }
                if (Request->StaticData != NULL)
                {
                    struct nsi_ip_forward_static *Static = (struct nsi_ip_forward_static *)Request->StaticData + i;

                    RtlZeroMemory(Static, sizeof(*Static));
                    Static->origin = Row->Origin;
                    Static->if_index = Row->Index;
                }
            }
            break;
        }
        default:
            Status = STATUS_INVALID_PARAMETER;
            break;
    }

    IfInfoFree(Table);
    IfInfoFree(Keys);
    if (!NT_SUCCESS(Status))
        return Status;
    Request->Count = Count;
    return Count > Capacity ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}

typedef NTSTATUS (*TI_NSI_ENUMERATE)(_In_ ULONG Table, _Inout_ PNSI_ENUMERATE_REQUEST Request);

static NTSTATUS
TiNsiGetFromEnumeration(
    _In_ TI_NSI_ENUMERATE Enumerate,
    _In_ ULONG Table,
    _In_ const NSI_ROW_SIZES *Sizes,
    _In_reads_bytes_(Sizes->Key) const VOID *Key,
    _Out_writes_bytes_opt_(Sizes->Rw) PVOID Rw,
    _Out_writes_bytes_opt_(Sizes->Dynamic) PVOID Dynamic,
    _Out_writes_bytes_opt_(Sizes->Static) PVOID Static)
{
    NSI_ENUMERATE_REQUEST Request;
    PUCHAR Block = NULL;
    ULONG Count = 0, Capacity = 0, RowSize, i, Attempt;
    NTSTATUS Status = STATUS_NOT_FOUND;

    RowSize = Sizes->Key + (Rw ? Sizes->Rw : 0) + (Dynamic ? Sizes->Dynamic : 0) + (Static ? Sizes->Static : 0);
    for (Attempt = 0; Attempt < 4; Attempt++)
    {
        RtlZeroMemory(&Request, sizeof(Request));
        Request.Count = Capacity;
        if (Capacity != 0)
        {
            Request.KeyData = Block;
            Request.RwData = Rw ? Block + Sizes->Key * Capacity : NULL;
            Request.DynamicData = Dynamic ? Block + (Sizes->Key + (Rw ? Sizes->Rw : 0)) * Capacity : NULL;
            Request.StaticData = Static ? Block + (Sizes->Key + (Rw ? Sizes->Rw : 0) + (Dynamic ? Sizes->Dynamic : 0)) * Capacity : NULL;
        }
        Status = Enumerate(Table, &Request);
        if (!NT_SUCCESS(Status) && Status != STATUS_BUFFER_OVERFLOW)
            break;
        Count = Request.Count;
        if (Count <= Capacity)
        {
            Status = STATUS_NOT_FOUND;
            for (i = 0; i < Count; i++)
            {
                if (RtlCompareMemory(Block + Sizes->Key * i, Key, Sizes->Key) != Sizes->Key)
                    continue;
                if (Rw != NULL)
                    RtlCopyMemory(Rw, (PUCHAR)Request.RwData + Sizes->Rw * i, Sizes->Rw);
                if (Dynamic != NULL)
                    RtlCopyMemory(Dynamic, (PUCHAR)Request.DynamicData + Sizes->Dynamic * i, Sizes->Dynamic);
                if (Static != NULL)
                    RtlCopyMemory(Static, (PUCHAR)Request.StaticData + Sizes->Static * i, Sizes->Static);
                Status = STATUS_SUCCESS;
                break;
            }
            break;
        }
        if (Block != NULL)
            ExFreePoolWithTag(Block, NSI_PROVIDER_TAG);
        Capacity = Count + 4;
        if (Capacity > TI_NSI_MAX_ROWS || RowSize == 0 || Capacity > MAXULONG / RowSize)
        {
            Block = NULL;
            Status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        Block = ExAllocatePoolWithTag(PagedPool, Capacity * RowSize, NSI_PROVIDER_TAG);
        if (Block == NULL)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        RtlZeroMemory(Block, Capacity * RowSize);
    }
    if (Block != NULL)
        ExFreePoolWithTag(Block, NSI_PROVIDER_TAG);
    return Status;
}

static NTSTATUS NTAPI
TiNsiIp6InterfaceEnumerate(_In_opt_ PVOID Context, _Inout_ PNSI_ENUMERATE_REQUEST Request)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Context);
    return TiNsiIp6Enumerate(Ip6TableInterfaces, Request);
}

static NTSTATUS NTAPI
TiNsiIp6UnicastEnumerate(_In_opt_ PVOID Context, _Inout_ PNSI_ENUMERATE_REQUEST Request)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Context);
    return TiNsiIp6Enumerate(Ip6TableAddresses, Request);
}

static NTSTATUS NTAPI
TiNsiIp6NeighbourEnumerate(_In_opt_ PVOID Context, _Inout_ PNSI_ENUMERATE_REQUEST Request)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Context);
    return TiNsiIp6Enumerate(Ip6TableNeighbors, Request);
}

static NTSTATUS NTAPI
TiNsiIp6ForwardEnumerate(_In_opt_ PVOID Context, _Inout_ PNSI_ENUMERATE_REQUEST Request)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Context);
    return TiNsiIp6Enumerate(Ip6TableRoutes, Request);
}

static const NSI_PROVIDER_DISPATCH TiNsiIp6InterfaceDispatch;
static const NSI_PROVIDER_DISPATCH TiNsiIp6UnicastDispatch;
static const NSI_PROVIDER_DISPATCH TiNsiIp6NeighbourDispatch;
static const NSI_PROVIDER_DISPATCH TiNsiIp6ForwardDispatch;

static NTSTATUS NTAPI
TiNsiIp6GetAll(
    _In_opt_ PVOID Context,
    _In_ ULONG FirstArg,
    _In_reads_bytes_(KeySize) const VOID *Key,
    _In_ ULONG KeySize,
    _Out_writes_bytes_opt_(0) PVOID Rw,
    _Out_writes_bytes_opt_(0) PVOID Dynamic,
    _Out_writes_bytes_opt_(0) PVOID Static)
{
    const NSI_PROVIDER_DISPATCH *Dispatch = Context;
    ULONG Table;

    PAGED_CODE();
    UNREFERENCED_PARAMETER(FirstArg);

    if (Dispatch == NULL || KeySize != Dispatch->RowSizes.Key)
        return STATUS_INVALID_PARAMETER;
    if (Dispatch == &TiNsiIp6InterfaceDispatch)
        Table = Ip6TableInterfaces;
    else if (Dispatch == &TiNsiIp6UnicastDispatch)
        Table = Ip6TableAddresses;
    else if (Dispatch == &TiNsiIp6NeighbourDispatch)
        Table = Ip6TableNeighbors;
    else if (Dispatch == &TiNsiIp6ForwardDispatch)
        Table = Ip6TableRoutes;
    else
        return STATUS_INVALID_PARAMETER;
    return TiNsiGetFromEnumeration(TiNsiIp6Enumerate, Table, &Dispatch->RowSizes, Key, Rw, Dynamic, Static);
}

static const NSI_PROVIDER_DISPATCH TiNsiIp6InterfaceDispatch =
{
    sizeof(NSI_PROVIDER_DISPATCH),
    { sizeof(struct nsi_ip_interface_key), sizeof(struct nsi_ip_interface_rw), sizeof(struct nsi_ip_interface_dynamic), sizeof(struct nsi_ip_interface_static) },
    TiNsiIp6InterfaceEnumerate,
    TiNsiIp6GetAll,
    NULL
};

static const NSI_PROVIDER_DISPATCH TiNsiIp6UnicastDispatch =
{
    sizeof(NSI_PROVIDER_DISPATCH),
    { sizeof(struct nsi_ipv6_unicast_key), sizeof(struct nsi_ip_unicast_rw), sizeof(struct nsi_ip_unicast_dynamic), sizeof(struct nsi_ip_unicast_static) },
    TiNsiIp6UnicastEnumerate,
    TiNsiIp6GetAll,
    NULL
};

static const NSI_PROVIDER_DISPATCH TiNsiIp6NeighbourDispatch =
{
    sizeof(NSI_PROVIDER_DISPATCH),
    { sizeof(struct nsi_ipv6_neighbour_key), sizeof(struct nsi_ip_neighbour_rw), sizeof(struct nsi_ip_neighbour_dynamic), 0 },
    TiNsiIp6NeighbourEnumerate,
    TiNsiIp6GetAll,
    NULL
};

static const NSI_PROVIDER_DISPATCH TiNsiIp6ForwardDispatch =
{
    sizeof(NSI_PROVIDER_DISPATCH),
    { sizeof(struct nsi_ipv6_forward_key), sizeof(struct nsi_ip_forward_rw), sizeof(struct nsi_ipv6_forward_dynamic), sizeof(struct nsi_ip_forward_static) },
    TiNsiIp6ForwardEnumerate,
    TiNsiIp6GetAll,
    NULL
};

static PNSI_PROVIDER TiNsiIp6Providers[4];

static VOID
TiNsiRegisterIp6(VOID)
{
    NTSTATUS Status;
    static const struct
    {
        ULONG Table;
        const NSI_PROVIDER_DISPATCH *Dispatch;
    } Tables[4] =
    {
        { NSI_IP_INTERFACE_TABLE, &TiNsiIp6InterfaceDispatch },
        { NSI_IP_UNICAST_TABLE, &TiNsiIp6UnicastDispatch },
        { NSI_IP_NEIGHBOUR_TABLE, &TiNsiIp6NeighbourDispatch },
        { NSI_IP_FORWARD_TABLE, &TiNsiIp6ForwardDispatch },
    };
    ULONG i;

    for (i = 0; i < 4; i++)
    {
        Status = NsiRegisterProvider(&NPI_MS_IPV6_MODULEID, Tables[i].Table, Tables[i].Dispatch,
                                     (PVOID)Tables[i].Dispatch, &TiNsiIp6Providers[i]);
        if (!NT_SUCCESS(Status))
        {
            TiNsiIp6Providers[i] = NULL;
            DPRINT1("NSI: registering IPv6 table %lu failed, status 0x%08lx\n", Tables[i].Table, Status);
        }
    }
}

static VOID
TiNsiDeregisterIp6(VOID)
{
    ULONG i;

    for (i = 0; i < 4; i++)
    {
        if (TiNsiIp6Providers[i] != NULL)
        {
            NsiDeregisterProvider(TiNsiIp6Providers[i]);
            TiNsiIp6Providers[i] = NULL;
        }
    }
}


#define TI_NSI_IP4_INTERFACE 0
#define TI_NSI_IP4_UNICAST 1
#define TI_NSI_IP4_NEIGHBOUR 2
#define TI_NSI_IP4_FORWARD 3

static UINT
TiNsiPrefixLength(
    _In_ ULONG Mask)
{
    ULONG Value = RtlUlongByteSwap(Mask);
    UINT Length = 0;

    while (Value & 0x80000000)
    {
        Length++;
        Value <<= 1;
    }
    return Length;
}

static VOID
TiNsiFillIp4Interface(
    _In_ const IF_INFO_KEY *Key,
    _In_opt_ const IP4_INTERFACE_INFO *Info,
    _In_ NET_LUID Luid,
    _Out_opt_ struct nsi_ip_interface_key *OutKey,
    _Out_opt_ struct nsi_ip_interface_rw *Rw,
    _Out_opt_ struct nsi_ip_interface_dynamic *Dynamic,
    _Out_opt_ struct nsi_ip_interface_static *Static)
{
    static const ULONG NoOverrides[ScopeLevelCount] = { 0 };

    if (OutKey != NULL)
        OutKey->luid = Luid;
    if (Rw != NULL)
    {
        RtlZeroMemory(Rw, sizeof(*Rw));
        Rw->metric = Info ? Info->Metric : 0;
        Rw->base_reachable_time = Info ? Info->BaseReachableTime : 30000;
        Rw->retransmit_time = Info ? Info->RetransmitTime : 1000;
        Rw->path_mtu_discovery_timeout = Info ? Info->PathMtuDiscoveryTimeout : 600000;
        Rw->dad_transmits = Key->Loopback ? 0 : Info ? Info->DadTransmits : 3;
        Rw->router_discovery_behaviour = Info ? Info->RouterDiscoveryBehavior : RouterDiscoveryDhcp;
        Rw->link_local_address_behavior = LinkLocalDelayed;
        Rw->link_local_address_timeout = Info ? Info->LinkLocalAddressTimeout : 6500;
        Rw->mtu = Key->Loopback ? ~0u : Info ? Info->Mtu : Key->Mtu;
        Rw->site_prefix_len = Info ? Info->SitePrefixLength : 64;
        TiNsiZoneIndices(Rw->zone_indices, Key->Index, Key->Loopback, Info ? Info->ZoneIndices : NoOverrides);
    }
    if (Dynamic != NULL)
    {
        RtlZeroMemory(Dynamic, sizeof(*Dynamic));
        Dynamic->if_index = Key->Index;
        Dynamic->reachable_time = Info ? Info->ReachableTime : 30000;
        Dynamic->connected = Key->OperStatus == MIB_IF_OPER_STATUS_OPERATIONAL;
    }
    if (Static != NULL)
        RtlZeroMemory(Static, sizeof(*Static));
}

static VOID
TiNsiFillIp4Unicast(
    _In_ const IF_INFO_KEY *Key,
    _In_ const IP4_ADDRESS_ROW *Row,
    _In_ NET_LUID Luid,
    _Out_opt_ struct nsi_ipv4_unicast_key *OutKey,
    _Out_opt_ struct nsi_ip_unicast_rw *Rw,
    _Out_opt_ struct nsi_ip_unicast_dynamic *Dynamic,
    _Out_opt_ struct nsi_ip_unicast_static *Static)
{
    if (OutKey != NULL)
    {
        RtlZeroMemory(OutKey, sizeof(*OutKey));
        OutKey->luid = Luid;
        OutKey->addr.S_un.S_addr = Row->Address;
    }
    if (Rw != NULL)
    {
        RtlZeroMemory(Rw, sizeof(*Rw));
        Rw->preferred_lifetime = ~0u;
        Rw->valid_lifetime = ~0u;
        Rw->prefix_origin = Key->Loopback ? IpPrefixOriginWellKnown : IpPrefixOriginManual;
        Rw->suffix_origin = Key->Loopback ? IpSuffixOriginWellKnown : IpSuffixOriginManual;
        Rw->on_link_prefix = TiNsiPrefixLength(Row->Mask);
    }
    if (Dynamic != NULL)
    {
        RtlZeroMemory(Dynamic, sizeof(*Dynamic));
        Dynamic->dad_state = Row->DadState;
    }
    if (Static != NULL)
    {
        RtlZeroMemory(Static, sizeof(*Static));
        Static->creation_time = Row->CreationTime.QuadPart;
    }
}

static NTSTATUS
TiNsiIp4AddressesOf(
    _In_ UINT Index,
    _Outptr_result_maybenull_ PIP4_ADDRESS_ROW *Rows,
    _Out_ PULONG Count)
{
    PIP4_ADDRESS_ROW Buffer = NULL;
    ULONG Capacity = 0, Total = 0, Attempt;
    NTSTATUS Status;

    *Rows = NULL;
    *Count = 0;
    for (Attempt = 0; Attempt < 4; Attempt++)
    {
        Status = IfInfoIp4Addresses(Index, Buffer, Capacity, &Total);
        if (!NT_SUCCESS(Status) && Status != STATUS_BUFFER_OVERFLOW)
        {
            IfInfoFree(Buffer);
            return Status == STATUS_NOT_FOUND ? STATUS_SUCCESS : Status;
        }
        if (Total <= Capacity)
        {
            *Rows = Buffer;
            *Count = Total;
            return STATUS_SUCCESS;
        }
        IfInfoFree(Buffer);
        Capacity = Total + 2;
        Buffer = ExAllocatePoolWithTag(PagedPool, Capacity * sizeof(IP4_ADDRESS_ROW), NSI_PROVIDER_TAG);
        if (Buffer == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;
    }
    ExFreePoolWithTag(Buffer, NSI_PROVIDER_TAG);
    return STATUS_BUFFER_OVERFLOW;
}

static NTSTATUS
TiNsiIp4Enumerate(
    _In_ ULONG Table,
    _Inout_ PNSI_ENUMERATE_REQUEST Request);

#define TI_NSI_DERIVED_METRIC 256
#define TI_NSI_MULTICAST_PREFIX 0xe0000000
#define TI_NSI_MULTICAST_MASK 0xf0000000

typedef struct _TI_NSI_IP4_ROUTE
{
    ULONG Destination;
    ULONG Mask;
    ULONG NextHop;
    UINT IfIndex;
    NET_LUID Luid;
    ULONG Metric;
    ULONG Protocol;
    ULONG Origin;
    BOOLEAN Loopback;
    BOOLEAN Immortal;
    ULONG Lifetime;
    ULONG Age;
} TI_NSI_IP4_ROUTE, *PTI_NSI_IP4_ROUTE;

typedef struct _TI_NSI_IP4_ROUTES
{
    PTI_NSI_IP4_ROUTE Routes;
    ULONG Count;
    ULONG Capacity;
} TI_NSI_IP4_ROUTES, *PTI_NSI_IP4_ROUTES;

static VOID
TiNsiFillIp4Neighbour(
    _In_ const IP4_NEIGHBOR_ROW *Row,
    _In_ NET_LUID Luid,
    _Out_opt_ struct nsi_ipv4_neighbour_key *OutKey,
    _Out_opt_ struct nsi_ip_neighbour_rw *Rw,
    _Out_opt_ struct nsi_ip_neighbour_dynamic *Dynamic)
{
    if (OutKey != NULL)
    {
        RtlZeroMemory(OutKey, sizeof(*OutKey));
        OutKey->luid = OutKey->luid2 = Luid;
        OutKey->addr.S_un.S_addr = Row->Address;
    }
    if (Rw != NULL)
    {
        RtlZeroMemory(Rw, sizeof(*Rw));
        RtlCopyMemory(Rw->phys_addr, Row->PhysAddr, min(Row->PhysAddrLength, IFINFO_MAX_PHYSADDR));
    }
    if (Dynamic != NULL)
    {
        RtlZeroMemory(Dynamic, sizeof(*Dynamic));
        Dynamic->state = Row->Type == IFINFO_NEIGHBOR_STATIC ? NlnsPermanent : NlnsReachable;
        Dynamic->phys_addr_len = (USHORT)min(Row->PhysAddrLength, IFINFO_MAX_PHYSADDR);
    }
}

static NTSTATUS
TiNsiIp4NeighborsOf(
    _In_ UINT Index,
    _Outptr_result_maybenull_ PIP4_NEIGHBOR_ROW *Rows,
    _Out_ PULONG Count)
{
    PIP4_NEIGHBOR_ROW Buffer = NULL;
    ULONG Capacity = 0, Total = 0, Attempt;
    NTSTATUS Status;

    *Rows = NULL;
    *Count = 0;
    for (Attempt = 0; Attempt < 4; Attempt++)
    {
        Status = IfInfoIp4Neighbors(Index, Buffer, Capacity, &Total);
        if (!NT_SUCCESS(Status) && Status != STATUS_BUFFER_OVERFLOW)
        {
            IfInfoFree(Buffer);
            return Status == STATUS_NOT_FOUND ? STATUS_SUCCESS : Status;
        }
        if (Total <= Capacity)
        {
            *Rows = Buffer;
            *Count = Total;
            return STATUS_SUCCESS;
        }
        IfInfoFree(Buffer);
        Capacity = Total + 8;
        Buffer = ExAllocatePoolWithTag(PagedPool, Capacity * sizeof(IP4_NEIGHBOR_ROW), NSI_PROVIDER_TAG);
        if (Buffer == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;
    }
    ExFreePoolWithTag(Buffer, NSI_PROVIDER_TAG);
    return STATUS_BUFFER_OVERFLOW;
}

static NTSTATUS
TiNsiIp4RoutesOf(
    _In_ UINT Index,
    _Outptr_result_maybenull_ PIP4_ROUTE_ROW *Rows,
    _Out_ PULONG Count)
{
    PIP4_ROUTE_ROW Buffer = NULL;
    ULONG Capacity = 0, Total = 0, Attempt;
    NTSTATUS Status;

    *Rows = NULL;
    *Count = 0;
    for (Attempt = 0; Attempt < 4; Attempt++)
    {
        Status = IfInfoIp4Routes(Index, Buffer, Capacity, &Total);
        if (!NT_SUCCESS(Status) && Status != STATUS_BUFFER_OVERFLOW)
        {
            IfInfoFree(Buffer);
            return Status == STATUS_NOT_FOUND ? STATUS_SUCCESS : Status;
        }
        if (Total <= Capacity)
        {
            *Rows = Buffer;
            *Count = Total;
            return STATUS_SUCCESS;
        }
        IfInfoFree(Buffer);
        Capacity = Total + 8;
        Buffer = ExAllocatePoolWithTag(PagedPool, Capacity * sizeof(IP4_ROUTE_ROW), NSI_PROVIDER_TAG);
        if (Buffer == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;
    }
    ExFreePoolWithTag(Buffer, NSI_PROVIDER_TAG);
    return STATUS_BUFFER_OVERFLOW;
}

static NTSTATUS
TiNsiAddIp4Route(
    _Inout_ PTI_NSI_IP4_ROUTES Set,
    _In_ ULONG Destination,
    _In_ ULONG Mask,
    _In_ ULONG NextHop,
    _In_ UINT IfIndex,
    _In_ NET_LUID Luid,
    _In_ ULONG Metric,
    _In_ ULONG Protocol,
    _In_ ULONG Origin,
    _In_ BOOLEAN Loopback,
    _In_ BOOLEAN Immortal)
{
    PTI_NSI_IP4_ROUTE Route;

    if (Set->Count == Set->Capacity)
    {
        ULONG NewCapacity = Set->Capacity ? Set->Capacity * 2 : 32;
        PTI_NSI_IP4_ROUTE Grown;

        if (NewCapacity > TI_NSI_MAX_ROWS)
            return STATUS_INSUFFICIENT_RESOURCES;
        Grown = ExAllocatePoolWithTag(PagedPool, NewCapacity * sizeof(*Grown), NSI_PROVIDER_TAG);
        if (Grown == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;
        if (Set->Routes != NULL)
        {
            RtlCopyMemory(Grown, Set->Routes, Set->Count * sizeof(*Grown));
            ExFreePoolWithTag(Set->Routes, NSI_PROVIDER_TAG);
        }
        Set->Routes = Grown;
        Set->Capacity = NewCapacity;
    }
    Route = &Set->Routes[Set->Count++];
    RtlZeroMemory(Route, sizeof(*Route));
    Route->Destination = Destination;
    Route->Mask = Mask;
    Route->NextHop = NextHop;
    Route->IfIndex = IfIndex;
    Route->Luid = Luid;
    Route->Metric = Metric;
    Route->Protocol = Protocol;
    Route->Origin = Origin;
    Route->Loopback = Loopback;
    Route->Immortal = Immortal;
    Route->Lifetime = ~0u;
    return STATUS_SUCCESS;
}

static BOOLEAN
TiNsiIp4HasAddress(
    _In_reads_(Count) const IP4_ADDRESS_ROW *Rows,
    _In_ ULONG Count,
    _In_ ULONG Address)
{
    ULONG i;

    for (i = 0; i < Count; i++)
        if (Rows[i].Address == Address)
            return TRUE;
    return FALSE;
}

static BOOLEAN
TiNsiIp4HasLocalRoute(
    _In_reads_(Limit) const IP4_ADDRESS_ROW *Rows,
    _In_ ULONG Limit,
    _In_ ULONG Destination,
    _In_ ULONG Mask)
{
    ULONG i;

    for (i = 0; i < Limit; i++)
    {
        ULONG Address = Rows[i].Address, AddressMask = Rows[i].Mask;

        if (AddressMask != INADDR_NONE && Destination == (Address & AddressMask) && Mask == AddressMask)
            return TRUE;
        if (Destination == Address && Mask == INADDR_NONE)
            return TRUE;
        if (TiNsiPrefixLength(AddressMask) < 31 && Destination == (Address | ~AddressMask) && Mask == INADDR_NONE)
            return TRUE;
    }
    return FALSE;
}

static int
TiNsiCompareIp4Route(
    _In_ const VOID *Left,
    _In_ const VOID *Right)
{
    const TI_NSI_IP4_ROUTE *A = Left, *B = Right;
    ULONG DestA = RtlUlongByteSwap(A->Destination), DestB = RtlUlongByteSwap(B->Destination);
    ULONG MaskA = RtlUlongByteSwap(A->Mask), MaskB = RtlUlongByteSwap(B->Mask);

    if (DestA != DestB)
        return DestA < DestB ? -1 : 1;
    if (MaskA != MaskB)
        return MaskA < MaskB ? -1 : 1;
    if (A->IfIndex != B->IfIndex)
        return A->IfIndex < B->IfIndex ? -1 : 1;
    return 0;
}

static NTSTATUS
TiNsiBuildIp4Routes(
    _In_reads_(KeyCount) const IF_INFO_KEY *Keys,
    _In_ ULONG KeyCount,
    _Inout_ PTI_NSI_IP4_ROUTES Set)
{
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG i, j;

    for (i = 0; i < KeyCount && NT_SUCCESS(Status); i++)
    {
        NET_LUID Luid = TiNsiLuidForIndex(Keys, KeyCount, Keys[i].Index);
        PIP4_ADDRESS_ROW Addresses = NULL;
        PIP4_ROUTE_ROW Fibs = NULL;
        ULONG AddressCount = 0, FibCount = 0;

        Status = TiNsiIp4AddressesOf(Keys[i].Index, &Addresses, &AddressCount);
        if (NT_SUCCESS(Status))
            Status = TiNsiIp4RoutesOf(Keys[i].Index, &Fibs, &FibCount);

        for (j = 0; NT_SUCCESS(Status) && j < AddressCount; j++)
        {
            ULONG Address = Addresses[j].Address, Mask = Addresses[j].Mask;

            if (Mask != INADDR_NONE && !TiNsiIp4HasLocalRoute(Addresses, j, Address & Mask, Mask))
                Status = TiNsiAddIp4Route(Set, Address & Mask, Mask, INADDR_ANY, Keys[i].Index, Luid, TI_NSI_DERIVED_METRIC,
                                          MIB_IPPROTO_LOCAL, NlroManual, FALSE, FALSE);
            if (NT_SUCCESS(Status))
                Status = TiNsiAddIp4Route(Set, Address, INADDR_NONE, INADDR_ANY, Keys[i].Index, Luid, TI_NSI_DERIVED_METRIC,
                                          MIB_IPPROTO_LOCAL, NlroManual, TRUE, FALSE);
            if (NT_SUCCESS(Status) && TiNsiPrefixLength(Mask) < 31 &&
                !TiNsiIp4HasLocalRoute(Addresses, j, Address | ~Mask, INADDR_NONE))
                Status = TiNsiAddIp4Route(Set, Address | ~Mask, INADDR_NONE, INADDR_ANY, Keys[i].Index, Luid, TI_NSI_DERIVED_METRIC,
                                          MIB_IPPROTO_LOCAL, NlroManual, TRUE, FALSE);
        }
        if (NT_SUCCESS(Status) && AddressCount != 0)
        {
            Status = TiNsiAddIp4Route(Set, RtlUlongByteSwap(TI_NSI_MULTICAST_PREFIX), RtlUlongByteSwap(TI_NSI_MULTICAST_MASK), INADDR_ANY,
                                      Keys[i].Index, Luid, TI_NSI_DERIVED_METRIC, MIB_IPPROTO_LOCAL, NlroWellKnown, FALSE, FALSE);
            if (NT_SUCCESS(Status))
                Status = TiNsiAddIp4Route(Set, INADDR_NONE, INADDR_NONE, INADDR_ANY, Keys[i].Index, Luid, TI_NSI_DERIVED_METRIC,
                                          MIB_IPPROTO_LOCAL, NlroManual, TRUE, FALSE);
        }

        for (j = 0; NT_SUCCESS(Status) && j < FibCount; j++)
        {
            const IP4_ROUTE_ROW *Fib = &Fibs[j];
            BOOLEAN OnLink = Fib->Gateway == INADDR_ANY || TiNsiIp4HasAddress(Addresses, AddressCount, Fib->Gateway);

            if (OnLink)
            {
                if (TiNsiIp4HasLocalRoute(Addresses, AddressCount, Fib->Destination, Fib->Mask))
                    continue;
                Status = TiNsiAddIp4Route(Set, Fib->Destination, Fib->Mask, INADDR_ANY, Keys[i].Index, Luid, Fib->Metric,
                                          MIB_IPPROTO_NETMGMT, NlroManual, FALSE, TRUE);
                continue;
            }
            Status = TiNsiAddIp4Route(Set, Fib->Destination, Fib->Mask, Fib->Gateway, Keys[i].Index, Luid, Fib->Metric,
                                      MIB_IPPROTO_NETMGMT, NlroManual, FALSE, TRUE);
            if (NT_SUCCESS(Status))
            {
                PTI_NSI_IP4_ROUTE Route = &Set->Routes[Set->Count - 1];

                Route->Age = Fib->Age;
                Route->Lifetime = Fib->ValidLifetime;
                if (Fib->Origin == NlroRouterAdvertisement)
                {
                    Route->Origin = NlroRouterAdvertisement;
                    Route->Immortal = FALSE;
                }
            }
        }

        IfInfoFree(Fibs);
        IfInfoFree(Addresses);
    }
    return Status;
}

static VOID
TiNsiFillIp4Forward(
    _In_ const TI_NSI_IP4_ROUTE *Route,
    _Out_opt_ struct nsi_ipv4_forward_key *OutKey,
    _Out_opt_ struct nsi_ip_forward_rw *Rw,
    _Out_opt_ struct nsi_ipv4_forward_dynamic *Dynamic,
    _Out_opt_ struct nsi_ip_forward_static *Static)
{
    if (OutKey != NULL)
    {
        RtlZeroMemory(OutKey, sizeof(*OutKey));
        OutKey->prefix.S_un.S_addr = Route->Destination;
        OutKey->prefix_len = (BYTE)TiNsiPrefixLength(Route->Mask);
        OutKey->luid = OutKey->luid2 = Route->Luid;
        OutKey->next_hop.S_un.S_addr = Route->NextHop;
    }
    if (Rw != NULL)
    {
        RtlZeroMemory(Rw, sizeof(*Rw));
        Rw->valid_lifetime = Route->Lifetime;
        Rw->preferred_lifetime = Route->Lifetime;
        Rw->metric = Route->Metric;
        Rw->protocol = Route->Protocol;
        Rw->loopback = Route->Loopback;
        Rw->autoconf = TRUE;
        Rw->immortal = Route->Immortal;
    }
    if (Dynamic != NULL)
    {
        RtlZeroMemory(Dynamic, sizeof(*Dynamic));
        Dynamic->age = Route->Age;
        Dynamic->addr2.S_un.S_addr = Route->Destination;
    }
    if (Static != NULL)
    {
        RtlZeroMemory(Static, sizeof(*Static));
        Static->origin = Route->Origin;
        Static->if_index = Route->IfIndex;
    }
}

static NTSTATUS
TiNsiIp4EnumerateNeighbours(
    _In_reads_(KeyCount) const IF_INFO_KEY *Keys,
    _In_ ULONG KeyCount,
    _Inout_ PNSI_ENUMERATE_REQUEST Request)
{
    ULONG Capacity = Request->Count, Written = 0, Total = 0, i, j;
    NTSTATUS Status = STATUS_SUCCESS;

    for (i = 0; i < KeyCount && NT_SUCCESS(Status); i++)
    {
        NET_LUID Luid = TiNsiLuidForIndex(Keys, KeyCount, Keys[i].Index);
        PIP4_NEIGHBOR_ROW Rows = NULL;
        ULONG Count = 0;

        Status = TiNsiIp4NeighborsOf(Keys[i].Index, &Rows, &Count);
        for (j = 0; NT_SUCCESS(Status) && j < Count; j++)
        {
            if (Written < Capacity)
            {
                TiNsiFillIp4Neighbour(&Rows[j], Luid,
                                      Request->KeyData ? (struct nsi_ipv4_neighbour_key *)Request->KeyData + Written : NULL,
                                      Request->RwData ? (struct nsi_ip_neighbour_rw *)Request->RwData + Written : NULL,
                                      Request->DynamicData ? (struct nsi_ip_neighbour_dynamic *)Request->DynamicData + Written : NULL);
                Written++;
            }
            Total++;
        }
        IfInfoFree(Rows);
    }
    if (!NT_SUCCESS(Status))
        return Status;
    Request->Count = Total;
    return Total > Capacity ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}

static NTSTATUS
TiNsiIp4EnumerateRoutes(
    _In_reads_(KeyCount) const IF_INFO_KEY *Keys,
    _In_ ULONG KeyCount,
    _Inout_ PNSI_ENUMERATE_REQUEST Request)
{
    TI_NSI_IP4_ROUTES Set = { NULL, 0, 0 };
    TI_NSI_IP4_ROUTE Scratch;
    ULONG Capacity = Request->Count, i;
    NTSTATUS Status;

    Status = TiNsiBuildIp4Routes(Keys, KeyCount, &Set);
    if (NT_SUCCESS(Status))
    {
        TiNsiSort(Set.Routes, Set.Count, sizeof(*Set.Routes), TiNsiCompareIp4Route, &Scratch);
        for (i = 0; i < Set.Count && i < Capacity; i++)
        {
            TiNsiFillIp4Forward(&Set.Routes[i],
                                Request->KeyData ? (struct nsi_ipv4_forward_key *)Request->KeyData + i : NULL,
                                Request->RwData ? (struct nsi_ip_forward_rw *)Request->RwData + i : NULL,
                                Request->DynamicData ? (struct nsi_ipv4_forward_dynamic *)Request->DynamicData + i : NULL,
                                Request->StaticData ? (struct nsi_ip_forward_static *)Request->StaticData + i : NULL);
        }
        Request->Count = Set.Count;
        Status = Set.Count > Capacity ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
    }
    if (Set.Routes != NULL)
        ExFreePoolWithTag(Set.Routes, NSI_PROVIDER_TAG);
    return Status;
}

static NTSTATUS NTAPI
TiNsiIp4NeighbourEnumerate(_In_opt_ PVOID Context, _Inout_ PNSI_ENUMERATE_REQUEST Request)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Context);
    return TiNsiIp4Enumerate(TI_NSI_IP4_NEIGHBOUR, Request);
}

static NTSTATUS NTAPI
TiNsiIp4ForwardEnumerate(_In_opt_ PVOID Context, _Inout_ PNSI_ENUMERATE_REQUEST Request)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Context);
    return TiNsiIp4Enumerate(TI_NSI_IP4_FORWARD, Request);
}

static NTSTATUS
TiNsiIp4Enumerate(
    _In_ ULONG Table,
    _Inout_ PNSI_ENUMERATE_REQUEST Request)
{
    PIF_INFO_KEY Keys = NULL;
    ULONG KeyCount = 0, Capacity = Request->Count, Written = 0, Total = 0, i, j;
    NTSTATUS Status;

    Status = TiNsiInterfaceKeys(&Keys, &KeyCount);
    if (!NT_SUCCESS(Status))
        return Status;
    if (Table == TI_NSI_IP4_NEIGHBOUR || Table == TI_NSI_IP4_FORWARD)
    {
        Status = Table == TI_NSI_IP4_NEIGHBOUR ? TiNsiIp4EnumerateNeighbours(Keys, KeyCount, Request)
                                               : TiNsiIp4EnumerateRoutes(Keys, KeyCount, Request);
        IfInfoFree(Keys);
        return Status;
    }

    for (i = 0; i < KeyCount && NT_SUCCESS(Status); i++)
    {
        NET_LUID Luid = TiNsiLuidForIndex(Keys, KeyCount, Keys[i].Index);

        switch (Table)
        {
            case TI_NSI_IP4_INTERFACE:
            {
                IP4_INTERFACE_INFO Info;
                BOOLEAN HaveInfo = NT_SUCCESS(IfInfoIp4Interface(Keys[i].Index, &Info));

                if (Written < Capacity)
                {
                    TiNsiFillIp4Interface(&Keys[i], HaveInfo ? &Info : NULL, Luid,
                                          Request->KeyData ? (struct nsi_ip_interface_key *)Request->KeyData + Written : NULL,
                                          Request->RwData ? (struct nsi_ip_interface_rw *)Request->RwData + Written : NULL,
                                          Request->DynamicData ? (struct nsi_ip_interface_dynamic *)Request->DynamicData + Written : NULL,
                                          Request->StaticData ? (struct nsi_ip_interface_static *)Request->StaticData + Written : NULL);
                    Written++;
                }
                Total++;
                break;
            }
            case TI_NSI_IP4_UNICAST:
            {
                PIP4_ADDRESS_ROW Rows;
                ULONG Count;

                Status = TiNsiIp4AddressesOf(Keys[i].Index, &Rows, &Count);
                for (j = 0; NT_SUCCESS(Status) && j < Count; j++)
                {
                    if (Written < Capacity)
                    {
                        TiNsiFillIp4Unicast(&Keys[i], &Rows[j], Luid,
                                            Request->KeyData ? (struct nsi_ipv4_unicast_key *)Request->KeyData + Written : NULL,
                                            Request->RwData ? (struct nsi_ip_unicast_rw *)Request->RwData + Written : NULL,
                                            Request->DynamicData ? (struct nsi_ip_unicast_dynamic *)Request->DynamicData + Written : NULL,
                                            Request->StaticData ? (struct nsi_ip_unicast_static *)Request->StaticData + Written : NULL);
                        Written++;
                    }
                    Total++;
                }
                IfInfoFree(Rows);
                break;
            }
            default:
                Status = STATUS_INVALID_PARAMETER;
                break;
        }
    }

    IfInfoFree(Keys);
    if (!NT_SUCCESS(Status))
        return Status;
    Request->Count = Total;
    return Total > Capacity ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}

static NTSTATUS NTAPI
TiNsiIp4InterfaceEnumerate(_In_opt_ PVOID Context, _Inout_ PNSI_ENUMERATE_REQUEST Request)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Context);
    return TiNsiIp4Enumerate(TI_NSI_IP4_INTERFACE, Request);
}

static NTSTATUS NTAPI
TiNsiIp4UnicastEnumerate(_In_opt_ PVOID Context, _Inout_ PNSI_ENUMERATE_REQUEST Request)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Context);
    return TiNsiIp4Enumerate(TI_NSI_IP4_UNICAST, Request);
}

static const NSI_PROVIDER_DISPATCH TiNsiIp4Dispatch[4];

static NTSTATUS NTAPI
TiNsiIp4GetAll(
    _In_opt_ PVOID Context,
    _In_ ULONG FirstArg,
    _In_reads_bytes_(KeySize) const VOID *Key,
    _In_ ULONG KeySize,
    _Out_writes_bytes_opt_(0) PVOID Rw,
    _Out_writes_bytes_opt_(0) PVOID Dynamic,
    _Out_writes_bytes_opt_(0) PVOID Static)
{
    ULONG Table = (ULONG)(ULONG_PTR)Context;

    PAGED_CODE();
    UNREFERENCED_PARAMETER(FirstArg);

    if (Table >= 4 || KeySize != TiNsiIp4Dispatch[Table].RowSizes.Key)
        return STATUS_INVALID_PARAMETER;
    return TiNsiGetFromEnumeration(TiNsiIp4Enumerate, Table, &TiNsiIp4Dispatch[Table].RowSizes, Key, Rw, Dynamic, Static);
}

static const NSI_PROVIDER_DISPATCH TiNsiIp4Dispatch[4] =
{
    {
        sizeof(NSI_PROVIDER_DISPATCH),
        { sizeof(struct nsi_ip_interface_key), sizeof(struct nsi_ip_interface_rw), sizeof(struct nsi_ip_interface_dynamic), sizeof(struct nsi_ip_interface_static) },
        TiNsiIp4InterfaceEnumerate,
        TiNsiIp4GetAll,
        NULL
    },
    {
        sizeof(NSI_PROVIDER_DISPATCH),
        { sizeof(struct nsi_ipv4_unicast_key), sizeof(struct nsi_ip_unicast_rw), sizeof(struct nsi_ip_unicast_dynamic), sizeof(struct nsi_ip_unicast_static) },
        TiNsiIp4UnicastEnumerate,
        TiNsiIp4GetAll,
        NULL
    },
    {
        sizeof(NSI_PROVIDER_DISPATCH),
        { sizeof(struct nsi_ipv4_neighbour_key), sizeof(struct nsi_ip_neighbour_rw), sizeof(struct nsi_ip_neighbour_dynamic), 0 },
        TiNsiIp4NeighbourEnumerate,
        TiNsiIp4GetAll,
        NULL
    },
    {
        sizeof(NSI_PROVIDER_DISPATCH),
        { sizeof(struct nsi_ipv4_forward_key), sizeof(struct nsi_ip_forward_rw), sizeof(struct nsi_ipv4_forward_dynamic), sizeof(struct nsi_ip_forward_static) },
        TiNsiIp4ForwardEnumerate,
        TiNsiIp4GetAll,
        NULL
    },
};

static const ULONG TiNsiIp4Tables[4] = { NSI_IP_INTERFACE_TABLE, NSI_IP_UNICAST_TABLE, NSI_IP_NEIGHBOUR_TABLE, NSI_IP_FORWARD_TABLE };
static PNSI_PROVIDER TiNsiIp4Providers[4];

static VOID
TiNsiRegisterIp4(VOID)
{
    ULONG i;
    NTSTATUS Status;

    for (i = 0; i < 4; i++)
    {
        if (TiNsiIp4Dispatch[i].Enumerate == NULL)
            continue;
        Status = NsiRegisterProvider(&NPI_MS_IPV4_MODULEID, TiNsiIp4Tables[i], &TiNsiIp4Dispatch[i],
                                     (PVOID)(ULONG_PTR)i, &TiNsiIp4Providers[i]);
        if (!NT_SUCCESS(Status))
        {
            TiNsiIp4Providers[i] = NULL;
            DPRINT1("NSI: registering IPv4 table %lu failed, status 0x%08lx\n", TiNsiIp4Tables[i], Status);
        }
    }
}

static VOID
TiNsiDeregisterIp4(VOID)
{
    ULONG i;

    for (i = 0; i < 4; i++)
    {
        if (TiNsiIp4Providers[i] != NULL)
        {
            NsiDeregisterProvider(TiNsiIp4Providers[i]);
            TiNsiIp4Providers[i] = NULL;
        }
    }
}


#define TI_NSI_TCP_ALL 0
#define TI_NSI_TCP_ESTAB 1
#define TI_NSI_TCP_LISTEN 2
#define TI_NSI_UDP_ENDPOINT 3
#define TI_NSI_ENDPOINT_FIRST_CAPACITY 64

static VOID
TiNsiFillSockaddr(
    _Out_ SOCKADDR_INET *Address,
    _In_ ULONG Family,
    _In_reads_(16) const UCHAR *Bytes,
    _In_ ULONG ScopeId,
    _In_ USHORT Port)
{
    RtlZeroMemory(Address, sizeof(*Address));
    if (Family == TI_NSI_FAMILY_IPV6)
    {
        Address->Ipv6.sin6_family = AF_INET6;
        RtlCopyMemory(&Address->Ipv6.sin6_addr, Bytes, sizeof(Address->Ipv6.sin6_addr));
        Address->Ipv6.sin6_scope_id = ScopeId;
        Address->Ipv6.sin6_port = Port;
    }
    else
    {
        Address->Ipv4.sin_family = AF_INET;
        RtlCopyMemory(&Address->Ipv4.sin_addr, Bytes, sizeof(Address->Ipv4.sin_addr));
        Address->Ipv4.sin_port = Port;
    }
}

static BOOLEAN
TiNsiEndpointWanted(
    _In_ ULONG Table,
    _In_ const ENDPOINT_ROW *Row)
{
    switch (Table)
    {
        case TI_NSI_TCP_ALL:
            return Row->Protocol == IFINFO_PROTOCOL_TCP;
        case TI_NSI_TCP_ESTAB:
            return Row->Protocol == IFINFO_PROTOCOL_TCP && Row->State == MIB_TCP_STATE_ESTAB;
        case TI_NSI_TCP_LISTEN:
            return Row->Protocol == IFINFO_PROTOCOL_TCP && Row->State == MIB_TCP_STATE_LISTEN;
        default:
            return Row->Protocol == IFINFO_PROTOCOL_UDP;
    }
}

static VOID
TiNsiFillEndpoint(
    _In_ ULONG Table,
    _In_ const ENDPOINT_ROW *Row,
    _In_ ULONG Index,
    _Inout_ PNSI_ENUMERATE_REQUEST Request)
{
    if (Table == TI_NSI_UDP_ENDPOINT)
    {
        if (Request->KeyData != NULL)
        {
            struct nsi_udp_endpoint_key *Key = (struct nsi_udp_endpoint_key *)Request->KeyData + Index;

            TiNsiFillSockaddr(&Key->local, Row->Family, Row->LocalAddress, Row->LocalScopeId, Row->LocalPort);
        }
        if (Request->StaticData != NULL)
        {
            struct nsi_udp_endpoint_static *Static = (struct nsi_udp_endpoint_static *)Request->StaticData + Index;

            RtlZeroMemory(Static, sizeof(*Static));
            Static->pid = Row->Pid;
            Static->create_time = Row->CreateTime.QuadPart;
            Static->mod_info = Row->ModuleInfo;
        }
        return;
    }
    if (Request->KeyData != NULL)
    {
        struct nsi_tcp_conn_key *Key = (struct nsi_tcp_conn_key *)Request->KeyData + Index;

        TiNsiFillSockaddr(&Key->local, Row->Family, Row->LocalAddress, Row->LocalScopeId, Row->LocalPort);
        TiNsiFillSockaddr(&Key->remote, Row->Family, Row->RemoteAddress, Row->RemoteScopeId, Row->RemotePort);
    }
    if (Request->DynamicData != NULL)
    {
        struct nsi_tcp_conn_dynamic *Dynamic = (struct nsi_tcp_conn_dynamic *)Request->DynamicData + Index;

        RtlZeroMemory(Dynamic, sizeof(*Dynamic));
        Dynamic->state = Row->State;
    }
    if (Request->StaticData != NULL)
    {
        struct nsi_tcp_conn_static *Static = (struct nsi_tcp_conn_static *)Request->StaticData + Index;

        RtlZeroMemory(Static, sizeof(*Static));
        Static->pid = Row->Pid;
        Static->create_time = Row->CreateTime.QuadPart;
        Static->mod_info = Row->ModuleInfo;
    }
}

static NTSTATUS
TiNsiEndpointEnumerate(
    _In_ ULONG Table,
    _Inout_ PNSI_ENUMERATE_REQUEST Request)
{
    PENDPOINT_ROW Rows = NULL;
    ULONG Protocol = Table == TI_NSI_UDP_ENDPOINT ? IFINFO_PROTOCOL_UDP : IFINFO_PROTOCOL_TCP;
    ULONG Capacity = TI_NSI_ENDPOINT_FIRST_CAPACITY, Count = 0, Available = 0, i, Attempt;
    NTSTATUS Status = STATUS_INSUFFICIENT_RESOURCES;

    PAGED_CODE();

    for (Attempt = 0; Attempt < 4; Attempt++)
    {
        Rows = ExAllocatePoolWithTag(PagedPool, Capacity * sizeof(*Rows), NSI_PROVIDER_TAG);
        if (Rows == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;
        Status = IfInfoEndpoints(IFINFO_FAMILY_ANY, Protocol, Rows, Capacity, &Count);
        if (Status != STATUS_BUFFER_OVERFLOW)
            break;
        ExFreePoolWithTag(Rows, NSI_PROVIDER_TAG);
        Rows = NULL;
        if (Count > TI_NSI_MAX_ROWS)
            return STATUS_INSUFFICIENT_RESOURCES;
        Capacity = Count + 16;
    }
    if (Rows == NULL)
    {
        Request->Count = Count;
        return STATUS_BUFFER_OVERFLOW;
    }
    if (NT_SUCCESS(Status))
    {
        for (i = 0; i < Count; i++)
        {
            if (!TiNsiEndpointWanted(Table, &Rows[i]))
                continue;
            if (Available < Request->Count)
                TiNsiFillEndpoint(Table, &Rows[i], Available, Request);
            Available++;
        }
        Status = Available > Request->Count ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
        Request->Count = Available;
    }
    ExFreePoolWithTag(Rows, NSI_PROVIDER_TAG);
    return Status;
}

static NTSTATUS NTAPI
TiNsiTcpAllEnumerate(_In_opt_ PVOID Context, _Inout_ PNSI_ENUMERATE_REQUEST Request)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Context);
    return TiNsiEndpointEnumerate(TI_NSI_TCP_ALL, Request);
}

static NTSTATUS NTAPI
TiNsiTcpEstabEnumerate(_In_opt_ PVOID Context, _Inout_ PNSI_ENUMERATE_REQUEST Request)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Context);
    return TiNsiEndpointEnumerate(TI_NSI_TCP_ESTAB, Request);
}

static NTSTATUS NTAPI
TiNsiTcpListenEnumerate(_In_opt_ PVOID Context, _Inout_ PNSI_ENUMERATE_REQUEST Request)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Context);
    return TiNsiEndpointEnumerate(TI_NSI_TCP_LISTEN, Request);
}

static NTSTATUS NTAPI
TiNsiUdpEndpointEnumerate(_In_opt_ PVOID Context, _Inout_ PNSI_ENUMERATE_REQUEST Request)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Context);
    return TiNsiEndpointEnumerate(TI_NSI_UDP_ENDPOINT, Request);
}

static const NSI_PROVIDER_DISPATCH TiNsiEndpointDispatch[4];

static NTSTATUS NTAPI
TiNsiEndpointGetAll(
    _In_opt_ PVOID Context,
    _In_ ULONG FirstArg,
    _In_reads_bytes_(KeySize) const VOID *Key,
    _In_ ULONG KeySize,
    _Out_writes_bytes_opt_(0) PVOID Rw,
    _Out_writes_bytes_opt_(0) PVOID Dynamic,
    _Out_writes_bytes_opt_(0) PVOID Static)
{
    ULONG Table = (ULONG)(ULONG_PTR)Context;

    PAGED_CODE();
    UNREFERENCED_PARAMETER(FirstArg);

    if (Table >= 4 || KeySize != TiNsiEndpointDispatch[Table].RowSizes.Key)
        return STATUS_INVALID_PARAMETER;
    return TiNsiGetFromEnumeration(TiNsiEndpointEnumerate, Table, &TiNsiEndpointDispatch[Table].RowSizes, Key, Rw, Dynamic, Static);
}

static const NSI_PROVIDER_DISPATCH TiNsiEndpointDispatch[4] =
{
    {
        sizeof(NSI_PROVIDER_DISPATCH),
        { sizeof(struct nsi_tcp_conn_key), 0, sizeof(struct nsi_tcp_conn_dynamic), sizeof(struct nsi_tcp_conn_static) },
        TiNsiTcpAllEnumerate,
        TiNsiEndpointGetAll,
        NULL
    },
    {
        sizeof(NSI_PROVIDER_DISPATCH),
        { sizeof(struct nsi_tcp_conn_key), 0, sizeof(struct nsi_tcp_conn_dynamic), sizeof(struct nsi_tcp_conn_static) },
        TiNsiTcpEstabEnumerate,
        TiNsiEndpointGetAll,
        NULL
    },
    {
        sizeof(NSI_PROVIDER_DISPATCH),
        { sizeof(struct nsi_tcp_conn_key), 0, sizeof(struct nsi_tcp_conn_dynamic), sizeof(struct nsi_tcp_conn_static) },
        TiNsiTcpListenEnumerate,
        TiNsiEndpointGetAll,
        NULL
    },
    {
        sizeof(NSI_PROVIDER_DISPATCH),
        { sizeof(struct nsi_udp_endpoint_key), 0, 0, sizeof(struct nsi_udp_endpoint_static) },
        TiNsiUdpEndpointEnumerate,
        TiNsiEndpointGetAll,
        NULL
    },
};

static const NPI_MODULEID *const TiNsiEndpointModules[4] = { &NPI_MS_TCP_MODULEID, &NPI_MS_TCP_MODULEID, &NPI_MS_TCP_MODULEID, &NPI_MS_UDP_MODULEID };
static const ULONG TiNsiEndpointTables[4] = { NSI_TCP_ALL_TABLE, NSI_TCP_ESTAB_TABLE, NSI_TCP_LISTEN_TABLE, NSI_UDP_ENDPOINT_TABLE };
static PNSI_PROVIDER TiNsiEndpointProviders[4];

static VOID
TiNsiRegisterEndpoints(VOID)
{
    ULONG i;
    NTSTATUS Status;

    for (i = 0; i < 4; i++)
    {
        Status = NsiRegisterProvider(TiNsiEndpointModules[i], TiNsiEndpointTables[i], &TiNsiEndpointDispatch[i],
                                     (PVOID)(ULONG_PTR)i, &TiNsiEndpointProviders[i]);
        if (!NT_SUCCESS(Status))
        {
            TiNsiEndpointProviders[i] = NULL;
            DPRINT1("NSI: registering %s table %lu failed, status 0x%08lx\n",
                    i == TI_NSI_UDP_ENDPOINT ? "UDP" : "TCP", TiNsiEndpointTables[i], Status);
        }
    }
}

static VOID
TiNsiDeregisterEndpoints(VOID)
{
    ULONG i;

    for (i = 0; i < 4; i++)
    {
        if (TiNsiEndpointProviders[i] != NULL)
        {
            NsiDeregisterProvider(TiNsiEndpointProviders[i]);
            TiNsiEndpointProviders[i] = NULL;
        }
    }
}

/*
 * Change notifications are throttled per (family, table): the first change
 * goes out at once, every further change inside the window is folded into
 * one trailing notification. A consumer re-reads and diffs the table on each
 * notification, so one notification stands for any number of changes.
 */
#define TI_NSI_NOTIFY_COALESCE_MS 100

static KTIMER TiNsiNotifyTimer;
static KDPC TiNsiNotifyDpc;
static volatile LONG TiNsiNotifyPending[2][4];   /* change types folded while throttled */
static volatile LONG TiNsiNotifyThrottled[2][4]; /* a notification went out in this window */
static volatile LONG TiNsiNotifyTimerArmed;
static BOOLEAN TiNsiNotifyReady;

static PNSI_PROVIDER
TiNsiIpProvider(
    _In_ ULONG Family,
    _In_ ULONG Table)
{
    if (Table >= 4)
        return NULL;
    return Family == TI_NSI_FAMILY_IPV6 ? TiNsiIp6Providers[Table] : TiNsiIp4Providers[Table];
}

static VOID
TiNsiNotifyArmTimer(VOID)
{
    LARGE_INTEGER Due;

    if (InterlockedCompareExchange(&TiNsiNotifyTimerArmed, 1, 0) == 0)
    {
        Due.QuadPart = -(LONGLONG)TI_NSI_NOTIFY_COALESCE_MS * 10000;
        KeSetTimer(&TiNsiNotifyTimer, Due, &TiNsiNotifyDpc);
    }
}

static NSI_CHANGE_TYPE
TiNsiFoldedChange(
    _In_ LONG Mask)
{
    BOOLEAN Add = (Mask & (1 << TI_NSI_CHANGE_ADD)) != 0;
    BOOLEAN Delete = (Mask & (1 << TI_NSI_CHANGE_DELETE)) != 0;

    if (Add && Delete)
        return NsiChangeBulk;
    if (Add)
        return NsiChangeAdd;
    if (Delete)
        return NsiChangeDelete;
    return NsiChangeParameter;
}

static VOID NTAPI
TiNsiNotifyDpcRoutine(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    ULONG Family, Table;
    BOOLEAN Rearm = FALSE;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(DeferredContext);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    /* Disarm before scanning so a change folded after the scan arms it again */
    InterlockedExchange(&TiNsiNotifyTimerArmed, 0);
    for (Family = 0; Family < 2; Family++)
    {
        for (Table = 0; Table < 4; Table++)
        {
            LONG Mask = InterlockedExchange(&TiNsiNotifyPending[Family][Table], 0);
            PNSI_PROVIDER Provider;

            if (Mask == 0)
            {
                /* Quiet window: the next change goes out at once again */
                InterlockedExchange(&TiNsiNotifyThrottled[Family][Table], 0);
                continue;
            }
            /* The trailing notification opens a new window */
            Provider = TiNsiIpProvider(Family, Table);
            if (Provider != NULL)
                NsiNotifyChange(Provider, TiNsiFoldedChange(Mask), NULL, 0);
            Rearm = TRUE;
        }
    }
    if (Rearm && TiNsiNotifyReady)
        TiNsiNotifyArmTimer();
}

VOID
TiNsiNotifyIpTable(
    _In_ ULONG Family,
    _In_ ULONG Table,
    _In_ ULONG Change)
{
    PNSI_PROVIDER Provider;

    if (!TiNsiNotifyReady || Family >= 2 || Table >= 4)
        return;
    if (InterlockedCompareExchange(&TiNsiNotifyThrottled[Family][Table], 1, 0) != 0)
    {
        /* Inside the window: fold into the trailing notification */
        InterlockedOr(&TiNsiNotifyPending[Family][Table], 1 << Change);
        TiNsiNotifyArmTimer();
        return;
    }
    Provider = TiNsiIpProvider(Family, Table);
    if (Provider != NULL)
        NsiNotifyChange(Provider, (NSI_CHANGE_TYPE)Change, NULL, 0);
    TiNsiNotifyArmTimer();
}

static VOID
TiNsiNotifyStart(VOID)
{
    RtlZeroMemory((PVOID)TiNsiNotifyPending, sizeof(TiNsiNotifyPending));
    RtlZeroMemory((PVOID)TiNsiNotifyThrottled, sizeof(TiNsiNotifyThrottled));
    TiNsiNotifyTimerArmed = 0;
    KeInitializeTimer(&TiNsiNotifyTimer);
    KeInitializeDpc(&TiNsiNotifyDpc, TiNsiNotifyDpcRoutine, NULL);
    TiNsiNotifyReady = TRUE;
}

static VOID
TiNsiNotifyStop(VOID)
{
    TiNsiNotifyReady = FALSE;
    KeCancelTimer(&TiNsiNotifyTimer);
    KeFlushQueuedDpcs();
}

VOID
TiNsiRegister(VOID)
{
    NTSTATUS Status;

    PAGED_CODE();

    Status = NsiRegisterProvider(&NPI_MS_NDIS_MODULEID, NSI_NDIS_IFINFO_TABLE, &TiNsiIfInfoDispatch, NULL, &TiNsiIfInfo);
    if (!NT_SUCCESS(Status))
    {
        TiNsiIfInfo = NULL;
        DPRINT1("NSI: registering the NDIS IFINFO table failed, status 0x%08lx\n", Status);
    }
    Status = NsiRegisterProvider(&NPI_MS_NDIS_MODULEID, NSI_NDIS_INDEX_LUID_TABLE, &TiNsiIndexLuidDispatch, NULL, &TiNsiIndexLuid);
    if (!NT_SUCCESS(Status))
    {
        TiNsiIndexLuid = NULL;
        DPRINT1("NSI: registering the NDIS INDEX_LUID table failed, status 0x%08lx\n", Status);
    }
    TiNsiRegisterIp6();
    TiNsiRegisterIp4();
    TiNsiRegisterEndpoints();
    TiNsiNotifyStart();
}

VOID
TiNsiNotifyInterface(
    _In_ UINT Index,
    _In_ ULONG Change)
{
    if (TiNsiIfInfo != NULL)
        NsiNotifyChange(TiNsiIfInfo, (NSI_CHANGE_TYPE)Change, &Index, sizeof(Index));
    if (TiNsiIndexLuid != NULL && Change != TI_NSI_CHANGE_PARAMETER)
        NsiNotifyChange(TiNsiIndexLuid, (NSI_CHANGE_TYPE)Change, &Index, sizeof(Index));
}

VOID
TiNsiDeregister(VOID)
{
    PAGED_CODE();

    TiNsiNotifyStop();
    TiNsiDeregisterEndpoints();
    TiNsiDeregisterIp4();
    TiNsiDeregisterIp6();
    if (TiNsiIfInfo != NULL)
    {
        NsiDeregisterProvider(TiNsiIfInfo);
        TiNsiIfInfo = NULL;
    }
    if (TiNsiIndexLuid != NULL)
    {
        NsiDeregisterProvider(TiNsiIndexLuid);
        TiNsiIndexLuid = NULL;
    }
}
