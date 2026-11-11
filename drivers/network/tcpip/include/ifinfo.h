/*
 * PROJECT:     LiberNT TCP/IP protocol driver
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Interface facts shared by the TDI information arm and the network store producer
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * This header uses only NT base types so that tcpip/nsiprov.c, which is
 * built outside the driver's precompiled header, can include it.
 */

#pragma once

#define IFINFO_MAX_DESCRIPTION 128
#define IFINFO_MAX_PHYSADDR 8

typedef struct _SEND_RECV_STATS {
    ULONG64 InBytes;
    ULONG64 InUnicast;
    ULONG64 InNUnicast;
    ULONG64 InDiscarded;
    ULONG64 InErrors;
    ULONG64 InDiscardedUnknownProto;
    ULONG64 OutBytes;
    ULONG64 OutUnicast;
    ULONG64 OutNUnicast;
    ULONG64 OutDiscarded;
    ULONG64 OutErrors;
} SEND_RECV_STATS, *PSEND_RECV_STATS;

typedef struct _IF_INFO_ROW {
    UINT Index;
    UINT Type;
    UINT Mtu;
    UINT Speed;
    UINT AdminStatus;
    ULONG OperStatus;
    UINT PhysAddrLength;
    UCHAR PhysAddr[IFINFO_MAX_PHYSADDR];
    SEND_RECV_STATS Stats;
    ULONG InErrors;
    ULONG OutErrors;
    ULONG PhysicalMedium;
    GUID Guid;
    UINT DescriptionLength;
    CHAR Description[IFINFO_MAX_DESCRIPTION + 1];
} IF_INFO_ROW, *PIF_INFO_ROW;

struct _IP_INTERFACE;

VOID IfInfoFillRow(struct _IP_INTERFACE *Interface, PIF_INFO_ROW Row);
NTSTATUS IfInfoSnapshot(ULONG Capacity, PIF_INFO_ROW Rows, PULONG Count);
NTSTATUS IfInfoSnapshotByIndex(UINT Index, PIF_INFO_ROW Row);

#define IFINFO_DAD_PREFERRED 4

#define IFINFO_NEIGHBOR_OTHER 1
#define IFINFO_NEIGHBOR_INVALID 2
#define IFINFO_NEIGHBOR_DYNAMIC 3
#define IFINFO_NEIGHBOR_STATIC 4

typedef struct _IF_INFO_KEY {
    UINT Index;
    UINT Type;
    ULONG OperStatus;
    UINT Mtu;
    BOOLEAN Loopback;
} IF_INFO_KEY, *PIF_INFO_KEY;

typedef struct _IP4_ADDRESS_ROW {
    ULONG Address;
    ULONG Mask;
    ULONG Broadcast;
    ULONG DadState;
    LARGE_INTEGER CreationTime;
} IP4_ADDRESS_ROW, *PIP4_ADDRESS_ROW;

typedef struct _IP4_ROUTE_ROW {
    ULONG Destination;
    ULONG Mask;
    ULONG Gateway;
    ULONG Metric;
    ULONG Origin;
    ULONG Age;
    ULONG ValidLifetime;
} IP4_ROUTE_ROW, *PIP4_ROUTE_ROW;

typedef struct _IP4_NEIGHBOR_ROW {
    ULONG Address;
    UCHAR PhysAddr[IFINFO_MAX_PHYSADDR];
    ULONG PhysAddrLength;
    ULONG Type;
} IP4_NEIGHBOR_ROW, *PIP4_NEIGHBOR_ROW;

struct _IP4_INTERFACE_INFO;

NTSTATUS IfInfoInterfaceKeys(PIF_INFO_KEY Keys, ULONG Capacity, PULONG Count);
NTSTATUS IfInfoIp4Interface(UINT Index, struct _IP4_INTERFACE_INFO *Info);
NTSTATUS IfInfoIp4Addresses(UINT Index, PIP4_ADDRESS_ROW Rows, ULONG Capacity, PULONG Count);
NTSTATUS IfInfoIp4Routes(UINT Index, PIP4_ROUTE_ROW Rows, ULONG Capacity, PULONG Count);
NTSTATUS IfInfoIp4Neighbors(UINT Index, PIP4_NEIGHBOR_ROW Rows, ULONG Capacity, PULONG Count);
NTSTATUS IfInfoIp6Query(ULONG TableId, PVOID *Buffer, PULONG Count);
VOID IfInfoFree(PVOID Buffer);

#define TI_NSI_CHANGE_ADD 1
#define TI_NSI_CHANGE_DELETE 2
#define TI_NSI_CHANGE_PARAMETER 3

VOID TiNsiRegister(VOID);
VOID TiNsiDeregister(VOID);
VOID TiNsiNotifyInterface(UINT Index, ULONG Change);

#define TI_NSI_FAMILY_IPV4 0
#define TI_NSI_FAMILY_IPV6 1
#define TI_NSI_TABLE_INTERFACE 0
#define TI_NSI_TABLE_UNICAST 1
#define TI_NSI_TABLE_NEIGHBOUR 2
#define TI_NSI_TABLE_FORWARD 3

VOID TiNsiNotifyIpTable(ULONG Family, ULONG Table, ULONG Change);

#define IFINFO_FAMILY_ANY 2
#define IFINFO_PROTOCOL_ANY 0
#define IFINFO_PROTOCOL_TCP 6
#define IFINFO_PROTOCOL_UDP 17

typedef struct _ENDPOINT_ROW {
    ULONG Family;
    ULONG Protocol;
    UCHAR LocalAddress[16];
    ULONG LocalScopeId;
    USHORT LocalPort;
    USHORT RemotePort;
    UCHAR RemoteAddress[16];
    ULONG RemoteScopeId;
    ULONG State;
    ULONG Pid;
    LARGE_INTEGER CreateTime;
    ULONGLONG ModuleInfo;
} ENDPOINT_ROW, *PENDPOINT_ROW;

NTSTATUS IfInfoEndpoints(ULONG Family, ULONG Protocol, PENDPOINT_ROW Rows, ULONG Capacity, PULONG Count);
