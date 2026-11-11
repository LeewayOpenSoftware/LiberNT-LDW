/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Loopback traffic survives adding and deleting a second 127.x address;
 *              an interface's own address is never published as its neighbour
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <icmpapi.h>

#define ALIAS_ADDRESS 0x7f0000fe
#define RECEIVE_TIMEOUT_MS 3000
#define ECHO_TIMEOUT_MS 1000

static BOOL
FindLoopback(
    _Out_ NET_LUID *Luid,
    _Out_ NET_IFINDEX *Index)
{
    MIB_IF_TABLE2 *Table = NULL;
    ULONG i;
    BOOL Found = FALSE;

    if (GetIfTable2(&Table) != NO_ERROR)
        return FALSE;
    for (i = 0; i < Table->NumEntries && !Found; i++)
    {
        if (Table->Table[i].Type == IF_TYPE_SOFTWARE_LOOPBACK)
        {
            *Luid = Table->Table[i].InterfaceLuid;
            *Index = Table->Table[i].InterfaceIndex;
            Found = TRUE;
        }
    }
    FreeMibTable(Table);
    return Found;
}

static BOOL
UdpLoopback(
    _In_ const char *Stage)
{
    SOCKET Receiver = INVALID_SOCKET, Sender = INVALID_SOCKET;
    struct sockaddr_in Address;
    int Length = sizeof(Address);
    DWORD Timeout = RECEIVE_TIMEOUT_MS;
    char Buffer[16];
    int Result;
    BOOL Ok = FALSE;

    Receiver = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    Sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (Receiver == INVALID_SOCKET || Sender == INVALID_SOCKET)
    {
        ok(FALSE, "%s: socket failed, error %d\n", Stage, WSAGetLastError());
        goto done;
    }
    memset(&Address, 0, sizeof(Address));
    Address.sin_family = AF_INET;
    Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(Receiver, (struct sockaddr *)&Address, sizeof(Address)) != 0 ||
        getsockname(Receiver, (struct sockaddr *)&Address, &Length) != 0)
    {
        ok(FALSE, "%s: bind to 127.0.0.1 failed, error %d\n", Stage, WSAGetLastError());
        goto done;
    }
    setsockopt(Receiver, SOL_SOCKET, SO_RCVTIMEO, (const char *)&Timeout, sizeof(Timeout));

    Result = sendto(Sender, "lo", 2, 0, (struct sockaddr *)&Address, sizeof(Address));
    ok(Result == 2, "%s: sendto 127.0.0.1 returned %d, error %d\n", Stage, Result, WSAGetLastError());
    if (Result != 2)
        goto done;
    Result = recv(Receiver, Buffer, sizeof(Buffer), 0);
    ok(Result == 2, "%s: recv returned %d, error %d\n", Stage, Result, WSAGetLastError());
    Ok = Result == 2;

done:
    if (Sender != INVALID_SOCKET)
        closesocket(Sender);
    if (Receiver != INVALID_SOCKET)
        closesocket(Receiver);
    return Ok;
}

static void
IcmpLoopback(
    _In_ const char *Stage)
{
    HANDLE Icmp;
    char Payload[32] = "loopback alias";
    char Reply[sizeof(ICMP_ECHO_REPLY) + sizeof(Payload) + 8];
    DWORD Count;

    Icmp = IcmpCreateFile();
    if (Icmp == INVALID_HANDLE_VALUE)
    {
        skip("%s: IcmpCreateFile failed, error %lu\n", Stage, GetLastError());
        return;
    }
    Count = IcmpSendEcho(Icmp, htonl(INADDR_LOOPBACK), Payload, sizeof(Payload), NULL, Reply, sizeof(Reply), ECHO_TIMEOUT_MS);
    ok(Count == 1, "%s: IcmpSendEcho to 127.0.0.1 returned %lu replies, error %lu\n", Stage, Count, GetLastError());
    IcmpCloseHandle(Icmp);
}

static void
CheckLoopbackNeighbour(
    _In_ NET_IFINDEX Index,
    _In_ const char *Stage)
{
    MIB_IPNET_TABLE2 *Table = NULL;
    ULONG i;
    BOOL Found = FALSE;
    DWORD Error;

    Error = GetIpNetTable2(AF_INET, &Table);
    ok(Error == NO_ERROR, "%s: GetIpNetTable2 returned %lu\n", Stage, Error);
    if (Error != NO_ERROR)
        return;
    for (i = 0; i < Table->NumEntries; i++)
    {
        if (Table->Table[i].InterfaceIndex == Index &&
            Table->Table[i].Address.Ipv4.sin_addr.s_addr == htonl(INADDR_LOOPBACK))
            Found = TRUE;
    }
    FreeMibTable(Table);
    ok(!Found, "%s: 127.0.0.1, the loopback interface's own address, is listed as its neighbour\n", Stage);
}

START_TEST(loopback_alias)
{
    WSADATA Data;
    NET_LUID Luid;
    NET_IFINDEX Index;
    MIB_UNICASTIPADDRESS_ROW Row;
    DWORD Error;

    if (WSAStartup(MAKEWORD(2, 2), &Data) != 0)
    {
        skip("WSAStartup failed\n");
        return;
    }
    if (!FindLoopback(&Luid, &Index))
    {
        skip("no loopback interface\n");
        WSACleanup();
        return;
    }

    UdpLoopback("before the alias");
    CheckLoopbackNeighbour(Index, "before the alias");

    InitializeUnicastIpAddressEntry(&Row);
    Row.InterfaceLuid = Luid;
    Row.InterfaceIndex = Index;
    Row.Address.Ipv4.sin_family = AF_INET;
    Row.Address.Ipv4.sin_addr.s_addr = htonl(ALIAS_ADDRESS);
    Row.OnLinkPrefixLength = 8;
    Error = CreateUnicastIpAddressEntry(&Row);
    if (Error == ERROR_ACCESS_DENIED)
    {
        skip("adding an address needs administrative rights\n");
        WSACleanup();
        return;
    }
    ok(Error == NO_ERROR, "CreateUnicastIpAddressEntry(127.0.0.254) returned %lu\n", Error);
    if (Error == NO_ERROR)
    {
        UdpLoopback("with the alias");
        Error = DeleteUnicastIpAddressEntry(&Row);
        ok(Error == NO_ERROR, "DeleteUnicastIpAddressEntry(127.0.0.254) returned %lu\n", Error);
    }

    UdpLoopback("after the alias was deleted");
    IcmpLoopback("after the alias was deleted");
    CheckLoopbackNeighbour(Index, "after the alias was deleted");

    WSACleanup();
}
