/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Network store (NSI) provider and table access test, user half
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <kmt_test.h>
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <netiodef.h>
#include <wine/nsi.h>

#include "nsi_store.h"

static const GUID StoreTestGuid = NSISTORE_MODULE_GUID;

static HANDLE
OpenNsiDevice(
    _In_ DWORD Flags)
{
    return CreateFileW(L"\\\\.\\Nsi", 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, Flags, NULL);
}

static DWORD
Ioctl(
    _In_ HANDLE Device,
    _In_ DWORD Code,
    _In_reads_bytes_(InSize) PVOID In,
    _In_ DWORD InSize,
    _Out_writes_bytes_opt_(OutSize) PVOID Out,
    _In_ DWORD OutSize,
    _Out_ PDWORD Returned)
{
    *Returned = 0;
    if (DeviceIoControl(Device, Code, In, InSize, Out, OutSize, Returned, NULL))
        return ERROR_SUCCESS;
    return GetLastError();
}

static void
TestDeviceEnumerate(
    _In_ HANDLE Device,
    _In_ const NPI_MODULEID *Module)
{
    struct nsiproxy_enumerate_all in;
    BYTE out[sizeof(DWORD) + 4 * (sizeof(ULONG) + sizeof(NSISTORE_RW) + sizeof(NSISTORE_DYNAMIC) + sizeof(NSISTORE_STATIC))];
    DWORD returned, err, count;
    const ULONG *keys;
    const NSISTORE_DYNAMIC *dynamic;

    memset(&in, 0, sizeof(in));
    in.module = *Module;
    in.table = NSISTORE_TABLE;
    in.key_size = sizeof(ULONG);
    in.rw_size = sizeof(NSISTORE_RW);
    in.dynamic_size = sizeof(NSISTORE_DYNAMIC);
    in.static_size = sizeof(NSISTORE_STATIC);
    in.count = 4;
    memset(out, 0, sizeof(out));
    err = Ioctl(Device, IOCTL_NSIPROXY_WINE_ENUMERATE_ALL, &in, sizeof(in), out, sizeof(out), &returned);
    ok_eq_ulong(err, (DWORD)ERROR_SUCCESS);
    count = *(DWORD *)out;
    ok_eq_ulong(count, (DWORD)NSISTORE_ROWS);
    keys = (const ULONG *)(out + sizeof(DWORD));
    dynamic = (const NSISTORE_DYNAMIC *)(out + sizeof(DWORD) + 4 * (sizeof(ULONG) + sizeof(NSISTORE_RW)));
    ok_eq_ulong(keys[0], 1UL);
    ok_eq_ulong(keys[1], 2UL);
    ok_eq_ulong(dynamic[1].X, (ULONG)NSISTORE_X(2));
    ok(returned >= sizeof(DWORD) + 2 * sizeof(ULONG), "enumerate returned %lu bytes\n", returned);

    in.count = 1;
    memset(out, 0, sizeof(out));
    err = Ioctl(Device, IOCTL_NSIPROXY_WINE_ENUMERATE_ALL, &in, sizeof(in), out, sizeof(out), &returned);
    ok_eq_ulong(err, (DWORD)ERROR_MORE_DATA);
    ok_eq_ulong(*(DWORD *)out, (DWORD)NSISTORE_ROWS);
    ok_eq_ulong(keys[0], 1UL);

    in.count = 4;
    in.rw_size = sizeof(NSISTORE_RW) + 4;
    err = Ioctl(Device, IOCTL_NSIPROXY_WINE_ENUMERATE_ALL, &in, sizeof(in), out, sizeof(out), &returned);
    ok_eq_ulong(err, (DWORD)ERROR_INVALID_PARAMETER);
    in.rw_size = sizeof(NSISTORE_RW);

    err = Ioctl(Device, IOCTL_NSIPROXY_WINE_ENUMERATE_ALL, &in, sizeof(in), out, sizeof(DWORD), &returned);
    ok_eq_ulong(err, (DWORD)ERROR_INSUFFICIENT_BUFFER);

    err = Ioctl(Device, IOCTL_NSIPROXY_WINE_ENUMERATE_ALL, &in, sizeof(in) - 4, out, sizeof(out), &returned);
    ok_eq_ulong(err, (DWORD)ERROR_INVALID_PARAMETER);

    in.table = NSISTORE_SCRATCH_TABLE;
    err = Ioctl(Device, IOCTL_NSIPROXY_WINE_ENUMERATE_ALL, &in, sizeof(in), out, sizeof(out), &returned);
    ok_eq_ulong(err, (DWORD)ERROR_NOT_FOUND);
}

static void
TestDeviceRows(
    _In_ HANDLE Device,
    _In_ const NPI_MODULEID *Module)
{
    struct
    {
        struct nsiproxy_get_all_parameters header;
        BYTE key_rest[sizeof(ULONG) - 1];
    } get_all;
    struct
    {
        struct nsiproxy_get_parameter header;
        BYTE key_rest[sizeof(ULONG) - 1];
    } get_param;
    BYTE out[sizeof(NSISTORE_RW) + sizeof(NSISTORE_DYNAMIC) + sizeof(NSISTORE_STATIC)];
    const NSISTORE_RW *rw = (const NSISTORE_RW *)out;
    const NSISTORE_STATIC *stat = (const NSISTORE_STATIC *)(out + sizeof(NSISTORE_RW) + sizeof(NSISTORE_DYNAMIC));
    DWORD returned, err;
    ULONG value, key;

    memset(&get_all, 0, sizeof(get_all));
    get_all.header.module = *Module;
    get_all.header.table = NSISTORE_TABLE;
    get_all.header.key_size = sizeof(ULONG);
    get_all.header.rw_size = sizeof(NSISTORE_RW);
    get_all.header.dynamic_size = sizeof(NSISTORE_DYNAMIC);
    get_all.header.static_size = sizeof(NSISTORE_STATIC);
    key = 2;
    memcpy(get_all.header.key, &key, sizeof(key));
    memset(out, 0, sizeof(out));
    err = Ioctl(Device, IOCTL_NSIPROXY_WINE_GET_ALL_PARAMETERS, &get_all, sizeof(get_all), out, sizeof(out), &returned);
    ok_eq_ulong(err, (DWORD)ERROR_SUCCESS);
    ok_eq_ulong(returned, (DWORD)sizeof(out));
    ok_eq_ulong(rw->B, (ULONG)NSISTORE_RW_B(2));
    ok_eq_ulong(stat->S[1], (ULONG)NSISTORE_S1(2));

    key = 3;
    memcpy(get_all.header.key, &key, sizeof(key));
    err = Ioctl(Device, IOCTL_NSIPROXY_WINE_GET_ALL_PARAMETERS, &get_all, sizeof(get_all), out, sizeof(out), &returned);
    ok_eq_ulong(err, (DWORD)ERROR_NOT_FOUND);

    key = 1;
    memcpy(get_all.header.key, &key, sizeof(key));
    err = Ioctl(Device, IOCTL_NSIPROXY_WINE_GET_ALL_PARAMETERS, &get_all, sizeof(get_all), out, sizeof(out) - 1, &returned);
    ok_eq_ulong(err, (DWORD)ERROR_INSUFFICIENT_BUFFER);

    get_all.header.key_size = sizeof(USHORT);
    err = Ioctl(Device, IOCTL_NSIPROXY_WINE_GET_ALL_PARAMETERS, &get_all, sizeof(get_all), out, sizeof(out), &returned);
    ok_eq_ulong(err, (DWORD)ERROR_INVALID_PARAMETER);

    memset(&get_param, 0, sizeof(get_param));
    get_param.header.module = *Module;
    get_param.header.table = NSISTORE_TABLE;
    get_param.header.key_size = sizeof(ULONG);
    get_param.header.param_type = NSI_PARAM_TYPE_DYNAMIC;
    get_param.header.data_offset = FIELD_OFFSET(NSISTORE_DYNAMIC, Y);
    key = 1;
    memcpy(get_param.header.key, &key, sizeof(key));
    value = 0;
    err = Ioctl(Device, IOCTL_NSIPROXY_WINE_GET_PARAMETER, &get_param, sizeof(get_param), &value, sizeof(value), &returned);
    ok_eq_ulong(err, (DWORD)ERROR_SUCCESS);
    ok_eq_ulong(returned, (DWORD)sizeof(value));
    ok_eq_ulong(value, (ULONG)NSISTORE_Y(1));

    get_param.header.data_offset = FIELD_OFFSET(NSISTORE_DYNAMIC, X);
    memset(out, 0xEE, sizeof(out));
    err = Ioctl(Device, IOCTL_NSIPROXY_WINE_GET_PARAMETER, &get_param, sizeof(get_param), out, sizeof(out), &returned);
    ok_eq_ulong(err, (DWORD)ERROR_SUCCESS);
    ok_eq_ulong(returned, (DWORD)(sizeof(NSISTORE_DYNAMIC) - FIELD_OFFSET(NSISTORE_DYNAMIC, X)));
    ok_eq_ulong(((const ULONG *)out)[0], (ULONG)NSISTORE_X(1));
    ok_eq_ulong(((const ULONG *)out)[1], (ULONG)NSISTORE_Y(1));
    ok(out[returned] == 0xEE, "byte after the clamped parameter read was written: 0x%02x\n", out[returned]);

    get_param.header.data_offset = sizeof(NSISTORE_DYNAMIC);
    err = Ioctl(Device, IOCTL_NSIPROXY_WINE_GET_PARAMETER, &get_param, sizeof(get_param), &value, sizeof(value), &returned);
    ok_eq_ulong(err, (DWORD)ERROR_INVALID_PARAMETER);

    get_param.header.data_offset = 0;
    get_param.header.param_type = NSI_PARAM_TYPE_STATIC + 1;
    err = Ioctl(Device, IOCTL_NSIPROXY_WINE_GET_PARAMETER, &get_param, sizeof(get_param), &value, sizeof(value), &returned);
    ok_eq_ulong(err, (DWORD)ERROR_INVALID_PARAMETER);

    err = Ioctl(Device, IOCTL_NSIPROXY_WINE_ICMP_ECHO, &get_param, sizeof(get_param), out, sizeof(out), &returned);
    ok_eq_ulong(err, (DWORD)ERROR_NOT_SUPPORTED);
}

static void
TestDeviceInterfaces(
    _In_ HANDLE Device)
{
    struct nsiproxy_enumerate_all in;
    struct
    {
        struct nsiproxy_get_parameter header;
        BYTE key_rest[sizeof(UINT) - 1];
    } get_param;
    const UINT rows = 32;
    BYTE *out;
    DWORD out_size, returned, err, count, i;
    const NET_LUID *luids;
    const struct nsi_ndis_ifinfo_static *stat;
    MIB_IFTABLE *table = NULL;
    ULONG table_size = 0;

    memset(&in, 0, sizeof(in));
    in.module = NPI_MS_NDIS_MODULEID;
    in.table = NSI_NDIS_IFINFO_TABLE;
    in.key_size = sizeof(NET_LUID);
    in.static_size = sizeof(struct nsi_ndis_ifinfo_static);
    in.count = rows;
    out_size = sizeof(DWORD) + rows * (in.key_size + in.static_size);
    out = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, out_size);
    if (!skip(out != NULL, "no buffer for the interface table\n"))
    {
        err = Ioctl(Device, IOCTL_NSIPROXY_WINE_ENUMERATE_ALL, &in, sizeof(in), out, out_size, &returned);
        if (!skip(err != ERROR_NOT_FOUND, "tcpip does not provide the NDIS interface table\n"))
        {
            ok_eq_ulong(err, (DWORD)ERROR_SUCCESS);
            count = *(DWORD *)out;
            ok(count >= 1 && count <= rows, "interface table has %lu rows\n", count);
            luids = (const NET_LUID *)(out + sizeof(DWORD));
            stat = (const struct nsi_ndis_ifinfo_static *)(out + sizeof(DWORD) + rows * in.key_size);
            for (i = 0; i < count && i < rows; i++)
            {
                NET_LUID luid;

                ok(stat[i].if_index != 0, "row %lu has interface index 0\n", i);
                ok_eq_ulong((ULONG)luids[i].Info.NetLuidIndex, (ULONG)stat[i].if_index);
                ok_eq_ulong((ULONG)luids[i].Info.IfType, (ULONG)stat[i].type);
                ok(stat[i].descr.Length != 0, "row %lu has no description\n", i);
                trace("interface %u type %u luid %I64x\n", stat[i].if_index, stat[i].type, luids[i].Value);

                memset(&get_param, 0, sizeof(get_param));
                get_param.header.module = NPI_MS_NDIS_MODULEID;
                get_param.header.table = NSI_NDIS_INDEX_LUID_TABLE;
                get_param.header.key_size = sizeof(UINT);
                get_param.header.param_type = NSI_PARAM_TYPE_STATIC;
                memcpy(get_param.header.key, &stat[i].if_index, sizeof(UINT));
                luid.Value = 0;
                err = Ioctl(Device, IOCTL_NSIPROXY_WINE_GET_PARAMETER, &get_param, sizeof(get_param), &luid, sizeof(luid), &returned);
                ok_eq_ulong(err, (DWORD)ERROR_SUCCESS);
                ok(luid.Value == luids[i].Value, "index %u maps to luid %I64x, enumerated %I64x\n", stat[i].if_index, luid.Value, luids[i].Value);
            }
            if (GetIfTable(NULL, &table_size, FALSE) == ERROR_INSUFFICIENT_BUFFER)
                table = HeapAlloc(GetProcessHeap(), 0, table_size);
            if (table != NULL && GetIfTable(table, &table_size, FALSE) == NO_ERROR)
                ok_eq_ulong(count, table->dwNumEntries);
            else
                skip(FALSE, "GetIfTable unavailable for the row count comparison\n");
            if (table != NULL)
                HeapFree(GetProcessHeap(), 0, table);
        }
        HeapFree(GetProcessHeap(), 0, out);
    }
}

static DWORD
EnumerateTable(
    _In_ HANDLE Device,
    _In_ const NPI_MODULEID *Module,
    _In_ UINT Table,
    _In_reads_(4) const UINT *Sizes,
    _In_ UINT Rows,
    _Outptr_result_maybenull_ BYTE **Out,
    _Out_ DWORD *Count)
{
    struct nsiproxy_enumerate_all in;
    DWORD out_size = sizeof(DWORD) + Rows * (Sizes[0] + Sizes[1] + Sizes[2] + Sizes[3]), returned, err;

    *Count = 0;
    *Out = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, out_size);
    if (*Out == NULL)
        return ERROR_OUTOFMEMORY;
    memset(&in, 0, sizeof(in));
    in.module = *Module;
    in.table = Table;
    in.key_size = Sizes[0];
    in.rw_size = Sizes[1];
    in.dynamic_size = Sizes[2];
    in.static_size = Sizes[3];
    in.count = Rows;
    err = Ioctl(Device, IOCTL_NSIPROXY_WINE_ENUMERATE_ALL, &in, sizeof(in), *Out, out_size, &returned);
    if (err == ERROR_SUCCESS || err == ERROR_MORE_DATA)
        *Count = *(DWORD *)*Out;
    return err;
}

#define TABLE_ROWS 64

static BOOL
IsIn6Loopback(
    _In_reads_(16) const BYTE *Address)
{
    static const BYTE loopback[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };

    return memcmp(Address, loopback, sizeof(loopback)) == 0;
}
#define SECTION(out, rows, offset) ((out) + sizeof(DWORD) + (rows) * (offset))

static BOOL
LuidKnown(
    _In_ const NET_LUID *Luids,
    _In_ DWORD Count,
    _In_ NET_LUID Luid)
{
    DWORD i;

    for (i = 0; i < Count; i++)
        if (Luids[i].Value == Luid.Value)
            return TRUE;
    return FALSE;
}

static void
TestDeviceIpTables(
    _In_ HANDLE Device,
    _In_ ADDRESS_FAMILY Family)
{
    const NPI_MODULEID *module = Family == AF_INET ? &NPI_MS_IPV4_MODULEID : &NPI_MS_IPV6_MODULEID;
    const UINT bits = Family == AF_INET ? 32 : 128;
    const UINT if_sizes[4] = { sizeof(NET_LUID), 0, 0, sizeof(struct nsi_ndis_ifinfo_static) };
    const UINT ipif_sizes[4] = { sizeof(struct nsi_ip_interface_key), sizeof(struct nsi_ip_interface_rw), sizeof(struct nsi_ip_interface_dynamic), 0 };
    const UINT uni_sizes[4] = { Family == AF_INET ? sizeof(struct nsi_ipv4_unicast_key) : sizeof(struct nsi_ipv6_unicast_key), sizeof(struct nsi_ip_unicast_rw), sizeof(struct nsi_ip_unicast_dynamic), sizeof(struct nsi_ip_unicast_static) };
    const UINT nb_sizes[4] = { Family == AF_INET ? sizeof(struct nsi_ipv4_neighbour_key) : sizeof(struct nsi_ipv6_neighbour_key), sizeof(struct nsi_ip_neighbour_rw), sizeof(struct nsi_ip_neighbour_dynamic), 0 };
    const UINT fw_sizes[4] = { Family == AF_INET ? sizeof(struct nsi_ipv4_forward_key) : sizeof(struct nsi_ipv6_forward_key), sizeof(struct nsi_ip_forward_rw), Family == AF_INET ? sizeof(struct nsi_ipv4_forward_dynamic) : sizeof(struct nsi_ipv6_forward_dynamic), sizeof(struct nsi_ip_forward_static) };
    BYTE *ifinfo = NULL, *ipif = NULL, *uni = NULL, *nb = NULL, *fw = NULL;
    const NET_LUID *luids;
    const struct nsi_ndis_ifinfo_static *ifstat;
    NET_LUID loopback_luid;
    DWORD ifcount, count, i, err;
    BOOL have_loopback_addr = FALSE;
    MIB_UNICASTIPADDRESS_TABLE *unicast = NULL;
    MIB_IPFORWARD_TABLE2 *forward = NULL;
    MIB_IPNET_TABLE2 *neighbours = NULL;

    loopback_luid.Value = 0;
    err = EnumerateTable(Device, &NPI_MS_NDIS_MODULEID, NSI_NDIS_IFINFO_TABLE, if_sizes, TABLE_ROWS, &ifinfo, &ifcount);
    if (skip(err == ERROR_SUCCESS && ifcount != 0, "no interface table for the IP checks (%lu)\n", err))
        goto done;
    luids = (const NET_LUID *)SECTION(ifinfo, TABLE_ROWS, 0);
    ifstat = (const struct nsi_ndis_ifinfo_static *)SECTION(ifinfo, TABLE_ROWS, if_sizes[0]);
    for (i = 0; i < ifcount && i < TABLE_ROWS; i++)
        if (ifstat[i].type == MIB_IF_TYPE_LOOPBACK)
            loopback_luid = luids[i];

    err = EnumerateTable(Device, module, NSI_IP_INTERFACE_TABLE, ipif_sizes, TABLE_ROWS, &ipif, &count);
    if (skip(err != ERROR_NOT_FOUND, "tcpip does not provide the IP tables for family %u yet\n", Family))
        goto done;
    ok_eq_ulong(err, (DWORD)ERROR_SUCCESS);
    ok(count >= 1 && count <= TABLE_ROWS, "family %u interface table has %lu rows\n", Family, count);
    for (i = 0; i < count && i < TABLE_ROWS; i++)
    {
        const struct nsi_ip_interface_key *key = (const struct nsi_ip_interface_key *)SECTION(ipif, TABLE_ROWS, 0) + i;
        const struct nsi_ip_interface_dynamic *dyn = (const struct nsi_ip_interface_dynamic *)SECTION(ipif, TABLE_ROWS, ipif_sizes[0] + ipif_sizes[1]) + i;

        ok(LuidKnown(luids, ifcount, key->luid), "family %u interface row %lu: luid %I64x unknown\n", Family, i, key->luid.Value);
        ok(dyn->if_index != 0, "family %u interface row %lu: index 0\n", Family, i);
        ok_eq_ulong((ULONG)dyn->if_index, (ULONG)key->luid.Info.NetLuidIndex);
    }

    err = EnumerateTable(Device, module, NSI_IP_UNICAST_TABLE, uni_sizes, TABLE_ROWS, &uni, &count);
    ok_eq_ulong(err, (DWORD)ERROR_SUCCESS);
    trace("family %u: %lu unicast rows\n", Family, count);
    for (i = 0; i < count && i < TABLE_ROWS; i++)
    {
        const BYTE *key = SECTION(uni, TABLE_ROWS, 0) + i * uni_sizes[0];
        const struct nsi_ip_unicast_rw *rw = (const struct nsi_ip_unicast_rw *)SECTION(uni, TABLE_ROWS, uni_sizes[0]) + i;
        const struct nsi_ip_unicast_dynamic *dyn = (const struct nsi_ip_unicast_dynamic *)SECTION(uni, TABLE_ROWS, uni_sizes[0] + uni_sizes[1]) + i;
        NET_LUID luid;

        memcpy(&luid, key, sizeof(luid));
        ok(LuidKnown(luids, ifcount, luid), "family %u unicast row %lu: luid %I64x unknown\n", Family, i, luid.Value);
        ok(rw->on_link_prefix <= bits, "family %u unicast row %lu: prefix %u\n", Family, i, rw->on_link_prefix);
        ok(dyn->dad_state <= IpDadStatePreferred, "family %u unicast row %lu: dad state %u\n", Family, i, dyn->dad_state);
        if (Family == AF_INET6 && luid.Value == loopback_luid.Value && IsIn6Loopback(key + sizeof(NET_LUID)))
            have_loopback_addr = TRUE;
        if (Family == AF_INET && luid.Value == loopback_luid.Value && ((const struct nsi_ipv4_unicast_key *)key)->addr.S_un.S_addr == htonl(INADDR_LOOPBACK))
            have_loopback_addr = TRUE;
    }
    if (count != 0)
        ok(have_loopback_addr, "family %u: loopback address missing on luid %I64x\n", Family, loopback_luid.Value);
    if (GetUnicastIpAddressTable(Family, &unicast) == NO_ERROR)
    {
        ok_eq_ulong(count, unicast->NumEntries);
        FreeMibTable(unicast);
    }

    err = EnumerateTable(Device, module, NSI_IP_NEIGHBOUR_TABLE, nb_sizes, TABLE_ROWS, &nb, &count);
    ok_eq_ulong(err, (DWORD)ERROR_SUCCESS);
    trace("family %u: %lu neighbour rows\n", Family, count);
    for (i = 0; i < count && i < TABLE_ROWS; i++)
    {
        const BYTE *key = SECTION(nb, TABLE_ROWS, 0) + i * nb_sizes[0];
        const struct nsi_ip_neighbour_dynamic *dyn = (const struct nsi_ip_neighbour_dynamic *)SECTION(nb, TABLE_ROWS, nb_sizes[0] + nb_sizes[1]) + i;
        NET_LUID luid;

        memcpy(&luid, key, sizeof(luid));
        ok(LuidKnown(luids, ifcount, luid), "family %u neighbour row %lu: luid %I64x unknown\n", Family, i, luid.Value);
        ok(dyn->state <= NlnsPermanent, "family %u neighbour row %lu: state %u\n", Family, i, dyn->state);
        ok(dyn->phys_addr_len <= IF_MAX_PHYS_ADDRESS_LENGTH, "family %u neighbour row %lu: address length %u\n", Family, i, dyn->phys_addr_len);
    }
    if (GetIpNetTable2(Family, &neighbours) == NO_ERROR)
    {
        LONG delta = (LONG)count - (LONG)neighbours->NumEntries;

        ok(delta >= -2 && delta <= 2, "family %u: device has %lu neighbours, iphlpapi %lu\n", Family, count, neighbours->NumEntries);
        FreeMibTable(neighbours);
    }

    err = EnumerateTable(Device, module, NSI_IP_FORWARD_TABLE, fw_sizes, TABLE_ROWS, &fw, &count);
    ok_eq_ulong(err, (DWORD)ERROR_SUCCESS);
    trace("family %u: %lu forward rows\n", Family, count);
    for (i = 0; i < count && i < TABLE_ROWS; i++)
    {
        const BYTE *key = SECTION(fw, TABLE_ROWS, 0) + i * fw_sizes[0];
        const struct nsi_ip_forward_rw *rw = (const struct nsi_ip_forward_rw *)SECTION(fw, TABLE_ROWS, fw_sizes[0]) + i;
        const struct nsi_ip_forward_static *stat = (const struct nsi_ip_forward_static *)SECTION(fw, TABLE_ROWS, fw_sizes[0] + fw_sizes[1] + fw_sizes[2]) + i;
        NET_LUID luid;
        UINT prefix_len;

        if (Family == AF_INET)
        {
            luid = ((const struct nsi_ipv4_forward_key *)key)->luid;
            prefix_len = ((const struct nsi_ipv4_forward_key *)key)->prefix_len;
        }
        else
        {
            luid = ((const struct nsi_ipv6_forward_key *)key)->luid;
            prefix_len = ((const struct nsi_ipv6_forward_key *)key)->prefix_len;
        }
        ok(LuidKnown(luids, ifcount, luid), "family %u route %lu: luid %I64x unknown\n", Family, i, luid.Value);
        ok(prefix_len <= bits, "family %u route %lu: prefix length %u\n", Family, i, prefix_len);
        ok(stat->if_index != 0, "family %u route %lu: interface index 0\n", Family, i);
        ok(rw->metric != 0 || rw->loopback, "family %u route %lu: metric 0 on a non-loopback route\n", Family, i);
    }
    if (GetIpForwardTable2(Family, &forward) == NO_ERROR)
    {
        ok_eq_ulong(count, forward->NumEntries);
        FreeMibTable(forward);
    }

done:
    if (fw) HeapFree(GetProcessHeap(), 0, fw);
    if (nb) HeapFree(GetProcessHeap(), 0, nb);
    if (uni) HeapFree(GetProcessHeap(), 0, uni);
    if (ipif) HeapFree(GetProcessHeap(), 0, ipif);
    if (ifinfo) HeapFree(GetProcessHeap(), 0, ifinfo);
}

static DWORD
FindEndpointRows(
    _In_ const BYTE *Out,
    _In_ DWORD Count,
    _In_ UINT KeySize,
    _In_ UINT DynamicSize,
    _In_ const SOCKADDR_IN *Local,
    _In_opt_ const SOCKADDR_IN *Remote,
    _In_ int State,
    _Out_ DWORD *Index)
{
    DWORD i, found = 0;

    *Index = 0;
    for (i = 0; i < Count && i < TABLE_ROWS; i++)
    {
        const SOCKADDR_INET *key = (const SOCKADDR_INET *)(SECTION(Out, TABLE_ROWS, 0) + i * KeySize);

        if (key[0].si_family != AF_INET || key[0].Ipv4.sin_port != Local->sin_port ||
            key[0].Ipv4.sin_addr.S_un.S_addr != Local->sin_addr.S_un.S_addr)
            continue;
        if (Remote != NULL && (key[1].si_family != AF_INET || key[1].Ipv4.sin_port != Remote->sin_port ||
                               key[1].Ipv4.sin_addr.S_un.S_addr != Remote->sin_addr.S_un.S_addr))
            continue;
        if (State >= 0 && DynamicSize != 0 &&
            ((const struct nsi_tcp_conn_dynamic *)SECTION(Out, TABLE_ROWS, KeySize) + i)->state != (UINT)State)
            continue;
        if (found == 0)
            *Index = i;
        found++;
    }
    return found;
}

static DWORD
ApiEndpointCount(
    _In_ BOOL Tcp,
    _In_ ULONG Family)
{
    DWORD size = 0, count = 0, err;
    void *table;

    err = Tcp ? GetExtendedTcpTable(NULL, &size, FALSE, Family, TCP_TABLE_OWNER_PID_ALL, 0)
              : GetExtendedUdpTable(NULL, &size, FALSE, Family, UDP_TABLE_OWNER_PID, 0);
    if (err != ERROR_INSUFFICIENT_BUFFER || size < sizeof(DWORD))
        return 0;
    table = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
    if (table == NULL)
        return 0;
    err = Tcp ? GetExtendedTcpTable(table, &size, FALSE, Family, TCP_TABLE_OWNER_PID_ALL, 0)
              : GetExtendedUdpTable(table, &size, FALSE, Family, UDP_TABLE_OWNER_PID, 0);
    if (err == NO_ERROR)
        count = *(DWORD *)table;
    HeapFree(GetProcessHeap(), 0, table);
    return count;
}

static BOOL
IsIn6LinkLocal(
    _In_ const IN6_ADDR *Address)
{
    return Address->u.Byte[0] == 0xfe && (Address->u.Byte[1] & 0xc0) == 0x80;
}

static void
TestDeviceLinkLocalListener(
    _In_ HANDLE Device)
{
    const UINT tcp_sizes[4] = { sizeof(struct nsi_tcp_conn_key), 0, sizeof(struct nsi_tcp_conn_dynamic), sizeof(struct nsi_tcp_conn_static) };
    MIB_UNICASTIPADDRESS_TABLE *unicast = NULL;
    SOCKADDR_IN6 bind_addr, local;
    SOCKET listener = INVALID_SOCKET;
    BYTE *rows = NULL;
    DWORD count = 0, i, found = 0, err, if_index = 0;
    int len;
    static const BYTE zero16[16];

    if (GetUnicastIpAddressTable(AF_INET6, &unicast) != NO_ERROR)
    {
        skip(FALSE, "no IPv6 unicast table\n");
        return;
    }
    for (i = 0; i < unicast->NumEntries; i++)
    {
        if (IsIn6LinkLocal(&unicast->Table[i].Address.Ipv6.sin6_addr))
        {
            bind_addr = unicast->Table[i].Address.Ipv6;
            if_index = unicast->Table[i].InterfaceIndex;
            break;
        }
    }
    FreeMibTable(unicast);
    if (skip(if_index != 0, "no IPv6 link-local address to listen on\n"))
        return;
    bind_addr.sin6_port = 0;
    bind_addr.sin6_scope_id = if_index;
    listener = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (skip(listener != INVALID_SOCKET && bind(listener, (SOCKADDR *)&bind_addr, sizeof(bind_addr)) == 0 && listen(listener, 1) == 0,
             "IPv6 link-local listener not created: %d\n", WSAGetLastError()))
        goto done;
    len = sizeof(local);
    getsockname(listener, (SOCKADDR *)&local, &len);

    err = EnumerateTable(Device, &NPI_MS_TCP_MODULEID, NSI_TCP_LISTEN_TABLE, tcp_sizes, TABLE_ROWS, &rows, &count);
    ok(err == ERROR_SUCCESS || err == ERROR_MORE_DATA, "TCP LISTEN enumerate: %lu\n", err);
    for (i = 0; i < count && i < TABLE_ROWS; i++)
    {
        const struct nsi_tcp_conn_key *key = (const struct nsi_tcp_conn_key *)SECTION(rows, TABLE_ROWS, 0) + i;

        if (key->local.si_family != AF_INET6 || key->local.Ipv6.sin6_port != local.sin6_port ||
            memcmp(&key->local.Ipv6.sin6_addr, &local.sin6_addr, sizeof(local.sin6_addr)) != 0)
            continue;
        found++;
        ok_eq_ulong(key->local.Ipv6.sin6_scope_id, if_index);
        ok(key->remote.si_family == AF_INET6 && key->remote.Ipv6.sin6_port == 0 && key->remote.Ipv6.sin6_scope_id == 0 &&
           memcmp(&key->remote.Ipv6.sin6_addr, zero16, sizeof(zero16)) == 0, "IPv6 listener remote is not [::]:0\n");
    }
    ok_eq_ulong(found, 1UL);

done:
    if (rows) HeapFree(GetProcessHeap(), 0, rows);
    if (listener != INVALID_SOCKET) closesocket(listener);
}

static void
TestDeviceEndpoints(
    _In_ HANDLE Device)
{
    const UINT tcp_sizes[4] = { sizeof(struct nsi_tcp_conn_key), 0, sizeof(struct nsi_tcp_conn_dynamic), sizeof(struct nsi_tcp_conn_static) };
    const UINT udp_sizes[4] = { sizeof(struct nsi_udp_endpoint_key), 0, 0, sizeof(struct nsi_udp_endpoint_static) };
    WSADATA wsa;
    SOCKET listener = INVALID_SOCKET, client = INVALID_SOCKET, accepted = INVALID_SOCKET, udp = INVALID_SOCKET;
    SOCKADDR_IN listen_addr, client_addr, udp_addr;
    int len;
    BYTE *all = NULL, *estab = NULL, *listen_rows = NULL, *endpoint = NULL;
    DWORD count_all = 0, count_estab = 0, count_listen = 0, count_udp = 0, index, found, err;
    DWORD api_tcp_before, api_tcp_after, api_udp_before, api_udp_after;
    const struct nsi_tcp_conn_static *stat;
    const struct nsi_udp_endpoint_static *ustat;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        skip(FALSE, "WSAStartup failed: %d\n", WSAGetLastError());
        return;
    }
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (skip(listener != INVALID_SOCKET && client != INVALID_SOCKET && udp != INVALID_SOCKET, "sockets not created: %d\n", WSAGetLastError()))
        goto done;

    memset(&listen_addr, 0, sizeof(listen_addr));
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK);
    udp_addr = listen_addr;
    if (skip(bind(listener, (SOCKADDR *)&listen_addr, sizeof(listen_addr)) == 0 && listen(listener, 1) == 0 &&
             bind(udp, (SOCKADDR *)&udp_addr, sizeof(udp_addr)) == 0, "loopback bind or listen failed: %d\n", WSAGetLastError()))
        goto done;
    len = sizeof(listen_addr);
    getsockname(listener, (SOCKADDR *)&listen_addr, &len);
    len = sizeof(udp_addr);
    getsockname(udp, (SOCKADDR *)&udp_addr, &len);
    if (skip(connect(client, (SOCKADDR *)&listen_addr, sizeof(listen_addr)) == 0, "loopback connect failed: %d\n", WSAGetLastError()))
        goto done;
    len = sizeof(client_addr);
    getsockname(client, (SOCKADDR *)&client_addr, &len);
    accepted = accept(listener, NULL, NULL);
    ok(accepted != INVALID_SOCKET, "accept failed: %d\n", WSAGetLastError());

    api_tcp_before = ApiEndpointCount(TRUE, AF_INET) + ApiEndpointCount(TRUE, AF_INET6);
    api_udp_before = ApiEndpointCount(FALSE, AF_INET) + ApiEndpointCount(FALSE, AF_INET6);
    err = EnumerateTable(Device, &NPI_MS_TCP_MODULEID, NSI_TCP_ALL_TABLE, tcp_sizes, TABLE_ROWS, &all, &count_all);
    if (skip(err != ERROR_NOT_FOUND, "tcpip does not provide the TCP connection tables yet\n"))
        goto done;
    ok(err == ERROR_SUCCESS || err == ERROR_MORE_DATA, "TCP ALL enumerate: %lu\n", err);
    err = EnumerateTable(Device, &NPI_MS_TCP_MODULEID, NSI_TCP_ESTAB_TABLE, tcp_sizes, TABLE_ROWS, &estab, &count_estab);
    ok(err == ERROR_SUCCESS || err == ERROR_MORE_DATA, "TCP ESTAB enumerate: %lu\n", err);
    err = EnumerateTable(Device, &NPI_MS_TCP_MODULEID, NSI_TCP_LISTEN_TABLE, tcp_sizes, TABLE_ROWS, &listen_rows, &count_listen);
    ok(err == ERROR_SUCCESS || err == ERROR_MORE_DATA, "TCP LISTEN enumerate: %lu\n", err);
    err = EnumerateTable(Device, &NPI_MS_UDP_MODULEID, NSI_UDP_ENDPOINT_TABLE, udp_sizes, TABLE_ROWS, &endpoint, &count_udp);
    ok(err == ERROR_SUCCESS || err == ERROR_MORE_DATA, "UDP ENDPOINT enumerate: %lu\n", err);
    api_tcp_after = ApiEndpointCount(TRUE, AF_INET) + ApiEndpointCount(TRUE, AF_INET6);
    api_udp_after = ApiEndpointCount(FALSE, AF_INET) + ApiEndpointCount(FALSE, AF_INET6);
    trace("device: %lu all, %lu estab, %lu listen, %lu udp; api: %lu tcp, %lu udp\n",
          count_all, count_estab, count_listen, count_udp, api_tcp_after, api_udp_after);

    found = FindEndpointRows(listen_rows, count_listen, tcp_sizes[0], tcp_sizes[2], &listen_addr, NULL, -1, &index);
    ok_eq_ulong(found, 1UL);
    if (found == 1 && index < TABLE_ROWS)
    {
        const struct nsi_tcp_conn_key *key = (const struct nsi_tcp_conn_key *)SECTION(listen_rows, TABLE_ROWS, 0) + index;

        ok(key->remote.si_family == AF_INET && key->remote.Ipv4.sin_addr.S_un.S_addr == 0 && key->remote.Ipv4.sin_port == 0,
           "IPv4 listener remote is not 0.0.0.0:0\n");
        ok_eq_ulong((ULONG)((const struct nsi_tcp_conn_dynamic *)SECTION(listen_rows, TABLE_ROWS, tcp_sizes[0]) + index)->state, (ULONG)MIB_TCP_STATE_LISTEN);
        stat = (const struct nsi_tcp_conn_static *)SECTION(listen_rows, TABLE_ROWS, tcp_sizes[0] + tcp_sizes[2]) + index;
        ok_eq_ulong((ULONG)stat->pid, GetCurrentProcessId());
        ok(stat->create_time != 0, "listener has no creation time\n");
    }
    found = FindEndpointRows(all, count_all, tcp_sizes[0], tcp_sizes[2], &listen_addr, NULL, MIB_TCP_STATE_LISTEN, &index);
    ok_eq_ulong(found, 1UL);
    found = FindEndpointRows(estab, count_estab, tcp_sizes[0], tcp_sizes[2], &client_addr, &listen_addr, -1, &index);
    ok_eq_ulong(found, 1UL);
    if (found == 1 && index < TABLE_ROWS)
    {
        ok_eq_ulong((ULONG)((const struct nsi_tcp_conn_dynamic *)SECTION(estab, TABLE_ROWS, tcp_sizes[0]) + index)->state, (ULONG)MIB_TCP_STATE_ESTAB);
        stat = (const struct nsi_tcp_conn_static *)SECTION(estab, TABLE_ROWS, tcp_sizes[0] + tcp_sizes[2]) + index;
        ok_eq_ulong((ULONG)stat->pid, GetCurrentProcessId());
    }
    found = FindEndpointRows(estab, count_estab, tcp_sizes[0], tcp_sizes[2], &listen_addr, &client_addr, -1, &index);
    ok_eq_ulong(found, 1UL);
    found = FindEndpointRows(all, count_all, tcp_sizes[0], tcp_sizes[2], &client_addr, &listen_addr, MIB_TCP_STATE_ESTAB, &index);
    ok_eq_ulong(found, 1UL);
    found = FindEndpointRows(all, count_all, tcp_sizes[0], tcp_sizes[2], &listen_addr, &client_addr, MIB_TCP_STATE_ESTAB, &index);
    ok_eq_ulong(found, 1UL);
    ok(count_estab <= count_all && count_listen <= count_all, "ESTAB %lu or LISTEN %lu exceeds ALL %lu\n", count_estab, count_listen, count_all);

    found = FindEndpointRows(endpoint, count_udp, udp_sizes[0], 0, &udp_addr, NULL, -1, &index);
    ok_eq_ulong(found, 1UL);
    if (found == 1 && index < TABLE_ROWS)
    {
        ustat = (const struct nsi_udp_endpoint_static *)SECTION(endpoint, TABLE_ROWS, udp_sizes[0]) + index;
        ok_eq_ulong((ULONG)ustat->pid, GetCurrentProcessId());
        ok(ustat->create_time != 0, "UDP endpoint has no creation time\n");
    }

    TestDeviceLinkLocalListener(Device);

    if (api_tcp_before == api_tcp_after)
        ok_eq_ulong(count_all, api_tcp_after);
    else
        trace("TCP table changed under the test (%lu -> %lu), count not compared\n", api_tcp_before, api_tcp_after);
    if (api_udp_before == api_udp_after)
        ok_eq_ulong(count_udp, api_udp_after);
    else
        trace("UDP table changed under the test (%lu -> %lu), count not compared\n", api_udp_before, api_udp_after);

done:
    if (endpoint) HeapFree(GetProcessHeap(), 0, endpoint);
    if (listen_rows) HeapFree(GetProcessHeap(), 0, listen_rows);
    if (estab) HeapFree(GetProcessHeap(), 0, estab);
    if (all) HeapFree(GetProcessHeap(), 0, all);
    if (accepted != INVALID_SOCKET) closesocket(accepted);
    if (client != INVALID_SOCKET) closesocket(client);
    if (listener != INVALID_SOCKET) closesocket(listener);
    if (udp != INVALID_SOCKET) closesocket(udp);
    WSACleanup();
}

#define NOTIFY_LIMIT_MS 1000

static DWORD ArmNotification(_In_ HANDLE Device, _In_ const NPI_MODULEID *Module, _In_ UINT Table, _Inout_ OVERLAPPED *Overlapped);

static DWORD
ElapsedMs(
    _In_ const LARGE_INTEGER *Start)
{
    LARGE_INTEGER now, freq;

    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    return (DWORD)((now.QuadPart - Start->QuadPart) * 1000 / freq.QuadPart);
}

static void
TestDeviceIpNotifications(void)
{
    const UINT if_sizes[4] = { sizeof(NET_LUID), 0, 0, sizeof(struct nsi_ndis_ifinfo_static) };
    HANDLE unicast = OpenNsiDevice(FILE_FLAG_OVERLAPPED), forward = OpenNsiDevice(FILE_FLAG_OVERLAPPED), ifinfo = OpenNsiDevice(FILE_FLAG_OVERLAPPED);
    HANDLE sync = OpenNsiDevice(0);
    OVERLAPPED ovl_unicast, ovl_forward, ovl_ifinfo;
    BYTE *out = NULL;
    const NET_LUID *luids;
    const struct nsi_ndis_ifinfo_static *ifstat;
    NET_LUID loopback_luid;
    MIB_UNICASTIPADDRESS_ROW row;
    LARGE_INTEGER start;
    DWORD ifcount, i, err, bytes, elapsed;

    loopback_luid.Value = 0;
    if (unicast == INVALID_HANDLE_VALUE || forward == INVALID_HANDLE_VALUE || ifinfo == INVALID_HANDLE_VALUE || sync == INVALID_HANDLE_VALUE)
    {
        skip(FALSE, "overlapped device handles for the notification checks\n");
        goto done;
    }
    err = EnumerateTable(sync, &NPI_MS_NDIS_MODULEID, NSI_NDIS_IFINFO_TABLE, if_sizes, TABLE_ROWS, &out, &ifcount);
    if (skip(err == ERROR_SUCCESS && ifcount != 0, "no interface table for the notification checks (%lu)\n", err))
        goto done;
    luids = (const NET_LUID *)SECTION(out, TABLE_ROWS, 0);
    ifstat = (const struct nsi_ndis_ifinfo_static *)SECTION(out, TABLE_ROWS, if_sizes[0]);
    for (i = 0; i < ifcount && i < TABLE_ROWS; i++)
        if (ifstat[i].type == MIB_IF_TYPE_LOOPBACK)
            loopback_luid = luids[i];
    if (skip(loopback_luid.Value != 0, "no loopback interface for the notification checks\n"))
        goto done;

    memset(&ovl_unicast, 0, sizeof(ovl_unicast));
    memset(&ovl_forward, 0, sizeof(ovl_forward));
    memset(&ovl_ifinfo, 0, sizeof(ovl_ifinfo));
    ovl_unicast.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    ovl_forward.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    ovl_ifinfo.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    err = ArmNotification(unicast, &NPI_MS_IPV4_MODULEID, NSI_IP_UNICAST_TABLE, &ovl_unicast);
    if (skip(err == ERROR_IO_PENDING, "unicast notification request: %lu (tcpip does not provide the IP tables yet)\n", err))
        goto events;
    ok_eq_ulong(ArmNotification(forward, &NPI_MS_IPV4_MODULEID, NSI_IP_FORWARD_TABLE, &ovl_forward), (DWORD)ERROR_IO_PENDING);
    ok_eq_ulong(ArmNotification(ifinfo, &NPI_MS_NDIS_MODULEID, NSI_NDIS_IFINFO_TABLE, &ovl_ifinfo), (DWORD)ERROR_IO_PENDING);

    InitializeUnicastIpAddressEntry(&row);
    row.InterfaceLuid = loopback_luid;
    row.Address.Ipv4.sin_family = AF_INET;
    row.Address.Ipv4.sin_addr.S_un.S_addr = htonl(0x7F0000FE);
    row.OnLinkPrefixLength = 8;
    row.PrefixOrigin = IpPrefixOriginManual;
    row.SuffixOrigin = IpSuffixOriginManual;
    QueryPerformanceCounter(&start);
    err = CreateUnicastIpAddressEntry(&row);
    if (skip(err == NO_ERROR, "CreateUnicastIpAddressEntry on loopback: %lu\n", err))
    {
        CancelIoEx(unicast, &ovl_unicast);
        CancelIoEx(forward, &ovl_forward);
        CancelIoEx(ifinfo, &ovl_ifinfo);
        goto events;
    }
    ok(WaitForSingleObject(ovl_unicast.hEvent, 5000) == WAIT_OBJECT_0, "unicast change not notified after an address add\n");
    elapsed = ElapsedMs(&start);
    trace("unicast add notified after %lu ms\n", elapsed);
    ok(elapsed <= NOTIFY_LIMIT_MS, "unicast add notified after %lu ms\n", elapsed);
    ok(GetOverlappedResult(unicast, &ovl_unicast, &bytes, FALSE) && bytes != 0, "unicast notification without a sequence\n");
    ok(WaitForSingleObject(ovl_forward.hEvent, 5000) == WAIT_OBJECT_0, "forward change not notified after an address add\n");
    elapsed = ElapsedMs(&start);
    trace("forward (on-link route) notified after %lu ms\n", elapsed);
    ok(WaitForSingleObject(ovl_ifinfo.hEvent, 300) == WAIT_TIMEOUT, "interface notification fired on an address change\n");

    ok_eq_ulong(ArmNotification(unicast, &NPI_MS_IPV4_MODULEID, NSI_IP_UNICAST_TABLE, &ovl_unicast), (DWORD)ERROR_IO_PENDING);
    ok_eq_ulong(ArmNotification(forward, &NPI_MS_IPV4_MODULEID, NSI_IP_FORWARD_TABLE, &ovl_forward), (DWORD)ERROR_IO_PENDING);
    QueryPerformanceCounter(&start);
    err = DeleteUnicastIpAddressEntry(&row);
    ok_eq_ulong(err, (DWORD)NO_ERROR);
    ok(WaitForSingleObject(ovl_unicast.hEvent, 5000) == WAIT_OBJECT_0, "unicast change not notified after an address delete\n");
    elapsed = ElapsedMs(&start);
    trace("unicast delete notified after %lu ms\n", elapsed);
    ok(elapsed <= NOTIFY_LIMIT_MS, "unicast delete notified after %lu ms\n", elapsed);
    ok(WaitForSingleObject(ovl_forward.hEvent, 5000) == WAIT_OBJECT_0, "forward change not notified after an address delete\n");
    CancelIoEx(ifinfo, &ovl_ifinfo);
    WaitForSingleObject(ovl_ifinfo.hEvent, 5000);

events:
    if (ovl_unicast.hEvent) CloseHandle(ovl_unicast.hEvent);
    if (ovl_forward.hEvent) CloseHandle(ovl_forward.hEvent);
    if (ovl_ifinfo.hEvent) CloseHandle(ovl_ifinfo.hEvent);
done:
    if (out) HeapFree(GetProcessHeap(), 0, out);
    if (sync != INVALID_HANDLE_VALUE) CloseHandle(sync);
    if (ifinfo != INVALID_HANDLE_VALUE) CloseHandle(ifinfo);
    if (forward != INVALID_HANDLE_VALUE) CloseHandle(forward);
    if (unicast != INVALID_HANDLE_VALUE) CloseHandle(unicast);
}

typedef DWORD (WINAPI *PFN_NSI_ENUMERATE)(DWORD, DWORD, const NPI_MODULEID *, DWORD, void *, DWORD, void *, DWORD,
                                          void *, DWORD, void *, DWORD, DWORD *);
typedef DWORD (WINAPI *PFN_NSI_GET_PARAMETER)(DWORD, const NPI_MODULEID *, DWORD, const void *, DWORD, DWORD, void *, DWORD, DWORD);

static void
TestApiIpTables(
    _In_ HANDLE Device,
    _In_ PFN_NSI_ENUMERATE pEnumerate)
{
    static const struct { ADDRESS_FAMILY family; UINT table; UINT key4; UINT key6; } tables[4] =
    {
        { 0, NSI_IP_INTERFACE_TABLE, sizeof(struct nsi_ip_interface_key), sizeof(struct nsi_ip_interface_key) },
        { 0, NSI_IP_UNICAST_TABLE, sizeof(struct nsi_ipv4_unicast_key), sizeof(struct nsi_ipv6_unicast_key) },
        { 0, NSI_IP_NEIGHBOUR_TABLE, sizeof(struct nsi_ipv4_neighbour_key), sizeof(struct nsi_ipv6_neighbour_key) },
        { 0, NSI_IP_FORWARD_TABLE, sizeof(struct nsi_ipv4_forward_key), sizeof(struct nsi_ipv6_forward_key) },
    };
    UINT f, k, i, j;

    for (f = 0; f < 2; f++)
    {
        const NPI_MODULEID *module = f == 0 ? &NPI_MS_IPV4_MODULEID : &NPI_MS_IPV6_MODULEID;

        for (k = 0; k < 4; k++)
        {
            UINT key_size = f == 0 ? tables[k].key4 : tables[k].key6;
            UINT sizes[4] = { key_size, 0, 0, 0 };
            BYTE *dev = NULL, *api;
            DWORD dev_count, api_count = TABLE_ROWS, err, matched = 0;

            err = EnumerateTable(Device, module, tables[k].table, sizes, TABLE_ROWS, &dev, &dev_count);
            api = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, TABLE_ROWS * key_size);
            if (!skip(err == ERROR_SUCCESS && api != NULL, "family %u table %u through the device: %lu\n", f, tables[k].table, err))
            {
                err = pEnumerate(1, 1, module, tables[k].table, api, key_size, NULL, 0, NULL, 0, NULL, 0, &api_count);
                ok_eq_ulong(err, (DWORD)ERROR_SUCCESS);
                ok(api_count == dev_count, "family %u table %u: api %lu rows, device %lu\n", f, tables[k].table, api_count, dev_count);
                for (i = 0; i < api_count && i < TABLE_ROWS; i++)
                    for (j = 0; j < dev_count && j < TABLE_ROWS; j++)
                        if (memcmp(api + i * key_size, SECTION(dev, TABLE_ROWS, 0) + j * key_size, key_size) == 0)
                        {
                            matched++;
                            break;
                        }
                ok(matched == min(api_count, (DWORD)TABLE_ROWS), "family %u table %u: %lu of %lu api keys found on the device\n", f, tables[k].table, matched, api_count);
            }
            if (api) HeapFree(GetProcessHeap(), 0, api);
            if (dev) HeapFree(GetProcessHeap(), 0, dev);
        }
    }
}

static void
TestApiEndpointTables(
    _In_ HANDLE Device,
    _In_ PFN_NSI_ENUMERATE pEnumerate)
{
    static const struct { const NPI_MODULEID *module; UINT table; UINT key; } tables[4] =
    {
        { &NPI_MS_TCP_MODULEID, NSI_TCP_ALL_TABLE, sizeof(struct nsi_tcp_conn_key) },
        { &NPI_MS_TCP_MODULEID, NSI_TCP_ESTAB_TABLE, sizeof(struct nsi_tcp_conn_key) },
        { &NPI_MS_TCP_MODULEID, NSI_TCP_LISTEN_TABLE, sizeof(struct nsi_tcp_conn_key) },
        { &NPI_MS_UDP_MODULEID, NSI_UDP_ENDPOINT_TABLE, sizeof(struct nsi_udp_endpoint_key) },
    };
    UINT k, i, j;

    for (k = 0; k < 4; k++)
    {
        UINT sizes[4] = { tables[k].key, 0, 0, 0 };
        BYTE *dev = NULL, *api;
        DWORD dev_count, api_count = TABLE_ROWS, err, matched = 0;

        err = EnumerateTable(Device, tables[k].module, tables[k].table, sizes, TABLE_ROWS, &dev, &dev_count);
        api = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, TABLE_ROWS * tables[k].key);
        if (!skip(err == ERROR_SUCCESS && api != NULL, "endpoint table %u through the device: %lu\n", tables[k].table, err))
        {
            err = pEnumerate(1, 1, tables[k].module, tables[k].table, api, tables[k].key, NULL, 0, NULL, 0, NULL, 0, &api_count);
            ok_eq_ulong(err, (DWORD)ERROR_SUCCESS);
            ok(api_count == dev_count, "endpoint table %u: api %lu rows, device %lu\n", tables[k].table, api_count, dev_count);
            for (i = 0; i < api_count && i < TABLE_ROWS; i++)
                for (j = 0; j < dev_count && j < TABLE_ROWS; j++)
                    if (memcmp(api + i * tables[k].key, SECTION(dev, TABLE_ROWS, 0) + j * tables[k].key, tables[k].key) == 0)
                    {
                        matched++;
                        break;
                    }
            ok(matched == min(api_count, (DWORD)TABLE_ROWS), "endpoint table %u: %lu of %lu api keys found on the device\n", tables[k].table, matched, api_count);
        }
        if (api) HeapFree(GetProcessHeap(), 0, api);
        if (dev) HeapFree(GetProcessHeap(), 0, dev);
    }
}

static void
TestApiBackend(
    _In_ HANDLE Device)
{
    HMODULE nsi = LoadLibraryW(L"nsi.dll");
    PFN_NSI_ENUMERATE pNsiEnumerateObjectsAllParameters;
    PFN_NSI_GET_PARAMETER pNsiGetParameter;
    struct nsiproxy_enumerate_all in;
    const DWORD rows = 32;
    NET_LUID *api_keys = NULL, *dev_keys;
    struct nsi_ndis_ifinfo_static *api_stat = NULL, *dev_stat;
    BYTE *out = NULL;
    DWORD out_size, returned, err, api_count = rows, dev_count, i, j, matched = 0;

    if (skip(nsi != NULL, "nsi.dll not loadable: %lu\n", GetLastError()))
        return;
    pNsiEnumerateObjectsAllParameters = (PFN_NSI_ENUMERATE)GetProcAddress(nsi, "NsiEnumerateObjectsAllParameters");
    pNsiGetParameter = (PFN_NSI_GET_PARAMETER)GetProcAddress(nsi, "NsiGetParameter");
    ok(pNsiEnumerateObjectsAllParameters != NULL && pNsiGetParameter != NULL, "nsi.dll exports missing\n");
    api_keys = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, rows * sizeof(*api_keys));
    api_stat = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, rows * sizeof(*api_stat));
    memset(&in, 0, sizeof(in));
    in.module = NPI_MS_NDIS_MODULEID;
    in.table = NSI_NDIS_IFINFO_TABLE;
    in.key_size = sizeof(NET_LUID);
    in.static_size = sizeof(struct nsi_ndis_ifinfo_static);
    in.count = rows;
    out_size = sizeof(DWORD) + rows * (in.key_size + in.static_size);
    out = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, out_size);
    if (pNsiEnumerateObjectsAllParameters != NULL && pNsiGetParameter != NULL && api_keys != NULL && api_stat != NULL && out != NULL)
    {
        err = Ioctl(Device, IOCTL_NSIPROXY_WINE_ENUMERATE_ALL, &in, sizeof(in), out, out_size, &returned);
        if (!skip(err == ERROR_SUCCESS, "device interface table: %lu\n", err))
        {
            dev_count = *(DWORD *)out;
            dev_keys = (NET_LUID *)(out + sizeof(DWORD));
            dev_stat = (struct nsi_ndis_ifinfo_static *)(out + sizeof(DWORD) + rows * in.key_size);
            err = pNsiEnumerateObjectsAllParameters(1, 1, &NPI_MS_NDIS_MODULEID, NSI_NDIS_IFINFO_TABLE,
                                                    api_keys, sizeof(*api_keys), NULL, 0, NULL, 0,
                                                    api_stat, sizeof(*api_stat), &api_count);
            ok_eq_ulong(err, (DWORD)ERROR_SUCCESS);
            ok_eq_ulong(api_count, dev_count);
            for (i = 0; i < api_count && i < rows; i++)
            {
                NET_LUID luid;

                for (j = 0; j < dev_count && j < rows; j++)
                {
                    if (dev_keys[j].Value != api_keys[i].Value)
                        continue;
                    matched++;
                    ok_eq_ulong((ULONG)api_stat[i].if_index, (ULONG)dev_stat[j].if_index);
                    ok_eq_ulong((ULONG)api_stat[i].type, (ULONG)dev_stat[j].type);
                    ok(IsEqualGUID(&api_stat[i].if_guid, &dev_stat[j].if_guid), "interface %u: guid differs between api and device\n", api_stat[i].if_index);
                    ok(api_stat[i].descr.Length != 0, "interface %u: empty description through the api\n", api_stat[i].if_index);
                    trace("interface %u api descr %ls / device descr %ls\n", api_stat[i].if_index, api_stat[i].descr.String, dev_stat[j].descr.String);
                }
                luid.Value = 0;
                err = pNsiGetParameter(1, &NPI_MS_NDIS_MODULEID, NSI_NDIS_INDEX_LUID_TABLE, &api_stat[i].if_index, sizeof(UINT),
                                       NSI_PARAM_TYPE_STATIC, &luid, sizeof(luid), 0);
                ok_eq_ulong(err, (DWORD)ERROR_SUCCESS);
                ok(luid.Value == api_keys[i].Value, "api index %u maps to luid %I64x, enumerated %I64x\n", api_stat[i].if_index, luid.Value, api_keys[i].Value);
            }
            ok(matched == api_count, "%lu of %lu api rows found on the device\n", matched, api_count);
            TestApiIpTables(Device, pNsiEnumerateObjectsAllParameters);
            TestApiEndpointTables(Device, pNsiEnumerateObjectsAllParameters);

            if (!skip(dev_count >= 2, "need two interfaces for the short-buffer check, have %lu\n", dev_count))
            {
                BYTE *guard = (BYTE *)&api_stat[1];
                DWORD short_count = 1;

                memset(api_keys, 0xCD, 2 * sizeof(*api_keys));
                memset(api_stat, 0xCD, 2 * sizeof(*api_stat));
                err = pNsiEnumerateObjectsAllParameters(1, 1, &NPI_MS_NDIS_MODULEID, NSI_NDIS_IFINFO_TABLE,
                                                        api_keys, sizeof(*api_keys), NULL, 0, NULL, 0,
                                                        api_stat, sizeof(*api_stat), &short_count);
                ok_eq_ulong(err, (DWORD)ERROR_MORE_DATA);
                ok_eq_ulong(short_count, dev_count);
                ok(api_keys[0].Value != 0xCDCDCDCDCDCDCDCDULL, "first key not written on a short enumerate\n");
                ok(api_keys[1].Value == 0xCDCDCDCDCDCDCDCDULL, "second key written although only one row fit\n");
                for (i = 0; i < sizeof(*api_stat); i++)
                {
                    if (guard[i] != 0xCD)
                        break;
                }
                ok(i == sizeof(*api_stat), "second static row written at offset %lu although only one row fit\n", i);
            }
        }
    }
    if (out != NULL)
        HeapFree(GetProcessHeap(), 0, out);
    if (api_stat != NULL)
        HeapFree(GetProcessHeap(), 0, api_stat);
    if (api_keys != NULL)
        HeapFree(GetProcessHeap(), 0, api_keys);
    FreeLibrary(nsi);
}

static DWORD
ArmNotification(
    _In_ HANDLE Device,
    _In_ const NPI_MODULEID *Module,
    _In_ UINT Table,
    _Inout_ OVERLAPPED *Overlapped)
{
    struct nsiproxy_request_notification in;
    DWORD returned;

    memset(&in, 0, sizeof(in));
    in.module = *Module;
    in.table = Table;
    ResetEvent(Overlapped->hEvent);
    if (DeviceIoControl(Device, IOCTL_NSIPROXY_WINE_CHANGE_NOTIFICATION, &in, sizeof(in), NULL, 0, &returned, Overlapped))
        return ERROR_SUCCESS;
    return GetLastError();
}

static void
TestDeviceNotifications(
    _In_ const NPI_MODULEID *Module)
{
    HANDLE device = OpenNsiDevice(FILE_FLAG_OVERLAPPED);
    OVERLAPPED ovl;
    DWORD err, bytes, first;

    ok(device != INVALID_HANDLE_VALUE, "overlapped open of the device: %lu\n", GetLastError());
    if (device == INVALID_HANDLE_VALUE)
        return;
    memset(&ovl, 0, sizeof(ovl));
    ovl.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    err = ArmNotification(device, Module, NSISTORE_TABLE, &ovl);
    ok_eq_ulong(err, (DWORD)ERROR_IO_PENDING);
    ok(WaitForSingleObject(ovl.hEvent, 300) == WAIT_TIMEOUT, "notification fired without a change\n");
    ok(KmtSendToDriver(IOCTL_NSISTORE_NOTIFY) == ERROR_SUCCESS, "notify request to the test driver failed\n");
    ok(WaitForSingleObject(ovl.hEvent, 5000) == WAIT_OBJECT_0, "notification not delivered after a change\n");
    bytes = 0;
    ok(GetOverlappedResult(device, &ovl, &bytes, FALSE), "GetOverlappedResult: %lu\n", GetLastError());
    ok(bytes != 0, "sequence is 0 after a change\n");
    first = bytes;

    err = ArmNotification(device, Module, NSISTORE_TABLE, &ovl);
    ok_eq_ulong(err, (DWORD)ERROR_IO_PENDING);
    ok(KmtSendToDriver(IOCTL_NSISTORE_NOTIFY) == ERROR_SUCCESS, "second notify request failed\n");
    ok(WaitForSingleObject(ovl.hEvent, 5000) == WAIT_OBJECT_0, "second notification not delivered\n");
    bytes = 0;
    ok(GetOverlappedResult(device, &ovl, &bytes, FALSE), "second GetOverlappedResult: %lu\n", GetLastError());
    ok_eq_ulong(bytes, first + 1);

    err = ArmNotification(device, Module, NSISTORE_TABLE, &ovl);
    ok_eq_ulong(err, (DWORD)ERROR_IO_PENDING);
    ok(CancelIoEx(device, &ovl), "CancelIoEx: %lu\n", GetLastError());
    ok(WaitForSingleObject(ovl.hEvent, 5000) == WAIT_OBJECT_0, "cancelled request not completed\n");
    ok(!GetOverlappedResult(device, &ovl, &bytes, FALSE), "cancelled request completed successfully\n");
    ok_eq_ulong(GetLastError(), (DWORD)ERROR_OPERATION_ABORTED);

    err = ArmNotification(device, Module, NSISTORE_SCRATCH_TABLE, &ovl);
    ok_eq_ulong(err, (DWORD)ERROR_NOT_FOUND);

    err = ArmNotification(device, Module, NSISTORE_TABLE, &ovl);
    ok_eq_ulong(err, (DWORD)ERROR_IO_PENDING);
    CloseHandle(device);
    ok(WaitForSingleObject(ovl.hEvent, 5000) == WAIT_OBJECT_0, "request not completed by closing the handle\n");
    ok_eq_hex((ULONG)ovl.Internal, 0xC0000120UL);
    CloseHandle(ovl.hEvent);
}

START_TEST(NsiStore)
{
    NPI_MODULEID module;
    HANDLE device;
    DWORD error;

    error = KmtLoadAndOpenDriver(L"NsiStore", FALSE);
    ok_eq_ulong(error, (DWORD)ERROR_SUCCESS);
    if (error)
        return;
    ok(KmtSendToDriver(IOCTL_NSISTORE_RUN) == ERROR_SUCCESS, "kernel half failed\n");

    device = OpenNsiDevice(0);
    if (!skip(device != INVALID_HANDLE_VALUE, "no \\\\.\\Nsi device: %lu\n", GetLastError()))
    {
        memset(&module, 0, sizeof(module));
        module.Length = sizeof(module);
        module.Type = MIT_GUID;
        module.Guid = StoreTestGuid;
        TestDeviceEnumerate(device, &module);
        TestDeviceRows(device, &module);
        TestDeviceInterfaces(device);
        TestDeviceIpTables(device, AF_INET6);
        TestDeviceIpTables(device, AF_INET);
        TestDeviceEndpoints(device);
        TestDeviceIpNotifications();
        TestApiBackend(device);
        CloseHandle(device);
        TestDeviceNotifications(&module);
    }

    KmtCloseDriver();
    KmtUnloadDriver();
}
