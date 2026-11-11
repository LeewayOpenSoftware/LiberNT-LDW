/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     NSI change notification delivery and latency for address and route changes
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <apitest.h>
#include <winsock2.h>
#include <winternl.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <netiodef.h>
#include <wine/nsi.h>

#define PROBE_ADDRESS 0x7f0000fe
#define LATENCY_LIMIT_MS 2500

typedef struct _WATCH
{
    const char *Name;
    OVERLAPPED Overlapped;
    HANDLE Handle;
    DWORD Error;
} WATCH;

static BOOL FindLoopback(NET_LUID *Luid, NET_IFINDEX *Index)
{
    MIB_IF_TABLE2 *table = NULL;
    DWORD i;
    BOOL found = FALSE;

    if (GetIfTable2(&table) != NO_ERROR)
        return FALSE;
    for (i = 0; i < table->NumEntries && !found; i++)
    {
        if (table->Table[i].Type == IF_TYPE_SOFTWARE_LOOPBACK)
        {
            *Luid = table->Table[i].InterfaceLuid;
            *Index = table->Table[i].InterfaceIndex;
            found = TRUE;
        }
    }
    FreeMibTable(table);
    return found;
}

static void Arm(WATCH *Watch, const char *Name, const NPI_MODULEID *Module, DWORD Table)
{
    memset(Watch, 0, sizeof(*Watch));
    Watch->Name = Name;
    Watch->Overlapped.hEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    Watch->Handle = NULL;
    Watch->Error = NsiRequestChangeNotification(0, Module, Table, &Watch->Overlapped, &Watch->Handle);
    ok(Watch->Error == ERROR_IO_PENDING, "%s: request returned %lu\n", Name, Watch->Error);
}

static DWORD WaitChange(WATCH *Watch, DWORD Timeout)
{
    DWORD start = GetTickCount(), wait, bytes;

    if (Watch->Error != ERROR_IO_PENDING)
        return (DWORD)-1;
    wait = WaitForSingleObject(Watch->Overlapped.hEvent, Timeout);
    if (wait != WAIT_OBJECT_0)
        return (DWORD)-1;
    GetOverlappedResult(Watch->Handle, &Watch->Overlapped, &bytes, FALSE);
    return GetTickCount() - start;
}

static void Disarm(WATCH *Watch)
{
    DWORD bytes;

    if (Watch->Error == ERROR_IO_PENDING && HasOverlappedIoCompleted(&Watch->Overlapped) == FALSE)
    {
        NsiCancelChangeNotification(&Watch->Overlapped);
        GetOverlappedResult(Watch->Handle, &Watch->Overlapped, &bytes, TRUE);
    }
    CloseHandle(Watch->Overlapped.hEvent);
}

static void test_address_change(void)
{
    MIB_UNICASTIPADDRESS_ROW row;
    NET_LUID luid;
    NET_IFINDEX index;
    WATCH unicast, forward, ifinfo;
    DWORD err, latency;

    if (!FindLoopback(&luid, &index))
    {
        skip("no loopback interface\n");
        return;
    }
    Arm(&unicast, "ipv4 unicast", &NPI_MS_IPV4_MODULEID, NSI_IP_UNICAST_TABLE);
    Arm(&forward, "ipv4 forward", &NPI_MS_IPV4_MODULEID, NSI_IP_FORWARD_TABLE);
    Arm(&ifinfo, "ndis ifinfo", &NPI_MS_NDIS_MODULEID, NSI_NDIS_IFINFO_TABLE);

    InitializeUnicastIpAddressEntry(&row);
    row.InterfaceLuid = luid;
    row.InterfaceIndex = index;
    row.Address.Ipv4.sin_family = AF_INET;
    row.Address.Ipv4.sin_addr.s_addr = htonl(PROBE_ADDRESS);
    row.OnLinkPrefixLength = 8;
    err = CreateUnicastIpAddressEntry(&row);
    if (err != NO_ERROR)
    {
        skip("CreateUnicastIpAddressEntry on loopback: %lu\n", err);
        Disarm(&unicast);
        Disarm(&forward);
        Disarm(&ifinfo);
        return;
    }

    latency = WaitChange(&unicast, 5000);
    ok(latency != (DWORD)-1, "unicast change not reported within 5 s after an address add\n");
    ok(latency <= LATENCY_LIMIT_MS, "unicast add reported after %lu ms\n", latency);
    trace("unicast add latency %lu ms\n", latency);
    latency = WaitChange(&forward, 5000);
    ok(latency != (DWORD)-1, "forward change not reported within 5 s after an address add\n");
    trace("forward (on-link route) latency %lu ms\n", latency);
    ok(WaitForSingleObject(ifinfo.Overlapped.hEvent, 1500) == WAIT_TIMEOUT, "ifinfo notification fired without an interface change\n");

    Arm(&unicast, "ipv4 unicast (delete)", &NPI_MS_IPV4_MODULEID, NSI_IP_UNICAST_TABLE);
    err = DeleteUnicastIpAddressEntry(&row);
    ok(err == NO_ERROR, "DeleteUnicastIpAddressEntry: %lu\n", err);
    latency = WaitChange(&unicast, 5000);
    ok(latency != (DWORD)-1, "unicast change not reported within 5 s after an address delete\n");
    ok(latency <= LATENCY_LIMIT_MS, "unicast delete reported after %lu ms\n", latency);
    trace("unicast delete latency %lu ms\n", latency);

    Disarm(&unicast);
    Disarm(&forward);
    Disarm(&ifinfo);
}

static void test_idle_tables(void)
{
    WATCH tcp, udp;

    Arm(&tcp, "tcp all", &NPI_MS_TCP_MODULEID, NSI_TCP_ALL_TABLE);
    Arm(&udp, "udp endpoint", &NPI_MS_UDP_MODULEID, NSI_UDP_ENDPOINT_TABLE);
    if (tcp.Error == ERROR_IO_PENDING)
        ok(WaitForSingleObject(tcp.Overlapped.hEvent, 1200) == WAIT_TIMEOUT, "tcp table notification fired while idle\n");
    if (udp.Error == ERROR_IO_PENDING)
        ok(WaitForSingleObject(udp.Overlapped.hEvent, 200) == WAIT_TIMEOUT, "udp table notification fired while idle\n");
    Disarm(&tcp);
    Disarm(&udp);
}

START_TEST(notify)
{
    test_idle_tables();
    test_address_change();
}
