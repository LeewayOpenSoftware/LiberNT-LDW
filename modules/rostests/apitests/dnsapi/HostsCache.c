/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         A hosts line added for a name that is already in the DNS cache wins at once,
 *                  for DnsQuery_W and for getaddrinfo, without any network
 *
 * The test points the resolver at a responder of its own on 127.0.0.1:53 (global NameServer value
 * of Tcpip\Parameters), resolves a name through it so that the DNS Client service caches the answer,
 * then adds a hosts line for the same name and asks again with no waiting.
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <windns.h>
#include <windns_undoc.h>
#include <winreg.h>
#include <apitest.h>

#define TEST_NAME       L"seed.hostscache.test"
#define TEST_NAME_A     "seed.hostscache.test"
#define PARAMS_KEY      L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters"
#define IP4(a, b, c, d) ((DWORD)((a) | ((b) << 8) | ((c) << 16) | ((DWORD)(d) << 24)))
#define DNS_ADDR        IP4(192, 0, 2, 55)
#define HOSTS_ADDR      IP4(198, 51, 100, 77)

static WCHAR g_HostsPath[MAX_PATH];
static BYTE *g_Saved;
static DWORD g_SavedSize;
static BOOL g_HadFile;

static volatile LONG g_Stop;
static volatile LONG g_AQueries;
static SOCKET g_Sock = INVALID_SOCKET;

static BOOL SaveHosts(void)
{
    HANDLE File;

    if (!GetSystemDirectoryW(g_HostsPath, ARRAYSIZE(g_HostsPath)))
        return FALSE;
    lstrcatW(g_HostsPath, L"\\drivers\\etc\\hosts");

    File = CreateFileW(g_HostsPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (File != INVALID_HANDLE_VALUE)
    {
        DWORD Size = GetFileSize(File, NULL), Read = 0;

        g_Saved = HeapAlloc(GetProcessHeap(), 0, Size ? Size : 1);
        if (!g_Saved || !ReadFile(File, g_Saved, Size, &Read, NULL) || Read != Size)
        {
            CloseHandle(File);
            return FALSE;
        }
        g_SavedSize = Size;
        g_HadFile = TRUE;
        CloseHandle(File);
    }
    return TRUE;
}

static void RestoreHosts(void)
{
    if (g_HadFile)
    {
        HANDLE File = CreateFileW(g_HostsPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);

        if (File != INVALID_HANDLE_VALUE)
        {
            DWORD Written;

            WriteFile(File, g_Saved, g_SavedSize, &Written, NULL);
            CloseHandle(File);
        }
    }
    else
        DeleteFileW(g_HostsPath);
}

static BOOL WriteHosts(const char *Text)
{
    HANDLE File = CreateFileW(g_HostsPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD Written;
    BOOL Ok;

    if (File == INVALID_HANDLE_VALUE)
        return FALSE;
    Ok = WriteFile(File, Text, (DWORD)strlen(Text), &Written, NULL);
    CloseHandle(File);
    return Ok;
}

/* A responder that answers every A question with DNS_ADDR (TTL 300) and every other type with no data */
static DWORD WINAPI ResponderThread(LPVOID Param)
{
    UCHAR Packet[512];
    UCHAR Reply[512 + 16];

    UNREFERENCED_PARAMETER(Param);

    while (!g_Stop)
    {
        fd_set Set;
        struct timeval Tv = { 0, 100000 };
        struct sockaddr_in From;
        int FromLen = sizeof(From), Len, Pos;
        WORD Type;

        FD_ZERO(&Set);
        FD_SET(g_Sock, &Set);
        if (select(0, &Set, NULL, NULL, &Tv) <= 0)
            continue;
        Len = recvfrom(g_Sock, (char *)Packet, sizeof(Packet), 0, (struct sockaddr *)&From, &FromLen);
        if (Len < 17)
            continue;

        /* skip the question name */
        Pos = 12;
        while (Pos < Len && Packet[Pos] != 0)
            Pos += Packet[Pos] + 1;
        Pos++;
        if (Pos + 4 > Len)
            continue;
        Type = (WORD)((Packet[Pos] << 8) | Packet[Pos + 1]);
        Pos += 4;

        memcpy(Reply, Packet, Pos);
        Reply[2] = 0x81;                /* response, recursion desired */
        Reply[3] = 0x80;                /* recursion available, no error */
        Reply[4] = 0; Reply[5] = 1;     /* one question */
        Reply[6] = 0; Reply[7] = 0;
        Reply[8] = Reply[9] = Reply[10] = Reply[11] = 0;
        if (Type == DNS_TYPE_A)
        {
            InterlockedIncrement(&g_AQueries);
            Reply[7] = 1;
            Reply[Pos++] = 0xC0; Reply[Pos++] = 0x0C;       /* name: pointer to the question */
            Reply[Pos++] = 0; Reply[Pos++] = 1;             /* A */
            Reply[Pos++] = 0; Reply[Pos++] = 1;             /* IN */
            Reply[Pos++] = 0; Reply[Pos++] = 0; Reply[Pos++] = 1; Reply[Pos++] = 0x2C; /* TTL 300 */
            Reply[Pos++] = 0; Reply[Pos++] = 4;
            Reply[Pos++] = 192; Reply[Pos++] = 0; Reply[Pos++] = 2; Reply[Pos++] = 55;
        }
        sendto(g_Sock, (char *)Reply, Pos, 0, (struct sockaddr *)&From, FromLen);
    }
    return 0;
}

static DNS_STATUS QueryA(LPCWSTR Name, DWORD *Ip4)
{
    PDNS_RECORD Record = NULL;
    DNS_STATUS Status = DnsQuery_W(Name, DNS_TYPE_A, DNS_QUERY_STANDARD, NULL, &Record, NULL);

    *Ip4 = 0;
    if (Status == ERROR_SUCCESS && Record)
    {
        *Ip4 = Record->Data.A.IpAddress;
        DnsRecordListFree(Record, DnsFreeRecordList);
    }
    return Status;
}

/* getaddrinfo, first IPv4 address of the answer (0 when there is none) */
static int GaiA(const char *Name, DWORD *Ip4)
{
    struct addrinfo Hints, *Result = NULL, *Cur;
    int Error;

    *Ip4 = 0;
    ZeroMemory(&Hints, sizeof(Hints));
    Hints.ai_family = AF_INET;
    Hints.ai_socktype = SOCK_STREAM;
    Error = getaddrinfo(Name, NULL, &Hints, &Result);
    if (Error == 0)
    {
        for (Cur = Result; Cur; Cur = Cur->ai_next)
        {
            if (Cur->ai_family == AF_INET)
            {
                *Ip4 = ((struct sockaddr_in *)Cur->ai_addr)->sin_addr.S_un.S_addr;
                break;
            }
        }
        freeaddrinfo(Result);
    }
    return Error;
}

/* ask until the answer is Expected or 2 seconds have passed */
static DWORD WaitForA(LPCWSTR Name, DWORD Expected, DWORD *Elapsed)
{
    DWORD Start = GetTickCount(), Ip4;

    for (;;)
    {
        QueryA(Name, &Ip4);
        *Elapsed = GetTickCount() - Start;
        if (Ip4 == Expected || *Elapsed >= 2000)
            return Ip4;
        Sleep(50);
    }
}

START_TEST(HostsCache)
{
    WSADATA Wsa;
    struct sockaddr_in Local;
    HANDLE Thread;
    HKEY Key;
    WCHAR OldServers[256];
    DWORD OldSize = sizeof(OldServers) - sizeof(WCHAR), OldType = 0, Elapsed, Ip4;
    BOOL HadServers = FALSE, SetServers = FALSE;
    LONG Cached;
    int Error;

    if (WSAStartup(MAKEWORD(2, 2), &Wsa) != 0)
    {
        skip("WSAStartup failed\n");
        return;
    }
    g_Sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ZeroMemory(&Local, sizeof(Local));
    Local.sin_family = AF_INET;
    Local.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK);
    Local.sin_port = htons(53);
    if (g_Sock == INVALID_SOCKET || bind(g_Sock, (struct sockaddr *)&Local, sizeof(Local)) != 0)
    {
        skip("cannot bind 127.0.0.1:53 (%d)\n", WSAGetLastError());
        goto done;
    }
    if (!SaveHosts() || !WriteHosts("# hosts cache apitest\r\n"))
    {
        skip("The hosts file is not writable\n");
        goto done;
    }

    /* point the resolver at the responder */
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, PARAMS_KEY, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &Key) != ERROR_SUCCESS)
    {
        skip("cannot open the Tcpip parameters\n");
        RestoreHosts();
        goto done;
    }
    ZeroMemory(OldServers, sizeof(OldServers));
    HadServers = RegQueryValueExW(Key, L"NameServer", NULL, &OldType, (PBYTE)OldServers, &OldSize) == ERROR_SUCCESS &&
                 OldType == REG_SZ;
    SetServers = RegSetValueExW(Key, L"NameServer", 0, REG_SZ, (const BYTE *)L"127.0.0.1", sizeof(L"127.0.0.1")) ==
                 ERROR_SUCCESS;
    if (!SetServers)
    {
        skip("cannot set the NameServer value\n");
        RegCloseKey(Key);
        RestoreHosts();
        goto done;
    }

    Thread = CreateThread(NULL, 0, ResponderThread, NULL, 0, NULL);
    DnsFlushResolverCache();

    /* 1. DNS answer, cached by the DNS Client service */
    Error = QueryA(TEST_NAME, &Ip4);
    ok(Error == ERROR_SUCCESS && Ip4 == DNS_ADDR, "seed: status %d address 0x%08lx\n", Error, Ip4);
    QueryA(TEST_NAME, &Ip4);
    Cached = g_AQueries;
    trace("responder saw %ld A queries for two lookups (%s)\n", Cached, Cached == 1 ? "cached by Dnscache" : "no cache");
    Error = GaiA(TEST_NAME_A, &Ip4);
    ok(Error == 0 && Ip4 == DNS_ADDR, "getaddrinfo seed: error %d address 0x%08lx\n", Error, Ip4);

    /* 2. a hosts line for the same name: the very next question must get it, no waiting */
    ok(WriteHosts("198.51.100.77 " TEST_NAME_A "\r\n"), "write failed\n");
    Error = QueryA(TEST_NAME, &Ip4);
    ok(Error == ERROR_SUCCESS && Ip4 == HOSTS_ADDR, "DnsQuery right after the edit: status %d address 0x%08lx\n", Error, Ip4);
    Error = GaiA(TEST_NAME_A, &Ip4);
    ok(Error == 0 && Ip4 == HOSTS_ADDR, "getaddrinfo right after the edit: error %d address 0x%08lx\n", Error, Ip4);
    Ip4 = WaitForA(TEST_NAME, HOSTS_ADDR, &Elapsed);
    ok(Ip4 == HOSTS_ADDR && Elapsed < 2000, "still not the hosts address after %lu ms\n", Elapsed);

    /* 3. the line is removed: DNS answers again */
    ok(WriteHosts("# empty\r\n"), "write failed\n");
    Ip4 = WaitForA(TEST_NAME, DNS_ADDR, &Elapsed);
    ok(Ip4 == DNS_ADDR, "after removal address 0x%08lx after %lu ms\n", Ip4, Elapsed);
    Error = GaiA(TEST_NAME_A, &Ip4);
    ok(Error == 0 && Ip4 == DNS_ADDR, "getaddrinfo after removal: error %d address 0x%08lx\n", Error, Ip4);

    g_Stop = 1;
    WaitForSingleObject(Thread, 5000);
    CloseHandle(Thread);
    if (HadServers)
        RegSetValueExW(Key, L"NameServer", 0, REG_SZ, (const BYTE *)OldServers, (lstrlenW(OldServers) + 1) * sizeof(WCHAR));
    else
        RegDeleteValueW(Key, L"NameServer");
    RegCloseKey(Key);
    RestoreHosts();
    DnsFlushResolverCache();

done:
    if (g_Sock != INVALID_SOCKET)
        closesocket(g_Sock);
    WSACleanup();
}
