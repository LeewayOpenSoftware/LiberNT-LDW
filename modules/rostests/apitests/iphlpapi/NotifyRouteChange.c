/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests for NotifyRouteChange route-table change notifications
 * COPYRIGHT:   Copyright 2026 Leeway Open Software <281492504+LeewayOpenSoftware@users.noreply.github.com>
 */

/* ntstatus.h defines nothing once WIN32_NO_STATUS is set, so it goes first. */
#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <apitest.h>

#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <ipmib.h>
#include <nldef.h>

/* 198.51.100.254 is in TEST-NET-2 (RFC 5737): never routed, so the route the
 * test adds cannot collide with anything the machine already has. */
#define TEST_ROUTE_DEST htonl((198u << 24) | (51u << 16) | (100u << 8) | 254u)

/* OVERLAPPED.Internal is a ULONG_PTR; on 64-bit builds a negative NTSTATUS
 * constant would sign-extend and never compare equal. */
#define OVERLAPPED_STATUS(ovr) ((NTSTATUS)(ULONG)(ovr)->Internal)

static void wait_for_notification(HANDLE event, OVERLAPPED *ovr, const char *op)
{
    DWORD wait = WaitForSingleObject(event, 5000);

    ok(wait == WAIT_OBJECT_0, "%s notification timed out\n", op);
    if (wait == WAIT_OBJECT_0)
        ok(OVERLAPPED_STATUS(ovr) == STATUS_SUCCESS, "%s notification completed with status %lx\n", op, (ULONG)ovr->Internal);
}

static BOOL register_route_change(HANDLE event, OVERLAPPED *ovr, HANDLE *handle)
{
    DWORD error;

    ZeroMemory(ovr, sizeof(*ovr));
    ovr->hEvent = event;
    *handle = NULL;

    error = NotifyRouteChange(handle, ovr);
    ok_long(error, ERROR_IO_PENDING);
    ok(*handle != NULL, "Expected a notification handle, got NULL\n");
    ok(OVERLAPPED_STATUS(ovr) == STATUS_PENDING, "Expected STATUS_PENDING, got %lx\n", (ULONG)ovr->Internal);

    return error == ERROR_IO_PENDING;
}

/* The handle NotifyRouteChange returns is only for GetOverlappedResult; it
 * belongs to iphlpapi and is not closed by the caller. */
static void cleanup_notification(HANDLE event, OVERLAPPED *ovr)
{
    CancelIPChangeNotify(ovr);
    if (event) CloseHandle(event);
}

/* The stack refuses routes on the loopback interface, so the test route
 * goes where the default route goes, or, when the machine has no IPv4
 * default route, on-link through the first real IPv4 address. */
static BOOL find_test_route(MIB_IPFORWARDROW *route)
{
    MIB_IPFORWARDTABLE *routes;
    MIB_IPADDRTABLE *addrs;
    ULONG size = 0, i;
    DWORD error;
    BOOL found = FALSE;

    error = GetIpForwardTable(NULL, &size, FALSE);
    if (error == ERROR_INSUFFICIENT_BUFFER && size && (routes = HeapAlloc(GetProcessHeap(), 0, size)))
    {
        error = GetIpForwardTable(routes, &size, FALSE);
        for (i = 0; error == NO_ERROR && i < routes->dwNumEntries; i++)
        {
            if (routes->table[i].dwForwardDest == 0 && routes->table[i].dwForwardMask == 0 &&
                routes->table[i].dwForwardNextHop != 0)
            {
                route->dwForwardNextHop = routes->table[i].dwForwardNextHop;
                route->dwForwardIfIndex = routes->table[i].dwForwardIfIndex;
                route->dwForwardType = MIB_IPROUTE_TYPE_INDIRECT;
                found = TRUE;
                break;
            }
        }
        HeapFree(GetProcessHeap(), 0, routes);
    }
    if (found)
        return TRUE;

    size = 0;
    error = GetIpAddrTable(NULL, &size, FALSE);
    if (error == ERROR_INSUFFICIENT_BUFFER && size && (addrs = HeapAlloc(GetProcessHeap(), 0, size)))
    {
        error = GetIpAddrTable(addrs, &size, FALSE);
        for (i = 0; error == NO_ERROR && i < addrs->dwNumEntries; i++)
        {
            DWORD addr = addrs->table[i].dwAddr;

            if (addr == 0 || (ntohl(addr) >> 24) == 127 || (ntohl(addr) >> 16) == 0xA9FE)
                continue;
            route->dwForwardNextHop = addr;
            route->dwForwardIfIndex = addrs->table[i].dwIndex;
            route->dwForwardType = MIB_IPROUTE_TYPE_DIRECT;
            found = TRUE;
            break;
        }
        HeapFree(GetProcessHeap(), 0, addrs);
    }
    return found;
}

static void test_cancel(void)
{
    OVERLAPPED ovr = {0};
    HANDLE event, handle = NULL;
    DWORD wait;
    BOOL ret;

    event = CreateEventW(NULL, FALSE, FALSE, NULL);
    ok(event != NULL, "CreateEvent failed\n");
    if (!event) return;

    if (!register_route_change(event, &ovr, &handle))
        goto cleanup;

    SetLastError(ERROR_SUCCESS);
    ret = CancelIPChangeNotify(&ovr);
    ok(ret, "CancelIPChangeNotify failed\n");
    ok_long(GetLastError(), ERROR_SUCCESS);

    /* The cancel completes the request with STATUS_CANCELLED, which signals
     * the event like any other completion. */
    wait = WaitForSingleObject(event, 1000);
    ok(wait == WAIT_OBJECT_0, "Canceled notification was not signaled\n");
    ok(OVERLAPPED_STATUS(&ovr) == STATUS_CANCELLED, "Expected STATUS_CANCELLED, got %lx\n", (ULONG)ovr.Internal);

cleanup:
    cleanup_notification(event, &ovr);
}

START_TEST(NotifyRouteChange)
{
    OVERLAPPED ovr = {0};
    HANDLE event, handle = NULL;
    MIB_IPFORWARDROW route = {0};
    DWORD error;
    BOOL added = FALSE;

    event = CreateEventW(NULL, FALSE, FALSE, NULL);
    ok(event != NULL, "CreateEvent failed\n");
    if (!event) return;

    if (!register_route_change(event, &ovr, &handle))
        goto cleanup;

    if (!find_test_route(&route))
    {
        win_skip("No IPv4 default route or non-loopback address, skipping route change checks\n");
        goto cleanup;
    }

    route.dwForwardDest = TEST_ROUTE_DEST;
    route.dwForwardMask = htonl(0xFFFFFFFF);
    route.dwForwardProto = MIB_IPPROTO_NETMGMT;
    route.dwForwardMetric1 = 1;

    error = CreateIpForwardEntry(&route);
    if (error == ERROR_OBJECT_ALREADY_EXISTS)
    {
        win_skip("Test route already exists\n");
    }
    else if (error != NO_ERROR)
    {
        win_skip("CreateIpForwardEntry failed (%lu), skipping route change checks\n", error);
        goto cleanup;
    }
    else
    {
        added = TRUE;
        wait_for_notification(event, &ovr, "add");
    }

    if (added)
    {
        if (!register_route_change(event, &ovr, &handle))
            goto cleanup;
    }

    error = DeleteIpForwardEntry(&route);
    if (added)
    {
        ok_long(error, NO_ERROR);
        if (error == NO_ERROR)
            wait_for_notification(event, &ovr, "delete");
    }

    test_cancel();

cleanup:
    cleanup_notification(event, &ovr);
}
