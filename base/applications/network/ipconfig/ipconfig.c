/*
 * PROJECT:     ReactOS ipconfig utility
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Display IP info for net adapters
 * COPYRIGHT:   Copyright 2005-2006 Ged Murphy <gedmurphy@gmail.com>
 */
/*
 * TODO:
 * implement registerdns, showclassid, setclassid
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winnls.h>
#include <winreg.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <ndk/rtlfuncs.h>
#include <inaddr.h>
#include <windns.h>
#include <windns_undoc.h>
#include <dhcpcsdk.h>
#include <dhcpcapi.h>
#include <strsafe.h>
#include <conutils.h>
#include <dhcpcsvc_c.h>

#include "resource.h"

typedef struct _RECORDTYPE
{
    WORD wRecordType;
    PCWSTR pszRecordName;
} RECORDTYPE, *PRECORDTYPE;

#define GUID_LEN 40

HINSTANCE hInstance;
HANDLE ProcessHeap;

RECORDTYPE TypeArray[] =
{
    {DNS_TYPE_ZERO,    L"ZERO"},
    {DNS_TYPE_A,       L"A"},
    {DNS_TYPE_NS,      L"NS"},
    {DNS_TYPE_MD,      L"MD"},
    {DNS_TYPE_MF,      L"MF"},
    {DNS_TYPE_CNAME,   L"CNAME"},
    {DNS_TYPE_SOA,     L"SOA"},
    {DNS_TYPE_MB,      L"MB"},
    {DNS_TYPE_MG,      L"MG"},
    {DNS_TYPE_MR,      L"MR"},
    {DNS_TYPE_NULL,    L"NULL"},
    {DNS_TYPE_WKS,     L"WKS"},
    {DNS_TYPE_PTR,     L"PTR"},
    {DNS_TYPE_HINFO,   L"HINFO"},
    {DNS_TYPE_MINFO,   L"MINFO"},
    {DNS_TYPE_MX,      L"MX"},
    {DNS_TYPE_TEXT,    L"TXT"},
    {DNS_TYPE_RP,      L"RP"},
    {DNS_TYPE_AFSDB,   L"AFSDB"},
    {DNS_TYPE_X25,     L"X25"},
    {DNS_TYPE_ISDN,    L"ISDN"},
    {DNS_TYPE_RT,      L"RT"},
    {DNS_TYPE_NSAP,    L"NSAP"},
    {DNS_TYPE_NSAPPTR, L"NSAPPTR"},
    {DNS_TYPE_SIG,     L"SIG"},
    {DNS_TYPE_KEY,     L"KEY"},
    {DNS_TYPE_PX,      L"PX"},
    {DNS_TYPE_GPOS,    L"GPOS"},
    {DNS_TYPE_AAAA,    L"AAAA"},
    {DNS_TYPE_LOC,     L"LOC"},
    {DNS_TYPE_NXT,     L"NXT"},
    {DNS_TYPE_EID,     L"EID"},
    {DNS_TYPE_NIMLOC,  L"NIMLOC"},
    {DNS_TYPE_SRV,     L"SRV"},
    {DNS_TYPE_ATMA,    L"ATMA"},
    {DNS_TYPE_NAPTR,   L"NAPTR"},
    {DNS_TYPE_KX,      L"KX"},
    {DNS_TYPE_CERT,    L"CERT"},
    {DNS_TYPE_A6,      L"A6"},
    {DNS_TYPE_DNAME,   L"DNAME"},
    {DNS_TYPE_SINK,    L"SINK"},
    {DNS_TYPE_OPT,     L"OPT"},
    {DNS_TYPE_UINFO,   L"UINFO"},
    {DNS_TYPE_UID,     L"UID"},
    {DNS_TYPE_GID,     L"GID"},
    {DNS_TYPE_UNSPEC,  L"UNSPEC"},
    {DNS_TYPE_ADDRS,   L"ADDRS"},
    {DNS_TYPE_TKEY,    L"TKEY"},
    {DNS_TYPE_TSIG,    L"TSIG"},
    {DNS_TYPE_IXFR,    L"IXFR"},
    {DNS_TYPE_AXFR,    L"AXFR"},
    {DNS_TYPE_MAILB,   L"MAILB"},
    {DNS_TYPE_MAILA,   L"MAILA"},
    {DNS_TYPE_ALL,     L"ALL"},
    {0, NULL}
};

PCWSTR
GetRecordTypeName(WORD wType)
{
    static WCHAR szType[8];
    INT i;

    for (i = 0; ; i++)
    {
         if (TypeArray[i].pszRecordName == NULL)
             break;

         if (TypeArray[i].wRecordType == wType)
             return TypeArray[i].pszRecordName;
    }

    _swprintf(szType, L"%hu", wType);

    return szType;
}

/* print MAC address */
PCSTR PrintMacAddr(PBYTE Mac)
{
    static CHAR MacAddr[20];

    sprintf(MacAddr, "%02X-%02X-%02X-%02X-%02X-%02X",
        Mac[0], Mac[1], Mac[2], Mac[3], Mac[4],  Mac[5]);

    return MacAddr;
}

/* convert time_t to localized string */
_Ret_opt_z_ PWSTR timeToStr(_In_ time_t TimeStamp)
{
    struct tm* ptm;
    SYSTEMTIME SystemTime;
    INT DateCchSize, TimeCchSize, TotalCchSize, i;
    PWSTR DateTimeString, psz;

    /* Convert Unix time to SYSTEMTIME */
    /* localtime_s may be preferred if available */
    ptm = localtime(&TimeStamp);
    if (!ptm)
    {
        return NULL;
    }
    SystemTime.wYear = ptm->tm_year + 1900;
    SystemTime.wMonth = ptm->tm_mon + 1;
    SystemTime.wDay = ptm->tm_mday;
    SystemTime.wHour = ptm->tm_hour;
    SystemTime.wMinute = ptm->tm_min;
    SystemTime.wSecond = ptm->tm_sec;

    /* Get total size in characters required of buffer */
    DateCchSize = GetDateFormatW(LOCALE_USER_DEFAULT, DATE_LONGDATE, &SystemTime, NULL, NULL, 0);
    if (!DateCchSize)
    {
        return NULL;
    }
    TimeCchSize = GetTimeFormatW(LOCALE_USER_DEFAULT, 0, &SystemTime, NULL, NULL, 0);
    if (!TimeCchSize)
    {
        return NULL;
    }
    /* Two terminating null are included, the first one will be replaced by space */
    TotalCchSize = DateCchSize + TimeCchSize;

    /* Allocate buffer and format datetime string */
    DateTimeString = (PWSTR)HeapAlloc(ProcessHeap, 0, TotalCchSize * sizeof(WCHAR));
    if (!DateTimeString)
    {
        return NULL;
    }

    /* Get date string */
    i = GetDateFormatW(LOCALE_USER_DEFAULT, DATE_LONGDATE, &SystemTime, NULL, DateTimeString, TotalCchSize);
    if (i)
    {
        /* Append space and move pointer */
        DateTimeString[i - 1] = L' ';
        psz = DateTimeString + i;
        TotalCchSize -= i;

        /* Get time string */
        if (GetTimeFormatW(LOCALE_USER_DEFAULT, 0, &SystemTime, NULL, psz, TotalCchSize))
        {
            return DateTimeString;
        }
    }

    HeapFree(ProcessHeap, 0, DateTimeString);
    return NULL;
}

VOID
DoFormatMessage(
    _In_ LONG ErrorCode)
{
    if (ErrorCode == 0)
        ErrorCode = GetLastError();

    ConMsgPuts(StdOut,
               FORMAT_MESSAGE_FROM_SYSTEM,
               NULL,
               ErrorCode,
               LANG_USER_DEFAULT);
}

PWSTR
GetUnicodeAdapterName(
    _In_ PSTR pszAnsiName)
{
    PWSTR pszUnicodeName;
    size_t i, len;

    len = strlen(pszAnsiName);
    pszUnicodeName = HeapAlloc(GetProcessHeap(), 0, (len + 1) * sizeof(WCHAR));
    if (pszUnicodeName == NULL)
        return NULL;

    for (i = 0; i < len; i++)
        pszUnicodeName[i] = (WCHAR)pszAnsiName[i];
    pszUnicodeName[i] = UNICODE_NULL;

    return pszUnicodeName;
}

VOID
GetAdapterFriendlyName(
    _In_ PSTR lpClass,
    _In_ DWORD cchFriendlyNameLength,
    _Out_ PWSTR pszFriendlyName)
{
    HKEY hKey = NULL;
    CHAR Path[256];
    PCSTR PrePath = "SYSTEM\\CurrentControlSet\\Control\\Network\\{4D36E972-E325-11CE-BFC1-08002BE10318}\\";
    PCSTR PostPath = "\\Connection";
    size_t PathSize;
    DWORD dwType;
    DWORD dwDataSize;

    if (cchFriendlyNameLength == 0)
        return;
    pszFriendlyName[0] = UNICODE_NULL;

    /* don't overflow the buffer */
    PathSize = strlen(PrePath) + strlen(lpClass) + strlen(PostPath) + 1;
    if (PathSize > _countof(Path))
        return;

    sprintf(Path, "%s%s%s", PrePath, lpClass, PostPath);

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      Path,
                      0,
                      KEY_READ,
                      &hKey) == ERROR_SUCCESS)
    {
        dwDataSize = cchFriendlyNameLength * sizeof(WCHAR);
        if (RegQueryValueExW(hKey,
                             L"Name",
                             NULL,
                             &dwType,
                             (PBYTE)pszFriendlyName,
                             &dwDataSize) != ERROR_SUCCESS ||
            dwType != REG_SZ)
        {
            pszFriendlyName[0] = UNICODE_NULL;
        }
        pszFriendlyName[cchFriendlyNameLength - 1] = UNICODE_NULL;
    }

    if (hKey != NULL)
        RegCloseKey(hKey);
}

VOID
GetInterfaceFriendlyName(
    _In_ PCWSTR lpDeviceName,
    _In_ DWORD cchFriendlyNameLength,
    _Out_ PWSTR pszFriendlyName)
{
    HKEY hKey = NULL;
    WCHAR Path[256];
    PCWSTR PrePath = L"SYSTEM\\CurrentControlSet\\Control\\Network\\{4D36E972-E325-11CE-BFC1-08002BE10318}\\";
    PCWSTR PostPath = L"\\Connection";
    PCWSTR DevicePrefix = L"\\DEVICE\\TCPIP_";
    size_t PathSize;
    DWORD dwType;
    DWORD dwDataSize;

    size_t dwPrefixLength = wcslen(DevicePrefix);

    if (cchFriendlyNameLength == 0)
        return;
    pszFriendlyName[0] = UNICODE_NULL;

    /* don't overflow the buffer */
    PathSize = wcslen(PrePath) + wcslen(lpDeviceName) - dwPrefixLength + wcslen(PostPath) + 1;
    if (PathSize > _countof(Path))
        return;

    _swprintf(Path, L"%s%s%s", PrePath, &lpDeviceName[dwPrefixLength], PostPath);

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      Path,
                      0,
                      KEY_READ,
                      &hKey) == ERROR_SUCCESS)
    {
        dwDataSize = cchFriendlyNameLength * sizeof(WCHAR);
        if (RegQueryValueExW(hKey,
                             L"Name",
                             NULL,
                             &dwType,
                             (PBYTE)pszFriendlyName,
                             &dwDataSize) != ERROR_SUCCESS ||
            dwType != REG_SZ)
        {
            pszFriendlyName[0] = UNICODE_NULL;
        }
        pszFriendlyName[cchFriendlyNameLength - 1] = UNICODE_NULL;
    }

    if (hKey != NULL)
        RegCloseKey(hKey);
}

static
VOID
PrintAdapterDescription(PSTR lpClass)
{
    HKEY hBaseKey = NULL;
    HKEY hClassKey = NULL;
    PSTR lpKeyClass = NULL;
    PSTR lpConDesc = NULL;
    PWSTR lpPath = NULL;
    WCHAR szPrePath[] = L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E972-E325-11CE-BFC1-08002bE10318}\\";
    DWORD dwType;
    DWORD dwDataSize;
    INT i;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      szPrePath,
                      0,
                      KEY_READ,
                      &hBaseKey) != ERROR_SUCCESS)
    {
        return;
    }

    for (i = 0; ; i++)
    {
        size_t PathSize;
        LONG Status;
        WCHAR szName[10];
        DWORD NameLen = 9;

        if ((Status = RegEnumKeyExW(hBaseKey,
                                    i,
                                    szName,
                                    &NameLen,
                                    NULL,
                                    NULL,
                                    NULL,
                                    NULL)) != ERROR_SUCCESS)
        {
            if (Status == ERROR_NO_MORE_ITEMS)
            {
                DoFormatMessage(Status);
                lpConDesc = NULL;
                goto CLEANUP;
            }
            else
                continue;
        }

        PathSize = wcslen(szPrePath) + wcslen(szName) + 1;
        lpPath = (PWSTR)HeapAlloc(ProcessHeap,
                                  0,
                                  PathSize * sizeof(WCHAR));
        if (lpPath == NULL)
            goto CLEANUP;

        _swprintf(lpPath, L"%s%s", szPrePath, szName);

        //MessageBox(NULL, lpPath, NULL, 0);

        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                          lpPath,
                          0,
                          KEY_READ,
                          &hClassKey) != ERROR_SUCCESS)
        {
            goto CLEANUP;
        }

        HeapFree(ProcessHeap, 0, lpPath);
        lpPath = NULL;

        if (RegQueryValueExA(hClassKey,
                             "NetCfgInstanceId",
                             NULL,
                             &dwType,
                             NULL,
                             &dwDataSize) == ERROR_SUCCESS)
        {
            lpKeyClass = (PSTR)HeapAlloc(ProcessHeap,
                                         0,
                                         dwDataSize);
            if (lpKeyClass == NULL)
                goto CLEANUP;

            if (RegQueryValueExA(hClassKey,
                                 "NetCfgInstanceId",
                                 NULL,
                                 &dwType,
                                 (PBYTE)lpKeyClass,
                                 &dwDataSize) != ERROR_SUCCESS)
            {
                HeapFree(ProcessHeap, 0, lpKeyClass);
                lpKeyClass = NULL;
                continue;
            }
        }
        else
            continue;

        if (!_stricmp(lpClass, lpKeyClass))
        {
            HeapFree(ProcessHeap, 0, lpKeyClass);
            lpKeyClass = NULL;

            if (RegQueryValueExA(hClassKey,
                                 "DriverDesc",
                                 NULL,
                                 &dwType,
                                 NULL,
                                 &dwDataSize) == ERROR_SUCCESS)
            {
                lpConDesc = (PSTR)HeapAlloc(ProcessHeap,
                                            0,
                                            dwDataSize);
                if (lpConDesc != NULL)
                {
                    if (RegQueryValueExA(hClassKey,
                                         "DriverDesc",
                                         NULL,
                                         &dwType,
                                         (PBYTE)lpConDesc,
                                         &dwDataSize) == ERROR_SUCCESS)
                    {
                        printf("%s", lpConDesc);
                    }

                    HeapFree(ProcessHeap, 0, lpConDesc);
                    lpConDesc = NULL;
                }
            }

            break;
        }
    }

CLEANUP:
    if (hBaseKey != NULL)
        RegCloseKey(hBaseKey);
    if (hClassKey != NULL)
        RegCloseKey(hClassKey);
    if (lpPath != NULL)
        HeapFree(ProcessHeap, 0, lpPath);
    if (lpKeyClass != NULL)
        HeapFree(ProcessHeap, 0, lpKeyClass);
}

static
VOID
PrintNodeType(
    _In_ UINT NodeType)
{
    switch (NodeType)
    {
        case BROADCAST_NODETYPE:
            ConResPrintf(StdOut, IDS_NODETYPEBCAST);
            break;

        case PEER_TO_PEER_NODETYPE:
            ConResPrintf(StdOut, IDS_NODETYPEP2P);
            break;

        case MIXED_NODETYPE:
            ConResPrintf(StdOut, IDS_NODETYPEMIXED);
            break;

        case HYBRID_NODETYPE:
            ConResPrintf(StdOut, IDS_NODETYPEHYBRID);
            break;

        default :
            ConResPrintf(StdOut, IDS_NODETYPEUNKNOWN);
            break;
    }
}

static
VOID
PrintAdapterTypeAndName(
    PIP_ADAPTER_INFO pAdapterInfo)
{
    WCHAR szFriendlyName[MAX_PATH];

    GetAdapterFriendlyName(pAdapterInfo->AdapterName, MAX_PATH, szFriendlyName);

    switch (pAdapterInfo->Type)
    {
        case MIB_IF_TYPE_OTHER:
            ConResPrintf(StdOut, IDS_OTHER, szFriendlyName);
            break;

        case MIB_IF_TYPE_ETHERNET:
            ConResPrintf(StdOut, IDS_ETH, szFriendlyName);
            break;

        case MIB_IF_TYPE_TOKENRING:
            ConResPrintf(StdOut, IDS_TOKEN, szFriendlyName);
            break;

        case MIB_IF_TYPE_FDDI:
            ConResPrintf(StdOut, IDS_FDDI, szFriendlyName);
            break;

        case MIB_IF_TYPE_PPP:
            ConResPrintf(StdOut, IDS_PPP, szFriendlyName);
            break;

        case MIB_IF_TYPE_LOOPBACK:
            ConResPrintf(StdOut, IDS_LOOP, szFriendlyName);
            break;

        case MIB_IF_TYPE_SLIP:
            ConResPrintf(StdOut, IDS_SLIP, szFriendlyName);
            break;

        case IF_TYPE_IEEE80211:
            ConResPrintf(StdOut, IDS_WIFI, szFriendlyName);
            break;

        default:
            ConResPrintf(StdOut, IDS_UNKNOWNADAPTER, szFriendlyName);
            break;
    }
}

static
PIP_ADAPTER_ADDRESSES
GetAddressList(VOID)
{
    PIP_ADAPTER_ADDRESSES pList;
    ULONG Size = 16384;
    ULONG ret;

    for (;;)
    {
        pList = HeapAlloc(ProcessHeap, 0, Size);
        if (pList == NULL)
            return NULL;
        ret = GetAdaptersAddresses(AF_UNSPEC,
                                   GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                                   NULL, pList, &Size);
        if (ret == NO_ERROR)
            return pList;
        HeapFree(ProcessHeap, 0, pList);
        if (ret != ERROR_BUFFER_OVERFLOW)
            return NULL;
    }
}

static
PIP_ADAPTER_ADDRESSES
FindAdapterAddresses(PIP_ADAPTER_ADDRESSES pList, PCSTR AdapterName)
{
    for (; pList; pList = pList->Next)
    {
        if (!_stricmp(pList->AdapterName, AdapterName))
            return pList;
    }
    return NULL;
}

static
PCSTR
FormatSocketAddress(const SOCKADDR *pAddress, ULONG DefaultScope, PSTR Buffer, SIZE_T Size)
{
    Buffer[0] = ANSI_NULL;
    if (pAddress->sa_family == AF_INET6)
    {
        const SOCKADDR_IN6 *pAddress6 = (const SOCKADDR_IN6 *)pAddress;
        ULONG ScopeId = pAddress6->sin6_scope_id;
        CHAR Text[INET6_ADDRSTRLEN];

        RtlIpv6AddressToStringA(&pAddress6->sin6_addr, Text);
        if (!ScopeId && IN6_IS_ADDR_LINKLOCAL(&pAddress6->sin6_addr))
            ScopeId = DefaultScope;
        if (ScopeId && (IN6_IS_ADDR_LINKLOCAL(&pAddress6->sin6_addr) || IN6_IS_ADDR_SITELOCAL(&pAddress6->sin6_addr)))
            _snprintf(Buffer, Size, "%s%%%lu", Text, ScopeId);
        else
            _snprintf(Buffer, Size, "%s", Text);
    }
    else if (pAddress->sa_family == AF_INET)
    {
        RtlIpv4AddressToStringA(&((const SOCKADDR_IN *)pAddress)->sin_addr, Buffer);
    }
    Buffer[Size - 1] = ANSI_NULL;
    return Buffer;
}

static
VOID
PrintAddressState(BOOL bAll, IP_DAD_STATE DadState)
{
    if (!bAll)
        ConResPrintf(StdOut, IDS_ADDREND);
    else if (DadState == IpDadStateTentative)
        ConResPrintf(StdOut, IDS_ADDRTENTATIVE);
    else if (DadState == IpDadStateDeprecated)
        ConResPrintf(StdOut, IDS_ADDRDEPRECATED);
    else if (DadState == IpDadStateDuplicate)
        ConResPrintf(StdOut, IDS_ADDRDUPLICATE);
    else
        ConResPrintf(StdOut, IDS_ADDRPREFERRED);
}

static
VOID
PrintLeaseTimes(time_t Obtained, time_t Expires)
{
    PWSTR DateTimeString;

    DateTimeString = timeToStr(Obtained);
    ConResPrintf(StdOut, IDS_LEASEOBTAINED, DateTimeString ? DateTimeString : L"N/A");
    if (DateTimeString)
        HeapFree(ProcessHeap, 0, DateTimeString);
    DateTimeString = timeToStr(Expires);
    ConResPrintf(StdOut, IDS_LEASEEXPIRES, DateTimeString ? DateTimeString : L"N/A");
    if (DateTimeString)
        HeapFree(ProcessHeap, 0, DateTimeString);
}

static
VOID
PrintIpv6Addresses(PIP_ADAPTER_ADDRESSES pAddresses, BOOL bAll)
{
    PIP_ADAPTER_UNICAST_ADDRESS pUnicast;
    CHAR Buffer[INET6_ADDRSTRLEN + 16];
    INT Pass;

    for (Pass = 0; Pass < 3; Pass++)
    {
        for (pUnicast = pAddresses->FirstUnicastAddress; pUnicast; pUnicast = pUnicast->Next)
        {
            const SOCKADDR_IN6 *pAddress6 = (const SOCKADDR_IN6 *)pUnicast->Address.lpSockaddr;
            INT Kind;

            if (pAddress6->sin6_family != AF_INET6)
                continue;
            if (IN6_IS_ADDR_LINKLOCAL(&pAddress6->sin6_addr))
                Kind = 2;
            else if (pUnicast->PrefixOrigin == IpPrefixOriginRouterAdvertisement && pUnicast->SuffixOrigin == IpSuffixOriginRandom)
                Kind = 1;
            else
                Kind = 0;
            if (Kind != Pass)
                continue;

            FormatSocketAddress(pUnicast->Address.lpSockaddr, pAddresses->Ipv6IfIndex, Buffer, sizeof(Buffer));
            ConResPrintf(StdOut, Kind == 2 ? IDS_LINKLOCALIPV6ADDRESS : Kind == 1 ? IDS_TEMPIPV6ADDRESS : IDS_IPV6ADDRESS, Buffer);
            PrintAddressState(bAll, pUnicast->DadState);

            if (bAll && pUnicast->PrefixOrigin == IpPrefixOriginDhcp && pUnicast->ValidLifetime != 0xFFFFFFFF)
            {
                time_t Now = time(NULL);

                PrintLeaseTimes(Now - (pUnicast->LeaseLifetime > pUnicast->ValidLifetime ?
                                       pUnicast->LeaseLifetime - pUnicast->ValidLifetime : 0),
                                Now + pUnicast->ValidLifetime);
            }
        }
    }
}

static
VOID
PrintDhcpv6Identity(PCSTR AdapterName)
{
    CHAR KeyName[MAX_PATH];
    BYTE Duid[130];
    CHAR Text[sizeof(Duid) * 3 + 1];
    DWORD Size, Type, Iaid, i;
    HKEY hKey;

    _snprintf(KeyName, ARRAYSIZE(KeyName), "SYSTEM\\CurrentControlSet\\Services\\Tcpip6\\Parameters\\Interfaces\\%s", AdapterName);
    KeyName[ARRAYSIZE(KeyName) - 1] = ANSI_NULL;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, KeyName, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return;
    Size = sizeof(Iaid);
    if (RegQueryValueExA(hKey, "Dhcpv6Iaid", NULL, &Type, (PBYTE)&Iaid, &Size) != ERROR_SUCCESS || Type != REG_DWORD)
    {
        RegCloseKey(hKey);
        return;
    }
    RegCloseKey(hKey);
    ConResPrintf(StdOut, IDS_DHCPV6IAID, Iaid);

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Services\\Tcpip6\\Parameters",
                      0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return;
    Size = sizeof(Duid);
    if (RegQueryValueExA(hKey, "Dhcpv6DUID", NULL, &Type, Duid, &Size) == ERROR_SUCCESS && Type == REG_BINARY && Size)
    {
        for (i = 0; i < Size; i++)
            sprintf(&Text[i * 3], i + 1 < Size ? "%02X-" : "%02X", Duid[i]);
        ConResPrintf(StdOut, IDS_DHCPV6DUID, Text);
    }
    RegCloseKey(hKey);
}

static
VOID
PrintDnsSuffixSearchList(PIP_ADAPTER_ADDRESSES pList, PCSTR pszPrimary, PCSTR pszDomain)
{
    CHAR Entries[16][MAX_DNS_SUFFIX_STRING_LENGTH * 3];
    PCSTR Candidates[2] = { pszPrimary, pszDomain };
    PIP_ADAPTER_ADDRESSES pAdapter;
    PIP_ADAPTER_DNS_SUFFIX pSuffix;
    ULONG Count = 0, i, j;

    for (i = 0; i < ARRAYSIZE(Candidates); i++)
    {
        if (Candidates[i] && Candidates[i][0] && (!Count || _stricmp(Entries[0], Candidates[i])))
            StringCchCopyA(Entries[Count++], ARRAYSIZE(Entries[0]), Candidates[i]);
    }

    for (pAdapter = pList; pAdapter; pAdapter = pAdapter->Next)
    {
        for (pSuffix = pAdapter->FirstDnsSuffix; pSuffix && Count < ARRAYSIZE(Entries); pSuffix = pSuffix->Next)
        {
            if (!WideCharToMultiByte(CP_ACP, 0, pSuffix->String, -1, Entries[Count], ARRAYSIZE(Entries[0]), NULL, NULL))
                continue;
            for (j = 0; j < Count && _stricmp(Entries[j], Entries[Count]); j++);
            if (j == Count)
                Count++;
        }
    }

    if (!Count)
    {
        ConResPrintf(StdOut, IDS_DNSSUFFIXLIST, "");
        return;
    }
    for (i = 0; i < Count; i++)
        ConResPrintf(StdOut, i ? IDS_EMPTYLINE : IDS_DNSSUFFIXLIST, Entries[i]);
}

VOID
ShowInfo(
    BOOL bShowHeader,
    BOOL bAll)
{
    MIB_IFROW mibEntry;
    PIP_ADAPTER_INFO pAdapterInfo = NULL;
    PIP_ADAPTER_INFO pAdapter = NULL;
    PIP_ADDR_STRING pIPAddr;
    ULONG adaptOutBufLen = 0;
    PFIXED_INFO pFixedInfo = NULL;
    ULONG netOutBufLen = 0;
    PIP_PER_ADAPTER_INFO pPerAdapterInfo = NULL;
    ULONG ulPerAdapterInfoLength = 0;
    PSTR pszDomainName = NULL;
    DWORD dwDomainNameSize = 0;
    ULONG ret = 0;
    PIP_ADAPTER_ADDRESSES pAddressList = NULL;

    GetComputerNameExA(ComputerNameDnsDomain,
                       NULL,
                       &dwDomainNameSize);
    if (dwDomainNameSize > 0)
    {
        pszDomainName = HeapAlloc(ProcessHeap,
                                  0,
                                  dwDomainNameSize * sizeof(CHAR));
        if (pszDomainName != NULL)
            GetComputerNameExA(ComputerNameDnsDomain,
                               pszDomainName,
                               &dwDomainNameSize);
    }

    /* call GetAdaptersInfo to obtain the adapter info */
    ret = GetAdaptersInfo(pAdapterInfo, &adaptOutBufLen);
    if (ret == ERROR_BUFFER_OVERFLOW)
    {
        pAdapterInfo = (IP_ADAPTER_INFO *)HeapAlloc(ProcessHeap, 0, adaptOutBufLen);
        if (pAdapterInfo == NULL)
            goto done;

        ret = GetAdaptersInfo(pAdapterInfo, &adaptOutBufLen);
        if (ret != NO_ERROR)
        {
            DoFormatMessage(0);
            goto done;
        }
    }
    else
    {
        if (ret != ERROR_NO_DATA)
        {
            DoFormatMessage(0);
            goto done;
        }
    }

    /* call GetNetworkParams to obtain the network info */
    if (GetNetworkParams(pFixedInfo, &netOutBufLen) == ERROR_BUFFER_OVERFLOW)
    {
        pFixedInfo = (FIXED_INFO *)HeapAlloc(ProcessHeap, 0, netOutBufLen);
        if (pFixedInfo == NULL)
        {
            goto done;
        }
        if (GetNetworkParams(pFixedInfo, &netOutBufLen) != NO_ERROR)
        {
            DoFormatMessage(0);
            goto done;
        }
    }
    else
    {
        DoFormatMessage(0);
        goto done;
    }

    pAdapter = pAdapterInfo;
    pAddressList = GetAddressList();

    if (bShowHeader)
        ConResPrintf(StdOut, IDS_HEADER);

    if (bAll)
    {
        ConResPrintf(StdOut, IDS_HOSTNAME, pFixedInfo->HostName);
        ConResPrintf(StdOut, IDS_PRIMARYDNSSUFFIX, (pszDomainName != NULL) ? pszDomainName : "");

        PrintNodeType(pFixedInfo->NodeType);

        if (pFixedInfo->EnableRouting)
            ConResPrintf(StdOut, IDS_IPROUTINGYES);
        else
            ConResPrintf(StdOut, IDS_IPROUTINGNO);

        if (pAdapter && pAdapter->HaveWins)
            ConResPrintf(StdOut, IDS_WINSPROXYYES);
        else
            ConResPrintf(StdOut, IDS_WINSPROXYNO);

        PrintDnsSuffixSearchList(pAddressList, pszDomainName, pFixedInfo->DomainName);
    }

    while (pAdapter)
    {
        BOOLEAN bConnected = TRUE;
        PIP_ADAPTER_ADDRESSES pAddresses = FindAdapterAddresses(pAddressList, pAdapter->AdapterName);
        CHAR Buffer[INET6_ADDRSTRLEN + 16];

        mibEntry.dwIndex = pAdapter->Index;
        GetIfEntry(&mibEntry);

        PrintAdapterTypeAndName(pAdapter);

        if (GetPerAdapterInfo(pAdapter->Index, pPerAdapterInfo, &ulPerAdapterInfoLength) == ERROR_BUFFER_OVERFLOW)
        {
            pPerAdapterInfo = (PIP_PER_ADAPTER_INFO)HeapAlloc(ProcessHeap, 0, ulPerAdapterInfoLength);
            if (pPerAdapterInfo != NULL)
            {
                GetPerAdapterInfo(pAdapter->Index, pPerAdapterInfo, &ulPerAdapterInfoLength);
            }
        }

        /* check if the adapter is connected to the media */
        if (mibEntry.dwOperStatus != MIB_IF_OPER_STATUS_CONNECTED && mibEntry.dwOperStatus != MIB_IF_OPER_STATUS_OPERATIONAL)
        {
            bConnected = FALSE;
            ConResPrintf(StdOut, IDS_MEDIADISCONNECTED);
        }
        else
        {
            ConResPrintf(StdOut, IDS_CONNECTIONDNSSUFFIX, pFixedInfo->DomainName);
        }

        if (bAll)
        {
            ConResPrintf(StdOut, IDS_DESCRIPTION);
            PrintAdapterDescription(pAdapter->AdapterName);
            printf("\n");

            ConResPrintf(StdOut, IDS_PHYSICALADDRESS, PrintMacAddr(pAdapter->Address));

            if (bConnected)
            {
                if (pAdapter->DhcpEnabled)
                {
                    ConResPrintf(StdOut, IDS_DHCPYES);

                    if (pPerAdapterInfo != NULL)
                    {
                        if (pPerAdapterInfo->AutoconfigEnabled)
                            ConResPrintf(StdOut, IDS_AUTOCONFIGYES);
                        else
                            ConResPrintf(StdOut, IDS_AUTOCONFIGNO);
                    }
                }
                else
                {
                    ConResPrintf(StdOut, IDS_DHCPNO);
                }
            }
        }

        if (!bConnected)
        {
            HeapFree(ProcessHeap, 0, pPerAdapterInfo);
            pPerAdapterInfo = NULL;
            pAdapter = pAdapter->Next;
            continue;
        }

        if (pAddresses)
            PrintIpv6Addresses(pAddresses, bAll);

        for (pIPAddr = &pAdapter->IpAddressList; pIPAddr; pIPAddr = pIPAddr->Next)
        {
            ConResPrintf(StdOut, IDS_IPADDRESS, pIPAddr->IpAddress.String);
            ConResPrintf(StdOut, IDS_SUBNETMASK, pIPAddr->IpMask.String);
        }

        if (pAddresses && pAddresses->FirstGatewayAddress)
        {
            PIP_ADAPTER_GATEWAY_ADDRESS pGateway;
            BOOL bFirst = TRUE;
            INT Pass;

            for (Pass = 0; Pass < 2; Pass++)
            {
                for (pGateway = pAddresses->FirstGatewayAddress; pGateway; pGateway = pGateway->Next)
                {
                    if (pGateway->Address.lpSockaddr->sa_family != (Pass ? AF_INET : AF_INET6))
                        continue;
                    FormatSocketAddress(pGateway->Address.lpSockaddr, pAddresses->Ipv6IfIndex, Buffer, sizeof(Buffer));
                    ConResPrintf(StdOut, bFirst ? IDS_DEFAULTGATEWAY : IDS_EMPTYLINE, Buffer);
                    bFirst = FALSE;
                }
            }
        }
        else if (strcmp(pAdapter->GatewayList.IpAddress.String, "0.0.0.0"))
            ConResPrintf(StdOut, IDS_DEFAULTGATEWAY, pAdapter->GatewayList.IpAddress.String);
        else
            ConResPrintf(StdOut, IDS_DEFAULTGATEWAY, "");

        if (bAll)
        {
            if (pAdapter->DhcpEnabled)
                ConResPrintf(StdOut, IDS_DHCPSERVER, pAdapter->DhcpServer.IpAddress.String);

            PrintDhcpv6Identity(pAdapter->AdapterName);

            if (pAddresses)
            {
                PIP_ADAPTER_DNS_SERVER_ADDRESS pDns;

                for (pDns = pAddresses->FirstDnsServerAddress; pDns; pDns = pDns->Next)
                {
                    FormatSocketAddress(pDns->Address.lpSockaddr, pAddresses->Ipv6IfIndex, Buffer, sizeof(Buffer));
                    ConResPrintf(StdOut, pDns == pAddresses->FirstDnsServerAddress ? IDS_DNSSERVERS : IDS_EMPTYLINE, Buffer);
                }
            }
            else
            {
                ConResPrintf(StdOut, IDS_DNSSERVERS, pFixedInfo->DnsServerList.IpAddress.String);
                pIPAddr = pFixedInfo->DnsServerList.Next;
                while (pIPAddr)
                {
                    ConResPrintf(StdOut, IDS_EMPTYLINE, pIPAddr->IpAddress.String);
                    pIPAddr = pIPAddr->Next;
                }
            }

            if (pAdapter->HaveWins)
            {
                ConResPrintf(StdOut, IDS_PRIMARYWINSSERVER, pAdapter->PrimaryWinsServer.IpAddress.String);
                ConResPrintf(StdOut, IDS_SECONDARYWINSSERVER, pAdapter->SecondaryWinsServer.IpAddress.String);
            }

            if (pAdapter->DhcpEnabled && strcmp(pAdapter->DhcpServer.IpAddress.String, "255.255.255.255"))
            {
                PWSTR DateTimeString;
                DateTimeString = timeToStr(pAdapter->LeaseObtained);
                ConResPrintf(StdOut, IDS_LEASEOBTAINED, DateTimeString ? DateTimeString : L"N/A");
                if (DateTimeString)
                {
                    HeapFree(ProcessHeap, 0, DateTimeString);
                }
                DateTimeString = timeToStr(pAdapter->LeaseExpires);
                ConResPrintf(StdOut, IDS_LEASEEXPIRES, DateTimeString ? DateTimeString : L"N/A");
                if (DateTimeString)
                {
                    HeapFree(ProcessHeap, 0, DateTimeString);
                }
            }

            if (pAddresses && pAddresses->FirstDnsSuffix)
            {
                PIP_ADAPTER_DNS_SUFFIX pSuffix;
                CHAR Suffix[MAX_DNS_SUFFIX_STRING_LENGTH * 3];

                ConResPrintf(StdOut, IDS_CONNDNSSUFFIXLIST);
                for (pSuffix = pAddresses->FirstDnsSuffix; pSuffix; pSuffix = pSuffix->Next)
                {
                    if (WideCharToMultiByte(CP_ACP, 0, pSuffix->String, -1, Suffix, sizeof(Suffix), NULL, NULL))
                        ConResPrintf(StdOut, IDS_EMPTYLINE, Suffix);
                }
            }
        }

        HeapFree(ProcessHeap, 0, pPerAdapterInfo);
        pPerAdapterInfo = NULL;

        pAdapter = pAdapter->Next;
    }

done:
    if (pAddressList)
        HeapFree(ProcessHeap, 0, pAddressList);
    if (pszDomainName)
        HeapFree(ProcessHeap, 0, pszDomainName);
    if (pFixedInfo)
        HeapFree(ProcessHeap, 0, pFixedInfo);
    if (pAdapterInfo)
        HeapFree(ProcessHeap, 0, pAdapterInfo);
}

static
BOOL
MatchWildcard(
    _In_ PWSTR pszExpression,
    _In_ PWSTR pszName)
{
    WCHAR *pCharE, *pCharN, charE, charN;

    if (pszExpression == NULL)
        return TRUE;

    if (pszName == NULL)
        return FALSE;

    pCharE = pszExpression;
    pCharN = pszName;
    while (*pCharE != UNICODE_NULL)
    {
        charE = towlower(*pCharE);
        charN = towlower(*pCharN);

        if (charE == L'*')
        {
            if (*(pCharE + 1) != charN)
                pCharN++;
            else
                pCharE++;
        }
        else if (charE == L'?')
        {
            pCharE++;
            pCharN++;
        }
        else if (charE == charN)
        {
            pCharE++;
            pCharN++;
        }
        else
        {
            return FALSE;
        }
    }

    return TRUE;
}

void __RPC_FAR * __RPC_USER MIDL_user_allocate(SIZE_T len)
{
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, len);
}

void __RPC_USER MIDL_user_free(void __RPC_FAR * ptr)
{
    HeapFree(GetProcessHeap(), 0, ptr);
}

handle_t __RPC_USER
PDHCP_SERVER_NAME_bind(
    _In_ PDHCP_SERVER_NAME pszServerName)
{
    handle_t hBinding = NULL;
    LPWSTR pszStringBinding;

    if (RpcStringBindingComposeW(NULL, L"ncacn_np", pszServerName, L"\\pipe\\dhcpcsvc", NULL, &pszStringBinding))
        return NULL;
    RpcBindingFromStringBindingW(pszStringBinding, &hBinding);
    RpcStringFreeW(&pszStringBinding);
    return hBinding;
}

void __RPC_USER
PDHCP_SERVER_NAME_unbind(
    _In_ PDHCP_SERVER_NAME pszServerName,
    _In_ handle_t hBinding)
{
    RpcBindingFree(&hBinding);
}

static
DWORD
CallDhcpv6(
    PWSTR AdapterName,
    BOOL bRelease)
{
    DWORD ret;

    RpcTryExcept
    {
        ret = bRelease ? ReleaseParametersV6(NULL, AdapterName) : AcquireParametersV6(NULL, AdapterName);
    }
    RpcExcept(EXCEPTION_EXECUTE_HANDLER)
    {
        ret = I_RpcMapWin32Status(RpcExceptionCode());
    }
    RpcEndExcept;

    return ret;
}

VOID
Release(
    PWSTR pszAdapterName,
    BOOL bIpv6)
{
    PIP_ADAPTER_INFO pAdapterInfo = NULL;
    PIP_ADAPTER_INFO pAdapter = NULL;
    ULONG adaptOutBufLen = 0;
    ULONG ret = 0;
    WCHAR szFriendlyName[MAX_PATH];
    WCHAR szUnicodeAdapterName[MAX_ADAPTER_NAME_LENGTH + 4];
    MIB_IFROW mibEntry;
    BOOL bFoundAdapter = FALSE;
    DWORD dwVersion;

    ConResPrintf(StdOut, IDS_HEADER);

    /* call GetAdaptersInfo to obtain the adapter info */
    ret = GetAdaptersInfo(pAdapterInfo, &adaptOutBufLen);
    if (ret != ERROR_BUFFER_OVERFLOW)
    {
        DoFormatMessage(ret);
        return;
    }

    pAdapterInfo = (IP_ADAPTER_INFO *)HeapAlloc(ProcessHeap, 0, adaptOutBufLen);
    if (pAdapterInfo == NULL)
    {
        DoFormatMessage(ERROR_NOT_ENOUGH_MEMORY);
        return;
    }

    ret = GetAdaptersInfo(pAdapterInfo, &adaptOutBufLen);
    if (ret != NO_ERROR)
    {
        DoFormatMessage(0);
        goto done;
    }

    DhcpCApiInitialize(&dwVersion);

    pAdapter = pAdapterInfo;

    while (pAdapter)
    {
        GetAdapterFriendlyName(pAdapter->AdapterName, MAX_PATH, szFriendlyName);

        if ((pszAdapterName == NULL) || MatchWildcard(pszAdapterName, szFriendlyName))
        {
            bFoundAdapter = TRUE;

            mibEntry.dwIndex = pAdapter->Index;
            GetIfEntry(&mibEntry);

            if (mibEntry.dwOperStatus == MIB_IF_OPER_STATUS_CONNECTED ||
                mibEntry.dwOperStatus == MIB_IF_OPER_STATUS_OPERATIONAL)
            {
                if (bIpv6 || pAdapter->DhcpEnabled)
                {
                    if (bIpv6 || strcmp(pAdapter->IpAddressList.IpAddress.String, "0.0.0.0"))
                    {
                        mbstowcs(szUnicodeAdapterName, pAdapter->AdapterName, strlen(pAdapter->AdapterName) + 1);

                        /* Call DhcpReleaseParameters to release the IP address on the specified adapter. */
                        ret = bIpv6 ? CallDhcpv6(szUnicodeAdapterName, TRUE) : DhcpReleaseParameters(szUnicodeAdapterName);
                        if (ret != NO_ERROR)
                        {
                            ConResPrintf(StdOut, IDS_DHCPRELEASEERROR, szFriendlyName);
                            DoFormatMessage(ret);
                        }
                    }
                    else
                    {
                        ConResPrintf(StdOut, IDS_DHCPRELEASED, szFriendlyName);
                    }
                }
                else
                {
                    ConResPrintf(StdOut, IDS_DHCPNOTENABLED, szFriendlyName);
                }
            }
            else
            {
                ConResPrintf(StdOut, IDS_DHCPNOTCONNECTED, szFriendlyName);
            }
        }

        pAdapter = pAdapter->Next;
    }

    DhcpCApiCleanup();

    if (bFoundAdapter == FALSE)
    {
        ConResPrintf(StdOut, IDS_DHCPNOADAPTER);
    }
    else
    {
        ShowInfo(FALSE, FALSE);
    }

done:
    if (pAdapterInfo)
        HeapFree(ProcessHeap, 0, pAdapterInfo);
}

VOID
Renew(
    PWSTR pszAdapterName,
    BOOL bIpv6)
{
    PIP_ADAPTER_INFO pAdapterInfo = NULL;
    PIP_ADAPTER_INFO pAdapter = NULL;
    ULONG adaptOutBufLen = 0;
    ULONG ret = 0;
    WCHAR szFriendlyName[MAX_PATH];
    WCHAR szUnicodeAdapterName[MAX_ADAPTER_NAME_LENGTH + 4];
    MIB_IFROW mibEntry;
    BOOL bFoundAdapter = FALSE;
    DWORD dwVersion;

    ConResPrintf(StdOut, IDS_HEADER);

    /* call GetAdaptersInfo to obtain the adapter info */
    ret = GetAdaptersInfo(pAdapterInfo, &adaptOutBufLen);
    if (ret != ERROR_BUFFER_OVERFLOW)
    {
        DoFormatMessage(ret);
        return;
    }

    pAdapterInfo = (IP_ADAPTER_INFO *)HeapAlloc(ProcessHeap, 0, adaptOutBufLen);
    if (pAdapterInfo == NULL)
    {
        DoFormatMessage(ERROR_NOT_ENOUGH_MEMORY);
        return;
    }

    ret = GetAdaptersInfo(pAdapterInfo, &adaptOutBufLen);
    if (ret != NO_ERROR)
    {
        DoFormatMessage(0);
        goto done;
    }

    DhcpCApiInitialize(&dwVersion);

    pAdapter = pAdapterInfo;

    while (pAdapter)
    {
        GetAdapterFriendlyName(pAdapter->AdapterName, MAX_PATH, szFriendlyName);

        if ((pszAdapterName == NULL) || MatchWildcard(pszAdapterName, szFriendlyName))
        {
            bFoundAdapter = TRUE;

            mibEntry.dwIndex = pAdapter->Index;
            GetIfEntry(&mibEntry);

            if (mibEntry.dwOperStatus == MIB_IF_OPER_STATUS_CONNECTED ||
                mibEntry.dwOperStatus == MIB_IF_OPER_STATUS_OPERATIONAL)
            {
                if (bIpv6 || pAdapter->DhcpEnabled)
                {
                    mbstowcs(szUnicodeAdapterName, pAdapter->AdapterName, strlen(pAdapter->AdapterName) + 1);

                    /* Call DhcpAcquireParameters to renew the IP address on the specified adapter. */
                    ret = bIpv6 ? CallDhcpv6(szUnicodeAdapterName, FALSE) : DhcpAcquireParameters(szUnicodeAdapterName);
                    if (ret != NO_ERROR)
                    {
                        ConResPrintf(StdOut, IDS_DHCPRENEWERROR, szFriendlyName);
                        DoFormatMessage(ret);
                    }
                }
                else
                {
                    ConResPrintf(StdOut, IDS_DHCPNOTENABLED, szFriendlyName);
                }
            }
            else
            {
                ConResPrintf(StdOut, IDS_DHCPNOTCONNECTED, szFriendlyName);
            }
        }

        pAdapter = pAdapter->Next;
    }

    DhcpCApiCleanup();

    if (bFoundAdapter == FALSE)
    {
        ConResPrintf(StdOut, IDS_DHCPNOADAPTER);
    }
    else
    {
        ShowInfo(FALSE, FALSE);
    }

done:
    if (pAdapterInfo)
        HeapFree(ProcessHeap, 0, pAdapterInfo);
}

VOID
FlushDns(VOID)
{
    ConResPrintf(StdOut, IDS_HEADER);

    if (DnsFlushResolverCache())
    {
        ConResPrintf(StdOut, IDS_DNSFLUSHSUCCESS);
    }
    else
    {
        ConResPrintf(StdOut, IDS_DNSFLUSHERROR);
        DoFormatMessage(GetLastError());
    }
}

VOID
RegisterDns(VOID)
{
    /* FIXME */
    printf("\nSorry /registerdns is not implemented yet\n");
}

static
VOID
DisplayDnsRecord(
    PWSTR pszName,
    WORD wType)
{
    PDNS_RECORDW pQueryResults = NULL, pThisRecord, pNextRecord;
    WCHAR szBuffer[48];
    IN_ADDR Addr4;
    IN6_ADDR Addr6;
    DNS_STATUS Status;

    ConResPrintf(StdOut, IDS_DNSNAME, pszName);
    ConResPrintf(StdOut, IDS_DNSLINE);

    pQueryResults = NULL;
    Status = DnsQuery_W(pszName,
                        wType,
                        DNS_QUERY_NO_WIRE_QUERY,
                        NULL,
                        (PDNS_RECORD *)&pQueryResults,
                        NULL);
    if (Status != ERROR_SUCCESS)
    {
        if (Status == DNS_ERROR_RCODE_NAME_ERROR)
        {
            ConResPrintf(StdOut, IDS_DNSNONAME);
        }
        else if (Status == DNS_INFO_NO_RECORDS)
        {
            ConResPrintf(StdOut, IDS_DNSNORECORD, GetRecordTypeName(wType));
        }
        return;
    }

    pThisRecord = pQueryResults;
    while (pThisRecord != NULL)
    {
        pNextRecord = pThisRecord->pNext;

        ConResPrintf(StdOut, IDS_DNSRECORDNAME, pThisRecord->pName);
        ConResPrintf(StdOut, IDS_DNSRECORDTYPE, pThisRecord->wType);
        ConResPrintf(StdOut, IDS_DNSRECORDTTL, pThisRecord->dwTtl);
        ConResPrintf(StdOut, IDS_DNSRECORDLENGTH, pThisRecord->wDataLength);

        switch (pThisRecord->Flags.S.Section)
        {
            case DnsSectionQuestion:
                ConResPrintf(StdOut, IDS_DNSSECTIONQUESTION);
                break;

            case DnsSectionAnswer:
                ConResPrintf(StdOut, IDS_DNSSECTIONANSWER);
                break;

            case DnsSectionAuthority:
                ConResPrintf(StdOut, IDS_DNSSECTIONAUTHORITY);
                break;

            case DnsSectionAdditional:
                ConResPrintf(StdOut, IDS_DNSSECTIONADDITIONAL);
                break;
        }

        switch (pThisRecord->wType)
        {
            case DNS_TYPE_A:
                Addr4.S_un.S_addr = pThisRecord->Data.A.IpAddress;
                RtlIpv4AddressToStringW(&Addr4, szBuffer);
                ConResPrintf(StdOut, IDS_DNSTYPEA, szBuffer);
                break;

            case DNS_TYPE_NS:
                ConResPrintf(StdOut, IDS_DNSTYPENS, pThisRecord->Data.NS.pNameHost);
                break;

            case DNS_TYPE_CNAME:
                ConResPrintf(StdOut, IDS_DNSTYPECNAME, pThisRecord->Data.CNAME.pNameHost);
                break;

            case DNS_TYPE_SOA:
                ConResPrintf(StdOut, IDS_DNSTYPESOA1,
                             pThisRecord->Data.SOA.pNamePrimaryServer,
                             pThisRecord->Data.SOA.pNameAdministrator,
                             pThisRecord->Data.SOA.dwSerialNo);
                ConResPrintf(StdOut, IDS_DNSTYPESOA2,
                             pThisRecord->Data.SOA.dwRefresh,
                             pThisRecord->Data.SOA.dwRetry,
                             pThisRecord->Data.SOA.dwExpire,
                             pThisRecord->Data.SOA.dwDefaultTtl);
                break;

            case DNS_TYPE_PTR:
                ConResPrintf(StdOut, IDS_DNSTYPEPTR, pThisRecord->Data.PTR.pNameHost);
                break;

            case DNS_TYPE_MX:
                ConResPrintf(StdOut, IDS_DNSTYPEMX,
                             pThisRecord->Data.MX.pNameExchange,
                             pThisRecord->Data.MX.wPreference,
                             pThisRecord->Data.MX.Pad);
                break;

            case DNS_TYPE_AAAA:
                RtlCopyMemory(&Addr6, &pThisRecord->Data.AAAA.Ip6Address, sizeof(Addr6));
                RtlIpv6AddressToStringW(&Addr6, szBuffer);
                ConResPrintf(StdOut, IDS_DNSTYPEAAAA, szBuffer);
                break;

            case DNS_TYPE_ATMA:
                ConResPrintf(StdOut, IDS_DNSTYPEATMA);
                break;

            case DNS_TYPE_SRV:
                ConResPrintf(StdOut, IDS_DNSTYPESRV,
                             pThisRecord->Data.SRV.pNameTarget,
                             pThisRecord->Data.SRV.wPriority,
                             pThisRecord->Data.SRV.wWeight,
                             pThisRecord->Data.SRV.wPort);
                break;
        }
        ConPuts(StdOut, L"\n\n");

        pThisRecord = pNextRecord;
    }

    DnsRecordListFree((PDNS_RECORD)pQueryResults, DnsFreeRecordList);
}

VOID
DisplayDns(VOID)
{
    PDNS_CACHE_ENTRY DnsEntry = NULL, pThisEntry, pNextEntry;

    ConResPrintf(StdOut, IDS_HEADER);

    if (!DnsGetCacheDataTable(&DnsEntry))
    {
        DoFormatMessage(GetLastError());
        return;
    }

    if (DnsEntry == NULL)
        return;

    pThisEntry = DnsEntry;
    while (pThisEntry != NULL)
    {
        pNextEntry = pThisEntry->pNext;

        if (pThisEntry->wType1 != DNS_TYPE_ZERO)
            DisplayDnsRecord(pThisEntry->pszName, pThisEntry->wType1);

        if (pThisEntry->wType2 != DNS_TYPE_ZERO)
            DisplayDnsRecord(pThisEntry->pszName, pThisEntry->wType2);

        if (pThisEntry->pszName)
            LocalFree(pThisEntry->pszName);
        LocalFree(pThisEntry);

        pThisEntry = pNextEntry;
    }
}

VOID
ShowClassId(
    PWSTR pszAdapterName)
{
    printf("\nSorry /showclassid adapter is not implemented yet\n");
}

VOID
SetClassId(
    PWSTR pszAdapterName,
    PWSTR pszClassId)
{
    PIP_ADAPTER_INFO pAdapterInfo = NULL;
    PIP_ADAPTER_INFO pAdapter = NULL, pFoundAdapter = NULL;
    ULONG adaptOutBufLen = 0;
    ULONG ret = 0;
    WCHAR szFriendlyName[MAX_PATH];
    WCHAR szKeyName[256];
    WCHAR szUnicodeAdapterName[MAX_ADAPTER_NAME_LENGTH + 4];
    MIB_IFROW mibEntry;
    HKEY hKey;
    DHCP_PNP_EVENT PnpEvent;

    ConResPrintf(StdOut, IDS_HEADER);

    /* call GetAdaptersInfo to obtain the adapter info */
    ret = GetAdaptersInfo(pAdapterInfo, &adaptOutBufLen);
    if (ret != ERROR_BUFFER_OVERFLOW)
    {
        DoFormatMessage(ret);
        return;
    }

    pAdapterInfo = (IP_ADAPTER_INFO *)HeapAlloc(ProcessHeap, 0, adaptOutBufLen);
    if (pAdapterInfo == NULL)
    {
        DoFormatMessage(ERROR_NOT_ENOUGH_MEMORY);
        return;
    }

    ret = GetAdaptersInfo(pAdapterInfo, &adaptOutBufLen);
    if (ret != NO_ERROR)
    {
        DoFormatMessage(0);
        goto done;
    }

    pAdapter = pAdapterInfo;
    while (pAdapter)
    {
        GetAdapterFriendlyName(pAdapter->AdapterName, MAX_PATH, szFriendlyName);

        if (MatchWildcard(pszAdapterName, szFriendlyName))
        {
            mibEntry.dwIndex = pAdapter->Index;
            GetIfEntry(&mibEntry);

            if (mibEntry.dwOperStatus == MIB_IF_OPER_STATUS_CONNECTED ||
                mibEntry.dwOperStatus == MIB_IF_OPER_STATUS_OPERATIONAL)
            {
                pFoundAdapter = pAdapter;
                break;
            }
        }

        pAdapter = pAdapter->Next;
    }

    if (pFoundAdapter)
    {
        _swprintf(szKeyName,
                  L"System\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\%S",
                  pFoundAdapter->AdapterName);

        ret = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                            szKeyName,
                            0,
                            KEY_WRITE,
                            &hKey);
        if (ret != ERROR_SUCCESS)
        {
            ConResPrintf(StdOut, IDS_DHCPSETIDERROR, szFriendlyName);
            DoFormatMessage(ret);
            goto done;
        }

        if (pszClassId == NULL)
            pszClassId = L"";

        ret = RegSetValueExW(hKey,
                             L"DhcpClassId",
                             0, REG_SZ,
                             (PBYTE)pszClassId,
                             (DWORD)((wcslen(pszClassId) + 1) * sizeof(WCHAR)));
        RegCloseKey(hKey);
        if (ret != ERROR_SUCCESS)
        {
            ConResPrintf(StdOut, IDS_DHCPSETIDERROR, szFriendlyName);
            DoFormatMessage(ret);
            goto done;
        }

        mbstowcs(szUnicodeAdapterName, pFoundAdapter->AdapterName, strlen(pFoundAdapter->AdapterName) + 1);

        ZeroMemory(&PnpEvent, sizeof(PnpEvent));
        PnpEvent.Unknown5 = 1;

        ret = DhcpHandlePnPEvent(0,
                                 1,
                                 szUnicodeAdapterName,
                                 &PnpEvent,
                                 0);
        if (ret != ERROR_SUCCESS)
        {
            ConResPrintf(StdOut, IDS_DHCPSETIDERROR, szFriendlyName);
            DoFormatMessage(ret);
            goto done;
        }

        ConResPrintf(StdOut, IDS_DHCPSETIDSUCCESS, szFriendlyName);
    }
    else
    {
        ConResPrintf(StdOut, IDS_DHCPNOADAPTER);
    }

done:
    if (pAdapterInfo)
        HeapFree(ProcessHeap, 0, pAdapterInfo);
}

VOID
Usage(
    _In_ BOOL Error)
{
    if (Error)
        ConResPrintf(StdOut, IDS_CMDLINEERROR);
    ConResPrintf(StdOut, IDS_USAGE);
}

int wmain(int argc, wchar_t *argv[])
{
    BOOL DoUsage=FALSE;
    BOOL DoAll=FALSE;
    BOOL DoRelease=FALSE;
    BOOL DoRenew=FALSE;
    BOOL DoRelease6=FALSE;
    BOOL DoRenew6=FALSE;
    BOOL DoFlushdns=FALSE;
    BOOL DoRegisterdns=FALSE;
    BOOL DoDisplaydns=FALSE;
    BOOL DoShowclassid=FALSE;
    BOOL DoSetclassid=FALSE;

    /* Initialize the Console Standard Streams */
    ConInitStdStreams();

    hInstance = GetModuleHandle(NULL);
    ProcessHeap = GetProcessHeap();

    /* Parse command line for options we have been given. */
    if ((argc > 1) && (argv[1][0] == L'/' || argv[1][0] == L'-'))
    {
        if (!_wcsicmp(&argv[1][1], L"?"))
        {
            DoUsage = TRUE;
        }
        else if (!_wcsnicmp(&argv[1][1], L"ALL", wcslen(&argv[1][1])))
        {
            DoAll = TRUE;
        }
        else if (!_wcsnicmp(&argv[1][1], L"RELEASE", wcslen(&argv[1][1])))
        {
            DoRelease = TRUE;
        }
        else if (!_wcsnicmp(&argv[1][1], L"RENEW", wcslen(&argv[1][1])))
        {
            DoRenew = TRUE;
        }
        else if (!_wcsicmp(&argv[1][1], L"RELEASE6"))
        {
            DoRelease6 = TRUE;
        }
        else if (!_wcsicmp(&argv[1][1], L"RENEW6"))
        {
            DoRenew6 = TRUE;
        }
        else if (!_wcsnicmp(&argv[1][1], L"FLUSHDNS", wcslen(&argv[1][1])))
        {
            DoFlushdns = TRUE;
        }
        else if (!_wcsnicmp(&argv[1][1], L"FLUSHREGISTERDNS", wcslen(&argv[1][1])))
        {
            DoRegisterdns = TRUE;
        }
        else if (!_wcsnicmp(&argv[1][1], L"DISPLAYDNS", wcslen(&argv[1][1])))
        {
            DoDisplaydns = TRUE;
        }
        else if (!_wcsnicmp(&argv[1][1], L"SHOWCLASSID", wcslen(&argv[1][1])))
        {
            DoShowclassid = TRUE;
        }
        else if (!_wcsnicmp(&argv[1][1], L"SETCLASSID", wcslen(&argv[1][1])))
        {
            DoSetclassid = TRUE;
        }
    }

    switch (argc)
    {
        case 1:  /* Default behaviour if no options are given*/
            ShowInfo(TRUE, FALSE);
            break;
        case 2:  /* Process all the options that take no parameters */
            if (DoUsage)
                Usage(FALSE);
            else if (DoAll)
                ShowInfo(TRUE, TRUE);
            else if (DoRelease)
                Release(NULL, FALSE);
            else if (DoRenew)
                Renew(NULL, FALSE);
            else if (DoRelease6)
                Release(NULL, TRUE);
            else if (DoRenew6)
                Renew(NULL, TRUE);
            else if (DoFlushdns)
                FlushDns();
            else if (DoRegisterdns)
                RegisterDns();
            else if (DoDisplaydns)
                DisplayDns();
            else
                Usage(TRUE);
            break;
        case 3: /* Process all the options that can have 1 parameter */
            if (DoRelease)
                Release(argv[2], FALSE);
            else if (DoRenew)
                Renew(argv[2], FALSE);
            else if (DoRelease6)
                Release(argv[2], TRUE);
            else if (DoRenew6)
                Renew(argv[2], TRUE);
            else if (DoShowclassid)
                ShowClassId(argv[2]);
            else if (DoSetclassid)
                SetClassId(argv[2], NULL);
            else
                Usage(TRUE);
            break;
        case 4:  /* Process all the options that can have 2 parameters */
            if (DoSetclassid)
                SetClassId(argv[2], argv[3]);
            else
                Usage(TRUE);
            break;
        default:
            Usage(TRUE);
    }

    return 0;
}
