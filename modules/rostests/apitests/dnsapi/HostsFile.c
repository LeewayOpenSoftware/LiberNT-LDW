/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         DnsQuery_W answers names listed in the hosts file before asking DNS
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <windns.h>
#include <windns_undoc.h>
#include <apitest.h>

static WCHAR g_HostsPath[MAX_PATH];
static BYTE *g_Saved;
static DWORD g_SavedSize;
static BOOL g_HadFile;

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

/* returns the status, and the first address of the answer in *Ip4 when there is one */
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

static DNS_STATUS QueryAAAA(LPCWSTR Name, IP6_ADDRESS *Ip6)
{
    PDNS_RECORD Record = NULL;
    DNS_STATUS Status = DnsQuery_W(Name, DNS_TYPE_AAAA, DNS_QUERY_STANDARD, NULL, &Record, NULL);

    RtlZeroMemory(Ip6, sizeof(*Ip6));
    if (Status == ERROR_SUCCESS && Record)
    {
        *Ip6 = Record->Data.AAAA.Ip6Address;
        DnsRecordListFree(Record, DnsFreeRecordList);
    }
    return Status;
}

#define IP4(a, b, c, d) ((DWORD)((a) | ((b) << 8) | ((c) << 16) | ((DWORD)(d) << 24)))

/* Ask until the answer is Expected (0 = the name must not resolve) or 2 seconds have passed */
static DNS_STATUS WaitForA(LPCWSTR Name, DWORD Expected, DWORD *Ip4, DWORD *Elapsed)
{
    DWORD Start = GetTickCount();
    DNS_STATUS Status;

    for (;;)
    {
        Status = QueryA(Name, Ip4);
        *Elapsed = GetTickCount() - Start;
        if (*Ip4 == Expected || *Elapsed >= 2000)
            return Status;
        Sleep(50);
    }
}

START_TEST(HostsFile)
{
    DNS_STATUS Status;
    DWORD Ip4;
    IP6_ADDRESS Ip6;
    DWORD Start, Elapsed;
    BOOL Flushed;

    if (!SaveHosts() || !WriteHosts("# hosts apitest\r\n"))
    {
        skip("The hosts file is not writable\n");
        return;
    }

    /* comments, blank lines, tabs, aliases, mixed case; "ordinary" lines first */
    ok(WriteHosts("# comment line\r\n"
                  "\r\n"
                  "10.77.0.1\thosta.apitest   alias1.apitest\r\n"
                  "10.77.0.2 hostb.apitest # trailing comment hostc.apitest\r\n"
                  "   10.77.0.3    hostd.apitest\r\n"
                  "fec0::77 six.apitest\r\n"
                  "10.77.0.4 dual.apitest\r\n"
                  "fec0::78 dual.apitest\r\n"
                  "10.77.0.5 last.apitest"), "write failed\n");

    Status = QueryA(L"hosta.apitest", &Ip4);
    ok(Status == ERROR_SUCCESS, "hosta: status %lu\n", Status);
    ok(Ip4 == IP4(10, 77, 0, 1), "hosta: address 0x%08lx\n", Ip4);

    Status = QueryA(L"alias1.apitest", &Ip4);
    ok(Status == ERROR_SUCCESS && Ip4 == IP4(10, 77, 0, 1), "alias: status %lu address 0x%08lx\n", Status, Ip4);

    Status = QueryA(L"HOSTA.ApiTest", &Ip4);
    ok(Status == ERROR_SUCCESS && Ip4 == IP4(10, 77, 0, 1), "case: status %lu address 0x%08lx\n", Status, Ip4);

    Status = QueryA(L"hosta.apitest.", &Ip4);
    ok(Status == ERROR_SUCCESS && Ip4 == IP4(10, 77, 0, 1), "trailing dot: status %lu address 0x%08lx\n", Status, Ip4);

    Status = QueryA(L"hostb.apitest", &Ip4);
    ok(Status == ERROR_SUCCESS && Ip4 == IP4(10, 77, 0, 2), "comment: status %lu address 0x%08lx\n", Status, Ip4);

    /* a name that only appears after '#' is not an alias; it goes to DNS and must not resolve to the hosts address */
    Status = QueryA(L"hostc.apitest", &Ip4);
    ok(Ip4 != IP4(10, 77, 0, 2), "a commented name resolved to the line address\n");

    Status = QueryA(L"hostd.apitest", &Ip4);
    ok(Status == ERROR_SUCCESS && Ip4 == IP4(10, 77, 0, 3), "leading blanks: status %lu address 0x%08lx\n", Status, Ip4);

    Status = QueryA(L"last.apitest", &Ip4);
    ok(Status == ERROR_SUCCESS && Ip4 == IP4(10, 77, 0, 5), "no final line break: status %lu address 0x%08lx\n", Status, Ip4);

    Status = QueryAAAA(L"six.apitest", &Ip6);
    ok(Status == ERROR_SUCCESS, "six AAAA: status %lu\n", Status);
    ok(Ip6.IP6Dword[0] == 0xc0fe && Ip6.IP6Dword[3] == 0x77000000, "six AAAA: dwords 0x%08lx 0x%08lx\n", Ip6.IP6Dword[0], Ip6.IP6Dword[3]);

    /* listed without an address of the requested family: answer at once, do not wait for DNS */
    Start = GetTickCount();
    Status = QueryA(L"six.apitest", &Ip4);
    ok(Status != ERROR_SUCCESS && Ip4 == 0, "six A: status %lu address 0x%08lx\n", Status, Ip4);
    ok(GetTickCount() - Start < 2000, "six A took %lu ms\n", GetTickCount() - Start);

    /* both families of one name */
    Status = QueryA(L"dual.apitest", &Ip4);
    ok(Status == ERROR_SUCCESS && Ip4 == IP4(10, 77, 0, 4), "dual A: status %lu address 0x%08lx\n", Status, Ip4);
    Status = QueryAAAA(L"dual.apitest", &Ip6);
    ok(Status == ERROR_SUCCESS && Ip6.IP6Dword[0] == 0xc0fe, "dual AAAA: status %lu\n", Status);

    /* hosts precedence over DNS for a name DNS cannot know, and re-read on change: edit and ask again */
    /* The DNS Client service (Dnscache) may be running and caching: it has to notice the change by itself
     * within about a second, as on Windows */
    ok(WriteHosts("10.77.0.9 hosta.apitest\r\n"), "rewrite failed\n");
    Status = WaitForA(L"hosta.apitest", IP4(10, 77, 0, 9), &Ip4, &Elapsed);
    ok(Status == ERROR_SUCCESS && Ip4 == IP4(10, 77, 0, 9), "after edit: status %lu address 0x%08lx after %lu ms\n", Status, Ip4, Elapsed);

    ok(WriteHosts("# empty again\r\n"), "rewrite failed\n");
    Status = WaitForA(L"hosta.apitest", 0, &Ip4, &Elapsed);
    ok(Status != ERROR_SUCCESS && Ip4 == 0, "after removal the name still resolves: status %lu address 0x%08lx after %lu ms\n", Status, Ip4, Elapsed);

    /* a second add/remove cycle, so that a stale answer cannot be a first-query artifact */
    ok(WriteHosts("10.77.0.10 cycle.apitest\r\n"), "rewrite failed\n");
    Status = WaitForA(L"cycle.apitest", IP4(10, 77, 0, 10), &Ip4, &Elapsed);
    ok(Status == ERROR_SUCCESS && Ip4 == IP4(10, 77, 0, 10), "cycle add: status %lu address 0x%08lx after %lu ms\n", Status, Ip4, Elapsed);
    ok(WriteHosts("# empty\r\n"), "rewrite failed\n");
    Status = WaitForA(L"cycle.apitest", 0, &Ip4, &Elapsed);
    ok(Status != ERROR_SUCCESS && Ip4 == 0, "cycle removal: status %lu address 0x%08lx after %lu ms\n", Status, Ip4, Elapsed);

    /* DnsFlushResolverCache reloads the hosts file at once, no waiting */
    /* The flush is an RPC call to the service: as on Windows it fails when the DNS Client service is
     * stopped, and then there is no cache to flush, the file is read by every query itself. */
    ok(WriteHosts("10.77.0.11 flush.apitest\r\n"), "rewrite failed\n");
    Flushed = DnsFlushResolverCache();
    trace("DnsFlushResolverCache returned %d (error %lu)\n", Flushed, Flushed ? 0 : GetLastError());
    Status = QueryA(L"flush.apitest", &Ip4);
    ok(Status == ERROR_SUCCESS && Ip4 == IP4(10, 77, 0, 11), "after flush: status %lu address 0x%08lx\n", Status, Ip4);
    ok(WriteHosts("# empty\r\n"), "rewrite failed\n");
    Flushed = DnsFlushResolverCache();
    Status = QueryA(L"flush.apitest", &Ip4);
    ok(Status != ERROR_SUCCESS && Ip4 == 0, "after flush and removal: status %lu address 0x%08lx\n", Status, Ip4);

    /* the answer carries the spelling of the question, also when another spelling was cached first */
    ok(WriteHosts("10.77.0.12 Mixed.apitest\r\n"), "rewrite failed\n");
    DnsFlushResolverCache();
    Status = WaitForA(L"MIXED.apitest", IP4(10, 77, 0, 12), &Ip4, &Elapsed);
    ok(Status == ERROR_SUCCESS && Ip4 == IP4(10, 77, 0, 12), "mixed case: status %lu address 0x%08lx\n", Status, Ip4);
    {
        PDNS_RECORD Record = NULL;

        Status = DnsQuery_W(L"mixed.apitest", DNS_TYPE_A, DNS_QUERY_STANDARD, NULL, &Record, NULL);
        ok(Status == ERROR_SUCCESS && Record, "lowercase: status %lu\n", Status);
        if (Status == ERROR_SUCCESS && Record)
        {
            /* DnsQuery_W: the name is wide whatever the UNICODE setting of this test says */
            PCWSTR Name = (PCWSTR)Record->pName;

            ok(lstrcmpW(Name, L"mixed.apitest") == 0, "answer name %ls, expected mixed.apitest\n", Name);
            DnsRecordListFree(Record, DnsFreeRecordList);
        }
    }
    ok(WriteHosts("# empty\r\n"), "rewrite failed\n");
    DnsFlushResolverCache();

    RestoreHosts();
}
