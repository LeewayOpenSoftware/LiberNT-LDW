/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     NSI table sizes, cross-table invariants and agreement with iphlpapi
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

typedef struct _TABLE_SHAPE
{
    const NPI_MODULEID *Module;
    const char *Name;
    DWORD Table;
    DWORD KeySize;
    DWORD RwSize;
    DWORD DynamicSize;
    DWORD StaticSize;
    BOOL Enumerable;
} TABLE_SHAPE;

static const TABLE_SHAPE Shapes[] =
{
    { &NPI_MS_NDIS_MODULEID, "ndis.ifinfo", NSI_NDIS_IFINFO_TABLE, sizeof(NET_LUID), sizeof(struct nsi_ndis_ifinfo_rw), sizeof(struct nsi_ndis_ifinfo_dynamic), sizeof(struct nsi_ndis_ifinfo_static), TRUE },
    { &NPI_MS_IPV4_MODULEID, "ipv4.compartment", NSI_IP_COMPARTMENT_TABLE, sizeof(UINT), sizeof(struct nsi_ip_cmpt_rw), sizeof(struct nsi_ip_cmpt_dynamic), 0, FALSE },
    { &NPI_MS_IPV4_MODULEID, "ipv4.icmpstats", NSI_IP_ICMPSTATS_TABLE, 0, 0, sizeof(struct nsi_ip_icmpstats_dynamic), 0, FALSE },
    { &NPI_MS_IPV4_MODULEID, "ipv4.ipstats", NSI_IP_IPSTATS_TABLE, 0, 0, sizeof(struct nsi_ip_ipstats_dynamic), sizeof(struct nsi_ip_ipstats_static), FALSE },
    { &NPI_MS_IPV4_MODULEID, "ipv4.interface", NSI_IP_INTERFACE_TABLE, sizeof(struct nsi_ip_interface_key), sizeof(struct nsi_ip_interface_rw), sizeof(struct nsi_ip_interface_dynamic), sizeof(struct nsi_ip_interface_static), TRUE },
    { &NPI_MS_IPV4_MODULEID, "ipv4.unicast", NSI_IP_UNICAST_TABLE, sizeof(struct nsi_ipv4_unicast_key), sizeof(struct nsi_ip_unicast_rw), sizeof(struct nsi_ip_unicast_dynamic), sizeof(struct nsi_ip_unicast_static), TRUE },
    { &NPI_MS_IPV4_MODULEID, "ipv4.neighbour", NSI_IP_NEIGHBOUR_TABLE, sizeof(struct nsi_ipv4_neighbour_key), sizeof(struct nsi_ip_neighbour_rw), sizeof(struct nsi_ip_neighbour_dynamic), 0, TRUE },
    { &NPI_MS_IPV4_MODULEID, "ipv4.forward", NSI_IP_FORWARD_TABLE, sizeof(struct nsi_ipv4_forward_key), sizeof(struct nsi_ip_forward_rw), sizeof(struct nsi_ipv4_forward_dynamic), sizeof(struct nsi_ip_forward_static), TRUE },
    { &NPI_MS_IPV6_MODULEID, "ipv6.compartment", NSI_IP_COMPARTMENT_TABLE, sizeof(UINT), sizeof(struct nsi_ip_cmpt_rw), sizeof(struct nsi_ip_cmpt_dynamic), 0, FALSE },
    { &NPI_MS_IPV6_MODULEID, "ipv6.icmpstats", NSI_IP_ICMPSTATS_TABLE, 0, 0, sizeof(struct nsi_ip_icmpstats_dynamic), 0, FALSE },
    { &NPI_MS_IPV6_MODULEID, "ipv6.ipstats", NSI_IP_IPSTATS_TABLE, 0, 0, sizeof(struct nsi_ip_ipstats_dynamic), sizeof(struct nsi_ip_ipstats_static), FALSE },
    { &NPI_MS_IPV6_MODULEID, "ipv6.interface", NSI_IP_INTERFACE_TABLE, sizeof(struct nsi_ip_interface_key), sizeof(struct nsi_ip_interface_rw), sizeof(struct nsi_ip_interface_dynamic), sizeof(struct nsi_ip_interface_static), TRUE },
    { &NPI_MS_IPV6_MODULEID, "ipv6.unicast", NSI_IP_UNICAST_TABLE, sizeof(struct nsi_ipv6_unicast_key), sizeof(struct nsi_ip_unicast_rw), sizeof(struct nsi_ip_unicast_dynamic), sizeof(struct nsi_ip_unicast_static), TRUE },
    { &NPI_MS_IPV6_MODULEID, "ipv6.neighbour", NSI_IP_NEIGHBOUR_TABLE, sizeof(struct nsi_ipv6_neighbour_key), sizeof(struct nsi_ip_neighbour_rw), sizeof(struct nsi_ip_neighbour_dynamic), 0, TRUE },
    { &NPI_MS_IPV6_MODULEID, "ipv6.forward", NSI_IP_FORWARD_TABLE, sizeof(struct nsi_ipv6_forward_key), sizeof(struct nsi_ip_forward_rw), sizeof(struct nsi_ipv6_forward_dynamic), sizeof(struct nsi_ip_forward_static), TRUE },
    { &NPI_MS_TCP_MODULEID, "tcp.stats", NSI_TCP_STATS_TABLE, sizeof(USHORT), 0, sizeof(struct nsi_tcp_stats_dynamic), sizeof(struct nsi_tcp_stats_static), FALSE },
    { &NPI_MS_TCP_MODULEID, "tcp.all", NSI_TCP_ALL_TABLE, sizeof(struct nsi_tcp_conn_key), 0, sizeof(struct nsi_tcp_conn_dynamic), sizeof(struct nsi_tcp_conn_static), TRUE },
    { &NPI_MS_TCP_MODULEID, "tcp.listen", NSI_TCP_LISTEN_TABLE, sizeof(struct nsi_tcp_conn_key), 0, sizeof(struct nsi_tcp_conn_dynamic), sizeof(struct nsi_tcp_conn_static), TRUE },
    { &NPI_MS_UDP_MODULEID, "udp.stats", NSI_UDP_STATS_TABLE, sizeof(USHORT), 0, sizeof(struct nsi_udp_stats_dynamic), 0, FALSE },
    { &NPI_MS_UDP_MODULEID, "udp.endpoint", NSI_UDP_ENDPOINT_TABLE, sizeof(struct nsi_udp_endpoint_key), 0, 0, sizeof(struct nsi_udp_endpoint_static), TRUE },
};

static NET_LUID *IfLuids;
static struct nsi_ndis_ifinfo_static *IfStatic;
static DWORD IfCount;

static BOOL LuidKnown(const NET_LUID *Luid)
{
    DWORD i;

    for (i = 0; i < IfCount; i++)
    {
        if (IfLuids[i].Value == Luid->Value)
            return TRUE;
    }
    return FALSE;
}

static void test_shapes(void)
{
    DWORD i, err, count;
    void *key, *rw, *dyn, *stat;
    BYTE block[sizeof(struct nsi_ip_icmpstats_dynamic)];

    for (i = 0; i < ARRAYSIZE(Shapes); i++)
    {
        const TABLE_SHAPE *s = &Shapes[i];

        winetest_push_context("%s", s->Name);
        if (s->Enumerable)
        {
            key = rw = dyn = stat = NULL;
            err = NsiAllocateAndGetTable(1, s->Module, s->Table, &key, s->KeySize,
                                         s->RwSize ? &rw : NULL, s->RwSize,
                                         s->DynamicSize ? &dyn : NULL, s->DynamicSize,
                                         s->StaticSize ? &stat : NULL, s->StaticSize, &count, 0);
            ok(err == ERROR_SUCCESS, "enumerate with the right sizes: %lu\n", err);
            if (err == ERROR_SUCCESS)
            {
                trace("%lu rows\n", count);
                NsiFreeTable(key, rw, dyn, stat);
            }
            key = rw = dyn = stat = NULL;
            err = NsiAllocateAndGetTable(1, s->Module, s->Table, &key, s->KeySize + 1,
                                         s->RwSize ? &rw : NULL, s->RwSize,
                                         s->DynamicSize ? &dyn : NULL, s->DynamicSize,
                                         s->StaticSize ? &stat : NULL, s->StaticSize, &count, 0);
            ok(err == ERROR_INVALID_PARAMETER, "enumerate with a wrong key size: %lu\n", err);
            if (err == ERROR_SUCCESS)
                NsiFreeTable(key, rw, dyn, stat);
        }
        else
        {
            UINT cmpt = 1;
            USHORT family = 0;
            const void *keyptr = NULL;

            if (s->Table == NSI_IP_COMPARTMENT_TABLE)
                keyptr = &cmpt;
            else if (s->KeySize == sizeof(USHORT))
                keyptr = &family;
            memset(block, 0, sizeof(block));
            err = NsiGetAllParameters(1, s->Module, s->Table, keyptr, s->KeySize,
                                      s->RwSize ? block : NULL, s->RwSize,
                                      s->DynamicSize ? block : NULL, s->DynamicSize,
                                      s->StaticSize ? block : NULL, s->StaticSize);
            ok(err == ERROR_SUCCESS, "get with the right sizes: %lu\n", err);
            err = NsiGetAllParameters(1, s->Module, s->Table, keyptr, s->KeySize,
                                      s->RwSize ? block : NULL, s->RwSize,
                                      block, s->DynamicSize + 4,
                                      s->StaticSize ? block : NULL, s->StaticSize);
            ok(err == ERROR_INVALID_PARAMETER, "get with a wrong dynamic size: %lu\n", err);
        }
        winetest_pop_context();
    }
}

static void test_ifinfo_vs_iphlpapi(void)
{
    struct nsi_ndis_ifinfo_rw *rw = NULL;
    struct nsi_ndis_ifinfo_dynamic *dyn = NULL;
    MIB_IF_TABLE2 *table = NULL;
    DWORD err, i, j, matched = 0;

    err = NsiAllocateAndGetTable(1, &NPI_MS_NDIS_MODULEID, NSI_NDIS_IFINFO_TABLE, (void **)&IfLuids, sizeof(*IfLuids),
                                 (void **)&rw, sizeof(*rw), (void **)&dyn, sizeof(*dyn),
                                 (void **)&IfStatic, sizeof(*IfStatic), &IfCount, 0);
    ok(err == ERROR_SUCCESS, "ifinfo: %lu\n", err);
    if (err != ERROR_SUCCESS)
        return;
    ok(IfCount >= 1, "no interfaces\n");

    err = GetIfTable2(&table);
    ok(err == NO_ERROR, "GetIfTable2: %lu\n", err);
    if (err == NO_ERROR)
    {
        ok(table->NumEntries == IfCount, "GetIfTable2 has %lu rows, NSI %lu\n", table->NumEntries, IfCount);
        for (i = 0; i < IfCount; i++)
        {
            for (j = 0; j < table->NumEntries; j++)
            {
                const MIB_IF_ROW2 *row = &table->Table[j];

                if (row->InterfaceLuid.Value != IfLuids[i].Value)
                    continue;
                matched++;
                ok(IfStatic[i].if_index == row->InterfaceIndex, "luid %I64x: index %u vs %lu\n", IfLuids[i].Value, IfStatic[i].if_index, row->InterfaceIndex);
                ok(IfStatic[i].type == row->Type, "luid %I64x: type %u vs %lu\n", IfLuids[i].Value, IfStatic[i].type, row->Type);
                ok(dyn[i].mtu == row->Mtu, "luid %I64x: mtu %u vs %lu\n", IfLuids[i].Value, dyn[i].mtu, row->Mtu);
                ok(dyn[i].oper_status == (UINT)row->OperStatus, "luid %I64x: oper %u vs %u\n", IfLuids[i].Value, dyn[i].oper_status, row->OperStatus);
                ok(dyn[i].media_conn_state == (UINT)row->MediaConnectState, "luid %I64x: media %u vs %u\n", IfLuids[i].Value, dyn[i].media_conn_state, row->MediaConnectState);
                ok(!wcscmp(rw[i].alias.String, row->Alias), "luid %I64x: alias %ls vs %ls\n", IfLuids[i].Value, rw[i].alias.String, row->Alias);
                ok(!memcmp(&IfStatic[i].if_guid, &row->InterfaceGuid, sizeof(GUID)), "luid %I64x: guid differs\n", IfLuids[i].Value);
            }
        }
        ok(matched == IfCount, "%lu of %lu NSI interfaces found in GetIfTable2\n", matched, IfCount);
        FreeMibTable(table);
    }

    for (i = 0; i < IfCount; i++)
    {
        NET_LUID luid;
        UINT index = IfStatic[i].if_index;

        memset(&luid, 0, sizeof(luid));
        err = NsiGetParameter(1, &NPI_MS_NDIS_MODULEID, NSI_NDIS_INDEX_LUID_TABLE, &index, sizeof(index),
                              NSI_PARAM_TYPE_STATIC, &luid, sizeof(luid), 0);
        ok(err == ERROR_SUCCESS, "index %u -> luid: %lu\n", index, err);
        ok(luid.Value == IfLuids[i].Value, "index %u maps to %I64x, expected %I64x\n", index, luid.Value, IfLuids[i].Value);
    }
    NsiFreeTable(NULL, rw, dyn, NULL);
}

static void test_unicast(int family)
{
    const NPI_MODULEID *mod = family == AF_INET ? &NPI_MS_IPV4_MODULEID : &NPI_MS_IPV6_MODULEID;
    DWORD key_size = family == AF_INET ? sizeof(struct nsi_ipv4_unicast_key) : sizeof(struct nsi_ipv6_unicast_key);
    BYTE *keys = NULL;
    struct nsi_ip_unicast_rw *rw = NULL;
    struct nsi_ip_unicast_dynamic *dyn = NULL;
    struct nsi_ip_unicast_static *stat = NULL;
    MIB_UNICASTIPADDRESS_TABLE *table = NULL;
    DWORD err, count, i, j, matched = 0;

    winetest_push_context("unicast %s", family == AF_INET ? "v4" : "v6");
    err = NsiAllocateAndGetTable(1, mod, NSI_IP_UNICAST_TABLE, (void **)&keys, key_size, (void **)&rw, sizeof(*rw),
                                 (void **)&dyn, sizeof(*dyn), (void **)&stat, sizeof(*stat), &count, 0);
    ok(err == ERROR_SUCCESS, "enumerate: %lu\n", err);
    err = GetUnicastIpAddressTable(family, &table);
    ok(err == NO_ERROR, "GetUnicastIpAddressTable: %lu\n", err);
    if (keys && table)
    {
        ok(table->NumEntries == count, "iphlpapi has %lu rows, NSI %lu\n", table->NumEntries, count);
        for (i = 0; i < count; i++)
        {
            const NET_LUID *luid = (const NET_LUID *)(keys + i * key_size);
            const void *addr = keys + i * key_size + sizeof(NET_LUID);
            DWORD addr_len = family == AF_INET ? sizeof(IN_ADDR) : sizeof(IN6_ADDR);

            ok(LuidKnown(luid), "row %lu: luid %I64x not in ifinfo\n", i, luid->Value);
            for (j = 0; j < table->NumEntries; j++)
            {
                const MIB_UNICASTIPADDRESS_ROW *row = &table->Table[j];
                const void *row_addr = family == AF_INET ? (const void *)&row->Address.Ipv4.sin_addr : (const void *)&row->Address.Ipv6.sin6_addr;

                if (row->InterfaceLuid.Value != luid->Value || memcmp(row_addr, addr, addr_len))
                    continue;
                matched++;
                ok(rw[i].on_link_prefix == row->OnLinkPrefixLength, "row %lu: prefix %u vs %u\n", i, rw[i].on_link_prefix, row->OnLinkPrefixLength);
                ok(rw[i].prefix_origin == (UINT)row->PrefixOrigin, "row %lu: prefix origin %u vs %u\n", i, rw[i].prefix_origin, row->PrefixOrigin);
                ok(rw[i].suffix_origin == (UINT)row->SuffixOrigin, "row %lu: suffix origin %u vs %u\n", i, rw[i].suffix_origin, row->SuffixOrigin);
                ok(dyn[i].dad_state == (UINT)row->DadState, "row %lu: dad %u vs %u\n", i, dyn[i].dad_state, row->DadState);
                ok(stat[i].creation_time == (ULONG64)row->CreationTimeStamp.QuadPart, "row %lu: creation time differs\n", i);
            }
        }
        ok(matched == count, "%lu of %lu NSI rows found in iphlpapi\n", matched, count);
    }
    if (table)
        FreeMibTable(table);
    if (keys)
        NsiFreeTable(keys, rw, dyn, stat);
    winetest_pop_context();
}

static void test_forward(int family)
{
    const NPI_MODULEID *mod = family == AF_INET ? &NPI_MS_IPV4_MODULEID : &NPI_MS_IPV6_MODULEID;
    DWORD key_size = family == AF_INET ? sizeof(struct nsi_ipv4_forward_key) : sizeof(struct nsi_ipv6_forward_key);
    BYTE *keys = NULL;
    struct nsi_ip_forward_rw *rw = NULL;
    struct nsi_ip_forward_static *stat = NULL;
    MIB_IPFORWARD_TABLE2 *table = NULL;
    DWORD err, count, i;

    winetest_push_context("forward %s", family == AF_INET ? "v4" : "v6");
    err = NsiAllocateAndGetTable(1, mod, NSI_IP_FORWARD_TABLE, (void **)&keys, key_size, (void **)&rw, sizeof(*rw),
                                 NULL, 0, (void **)&stat, sizeof(*stat), &count, 0);
    ok(err == ERROR_SUCCESS, "enumerate: %lu\n", err);
    err = GetIpForwardTable2(family, &table);
    ok(err == NO_ERROR, "GetIpForwardTable2: %lu\n", err);
    if (keys && table)
    {
        ok(table->NumEntries == count, "iphlpapi has %lu routes, NSI %lu\n", table->NumEntries, count);
        for (i = 0; i < count; i++)
        {
            const NET_LUID *luid;

            if (family == AF_INET)
            {
                const struct nsi_ipv4_forward_key *key = (const struct nsi_ipv4_forward_key *)(keys + i * key_size);
                luid = &key->luid;
                ok(key->prefix_len <= 32, "route %lu: prefix length %u\n", i, key->prefix_len);
            }
            else
            {
                const struct nsi_ipv6_forward_key *key = (const struct nsi_ipv6_forward_key *)(keys + i * key_size);
                luid = &key->luid;
                ok(key->prefix_len <= 128, "route %lu: prefix length %u\n", i, key->prefix_len);
            }
            ok(LuidKnown(luid), "route %lu: luid %I64x not in ifinfo\n", i, luid->Value);
            ok(stat[i].if_index != 0, "route %lu: interface index 0\n", i);
            ok(rw[i].metric != 0 || rw[i].loopback, "route %lu: metric 0 on a non-loopback route\n", i);
        }
    }
    if (family == AF_INET && keys)
    {
        struct nsi_ipv4_unicast_key *ukeys = NULL;
        struct nsi_ip_unicast_rw *urw = NULL;
        DWORD ucount, j, found;

        err = NsiAllocateAndGetTable(1, mod, NSI_IP_UNICAST_TABLE, (void **)&ukeys, sizeof(*ukeys), (void **)&urw, sizeof(*urw),
                                     NULL, 0, NULL, 0, &ucount, 0);
        ok(err == ERROR_SUCCESS, "unicast for the on-link check: %lu\n", err);
        for (j = 0; ukeys && j < ucount; j++)
        {
            DWORD mask = urw[j].on_link_prefix ? htonl(~0u << (32 - urw[j].on_link_prefix)) : 0;
            DWORD prefix = ukeys[j].addr.s_addr & mask;

            found = 0;
            for (i = 0; i < count; i++)
            {
                const struct nsi_ipv4_forward_key *key = (const struct nsi_ipv4_forward_key *)(keys + i * key_size);

                if (key->luid.Value == ukeys[j].luid.Value && key->prefix.s_addr == prefix &&
                    key->prefix_len == urw[j].on_link_prefix && key->next_hop.s_addr == 0)
                    found++;
            }
            ok(found == 1, "address %lu: %lu on-link route(s) for its prefix, expected 1\n", j, found);
        }
        if (ukeys)
            NsiFreeTable(ukeys, urw, NULL, NULL);
    }
    if (table)
        FreeMibTable(table);
    if (keys)
        NsiFreeTable(keys, rw, NULL, stat);
    winetest_pop_context();
}

static void test_neighbour(int family)
{
    const NPI_MODULEID *mod = family == AF_INET ? &NPI_MS_IPV4_MODULEID : &NPI_MS_IPV6_MODULEID;
    DWORD key_size = family == AF_INET ? sizeof(struct nsi_ipv4_neighbour_key) : sizeof(struct nsi_ipv6_neighbour_key);
    BYTE *keys = NULL;
    struct nsi_ip_neighbour_dynamic *dyn = NULL;
    MIB_IPNET_TABLE2 *table = NULL;
    DWORD err, count, i;

    winetest_push_context("neighbour %s", family == AF_INET ? "v4" : "v6");
    err = NsiAllocateAndGetTable(1, mod, NSI_IP_NEIGHBOUR_TABLE, (void **)&keys, key_size, NULL, 0,
                                 (void **)&dyn, sizeof(*dyn), NULL, 0, &count, 0);
    ok(err == ERROR_SUCCESS, "enumerate: %lu\n", err);
    err = GetIpNetTable2(family, &table);
    ok(err == NO_ERROR || err == ERROR_NOT_FOUND, "GetIpNetTable2: %lu\n", err);
    if (keys && table)
    {
        LONG delta = (LONG)table->NumEntries - (LONG)count;

        ok(delta >= -2 && delta <= 2, "iphlpapi has %lu neighbours, NSI %lu\n", table->NumEntries, count);
        for (i = 0; i < count; i++)
        {
            const NET_LUID *luid = (const NET_LUID *)(keys + i * key_size);

            ok(LuidKnown(luid), "neighbour %lu: luid %I64x not in ifinfo\n", i, luid->Value);
            ok(dyn[i].state <= NlnsPermanent, "neighbour %lu: state %u\n", i, dyn[i].state);
        }
    }
    if (table)
        FreeMibTable(table);
    if (keys)
        NsiFreeTable(keys, NULL, dyn, NULL);
    winetest_pop_context();
}

static void test_endpoints(void)
{
    struct nsi_tcp_conn_key *tkeys = NULL;
    struct nsi_tcp_conn_dynamic *tdyn = NULL;
    struct nsi_tcp_conn_static *tstat = NULL;
    struct nsi_udp_endpoint_key *ukeys = NULL;
    struct nsi_udp_endpoint_static *ustat = NULL;
    struct sockaddr_in addr = { 0 };
    int addr_len = sizeof(addr);
    SOCKET tcp, udp;
    WSADATA data;
    DWORD err, count, i, found;
    USHORT tcp_port = 0, udp_port = 0;

    if (WSAStartup(MAKEWORD(2, 2), &data))
    {
        skip("WSAStartup failed\n");
        return;
    }
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ok(tcp != INVALID_SOCKET && udp != INVALID_SOCKET, "socket creation failed: %d\n", WSAGetLastError());
    if (tcp != INVALID_SOCKET && !bind(tcp, (struct sockaddr *)&addr, sizeof(addr)) && !listen(tcp, 1) &&
        !getsockname(tcp, (struct sockaddr *)&addr, &addr_len))
        tcp_port = addr.sin_port;
    addr.sin_port = 0;
    if (udp != INVALID_SOCKET && !bind(udp, (struct sockaddr *)&addr, sizeof(addr)) &&
        !getsockname(udp, (struct sockaddr *)&addr, &addr_len))
        udp_port = addr.sin_port;
    ok(tcp_port != 0 && udp_port != 0, "bind failed: %d\n", WSAGetLastError());

    err = NsiAllocateAndGetTable(1, &NPI_MS_TCP_MODULEID, NSI_TCP_LISTEN_TABLE, (void **)&tkeys, sizeof(*tkeys), NULL, 0,
                                 (void **)&tdyn, sizeof(*tdyn), (void **)&tstat, sizeof(*tstat), &count, 0);
    ok(err == ERROR_SUCCESS, "tcp listen table: %lu\n", err);
    found = 0;
    for (i = 0; tkeys && i < count; i++)
    {
        if (tkeys[i].local.si_family == AF_INET && tkeys[i].local.Ipv4.sin_port == tcp_port)
        {
            found++;
            ok(tdyn[i].state == MIB_TCP_STATE_LISTEN, "listener state %u\n", tdyn[i].state);
            ok(tstat[i].pid == GetCurrentProcessId(), "listener pid %u, expected %lu\n", tstat[i].pid, GetCurrentProcessId());
            ok(tstat[i].create_time != 0, "listener has no creation time\n");
        }
    }
    ok(found == 1, "listening socket on port %u appears %lu times\n", ntohs(tcp_port), found);
    if (tkeys)
        NsiFreeTable(tkeys, NULL, tdyn, tstat);

    err = NsiAllocateAndGetTable(1, &NPI_MS_UDP_MODULEID, NSI_UDP_ENDPOINT_TABLE, (void **)&ukeys, sizeof(*ukeys), NULL, 0,
                                 NULL, 0, (void **)&ustat, sizeof(*ustat), &count, 0);
    ok(err == ERROR_SUCCESS, "udp endpoint table: %lu\n", err);
    found = 0;
    for (i = 0; ukeys && i < count; i++)
    {
        if (ukeys[i].local.si_family == AF_INET && ukeys[i].local.Ipv4.sin_port == udp_port)
        {
            found++;
            ok(ustat[i].pid == GetCurrentProcessId(), "udp pid %u, expected %lu\n", ustat[i].pid, GetCurrentProcessId());
        }
    }
    ok(found == 1, "udp socket on port %u appears %lu times\n", ntohs(udp_port), found);
    if (ukeys)
        NsiFreeTable(ukeys, NULL, NULL, ustat);

    if (tcp != INVALID_SOCKET)
        closesocket(tcp);
    if (udp != INVALID_SOCKET)
        closesocket(udp);
    WSACleanup();
}

static void test_stats(void)
{
    struct nsi_ip_ipstats_dynamic ip4a, ip4b, ip6a, ip6b;
    struct nsi_tcp_stats_dynamic tcpa, tcpb;
    struct nsi_udp_stats_dynamic udpa, udpb;
    struct nsi_ip_icmpstats_dynamic icmp4;
    USHORT family = AF_INET;
    struct sockaddr_in addr = { 0 };
    int addr_len = sizeof(addr);
    SOCKET udp;
    WSADATA data;
    char payload[32] = "nsi stats probe";
    DWORD err, i;

    err = NsiGetAllParameters(1, &NPI_MS_IPV4_MODULEID, NSI_IP_IPSTATS_TABLE, NULL, 0, NULL, 0, &ip4a, sizeof(ip4a), NULL, 0);
    ok(err == ERROR_SUCCESS, "ipv4 ipstats: %lu\n", err);
    err = NsiGetAllParameters(1, &NPI_MS_IPV6_MODULEID, NSI_IP_IPSTATS_TABLE, NULL, 0, NULL, 0, &ip6a, sizeof(ip6a), NULL, 0);
    ok(err == ERROR_SUCCESS, "ipv6 ipstats: %lu\n", err);
    err = NsiGetAllParameters(1, &NPI_MS_TCP_MODULEID, NSI_TCP_STATS_TABLE, &family, sizeof(family), NULL, 0, &tcpa, sizeof(tcpa), NULL, 0);
    ok(err == ERROR_SUCCESS, "tcp stats: %lu\n", err);
    err = NsiGetAllParameters(1, &NPI_MS_UDP_MODULEID, NSI_UDP_STATS_TABLE, &family, sizeof(family), NULL, 0, &udpa, sizeof(udpa), NULL, 0);
    ok(err == ERROR_SUCCESS, "udp stats: %lu\n", err);
    err = NsiGetAllParameters(1, &NPI_MS_IPV4_MODULEID, NSI_IP_ICMPSTATS_TABLE, NULL, 0, NULL, 0, &icmp4, sizeof(icmp4), NULL, 0);
    ok(err == ERROR_SUCCESS, "ipv4 icmpstats: %lu\n", err);
    trace("before: ip4 in %I64u out %I64u, ip6 in %I64u out %I64u, tcp in %I64u out %I64u, udp in %I64u out %I64u, icmp4 in %u out %u\n",
          ip4a.in_recv, ip4a.out_reqs, ip6a.in_recv, ip6a.out_reqs, tcpa.in_segs, tcpa.out_segs,
          udpa.in_dgrams, udpa.out_dgrams, icmp4.in_msgs, icmp4.out_msgs);

    if (WSAStartup(MAKEWORD(2, 2), &data))
    {
        skip("WSAStartup failed\n");
        return;
    }
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp != INVALID_SOCKET && !bind(udp, (struct sockaddr *)&addr, sizeof(addr)) &&
        !getsockname(udp, (struct sockaddr *)&addr, &addr_len))
    {
        DWORD timeout = 1000;

        setsockopt(udp, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
        for (i = 0; i < 5; i++)
        {
            ok(sendto(udp, payload, sizeof(payload), 0, (struct sockaddr *)&addr, sizeof(addr)) == sizeof(payload), "sendto %lu failed: %d\n", i, WSAGetLastError());
            ok(recv(udp, payload, sizeof(payload), 0) == sizeof(payload), "recv %lu failed: %d\n", i, WSAGetLastError());
        }
    }
    else
        skip("no udp loopback socket: %d\n", WSAGetLastError());
    if (udp != INVALID_SOCKET)
        closesocket(udp);
    WSACleanup();

    err = NsiGetAllParameters(1, &NPI_MS_IPV4_MODULEID, NSI_IP_IPSTATS_TABLE, NULL, 0, NULL, 0, &ip4b, sizeof(ip4b), NULL, 0);
    ok(err == ERROR_SUCCESS, "ipv4 ipstats again: %lu\n", err);
    err = NsiGetAllParameters(1, &NPI_MS_IPV6_MODULEID, NSI_IP_IPSTATS_TABLE, NULL, 0, NULL, 0, &ip6b, sizeof(ip6b), NULL, 0);
    ok(err == ERROR_SUCCESS, "ipv6 ipstats again: %lu\n", err);
    err = NsiGetAllParameters(1, &NPI_MS_TCP_MODULEID, NSI_TCP_STATS_TABLE, &family, sizeof(family), NULL, 0, &tcpb, sizeof(tcpb), NULL, 0);
    ok(err == ERROR_SUCCESS, "tcp stats again: %lu\n", err);
    err = NsiGetAllParameters(1, &NPI_MS_UDP_MODULEID, NSI_UDP_STATS_TABLE, &family, sizeof(family), NULL, 0, &udpb, sizeof(udpb), NULL, 0);
    ok(err == ERROR_SUCCESS, "udp stats again: %lu\n", err);
    trace("after: ip4 in %I64u out %I64u, ip6 in %I64u out %I64u, tcp in %I64u out %I64u, udp in %I64u out %I64u\n",
          ip4b.in_recv, ip4b.out_reqs, ip6b.in_recv, ip6b.out_reqs, tcpb.in_segs, tcpb.out_segs, udpb.in_dgrams, udpb.out_dgrams);

    ok(ip4b.in_recv >= ip4a.in_recv && ip4b.out_reqs >= ip4a.out_reqs, "ipv4 counters went backwards\n");
    ok(ip6b.in_recv >= ip6a.in_recv && ip6b.out_reqs >= ip6a.out_reqs, "ipv6 counters went backwards\n");
    ok(tcpb.in_segs >= tcpa.in_segs && tcpb.out_segs >= tcpa.out_segs, "tcp counters went backwards\n");
    ok(udpb.in_dgrams >= udpa.in_dgrams && udpb.out_dgrams >= udpa.out_dgrams, "udp counters went backwards\n");
    ok(udpb.out_dgrams >= udpa.out_dgrams + 5, "udp out datagrams grew by %I64u after 5 sends\n", udpb.out_dgrams - udpa.out_dgrams);
    ok(udpb.in_dgrams >= udpa.in_dgrams + 5, "udp in datagrams grew by %I64u after 5 receives\n", udpb.in_dgrams - udpa.in_dgrams);
    ok(ip4b.out_reqs >= ip4a.out_reqs + 5, "ipv4 out requests grew by %I64u after 5 datagrams\n", ip4b.out_reqs - ip4a.out_reqs);
}

START_TEST(tables)
{
    test_shapes();
    test_ifinfo_vs_iphlpapi();
    if (!IfLuids)
    {
        skip("no interface table\n");
        return;
    }
    test_unicast(AF_INET);
    test_unicast(AF_INET6);
    test_forward(AF_INET);
    test_forward(AF_INET6);
    test_neighbour(AF_INET);
    test_neighbour(AF_INET6);
    test_endpoints();
    test_stats();
    NsiFreeTable(IfLuids, NULL, NULL, IfStatic);
}
