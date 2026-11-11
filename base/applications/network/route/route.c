/* Poor man's route
 *
 * Supported commands:
 *
 * "print"
 * "add" target ["mask" mask] gw ["metric" metric]
 * "delete" target gw
 *
 * Goals:
 *
 * Flexible, simple
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>

#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <winsock2.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>
#include <netioapi.h>
#include <ip2string.h>

#include <conutils.h>

#include "resource.h"

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

#define IPBUF 17
#define IN_ADDR_OF(x) *((struct in_addr *)&(x))

#define DELETE_FLAG       0x1
#define PERSISTENT_FLAG   0x2
#define IPV4_ONLY_FLAG    0x4
#define IPV6_ONLY_FLAG    0x8

static
BOOL
MatchWildcard(
    _In_ PWSTR Text,
    _In_ PWSTR Pattern)
{
    size_t TextLength, PatternLength, TextIndex = 0, PatternIndex = 0;
    size_t StartIndex = -1, MatchIndex = 0;

    if (Pattern == NULL)
        return TRUE;

    TextLength = wcslen(Text);
    PatternLength = wcslen(Pattern);

    while (TextIndex < TextLength)
    {
        if ((PatternIndex < PatternLength) &&
            ((Pattern[PatternIndex] == L'?') ||
             (towlower(Pattern[PatternIndex]) == towlower(Text[TextIndex]))))
        {
            TextIndex++;
            PatternIndex++;
        }
        else if ((PatternIndex < PatternLength) &&
                 (Pattern[PatternIndex] == L'*'))
        {
            StartIndex = PatternIndex;
            MatchIndex = TextIndex;
            PatternIndex++;
        }
        else if (StartIndex != -1)
        {
            PatternIndex = StartIndex + 1;
            MatchIndex++;
            TextIndex = MatchIndex;
        }
        else
        {
            return FALSE;
        }
    }

    while ((PatternIndex < PatternLength) &&
           (Pattern[PatternIndex] == L'*'))
    {
        PatternIndex++;
    }

    return (PatternIndex == PatternLength);
}

static
VOID
ParsePersistentRouteValue(
    _In_ PWSTR RouteValue,
    _Out_ PWSTR *Destination,
    _Out_ PWSTR *Netmask,
    _Out_ PWSTR *Gateway,
    _Out_ PWSTR *Metric)
{
    PWSTR Ptr, DestPtr;

    if (Destination)
        *Destination = NULL;

    if (Netmask)
        *Netmask = NULL;

    if (Gateway)
        *Gateway = NULL;

    if (Metric)
        *Metric = NULL;

    DestPtr = RouteValue;
    if (Destination)
        *Destination = DestPtr;

    Ptr = wcschr(DestPtr, L',');
    if (Ptr == NULL)
        return;

    *Ptr = L'\0';
    Ptr++;

    DestPtr = Ptr;
    if (Netmask)
        *Netmask = DestPtr;

    Ptr = wcschr(DestPtr, L',');
    if (Ptr == NULL)
        return;

    *Ptr = L'\0';
    Ptr++;

    DestPtr = Ptr;
    if (Gateway)
        *Gateway = DestPtr;

    Ptr = wcschr(DestPtr, L',');
    if (Ptr == NULL)
        return;

    *Ptr = L'\0';
    Ptr++;

    if (Metric)
        *Metric = Ptr;
}

static
BOOL
ParseCmdLine(
    _In_ int argc,
    _In_ WCHAR **argv,
    _In_ int start,
    _Out_ PWSTR *Destination,
    _Out_ PWSTR *Netmask,
    _Out_ PWSTR *Gateway,
    _Out_ PWSTR *Metric)
{
    int i;

    if (Destination)
        *Destination = NULL;

    if (Netmask)
        *Netmask = NULL;

    if (Gateway)
        *Gateway = NULL;

    if (Metric)
        *Metric = NULL;

    if (argc > start)
    {
        if (Destination)
            *Destination = argv[start];
    }
    else
        return FALSE;

    for (i = start + 1; i < argc; i++)
    {
        if (!_wcsicmp(argv[i], L"mask"))
        {
            i++;
            if (i >= argc)
                return FALSE;
            if (Netmask)
                *Netmask = argv[i];
        }
        else if (!_wcsicmp(argv[i], L"metric"))
        {
            i++;
            if (i >= argc) 
                return FALSE;
            if (Metric)
                *Metric = argv[i];
        }
        else
        {
            if (Gateway)
                *Gateway = argv[i];
        }
    }

    return TRUE;
}

static
VOID
PrintMacAddress(
    _In_ PBYTE Mac,
    _In_ PWSTR Buffer)
{
    swprintf(Buffer, L"%02X %02X %02X %02X %02X %02X ",
        Mac[0], Mac[1], Mac[2], Mac[3], Mac[4],  Mac[5]);
}

static
DWORD
PrintPersistentRoutes(
    _In_ PWSTR Filter)
{
    WCHAR szBuffer[64];
    DWORD dwIndex = 0, dwBufferSize;
    BOOL EntriesFound = FALSE;
    HKEY hKey;
    DWORD Error = ERROR_SUCCESS;
    PWSTR Destination, Netmask, Gateway, Metric;

    ConResPrintf(StdOut, IDS_SEPARATOR);
    ConResPrintf(StdOut, IDS_PERSISTENT_ROUTES);

    Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                          L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\PersistentRoutes",
                          0,
                          KEY_READ,
                          &hKey);
    if (Error == ERROR_SUCCESS)
    {
        for (;;)
        {
            dwBufferSize = 64;

            Error = RegEnumValueW(hKey,
                                  dwIndex,
                                  szBuffer,
                                  &dwBufferSize,
                                  NULL,
                                  NULL,
                                  NULL,
                                  NULL);
            if (Error != ERROR_SUCCESS)
            {
                if (Error == ERROR_NO_MORE_ITEMS)
                    Error = ERROR_SUCCESS;

                break;
            }

            ParsePersistentRouteValue(szBuffer,
                                      &Destination,
                                      &Netmask,
                                      &Gateway,
                                      &Metric);

            if (MatchWildcard(Destination, Filter))
            {
                if (EntriesFound == FALSE)
                    ConResPrintf(StdOut, IDS_PERSISTENT_HEADER);
                ConResPrintf(StdOut, IDS_PERSISTENT_ENTRY, Destination, Netmask, Gateway, Metric);
                EntriesFound = TRUE;
            }

            dwIndex++;
        }

        RegCloseKey(hKey);
    }

    if (EntriesFound == FALSE)
        ConResPrintf(StdOut, IDS_NONE);

    return Error;
}

static
VOID
FormatIPv4Address(
    _Out_ PWCHAR pBuffer,
    _In_ PIP_ADAPTER_UNICAST_ADDRESS UnicastAddress,
    _In_ DWORD IfIndex)
{
    PIP_ADAPTER_UNICAST_ADDRESS Ptr = UnicastAddress;

    while (Ptr)
    {
        if (Ptr->Address.lpSockaddr->sa_family == AF_INET)
        {
            struct sockaddr_in *si = (struct sockaddr_in *)(Ptr->Address.lpSockaddr);
            // DWORD dwStrLen = IPV4_ADDR_STRING_MAX_LEN;
            // WSAAddressToStringW(Ptr->Address.lpSockaddr, (DWORD)Ptr->Address.iSockaddrLength, NULL, pBuffer, &dwStrLen);
            // (void)InetNtopW(AF_INET, &(si->sin_addr), pBuffer, IPV4_ADDR_STRING_MAX_LEN);
            RtlIpv4AddressToStringW(&(si->sin_addr), pBuffer);
            return;
        }

        Ptr = Ptr->Next;
    }

    swprintf(pBuffer, L"%lx", IfIndex);
}

static
VOID
GetFirstIPv4AddressFromIndex(
    _Out_ PWSTR pBuffer,
    _In_ PIP_ADAPTER_ADDRESSES pAdapterAddresses,
    _In_ DWORD IfIndex)
{
    PIP_ADAPTER_ADDRESSES Ptr = pAdapterAddresses;

    while (Ptr)
    {
        if (Ptr->IfIndex == IfIndex)
        {
            FormatIPv4Address(pBuffer, Ptr->FirstUnicastAddress, Ptr->IfIndex);
            return;
        }

        Ptr = Ptr->Next;
    }

    swprintf(pBuffer, L"%lx", IfIndex);
}

static
int
CompareAdapters(
    _In_ const void *elem1,
    _In_ const void *elem2)
{
    return ((PIP_ADAPTER_ADDRESSES)elem2)->IfIndex - ((PIP_ADAPTER_ADDRESSES)elem1)->IfIndex;
}

static
DWORD
PrintActiveRoutes(
    _In_ PWSTR DestinationPattern,
    _In_ BOOL ShowIpv4)
{
    PMIB_IPFORWARDTABLE IpForwardTable = NULL;
    PIP_ADAPTER_ADDRESSES pAdapterAddresses = NULL, Ptr, *pAdapterSortArray = NULL;
    ULONG Size = 0;
    DWORD Error = ERROR_SUCCESS;
    ULONG adaptOutBufLen = 15000;
    WCHAR Destination[IPBUF], Gateway[IPBUF], Netmask[IPBUF], Interface[IPBUF];
    unsigned int i, AdapterCount, AdapterIndex;
    BOOL EntriesFound;
    ULONG Flags = /*GAA_FLAG_SKIP_UNICAST |*/ GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                  GAA_FLAG_SKIP_DNS_SERVER;

    /* set required buffer size */
    pAdapterAddresses = (PIP_ADAPTER_ADDRESSES)malloc(adaptOutBufLen);
    if (pAdapterAddresses == NULL)
    {
        Error = ERROR_NOT_ENOUGH_MEMORY;
        goto Error;
    }

    if (GetAdaptersAddresses(AF_INET, Flags, NULL, pAdapterAddresses, &adaptOutBufLen) == ERROR_BUFFER_OVERFLOW)
    {
       free(pAdapterAddresses);
       pAdapterAddresses = (PIP_ADAPTER_ADDRESSES)malloc(adaptOutBufLen);
       if (pAdapterAddresses == NULL)
       {
           Error = ERROR_NOT_ENOUGH_MEMORY;
           goto Error;
       }
    }

    if ((GetIpForwardTable(NULL, &Size, TRUE)) == ERROR_INSUFFICIENT_BUFFER)
    {
        if (!(IpForwardTable = malloc(Size)))
        {
            Error = ERROR_NOT_ENOUGH_MEMORY;
            goto Error;
        }
    }

    if (((Error = GetAdaptersAddresses(AF_INET, Flags, NULL, pAdapterAddresses, &adaptOutBufLen)) == NO_ERROR) &&
        ((Error = GetIpForwardTable(IpForwardTable, &Size, TRUE)) == NO_ERROR))
    {
        ConResPrintf(StdOut, IDS_SEPARATOR);
        ConResPrintf(StdOut, IDS_INTERFACE_LIST);

        AdapterCount = 0;
        Ptr = pAdapterAddresses;
        while (Ptr)
        {
            AdapterCount++;
            Ptr = Ptr->Next;
        }

        pAdapterSortArray = (PIP_ADAPTER_ADDRESSES*)malloc(AdapterCount * sizeof(PVOID));
        if (pAdapterSortArray == NULL)
        {
            Error = ERROR_NOT_ENOUGH_MEMORY;
            goto Error;
        }

        AdapterIndex = 0;
        Ptr = pAdapterAddresses;
        while (Ptr)
        {
            pAdapterSortArray[AdapterIndex] = Ptr;
            AdapterIndex++;
            Ptr = Ptr->Next;
        }

        qsort(pAdapterSortArray, AdapterCount, sizeof(PVOID), CompareAdapters);

        for (AdapterIndex = 0; AdapterIndex < AdapterCount; AdapterIndex++)
        {
            Ptr = pAdapterSortArray[AdapterIndex];
            if (Ptr->IfType == IF_TYPE_ETHERNET_CSMACD)
            {
                WCHAR PhysicalAddress[20];
                PrintMacAddress(Ptr->PhysicalAddress, PhysicalAddress);
                ConResPrintf(StdOut, IDS_ETHERNET_ENTRY, Ptr->IfIndex, PhysicalAddress, Ptr->Description);
            }
            else
            {
                ConResPrintf(StdOut, IDS_INTERFACE_ENTRY, Ptr->IfIndex, Ptr->Description);
            }
        }
        ConResPrintf(StdOut, IDS_SEPARATOR);

        if (!ShowIpv4)
            goto Error;

        ConResPrintf(StdOut, IDS_IPV4_ROUTE_TABLE);
        ConResPrintf(StdOut, IDS_SEPARATOR);
        ConResPrintf(StdOut, IDS_ACTIVE_ROUTES);
        EntriesFound = FALSE;
        for (i = 0; i < IpForwardTable->dwNumEntries; i++)
        {
            mbstowcs(Destination, inet_ntoa(IN_ADDR_OF(IpForwardTable->table[i].dwForwardDest)), IPBUF);
            mbstowcs(Netmask, inet_ntoa(IN_ADDR_OF(IpForwardTable->table[i].dwForwardMask)), IPBUF);
            mbstowcs(Gateway, inet_ntoa(IN_ADDR_OF(IpForwardTable->table[i].dwForwardNextHop)), IPBUF);
            GetFirstIPv4AddressFromIndex(Interface, pAdapterAddresses, IpForwardTable->table[i].dwForwardIfIndex);
            if (!wcscmp(Gateway, Interface))
                K32LoadStringW(GetModuleHandleW(NULL), IDS_ONLINK, Gateway, ARRAYSIZE(Gateway));

            if (MatchWildcard(Destination, DestinationPattern))
            {
                if (EntriesFound == FALSE)
                    ConResPrintf(StdOut, IDS_ROUTES_HEADER);
                ConResPrintf(StdOut, IDS_ROUTES_ENTRY,
                             Destination,
                             Netmask,
                             Gateway,
                             Interface,
                             IpForwardTable->table[i].dwForwardMetric1);
                EntriesFound = TRUE;
            }
        }

        if (DestinationPattern == NULL)
        {
            for (i = 0; i < IpForwardTable->dwNumEntries; i++)
            {
                if (IpForwardTable->table[i].dwForwardDest == 0)
                {
                    mbstowcs(Gateway, inet_ntoa(IN_ADDR_OF(IpForwardTable->table[i].dwForwardNextHop)), IPBUF);
                    ConResPrintf(StdOut, IDS_DEFAULT_GATEWAY, Gateway);
                }
            }
        }
        else if (EntriesFound == FALSE)
            ConResPrintf(StdOut, IDS_NONE);
    }

Error:
    if (pAdapterSortArray)
        free(pAdapterSortArray);
    if (pAdapterAddresses)
        free(pAdapterAddresses);
    if (IpForwardTable)
        free(IpForwardTable);

    if (Error != ERROR_SUCCESS)
        ConResPrintf(StdErr, IDS_ROUTE_ENUM_ERROR);

    return Error;
}

static
VOID
FormatIpv6Prefix(
    _Out_writes_(Count) PWSTR Buffer,
    _In_ SIZE_T Count,
    _In_ const IN6_ADDR *Address,
    _In_ UINT8 PrefixLength)
{
    WCHAR Text[INET6_ADDRSTRLEN];

    RtlIpv6AddressToStringW(Address, Text);
    _snwprintf(Buffer, Count, L"%s/%u", Text, PrefixLength);
    Buffer[Count - 1] = UNICODE_NULL;
}

static
DWORD
PrintActiveRoutes6(
    _In_ PWSTR DestinationPattern)
{
    PMIB_IPFORWARD_TABLE2 Table = NULL;
    WCHAR Destination[64], Gateway[INET6_ADDRSTRLEN], OnLink[32];
    BOOL EntriesFound = FALSE;
    DWORD Error;
    ULONG i;

    Error = GetIpForwardTable2(AF_INET6, &Table);
    if (Error != NO_ERROR)
    {
        ConResPrintf(StdErr, IDS_ROUTE_ENUM_ERROR);
        return Error;
    }

    K32LoadStringW(GetModuleHandleW(NULL), IDS_ONLINK, OnLink, ARRAYSIZE(OnLink));
    ConResPrintf(StdOut, IDS_IPV6_ROUTE_TABLE);
    ConResPrintf(StdOut, IDS_SEPARATOR);
    ConResPrintf(StdOut, IDS_ACTIVE_ROUTES);

    for (i = 0; i < Table->NumEntries; i++)
    {
        PMIB_IPFORWARD_ROW2 Row = &Table->Table[i];
        MIB_IPINTERFACE_ROW Interface;
        ULONG Metric = Row->Metric;

        FormatIpv6Prefix(Destination, ARRAYSIZE(Destination), &Row->DestinationPrefix.Prefix.Ipv6.sin6_addr,
                         Row->DestinationPrefix.PrefixLength);
        if (!MatchWildcard(Destination, DestinationPattern))
            continue;

        InitializeIpInterfaceEntry(&Interface);
        Interface.Family = AF_INET6;
        Interface.InterfaceIndex = Row->InterfaceIndex;
        if (GetIpInterfaceEntry(&Interface) == NO_ERROR)
            Metric += Interface.Metric;

        if (IN6_IS_ADDR_UNSPECIFIED(&Row->NextHop.Ipv6.sin6_addr))
            wcscpy(Gateway, OnLink);
        else
            RtlIpv6AddressToStringW(&Row->NextHop.Ipv6.sin6_addr, Gateway);

        if (EntriesFound == FALSE)
            ConResPrintf(StdOut, IDS_ROUTES6_HEADER);
        ConResPrintf(StdOut, wcslen(Destination) > 24 ? IDS_ROUTES6_LONG_ENTRY : IDS_ROUTES6_ENTRY,
                     Row->InterfaceIndex, Metric, Destination, Gateway);
        EntriesFound = TRUE;
    }

    if (EntriesFound == FALSE)
        ConResPrintf(StdOut, IDS_NONE);
    ConResPrintf(StdOut, IDS_SEPARATOR);
    ConResPrintf(StdOut, IDS_PERSISTENT_ROUTES);
    ConResPrintf(StdOut, IDS_NONE);

    FreeMibTable(Table);
    return NO_ERROR;
}

static
BOOL
ParseIpv6Prefix(
    _In_ PCWSTR String,
    _Out_ PIN6_ADDR Address,
    _Out_ PUINT8 PrefixLength)
{
    WCHAR Copy[64];
    PWSTR Slash;
    PCWSTR Terminator;
    ULONG Length = 128;

    if (wcslen(String) >= ARRAYSIZE(Copy))
        return FALSE;
    wcscpy(Copy, String);
    Slash = wcschr(Copy, L'/');
    if (Slash)
    {
        *Slash++ = UNICODE_NULL;
        Length = wcstoul(Slash, NULL, 10);
        if (Length > 128)
            return FALSE;
    }
    if (!NT_SUCCESS(RtlIpv6StringToAddressW(Copy, &Terminator, Address)) || *Terminator)
        return FALSE;
    *PrefixLength = (UINT8)Length;
    return TRUE;
}

static
int
AddRoute6(
    _In_ int argc,
    _In_ WCHAR **argv,
    _In_ int start,
    _In_ int flags)
{
    MIB_IPFORWARD_ROW2 Row;
    PCWSTR Terminator;
    DWORD Error;
    int i;

    if (flags & PERSISTENT_FLAG)
    {
        ConResPrintf(StdErr, IDS_ROUTE_ADD_ERROR);
        return 2;
    }

    InitializeIpForwardEntry(&Row);
    Row.DestinationPrefix.Prefix.Ipv6.sin6_family = AF_INET6;
    if (!ParseIpv6Prefix(argv[start], &Row.DestinationPrefix.Prefix.Ipv6.sin6_addr, &Row.DestinationPrefix.PrefixLength))
        return 1;
    Row.NextHop.Ipv6.sin6_family = AF_INET6;

    for (i = start + 1; i < argc; i++)
    {
        if (!_wcsicmp(argv[i], L"metric") && i + 1 < argc)
            Row.Metric = wcstoul(argv[++i], NULL, 10);
        else if (!_wcsicmp(argv[i], L"if") && i + 1 < argc)
            Row.InterfaceIndex = wcstoul(argv[++i], NULL, 10);
        else if (!NT_SUCCESS(RtlIpv6StringToAddressW(argv[i], &Terminator, &Row.NextHop.Ipv6.sin6_addr)) || *Terminator)
            return 1;
    }

    if (!Row.InterfaceIndex)
    {
        SOCKADDR_IN6 Target = Row.NextHop.Ipv6;

        if (IN6_IS_ADDR_UNSPECIFIED(&Target.sin6_addr) || IN6_IS_ADDR_LINKLOCAL(&Target.sin6_addr) ||
            GetBestInterfaceEx((PSOCKADDR)&Target, &Row.InterfaceIndex) != NO_ERROR)
        {
            ConResPrintf(StdErr, IDS_ROUTE_ADD_ERROR);
            return 2;
        }
    }

    Error = CreateIpForwardEntry2(&Row);
    if (Error != NO_ERROR)
    {
        ConResPrintf(StdErr, IDS_ROUTE_ADD_ERROR);
        return 2;
    }
    return 0;
}

static
int
DeleteRoutes6(
    _In_ int argc,
    _In_ WCHAR **argv,
    _In_ int start)
{
    PMIB_IPFORWARD_TABLE2 Table = NULL;
    IN6_ADDR Prefix, Gateway;
    UINT8 PrefixLength;
    PCWSTR Terminator;
    BOOL HaveGateway = FALSE, Deleted = FALSE;
    DWORD Error = NO_ERROR;
    ULONG i;

    if (!ParseIpv6Prefix(argv[start], &Prefix, &PrefixLength))
        return 1;
    if (argc > start + 1)
    {
        if (!NT_SUCCESS(RtlIpv6StringToAddressW(argv[start + 1], &Terminator, &Gateway)) || *Terminator)
            return 1;
        HaveGateway = TRUE;
    }

    if (GetIpForwardTable2(AF_INET6, &Table) != NO_ERROR)
    {
        ConResPrintf(StdErr, IDS_ROUTE_DEL_ERROR);
        return 2;
    }
    for (i = 0; i < Table->NumEntries; i++)
    {
        PMIB_IPFORWARD_ROW2 Row = &Table->Table[i];

        if (Row->DestinationPrefix.PrefixLength != PrefixLength ||
            memcmp(&Row->DestinationPrefix.Prefix.Ipv6.sin6_addr, &Prefix, sizeof(Prefix)) ||
            (HaveGateway && memcmp(&Row->NextHop.Ipv6.sin6_addr, &Gateway, sizeof(Gateway))))
            continue;
        Error = DeleteIpForwardEntry2(Row);
        if (Error != NO_ERROR)
            break;
        Deleted = TRUE;
    }
    FreeMibTable(Table);

    if (Error != NO_ERROR || !Deleted)
    {
        ConResPrintf(StdErr, IDS_ROUTE_DEL_ERROR);
        return 2;
    }
    return 0;
}

static
int
PrintRoutes(
    _In_ int argc,
    _In_ WCHAR **argv,
    _In_ int start,
    _In_ int flags)
{
    PWSTR DestinationPattern = NULL;
    BOOL ShowIpv4 = !(flags & IPV6_ONLY_FLAG), ShowIpv6 = !(flags & IPV4_ONLY_FLAG);
    DWORD Error;

    while (argc > start && (argv[start][0] == L'-' || argv[start][0] == L'/') &&
           (!_wcsicmp(&argv[start][1], L"4") || !_wcsicmp(&argv[start][1], L"6")))
    {
        if (!_wcsicmp(&argv[start][1], L"4"))
            ShowIpv6 = FALSE;
        else
            ShowIpv4 = FALSE;
        start++;
    }

    if (argc > start)
        ParseCmdLine(argc, argv, start, 
                     &DestinationPattern, NULL, NULL, NULL);

    Error = PrintActiveRoutes(DestinationPattern, ShowIpv4);
    if (Error == ERROR_SUCCESS && ShowIpv4)
        Error = PrintPersistentRoutes(DestinationPattern);
    if (Error == ERROR_SUCCESS && ShowIpv6)
        Error = PrintActiveRoutes6(DestinationPattern);

    return (Error == ERROR_SUCCESS) ? 0 : 2;
}

static
BOOL
ConvertAddCmdLine(
    _Out_ PMIB_IPFORWARDROW RowToAdd,
    _In_ int argc,
    _In_ WCHAR **argv,
    _In_ int start)
{
    int i;
    char addr[16];

    if (argc > start)
    {
        wcstombs(addr, argv[start], 16);
        RowToAdd->dwForwardDest = inet_addr(addr);
    }
    else
        return FALSE;

    for (i = start + 1; i < argc; i++)
    {
        if (!_wcsicmp(argv[i], L"mask"))
        {
            i++;
            if (i >= argc)
                return FALSE;
            wcstombs(addr, argv[i], 16);
            RowToAdd->dwForwardMask = inet_addr(addr);
        }
        else if (!_wcsicmp(argv[i], L"metric"))
        {
            i++;
            if (i >= argc) 
                return FALSE;
            RowToAdd->dwForwardMetric1 = _wtoi(argv[i]);
        }
        else if (!_wcsicmp(argv[i], L"if"))
        {
            i++;
            if (i >= argc)
                return FALSE;
            RowToAdd->dwForwardIfIndex = wcstoul(argv[i], NULL, 0);
        }
        else
        {
            wcstombs(addr, argv[i], 16);
            RowToAdd->dwForwardNextHop = inet_addr(addr);
        }
    }

    /* Restrict metric value to range 1 to 9999 */
    if (RowToAdd->dwForwardMetric1 == 0)
        RowToAdd->dwForwardMetric1 = 1;
    else if (RowToAdd->dwForwardMetric1 > 9999)
        RowToAdd->dwForwardMetric1 = 9999;

    if (!RowToAdd->dwForwardIfIndex &&
        GetBestInterface(RowToAdd->dwForwardNextHop, &RowToAdd->dwForwardIfIndex) != NO_ERROR)
        return FALSE;

    return TRUE;
}

static
DWORD
CreatePersistentIpForwardEntry(
    _In_ PMIB_IPFORWARDROW RowToAdd)
{
    WCHAR Destination[IPBUF], Gateway[IPBUF], Netmask[IPBUF];
    WCHAR ValueName[64];
    HKEY hKey;
    DWORD Error = ERROR_SUCCESS;

    mbstowcs(Destination, inet_ntoa(IN_ADDR_OF(RowToAdd->dwForwardDest)), IPBUF);
    mbstowcs(Netmask, inet_ntoa(IN_ADDR_OF(RowToAdd->dwForwardMask)), IPBUF);
    mbstowcs(Gateway, inet_ntoa(IN_ADDR_OF(RowToAdd->dwForwardNextHop)), IPBUF);

    swprintf(ValueName, L"%s,%s,%s,%ld", Destination, Netmask, Gateway, RowToAdd->dwForwardMetric1);

    Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                            L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\PersistentRoutes",
                            0,
                            NULL,
                            REG_OPTION_NON_VOLATILE,
                            KEY_WRITE,
                            NULL,
                            &hKey,
                            NULL);
    if (Error == ERROR_SUCCESS)
    {
        Error = RegSetValueEx(hKey,
                              ValueName,
                              0,
                              REG_SZ,
                              (LPBYTE)L"",
                              sizeof(WCHAR));

        RegCloseKey(hKey);
    }

    return Error;
}

static
DWORD
DeleteCustomRoutes(VOID)
{
    WCHAR Destination[IPBUF], Netmask[IPBUF];
    PMIB_IPFORWARDTABLE IpForwardTable = NULL;
    ULONG Size = 0;
    DWORD Error = ERROR_SUCCESS;
    ULONG i;

    if ((GetIpForwardTable(NULL, &Size, TRUE)) == ERROR_INSUFFICIENT_BUFFER)
    {
        if (!(IpForwardTable = malloc(Size)))
        {
            Error = ERROR_NOT_ENOUGH_MEMORY;
            goto done;
        }
    }

    Error = GetIpForwardTable(IpForwardTable, &Size, TRUE);
    if (Error != ERROR_SUCCESS)
        goto done;

    for (i = 0; i < IpForwardTable->dwNumEntries; i++)
    {
        mbstowcs(Destination, inet_ntoa(IN_ADDR_OF(IpForwardTable->table[i].dwForwardDest)), IPBUF);
        mbstowcs(Netmask, inet_ntoa(IN_ADDR_OF(IpForwardTable->table[i].dwForwardMask)), IPBUF);

        if ((wcscmp(Netmask, L"255.255.255.255") != 0) &&
            ((wcscmp(Destination, L"127.0.0.0") != 0) || (wcscmp(Netmask, L"255.0.0.0") != 0)) &&
            ((wcscmp(Destination, L"224.0.0.0") != 0) || (wcscmp(Netmask, L"240.0.0.0") != 0)))
        {
            Error = DeleteIpForwardEntry(&IpForwardTable->table[i]);
            if (Error != ERROR_SUCCESS)
                break;
        }
    }

done:
    if (IpForwardTable)
        free(IpForwardTable);

    return Error;
}

static
int
AddRoute(
    _In_ int argc,
    _In_ WCHAR **argv,
    _In_ int start,
    _In_ int flags)
{
    MIB_IPFORWARDROW RowToAdd = { 0 };
    DWORD Error;

    if (argc > start && wcschr(argv[start], L':'))
        return AddRoute6(argc, argv, start, flags);

    if ((argc <= start) || !ConvertAddCmdLine(&RowToAdd, argc, argv, start))
        return 1;

    if (flags & DELETE_FLAG)
        DeleteCustomRoutes();

    if (flags & PERSISTENT_FLAG)
        Error = CreatePersistentIpForwardEntry(&RowToAdd);
    else
        Error = CreateIpForwardEntry(&RowToAdd);
    if (Error != ERROR_SUCCESS)
    {
        ConResPrintf(StdErr, IDS_ROUTE_ADD_ERROR);
        return 2;
    }

    return 0;
}

static
DWORD
DeleteActiveRoutes(
    PWSTR DestinationPattern,
    PBOOL Deleted)
{
    WCHAR Destination[IPBUF], Netmask[IPBUF];
    PMIB_IPFORWARDTABLE IpForwardTable = NULL;
    ULONG Size = 0;
    DWORD Error = ERROR_SUCCESS;
    ULONG i;

    if ((GetIpForwardTable(NULL, &Size, TRUE)) == ERROR_INSUFFICIENT_BUFFER)
    {
        if (!(IpForwardTable = malloc(Size)))
        {
            Error = ERROR_NOT_ENOUGH_MEMORY;
            goto done;
        }
    }

    Error = GetIpForwardTable(IpForwardTable, &Size, TRUE);
    if (Error != ERROR_SUCCESS)
        goto done;

    for (i = 0; i < IpForwardTable->dwNumEntries; i++)
    {
        mbstowcs(Destination, inet_ntoa(IN_ADDR_OF(IpForwardTable->table[i].dwForwardDest)), IPBUF);
        mbstowcs(Netmask, inet_ntoa(IN_ADDR_OF(IpForwardTable->table[i].dwForwardMask)), IPBUF);

        if (MatchWildcard(Destination, DestinationPattern))
        {
            Error = DeleteIpForwardEntry(&IpForwardTable->table[i]);
            if (Error != ERROR_SUCCESS)
                break;
            *Deleted = TRUE;
        }
    }

done:
    if (IpForwardTable)
        free(IpForwardTable);

    return Error;
}

static
DWORD
DeletePersistentRoutes(
    PWSTR DestinationPattern,
    PBOOL Deleted)
{

    WCHAR szBuffer[64];
    DWORD dwIndex = 0, dwBufferSize;
    HKEY hKey;
    DWORD Error = ERROR_SUCCESS;
    PWSTR Destination = NULL;

    Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                          L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\PersistentRoutes",
                          0,
                          KEY_READ,
                          &hKey);
    if (Error == ERROR_SUCCESS)
    {
        for (;;)
        {
            dwBufferSize = 64;

            Error = RegEnumValueW(hKey,
                                  dwIndex,
                                  szBuffer,
                                  &dwBufferSize,
                                  NULL,
                                  NULL,
                                  NULL,
                                  NULL);
            if (Error != ERROR_SUCCESS)
            {
                if (Error == ERROR_NO_MORE_ITEMS)
                    Error = ERROR_SUCCESS;

                break;
            }

            ParsePersistentRouteValue(szBuffer,
                                      &Destination,
                                      NULL,
                                      NULL,
                                      NULL);

            if (MatchWildcard(Destination, DestinationPattern))
            {
                Error = RegDeleteValueW(hKey, szBuffer);
                if (Error != ERROR_SUCCESS)
                    break;
                *Deleted = TRUE;
            }

            dwIndex++;
        }

        RegCloseKey(hKey);
    }

    return Error;
}

static int
DeleteRoutes(
    _In_ int argc,
    _In_ WCHAR **argv,
    _In_ int start,
    _In_ int flags)
{
    PWSTR DestinationPattern = NULL;
    DWORD Error;
    BOOL Deleted = FALSE;

    if (argc > start && wcschr(argv[start], L':'))
        return DeleteRoutes6(argc, argv, start);

    if ((argc <= start) ||
        !ParseCmdLine(argc, argv, start, 
                      &DestinationPattern, NULL, NULL, NULL))
        return 1;

    if (flags & DELETE_FLAG)
        DeleteCustomRoutes();

    Error = DeleteActiveRoutes(DestinationPattern, &Deleted);
    if (Error == ERROR_SUCCESS)
        Error = DeletePersistentRoutes(DestinationPattern, &Deleted);

    /* Windows: "The route deletion failed: Element not found." */
    if (Error == ERROR_SUCCESS && !Deleted)
        Error = ERROR_NOT_FOUND;

    if (Error != ERROR_SUCCESS)
    {
        ConResPrintf(StdErr, IDS_ROUTE_DEL_ERROR);
        return 2;
    }

    return 0;
}

int
wmain(
    _In_ int argc,
    _In_ WCHAR **argv)
{
    int i, flags = 0, ret = 0;

    /* Initialize the Console Standard Streams */
    ConInitStdStreams();

    for (i = 1; i < argc; i++)
    {
        if (argv[i][0] == L'-' || argv[i][0] == L'/')
        {
            if (!_wcsicmp(&argv[i][1], L"f"))
            {
                flags |= DELETE_FLAG;
            }
            else if (!_wcsicmp(&argv[i][1], L"p"))
            {
                flags |= PERSISTENT_FLAG;
            }
            else if (!_wcsicmp(&argv[i][1], L"4"))
            {
                flags |= IPV4_ONLY_FLAG;
            }
            else if (!_wcsicmp(&argv[i][1], L"6"))
            {
                flags |= IPV6_ONLY_FLAG;
            }
            else
            {
                ret = 1;
                break;
            }
        }
        else
        {
            if (!_wcsicmp(argv[i], L"print"))
                ret = PrintRoutes(argc, argv, i + 1, flags);
            else if (!_wcsicmp(argv[i], L"add"))
                ret = AddRoute(argc, argv, i + 1, flags);
            else if (!_wcsicmp(argv[i], L"delete"))
                ret = DeleteRoutes(argc, argv, i + 1, flags);
            else
                ret = 1;

            break;
        }
    }

    if (argc == 1)
        ret = 1;

    if (ret == 1)
    {
        ConResPrintf(StdErr, IDS_USAGE1);
//        ConResPrintf(StdErr, IDS_USAGE2);
        ConResPrintf(StdErr, IDS_USAGE3);
    }

    return (ret == 0) ? 0 : 1;
}
