/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS TCP/IP protocol driver
 * FILE:        tcpip/ninfo.c
 * PURPOSE:     Network information
 * PROGRAMMERS: Art Yerkes
 * REVISIONS:
 *   CSH 01/08-2000 Created
 */

#include "precomp.h"

#include <tcpip_undoc.h>
#include <icmp.h>

#define IP_ROUTE_TYPE_ADD 3
#define IP_ROUTE_TYPE_DEL 2
#define IP_NOT_FORWARDING 2

/* Get IPRouteEntry s for each of the routes in the system */
TDI_STATUS InfoTdiQueryGetRouteTable( PIP_INTERFACE IF, PNDIS_BUFFER Buffer, PUINT BufferSize ) {
    TDI_STATUS Status;
    UINT RtCount;
    UINT Capacity = CountFIBs(IF);
    UINT Size;
    PFIB_ENTRY RCache, RCacheCur;
    PIPROUTE_ENTRY RouteEntries, RtCurrent;
    UINT i;

    TI_DbgPrint(DEBUG_INFO, ("Called, routes = %d\n",
				    Capacity));

    if (Capacity == 0)
        return InfoCopyOut(NULL, 0, NULL, BufferSize);

    for (;;) {
        if (Capacity > MAXULONG / sizeof(FIB_ENTRY))
            return TDI_NO_RESOURCES;

        RCache = ExAllocatePoolWithTag(NonPagedPool, sizeof(FIB_ENTRY) * Capacity, FIB_TAG);
        if (!RCache)
            return TDI_NO_RESOURCES;

        RtCount = CopyFIBs(IF, RCache, Capacity);
        if (RtCount <= Capacity)
            break;

        for (i = 0; i < Capacity; i++)
            NBDereferenceNeighbor(RCache[i].Router);

        ExFreePoolWithTag(RCache, FIB_TAG);
        Capacity = RtCount;
    }

    if (RtCount == 0) {
        ExFreePoolWithTag(RCache, FIB_TAG);
        return InfoCopyOut(NULL, 0, NULL, BufferSize);
    }

    if (RtCount > MAXULONG / sizeof(IPROUTE_ENTRY)) {
        for (i = 0; i < RtCount; i++)
            NBDereferenceNeighbor(RCache[i].Router);
        ExFreePoolWithTag(RCache, FIB_TAG);
        return TDI_NO_RESOURCES;
    }

    Size = sizeof(IPROUTE_ENTRY) * RtCount;
    RouteEntries = ExAllocatePoolWithTag(NonPagedPool, Size, ROUTE_ENTRY_TAG);
    if (!RouteEntries) {
        for (i = 0; i < RtCount; i++)
            NBDereferenceNeighbor(RCache[i].Router);
        ExFreePoolWithTag(RCache, FIB_TAG);
        return TDI_NO_RESOURCES;
    }

    RtlZeroMemory(RouteEntries, Size);
    RtCurrent = RouteEntries;
    RCacheCur = RCache;

    while( RtCurrent < RouteEntries + RtCount ) {
	ASSERT(RCacheCur->Router);

	RtlCopyMemory( &RtCurrent->Dest,
		       &RCacheCur->NetworkAddress.Address,
		       sizeof(RtCurrent->Dest) );
	RtlCopyMemory( &RtCurrent->Mask,
		       &RCacheCur->Netmask.Address,
		       sizeof(RtCurrent->Mask) );
	RtlCopyMemory( &RtCurrent->Gw,
		       &RCacheCur->Router->Address.Address,
		       sizeof(RtCurrent->Gw) );

	RtCurrent->Metric1 = RCacheCur->Metric;
	RtCurrent->Type = TDI_ADDRESS_TYPE_IP;

	TI_DbgPrint
	    (DEBUG_INFO,
	     ("%d: NA %08x NM %08x GW %08x MT %x\n",
	      RtCurrent - RouteEntries,
	      RtCurrent->Dest,
	      RtCurrent->Mask,
	      RtCurrent->Gw,
	      RtCurrent->Metric1 ));

	RtCurrent->Index = IF->Index;

	RtCurrent++; RCacheCur++;
    }

    Status = InfoCopyOut( (PCHAR)RouteEntries, Size, Buffer, BufferSize );

    for (i = 0; i < RtCount; i++)
        NBDereferenceNeighbor(RCache[i].Router);

    ExFreePoolWithTag( RouteEntries, ROUTE_ENTRY_TAG );
    ExFreePoolWithTag( RCache, FIB_TAG );

    TI_DbgPrint(DEBUG_INFO, ("Returning %08x\n", Status));

    return Status;
}

TDI_STATUS InfoTdiQueryGetAddrTable(TDIEntityID ID,
				    PNDIS_BUFFER Buffer,
				    PUINT BufferSize)
{
    KIRQL OldIrql;
    PIPADDR_ENTRY IPEntry;
    PIP_INTERFACE CurrentIF;
    UINT i, Count, Capacity;
    TDI_STATUS Status;

    TI_DbgPrint(DEBUG_INFO, ("Called.\n"));


    TcpipAcquireSpinLock(&EntityListLock, &OldIrql);

    for (i = 0; i < EntityCount; i++)
    {
        if (EntityList[i].tei_entity == ID.tei_entity &&
            EntityList[i].tei_instance == ID.tei_instance)
            break;
    }

    if (i == EntityCount)
    {
        TcpipReleaseSpinLock(&EntityListLock, OldIrql);
        return TDI_INVALID_PARAMETER;
    }

    CurrentIF = EntityList[i].context;

    for (Capacity = 1;;)
    {
        IPEntry = ExAllocatePoolZero(NonPagedPool, Capacity * sizeof(IPADDR_ENTRY), IP_ADDRESS_TAG);
        if (!IPEntry)
        {
            TcpipReleaseSpinLock(&EntityListLock, OldIrql);
            return TDI_NO_RESOURCES;
        }

        Count = CopyInterfaceAddresses(CurrentIF, IPEntry, Capacity);
        if (Count <= Capacity)
            break;

        ExFreePoolWithTag(IPEntry, IP_ADDRESS_TAG);
        Capacity = Count;
    }

    if (Count == 0)
    {
        Count = 1;
        IPEntry->Index = CurrentIF->Index;
        GetInterfaceIPv4Address(CurrentIF,
                                ADE_UNICAST,
                                &IPEntry->Addr);
        GetInterfaceIPv4Address(CurrentIF,
                                ADE_ADDRMASK,
                                &IPEntry->Mask);
        GetInterfaceIPv4Address(CurrentIF,
                                ADE_BROADCAST,
                                &IPEntry->BcastAddr);
    }

    TcpipReleaseSpinLock(&EntityListLock, OldIrql);

    Status = InfoCopyOut((PCHAR)IPEntry, Count * sizeof(IPADDR_ENTRY),
                         Buffer, BufferSize);

    ExFreePoolWithTag(IPEntry, IP_ADDRESS_TAG);

    return Status;
}

TDI_STATUS InfoTdiQueryGetIPSnmpInfo( TDIEntityID ID,
                                      PIP_INTERFACE IF,
				      PNDIS_BUFFER Buffer,
				      PUINT BufferSize ) {
    IPSNMPInfo SnmpInfo;
    UINT IfCount = CountInterfaces();
    UINT RouteCount = CountFIBs(IF);
    TDI_STATUS Status = TDI_INVALID_REQUEST;

    TI_DbgPrint(DEBUG_INFO, ("Called.\n"));

    RtlZeroMemory(&SnmpInfo, sizeof(SnmpInfo));

    SnmpInfo.ipsi_forwarding = IP_NOT_FORWARDING;
    SnmpInfo.ipsi_defaultttl = IP_DEFAULT_TTL;

    SnmpInfo.ipsi_numif = IfCount;
    SnmpInfo.ipsi_numaddr = max(CountInterfaceAddresses(IF), 1);
    SnmpInfo.ipsi_numroutes = RouteCount;

    {
        struct lib_ip_stats LibStats;
        LibIPGetStats(&LibStats);
        SnmpInfo.ipsi_inreceives = LibStats.InReceives;
        SnmpInfo.ipsi_indelivers = LibStats.InDelivers;
        SnmpInfo.ipsi_outrequests = LibStats.OutRequests;
        SnmpInfo.ipsi_forwdatagrams = LibStats.ForwardedDatagrams;
        SnmpInfo.ipsi_inhdrerrors = LibStats.InHeaderErrors;
        SnmpInfo.ipsi_inunknownprotos = LibStats.InUnknownProtocols;
        SnmpInfo.ipsi_indiscards = LibStats.InDiscards;
        SnmpInfo.ipsi_outnoroutes = LibStats.OutNoRoutes;
        SnmpInfo.ipsi_outdiscards = LibStats.OutDiscards;
        SnmpInfo.ipsi_reasmreqds = LibStats.ReasmReqds;
        SnmpInfo.ipsi_reasmoks = LibStats.ReasmOks;
        SnmpInfo.ipsi_reasmfails = LibStats.ReasmFails;
        SnmpInfo.ipsi_fragcreates = LibStats.FragCreates;
    }

    Status = InfoCopyOut( (PCHAR)&SnmpInfo, sizeof(SnmpInfo),
			  Buffer, BufferSize );

    TI_DbgPrint(DEBUG_INFO, ("Returning %08x\n", Status));

    return Status;
}

TDI_STATUS InfoTdiQueryGetTcpStats( PNDIS_BUFFER Buffer, PUINT BufferSize )
{
    MIB_TCPSTATS TcpStats;
    struct lib_ip_stats LibStats;

    RtlZeroMemory(&TcpStats, sizeof(TcpStats));
    LibIPGetStats(&LibStats);

    TcpStats.dwInSegs = LibStats.TcpInSegs;
    TcpStats.dwOutSegs = LibStats.TcpOutSegs;
    TcpStats.dwInErrs = LibStats.TcpInErrs;

    return InfoCopyOut( (PCHAR)&TcpStats, sizeof(TcpStats), Buffer, BufferSize );
}

TDI_STATUS InfoTdiQueryGetUdpStats( PNDIS_BUFFER Buffer, PUINT BufferSize )
{
    MIB_UDPSTATS UdpStats;
    struct lib_ip_stats LibStats;

    RtlZeroMemory(&UdpStats, sizeof(UdpStats));
    LibIPGetStats(&LibStats);

    UdpStats.dwInDatagrams = LibStats.UdpInDatagrams;
    UdpStats.dwOutDatagrams = LibStats.UdpOutDatagrams;
    UdpStats.dwNoPorts = LibStats.UdpNoPorts;
    UdpStats.dwInErrors = LibStats.UdpInErrors;

    return InfoCopyOut( (PCHAR)&UdpStats, sizeof(UdpStats), Buffer, BufferSize );
}

/* IPv4 ICMP MIB. IPv4 ICMP is handled by the native stack (ip/network/icmp.c),
   not lwIP, so the counters live there. ICMP_STATISTICS already mirrors
   MIBICMPINFO (two MIBICMPSTATS of 13 ULONGs = 104 bytes), so copy it straight
   out; this avoids pulling usermode ipmib.h (which drags in ifmib.h/nldef.h). */
TDI_STATUS InfoTdiQueryGetIcmpStats( PNDIS_BUFFER Buffer, PUINT BufferSize )
{
    return InfoCopyOut( (PCHAR)&ICMPStats, sizeof(ICMPStats), Buffer, BufferSize );
}

static BOOLEAN InfoTdiFillTcpConnectionRow(PADDRESS_FILE AddrFile,
                                           PCONNECTION_ENDPOINT Connection,
                                           PMIB_TCPROW_OWNER_MODULE TcpRow)
{
    TA_IP_ADDRESS EndPoint;

    if (!NT_SUCCESS(TCPGetSockAddress(Connection, (PTRANSPORT_ADDRESS)&EndPoint, sizeof(EndPoint), FALSE)) ||
        EndPoint.TAAddressCount < 1 || EndPoint.Address[0].AddressLength != TDI_ADDRESS_LENGTH_IP)
        return FALSE;
    TcpRow->dwLocalAddr = EndPoint.Address[0].Address[0].in_addr;
    TcpRow->dwLocalPort = EndPoint.Address[0].Address[0].sin_port;

    if (!NT_SUCCESS(TCPGetSockAddress(Connection, (PTRANSPORT_ADDRESS)&EndPoint, sizeof(EndPoint), TRUE)) ||
        EndPoint.TAAddressCount < 1 || EndPoint.Address[0].AddressLength != TDI_ADDRESS_LENGTH_IP)
        return FALSE;
    TcpRow->dwRemoteAddr = EndPoint.Address[0].Address[0].in_addr;
    TcpRow->dwRemotePort = EndPoint.Address[0].Address[0].sin_port;

    return NT_SUCCESS(TCPGetSocketStatus(Connection, &TcpRow->dwState));
}

/* One TCP address file can stand for several endpoints: its listener, and
 * every connection associated with it, which is how an accepted connection
 * exists at all (AFD associates it with the listening address). Report each
 * of them; the single row this used to return lost every server-side
 * connection. The connections are pinned under the address file lock and
 * queried after it is released, as the socket queries take the connection
 * lock, which is never taken below the address file lock. */
TDI_STATUS InfoTdiQueryGetConnectionTcpTable(PADDRESS_FILE AddrFile,
				    PNDIS_BUFFER Buffer,
				    PUINT BufferSize,
                    TDI_TCPUDP_CLASS_INFO Class)
{
    SIZE_T Size;
    MIB_TCPROW_OWNER_MODULE TcpRow;
    PCONNECTION_ENDPOINT *Connections = NULL, Connection;
    PUCHAR Rows;
    ULONG Count = 0, Written = 0, i;
    TDI_STATUS Status;

    TI_DbgPrint(DEBUG_INFO, ("Called.\n"));

    if (Class == TcpUdpClassOwnerPid)
    {
        Size = sizeof(MIB_TCPROW_OWNER_PID);
    }
    else if (Class == TcpUdpClassOwner)
    {
        Size = sizeof(MIB_TCPROW_OWNER_MODULE);
    }
    else
    {
        Size = sizeof(MIB_TCPROW);
    }

    LockObject(AddrFile);
    for (Connection = AddrFile->Connection; Connection != NULL; Connection = Connection->Next)
        Count++;
    if (Count != 0)
        Connections = ExAllocatePoolWithTag(NonPagedPool, Count * sizeof(*Connections), OUT_DATA_TAG);
    Count = 0;
    if (Connections != NULL)
    {
        for (Connection = AddrFile->Connection; Connection != NULL; Connection = Connection->Next)
        {
            ReferenceObject(Connection);
            Connections[Count++] = Connection;
        }
    }
    UnlockObject(AddrFile);

    Rows = ExAllocatePoolWithTag(PagedPool, (Count + 1) * Size, OUT_DATA_TAG);
    if (Rows == NULL)
    {
        for (i = 0; i < Count; i++)
            DereferenceObject(Connections[i]);
        if (Connections != NULL)
            ExFreePoolWithTag(Connections, OUT_DATA_TAG);
        return TDI_NO_RESOURCES;
    }

    RtlZeroMemory(&TcpRow, sizeof(TcpRow));
    TcpRow.dwOwningPid = HandleToUlong(AddrFile->ProcessId);
    TcpRow.liCreateTimestamp = AddrFile->CreationTime;
    if (Class == TcpUdpClassOwner)
        TcpRow.OwningModuleInfo[0] = (ULONG_PTR)AddrFile->SubProcessTag;

    if (AddrFile->Listener != NULL)
    {
        TcpRow.dwState = MIB_TCP_STATE_LISTEN;
        TcpRow.dwLocalAddr = AddrFile->Address.Address.IPv4Address;
        TcpRow.dwLocalPort = AddrFile->Port;
        TcpRow.dwRemoteAddr = 0;
        TcpRow.dwRemotePort = 0;
        RtlCopyMemory(Rows + Written * Size, &TcpRow, Size);
        Written++;
    }

    for (i = 0; i < Count; i++)
    {
        if (Connections[i] != AddrFile->Listener && Connections[i]->SocketContext != NULL &&
            InfoTdiFillTcpConnectionRow(AddrFile, Connections[i], &TcpRow))
        {
            RtlCopyMemory(Rows + Written * Size, &TcpRow, Size);
            Written++;
        }
        DereferenceObject(Connections[i]);
    }
    if (Connections != NULL)
        ExFreePoolWithTag(Connections, OUT_DATA_TAG);

    Status = InfoCopyOut((PCHAR)Rows, Written * Size, Buffer, BufferSize);
    ExFreePoolWithTag(Rows, OUT_DATA_TAG);

    TI_DbgPrint(DEBUG_INFO, ("Returning %08x, %lu rows\n", Status, Written));

    return Status;
}

TDI_STATUS InfoTdiSetConnectionTcp(PADDRESS_FILE AddrFile,
                                   PVOID Buffer,
                                   UINT BufferSize)
{
    PMIB_TCPROW Row = Buffer;
    PCONNECTION_ENDPOINT Connection;
    TA_IP_ADDRESS Local, Remote;
    TDI_STATUS Status = TDI_INVALID_REQUEST;

    if (!Buffer || BufferSize < sizeof(MIB_TCPROW))
        return TDI_INVALID_PARAMETER;
    if (Row->dwState != MIB_TCP_STATE_DELETE_TCB)
        return TDI_INVALID_PARAMETER;

    LockObject(AddrFile);
    Connection = AddrFile->Connection;
    if (Connection)
        ReferenceObject(Connection);
    UnlockObject(AddrFile);

    if (!Connection)
        return TDI_INVALID_REQUEST;

    if (Connection->SocketContext &&
        NT_SUCCESS(TCPGetSockAddress(Connection, (PTRANSPORT_ADDRESS)&Local, sizeof(Local), FALSE)) &&
        NT_SUCCESS(TCPGetSockAddress(Connection, (PTRANSPORT_ADDRESS)&Remote, sizeof(Remote), TRUE)) &&
        Local.Address[0].AddressType == TDI_ADDRESS_TYPE_IP &&
        Remote.Address[0].AddressType == TDI_ADDRESS_TYPE_IP &&
        Local.Address[0].Address[0].in_addr == Row->dwLocalAddr &&
        Local.Address[0].Address[0].sin_port == (USHORT)Row->dwLocalPort &&
        Remote.Address[0].Address[0].in_addr == Row->dwRemoteAddr &&
        Remote.Address[0].Address[0].sin_port == (USHORT)Row->dwRemotePort)
    {
        TCPDisconnect(Connection, TDI_DISCONNECT_ABORT, NULL, NULL, NULL, NULL, NULL);
        Status = TDI_SUCCESS;
    }

    DereferenceObject(Connection);
    return Status;
}

TDI_STATUS InfoTdiQueryGetConnectionUdpTable(PADDRESS_FILE AddrFile,
				    PNDIS_BUFFER Buffer,
				    PUINT BufferSize,
				    TDI_TCPUDP_CLASS_INFO Class)
{
    SIZE_T Size;
    MIB_UDPROW_OWNER_MODULE UdpRow;
    TDI_STATUS Status = TDI_INVALID_REQUEST;

    TI_DbgPrint(DEBUG_INFO, ("Called.\n"));

    if (Class == TcpUdpClassOwnerPid)
    {
        Size = sizeof(MIB_UDPROW_OWNER_PID);
    }
    else if (Class == TcpUdpClassOwner)
    {
        Size = sizeof(MIB_UDPROW_OWNER_MODULE);
    }
    else
    {
        Size = sizeof(MIB_UDPROW);
    }

    UdpRow.dwLocalAddr = AddrFile->Address.Address.IPv4Address;
    UdpRow.dwLocalPort = AddrFile->Port;
    UdpRow.dwOwningPid = HandleToUlong(AddrFile->ProcessId);
    UdpRow.liCreateTimestamp = AddrFile->CreationTime;
    UdpRow.dwFlags = 0; /* FIXME */
    if (Class == TcpUdpClassOwner)
    {
        RtlZeroMemory(&UdpRow.OwningModuleInfo[0], sizeof(UdpRow.OwningModuleInfo));
        UdpRow.OwningModuleInfo[0] = (ULONG_PTR)AddrFile->SubProcessTag;
    }

    Status = InfoCopyOut( (PCHAR)&UdpRow,
			  Size, Buffer, BufferSize );

    TI_DbgPrint(DEBUG_INFO, ("Returning %08x\n", Status));

    return Status;
}

TDI_STATUS InfoTdiSetRoute(PIP_INTERFACE IF, PVOID Buffer, UINT BufferSize)
{
    IP_ADDRESS Address, Netmask, Router;
    PIPROUTE_ENTRY Route = Buffer;

    if (!Buffer || BufferSize < sizeof(IPROUTE_ENTRY))
        return TDI_INVALID_PARAMETER;

    AddrInitIPv4( &Address, Route->Dest );
    AddrInitIPv4( &Netmask, Route->Mask );
    AddrInitIPv4( &Router,  Route->Gw );

    if (IF == Loopback)
    {
        DbgPrint("Failing attempt to add route to loopback adapter\n");
        return TDI_INVALID_PARAMETER;
    }

    if( Route->Type == IP_ROUTE_TYPE_ADD ) { /* Add the route */
        TI_DbgPrint(DEBUG_INFO,("Adding route (%s)\n", A2S(&Address)));
	if (!RouterCreateRoute(&Address, &Netmask, &Router, IF, Route->Metric1))
	    return TDI_NO_RESOURCES;

        return TDI_SUCCESS;
     } else if( Route->Type == IP_ROUTE_TYPE_DEL ) {
	TI_DbgPrint(DEBUG_INFO,("Removing route (%s)\n", A2S(&Address)));
	if (NT_SUCCESS(RouterRemoveRoute( &Address, &Router )))
            return TDI_SUCCESS;
        else
            return TDI_INVALID_PARAMETER;
     }

     return TDI_INVALID_REQUEST;
}

static
NTSTATUS
InfoIp6QueryEndpoints(PUCHAR Buffer, ULONG Length, PULONG Count, PULONG Needed, PIP6_STATISTICS Stats)
{
    PENDPOINT_ROW Rows = NULL;
    ULONG Capacity = 0, Total = 0, i, Attempt;
    NTSTATUS Status = STATUS_SUCCESS;

    for (Attempt = 0; Attempt < 4; Attempt++)
    {
        Status = IfInfoEndpoints(TI_NSI_FAMILY_IPV6, IFINFO_PROTOCOL_ANY, Rows, Capacity, &Total);
        if (!NT_SUCCESS(Status) && Status != STATUS_BUFFER_OVERFLOW)
        {
            IfInfoFree(Rows);
            return Status;
        }
        if (Total <= Capacity)
            break;
        IfInfoFree(Rows);
        Capacity = Total + 8;
        Rows = ExAllocatePoolWithTag(PagedPool, Capacity * sizeof(ENDPOINT_ROW), OUT_DATA_TAG);
        if (!Rows)
            return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (Total > Capacity)
    {
        IfInfoFree(Rows);
        return STATUS_BUFFER_OVERFLOW;
    }

    for (i = 0; i < Total; i++)
    {
        IP6_ENDPOINT_ROW Row;

        if (Stats)
        {
            if (Rows[i].Protocol == IPPROTO_UDP)
            {
                Stats->UdpNumAddresses++;
            }
            else
            {
                Stats->TcpNumConnections++;
                if (Rows[i].State == MIB_TCP_STATE_ESTAB || Rows[i].State == MIB_TCP_STATE_CLOSE_WAIT)
                    Stats->TcpCurrentEstablished++;
            }
            continue;
        }
        RtlZeroMemory(&Row, sizeof(Row));
        Row.Protocol = Rows[i].Protocol;
        RtlCopyMemory(Row.LocalAddress, Rows[i].LocalAddress, sizeof(Row.LocalAddress));
        Row.LocalScopeId = Rows[i].LocalScopeId;
        Row.LocalPort = Rows[i].LocalPort;
        Row.RemotePort = Rows[i].RemotePort;
        RtlCopyMemory(Row.RemoteAddress, Rows[i].RemoteAddress, sizeof(Row.RemoteAddress));
        Row.RemoteScopeId = Rows[i].RemoteScopeId;
        Row.State = Rows[i].State;
        Row.ProcessId = Rows[i].Pid;
        Row.CreateTime = Rows[i].CreateTime;
        Row.ModuleInfo = Rows[i].ModuleInfo;
        if (Buffer && *Needed + sizeof(Row) <= Length)
            RtlCopyMemory(Buffer + *Needed, &Row, sizeof(Row));
        *Needed += sizeof(Row);
        (*Count)++;
    }
    IfInfoFree(Rows);
    return STATUS_SUCCESS;
}

NTSTATUS
InfoIp6QueryTable(ULONG TableId, PVOID Buffer, ULONG Length, PULONG Information)
{
    PIP6_TABLE_HEADER Header = Buffer;
    ULONG Count = 0, Needed = sizeof(IP6_TABLE_HEADER);
    NTSTATUS Status;

    *Information = 0;
    if (Length < sizeof(IP6_TABLE_HEADER))
        return STATUS_BUFFER_TOO_SMALL;

    switch (TableId)
    {
    case Ip6TableInterfaces:
    case Ip6TableAddresses:
    case Ip6TableRoutes:
    case Ip6TableNeighbors:
    case Ip6TableDnsServers:
    case Ip6TableDnsSuffixes:
        Status = LibIP6QueryTable(TableId, Buffer, Length, &Count, &Needed, NULL);
        break;

    case Ip6TableStatistics:
    {
        IP6_STATISTICS Stats;
        ULONG Ignored, IgnoredNeeded;

        RtlZeroMemory(&Stats, sizeof(Stats));
        Status = LibIP6QueryTable(Ip6TableStatistics, NULL, 0, &Ignored, &IgnoredNeeded, &Stats);
        if (NT_SUCCESS(Status))
            Status = InfoIp6QueryEndpoints(NULL, 0, &Ignored, &IgnoredNeeded, &Stats);
        if (NT_SUCCESS(Status))
        {
            Count = 1;
            Needed = sizeof(IP6_TABLE_HEADER) + sizeof(Stats);
            if (Needed <= Length)
                RtlCopyMemory(Header + 1, &Stats, sizeof(Stats));
        }
        break;
    }

    case Ip6TableEndpoints:
        Status = InfoIp6QueryEndpoints(Buffer, Length, &Count, &Needed, NULL);
        break;

    default:
        return STATUS_INVALID_PARAMETER;
    }

    if (!NT_SUCCESS(Status))
        return Status;

    Header->Count = Count;
    Header->Needed = Needed;

    if (Needed > Length)
    {
        *Information = sizeof(IP6_TABLE_HEADER);
        return STATUS_BUFFER_OVERFLOW;
    }

    *Information = Needed;
    return STATUS_SUCCESS;
}
