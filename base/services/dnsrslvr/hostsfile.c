/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS system libraries
 * FILE:        base/services/dnsrslvr/hostsfile.c
 * PURPOSE:     HOSTS file routines
 * PROGRAMERS:  Art Yerkes
 *              Eric Kohl
 */

#include "precomp.h"
#include <inaddr.h>
#include <in6addr.h>


#define NDEBUG
#include <debug.h>

static WCHAR szHexChar[] = L"0123456789abcdef";

static
PWSTR
AnsiToUnicode(
    PSTR NarrowString)
{
    PWSTR WideString;
    int WideLen;

    WideLen = MultiByteToWideChar(CP_ACP,
                                  0,
                                  NarrowString,
                                  -1,
                                  NULL,
                                  0);
    if (WideLen == 0)
        return NULL;

    WideString = HeapAlloc(GetProcessHeap(),
                           0,
                           WideLen * sizeof(WCHAR));
    if (WideString == NULL)
        return NULL;

    MultiByteToWideChar(CP_ACP,
                        0,
                        NarrowString,
                        -1,
                        WideString,
                        WideLen);

    return WideString;
}


static
BOOL
ParseIpv4Address(
    _In_ PCSTR AddressString,
    _Out_ PIN_ADDR pAddress)
{
    PCSTR pTerminator = NULL;
    NTSTATUS Status;

    Status = RtlIpv4StringToAddressA(AddressString,
                                     TRUE,
                                     &pTerminator,
                                     pAddress);
    if (NT_SUCCESS(Status) && pTerminator != NULL && *pTerminator == '\0')
        return TRUE;

    return FALSE;
}


static
BOOL
ParseIpv6Address(
    _In_ LPCSTR AddressString,
    _Out_ PIN6_ADDR pAddress)
{
    PCSTR pTerminator = NULL;
    NTSTATUS Status;

    Status = RtlIpv6StringToAddressA(AddressString,
                                     &pTerminator,
                                     pAddress);
    if (NT_SUCCESS(Status) && pTerminator != NULL && *pTerminator == '\0')
        return TRUE;

    return FALSE;
}


static
VOID
AddIpv4HostEntries(
    PWSTR pszHostName,
    PIN_ADDR pAddress)
{
    DNS_RECORDW ARecord, PtrRecord;
    WCHAR szReverseName[32];

    /* Prepare the A record */
    ZeroMemory(&ARecord, sizeof(DNS_RECORDW));

    ARecord.pName = pszHostName;
    ARecord.wType = DNS_TYPE_A;
    ARecord.wDataLength = sizeof(DNS_A_DATA);
    ARecord.Flags.S.Section = DnsSectionAnswer;
    ARecord.Flags.S.CharSet = DnsCharSetUnicode;
    ARecord.dwTtl = 86400;

    ARecord.Data.A.IpAddress = pAddress->S_un.S_addr;

    /* Prepare the PTR record */
    _swprintf(szReverseName,
              L"%u.%u.%u.%u.in-addr.arpa.",
              pAddress->S_un.S_un_b.s_b4,
              pAddress->S_un.S_un_b.s_b3,
              pAddress->S_un.S_un_b.s_b2,
              pAddress->S_un.S_un_b.s_b1);

    ZeroMemory(&PtrRecord, sizeof(DNS_RECORDW));

    PtrRecord.pName = szReverseName;
    PtrRecord.wType = DNS_TYPE_PTR;
    PtrRecord.wDataLength = sizeof(DNS_PTR_DATA);
    PtrRecord.Flags.S.Section = DnsSectionAnswer;
    PtrRecord.Flags.S.CharSet = DnsCharSetUnicode;
    PtrRecord.dwTtl = 86400;

    PtrRecord.Data.PTR.pNameHost = pszHostName;

    DnsIntCacheAddEntry(ARecord.pName, DNS_TYPE_A, &ARecord, TRUE);
    DnsIntCacheAddEntry(PtrRecord.pName, DNS_TYPE_PTR, &PtrRecord, TRUE);
}


static
VOID
AddIpv6HostEntries(
    PWSTR pszHostName,
    PIN6_ADDR pAddress)
{
    DNS_RECORDW AAAARecord, PtrRecord;
    WCHAR szReverseName[80];
    DWORD i, j, k;

    /* Prepare the AAAA record */
    ZeroMemory(&AAAARecord, sizeof(DNS_RECORDW));

    AAAARecord.pName = pszHostName;
    AAAARecord.wType = DNS_TYPE_AAAA;
    AAAARecord.wDataLength = sizeof(DNS_AAAA_DATA);
    AAAARecord.Flags.S.Section = DnsSectionAnswer;
    AAAARecord.Flags.S.CharSet = DnsCharSetUnicode;
    AAAARecord.dwTtl = 86400;

    CopyMemory(&AAAARecord.Data.AAAA.Ip6Address,
               &pAddress->u.Byte,
               sizeof(IN6_ADDR));

    /* Prepare the PTR record */
    ZeroMemory(szReverseName, sizeof(szReverseName));

    for (i = 0; i < sizeof(IN6_ADDR); i++)
    {
        j = 4 * i;
        k = sizeof(IN6_ADDR) - 1 - i;
        szReverseName[j] = szHexChar[pAddress->u.Byte[k] & 0xF];
        szReverseName[j + 1] = L'.';
        szReverseName[j + 2] = szHexChar[(pAddress->u.Byte[k] >> 4) & 0xF];
        szReverseName[j + 3] = L'.';
    }
    wcscat(szReverseName, L"ip6.arpa.");

    ZeroMemory(&PtrRecord, sizeof(DNS_RECORDW));

    PtrRecord.pName = szReverseName;
    PtrRecord.wType = DNS_TYPE_PTR;
    PtrRecord.wDataLength = sizeof(DNS_PTR_DATA);
    PtrRecord.Flags.S.Section = DnsSectionAnswer;
    PtrRecord.Flags.S.CharSet = DnsCharSetUnicode;
    PtrRecord.dwTtl = 86400;

    PtrRecord.Data.PTR.pNameHost = pszHostName;

    DnsIntCacheAddEntry(AAAARecord.pName, DNS_TYPE_AAAA, &AAAARecord, TRUE);
    DnsIntCacheAddEntry(PtrRecord.pName, DNS_TYPE_PTR, &PtrRecord, TRUE);
}


/* Returns the full path of the hosts file in a process heap block, or NULL */
static
PWSTR
BuildHostsPath(VOID)
{
    PWSTR ExpandedPath;
    PWSTR DatabasePath;
    HKEY DatabaseKey;
    DWORD RegSize = 0;
    size_t StringLength;
    DWORD dwError;

    ExpandedPath = HeapAlloc(GetProcessHeap(), 0, MAX_PATH * sizeof(WCHAR));
    if (ExpandedPath == NULL)
        return NULL;

    /* Open the database path key */
    dwError = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                            L"System\\CurrentControlSet\\Services\\Tcpip\\Parameters",
                            0,
                            KEY_READ,
                            &DatabaseKey);
    if (dwError == ERROR_SUCCESS)
    {
        /* Read the actual path */
        RegQueryValueExW(DatabaseKey,
                         L"DatabasePath",
                         NULL,
                         NULL,
                         NULL,
                         &RegSize);

        DatabasePath = HeapAlloc(GetProcessHeap(), 0, RegSize);
        if (DatabasePath == NULL)
        {
            HeapFree(GetProcessHeap(), 0, ExpandedPath);
            RegCloseKey(DatabaseKey);
            return NULL;
        }

        /* Read the actual path */
        dwError = RegQueryValueExW(DatabaseKey,
                                   L"DatabasePath",
                                   NULL,
                                   NULL,
                                   (LPBYTE)DatabasePath,
                                   &RegSize);

        /* Close the key */
        RegCloseKey(DatabaseKey);

        if (dwError != ERROR_SUCCESS)
        {
            HeapFree(GetProcessHeap(), 0, DatabasePath);
            HeapFree(GetProcessHeap(), 0, ExpandedPath);
            return NULL;
        }

        /* Expand the name */
        ExpandEnvironmentStringsW(DatabasePath, ExpandedPath, MAX_PATH);

        HeapFree(GetProcessHeap(), 0, DatabasePath);
    }
    else
    {
        /* Use defalt path */
        GetSystemDirectoryW(ExpandedPath, MAX_PATH);

        StringCchLengthW(ExpandedPath, MAX_PATH, &StringLength);
        if (ExpandedPath[StringLength - 1] != L'\\')
        {
            /* It isn't, so add it ourselves */
            StringCchCatW(ExpandedPath, MAX_PATH, L"\\");
        }

        StringCchCatW(ExpandedPath, MAX_PATH, L"drivers\\etc\\");
    }

    /* Make sure that the path is backslash-terminated */
    StringCchLengthW(ExpandedPath, MAX_PATH, &StringLength);
    if (ExpandedPath[StringLength - 1] != L'\\')
    {
        /* It isn't, so add it ourselves */
        StringCchCatW(ExpandedPath, MAX_PATH, L"\\");
    }

    /* Add the database name */
    StringCchCatW(ExpandedPath, MAX_PATH, L"hosts");

    return ExpandedPath;
}

static
FILE *
OpenHostsFile(VOID)
{
    FILE *pHostsFile;
    PWSTR Path = BuildHostsPath();

    if (Path == NULL)
        return NULL;

    pHostsFile = _wfopen(Path, L"r");
    HeapFree(GetProcessHeap(), 0, Path);

    return pHostsFile;
}


BOOL
ReadHostsFile(VOID)
{
    CHAR szLineBuffer[512];
    FILE *pHostFile = NULL;
    CHAR *Ptr, *NameStart, *NameEnd, *AddressStart, *AddressEnd;
    struct in_addr Ipv4Address;
    struct in6_addr Ipv6Address;
    PWSTR pszHostName;

    pHostFile = OpenHostsFile();
    if (pHostFile == NULL)
        return FALSE;

    for (;;)
    {
        /* Read a line */
        if (fgets(szLineBuffer, sizeof(szLineBuffer), pHostFile) == NULL)
            break;

        NameStart = NameEnd = NULL;
        AddressStart = AddressEnd = NULL;

        /* Search for the start of the ip address */
        Ptr = szLineBuffer;
        for (;;)
        {
            if (*Ptr == 0 || *Ptr == '#')
                break;

            if (!isspace(*Ptr))
            {
                AddressStart = Ptr;
                Ptr = Ptr + 1;
                break;
            }

            Ptr = Ptr + 1;
        }

        /* Search for the end of the ip address */
        for (;;)
        {
            if (*Ptr == 0 || *Ptr == '#')
                break;

            if (isspace(*Ptr))
            {
                AddressEnd = Ptr;
                Ptr = Ptr + 1;
                break;
            }

            Ptr = Ptr + 1;
        }

        /* Search for the start of the name */
        for (;;)
        {
            if (*Ptr == 0 || *Ptr == '#')
                break;

            if (!isspace(*Ptr))
            {
                NameStart = Ptr;
                Ptr = Ptr + 1;
                break;
            }

            Ptr = Ptr + 1;
        }

        /* Search for the end of the name */
        for (;;)
        {
            if (*Ptr == 0 || *Ptr == '#')
            {
                /* the last line of the file may end right after the name */
                NameEnd = Ptr;
                break;
            }

            if (isspace(*Ptr))
            {
                NameEnd = Ptr;
                break;
            }

            Ptr = Ptr + 1;
        }

        if (AddressStart == NULL || AddressEnd == NULL ||
            NameStart == NULL || NameEnd == NULL)
            continue;

        *AddressEnd = 0;
        *NameEnd = 0;

        DPRINT("%s ==> %s\n", NameStart, AddressStart);

        if (ParseIpv4Address(AddressStart, &Ipv4Address))
        {
            DPRINT("IPv4: %s\n", AddressStart);

            pszHostName = AnsiToUnicode(NameStart);
            if (pszHostName != NULL)
            {
                AddIpv4HostEntries(pszHostName, &Ipv4Address);
                HeapFree(GetProcessHeap(), 0, pszHostName);
            }
        }
        else if (ParseIpv6Address(AddressStart, &Ipv6Address))
        {
            DPRINT("IPv6: %s\n", AddressStart);

            pszHostName = AnsiToUnicode(NameStart);
            if (pszHostName != NULL)
            {
                AddIpv6HostEntries(pszHostName, &Ipv6Address);
                HeapFree(GetProcessHeap(), 0, pszHostName);
            }
        }
    }

    fclose(pHostFile);

    return TRUE;
}


/* Reload and watch ***********************************************************/

static CRITICAL_SECTION HostsLock;
static BOOL HostsLockInitialized = FALSE;
static HANDLE HostsStopEvent = NULL;
static HANDLE HostsWatchThread = NULL;

/* What the file looked like when it was last read */
static BOOL HostsStampValid = FALSE;
static BOOL HostsStampExists = FALSE;
static FILETIME HostsStampTime;
static DWORD HostsStampSize;

static
BOOL
GetHostsStamp(
    _Out_ PFILETIME Time,
    _Out_ PDWORD Size)
{
    WIN32_FILE_ATTRIBUTE_DATA Data;
    PWSTR Path = BuildHostsPath();
    BOOL Exists;

    Time->dwLowDateTime = Time->dwHighDateTime = 0;
    *Size = 0;
    if (Path == NULL)
        return FALSE;

    Exists = GetFileAttributesExW(Path, GetFileExInfoStandard, &Data);
    HeapFree(GetProcessHeap(), 0, Path);
    if (Exists)
    {
        *Time = Data.ftLastWriteTime;
        *Size = Data.nFileSizeLow;
    }
    return Exists;
}

/* Caller holds HostsLock */
static
VOID
ReloadHostsFileLocked(VOID)
{
    /* Stamp first: an edit that lands while the file is read is seen again by the next check */
    HostsStampExists = GetHostsStamp(&HostsStampTime, &HostsStampSize);
    HostsStampValid = TRUE;

    DnsIntCacheFlush(CACHE_FLUSH_ALL);
    ReadHostsFile();
}

/*
 * Forget everything that was learned from the hosts file (and every answer that
 * DnsQuery got from it through dnsapi), then read the file again. Like the DNS
 * Client service on Windows, so that a removed line never survives in the cache.
 */
VOID
ReloadHostsFile(VOID)
{
    if (!HostsLockInitialized)
        return;

    EnterCriticalSection(&HostsLock);
    ReloadHostsFileLocked();
    LeaveCriticalSection(&HostsLock);
}

/*
 * Called before a query is answered from the cache. The change watcher reloads
 * the file about 100 ms after an edit; in that window a cached answer from DNS
 * would still win over a line that was just added. Look at the file's size and
 * time stamp (one cheap call) and reload at once when they differ from the last
 * read, so that the answer never depends on who is faster, the watcher or the query.
 */
VOID
HostsRefreshIfChanged(VOID)
{
    FILETIME Time;
    DWORD Size;
    BOOL Exists;

    if (!HostsLockInitialized)
        return;

    Exists = GetHostsStamp(&Time, &Size);

    EnterCriticalSection(&HostsLock);
    if (!HostsStampValid ||
        Exists != HostsStampExists ||
        (Exists && (Size != HostsStampSize || CompareFileTime(&Time, &HostsStampTime) != 0)))
    {
        ReloadHostsFileLocked();
    }
    LeaveCriticalSection(&HostsLock);
}

static
DWORD
WINAPI
HostsWatchRoutine(LPVOID lpParameter)
{
    HANDLE Handles[2];
    PWSTR Path;
    size_t Length;
    HANDLE Change = INVALID_HANDLE_VALUE;
    DWORD Wait;

    UNREFERENCED_PARAMETER(lpParameter);

    Handles[0] = HostsStopEvent;

    Path = BuildHostsPath();
    if (Path != NULL)
    {
        /* Cut the file name: watch the directory, the file may be replaced or recreated */
        Length = wcslen(Path);
        while (Length > 0 && Path[Length - 1] != L'\\')
            Length--;
        if (Length > 0)
            Path[Length - 1] = UNICODE_NULL;

        Change = FindFirstChangeNotificationW(Path,
                                              FALSE,
                                              FILE_NOTIFY_CHANGE_FILE_NAME |
                                              FILE_NOTIFY_CHANGE_SIZE |
                                              FILE_NOTIFY_CHANGE_LAST_WRITE);
        HeapFree(GetProcessHeap(), 0, Path);
    }

    if (Change == INVALID_HANDLE_VALUE)
    {
        DPRINT1("DNSRSLVR: cannot watch the hosts directory (%lu)\n", GetLastError());
        return 1;
    }
    Handles[1] = Change;

    for (;;)
    {
        Wait = WaitForMultipleObjects(2, Handles, FALSE, INFINITE);
        if (Wait != WAIT_OBJECT_0 + 1)
            break;

        /* An editor writes in several steps: let it finish, unless we are asked to stop */
        if (WaitForSingleObject(HostsStopEvent, 100) == WAIT_OBJECT_0)
            break;

        FindNextChangeNotification(Change);
        ReloadHostsFile();
    }

    FindCloseChangeNotification(Change);
    return 0;
}

BOOL
HostsWatchStart(VOID)
{
    if (HostsWatchThread != NULL)
        return TRUE;

    InitializeCriticalSection(&HostsLock);
    HostsLockInitialized = TRUE;

    HostsStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (HostsStopEvent != NULL)
        HostsWatchThread = CreateThread(NULL, 0, HostsWatchRoutine, NULL, 0, NULL);

    if (HostsWatchThread == NULL)
    {
        if (HostsStopEvent != NULL)
            CloseHandle(HostsStopEvent);
        HostsStopEvent = NULL;
        return FALSE;
    }

    return TRUE;
}

VOID
HostsWatchStop(VOID)
{
    if (HostsWatchThread == NULL)
        return;

    SetEvent(HostsStopEvent);
    WaitForSingleObject(HostsWatchThread, 5000);
    CloseHandle(HostsWatchThread);
    CloseHandle(HostsStopEvent);
    HostsWatchThread = NULL;
    HostsStopEvent = NULL;

    HostsLockInitialized = FALSE;
    DeleteCriticalSection(&HostsLock);
}

/* EOF */
