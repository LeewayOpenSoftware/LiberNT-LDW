/* Test Performance Counter Consumer - Milestone B independent consumer
   Reads HKEY_PERFORMANCE_DATA, decodes every counter (type, size, offset, value) and prints the value
   next to an independent reference collected with other APIs; also measures what a query costs. */
#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <windows.h>
#include <winperf.h>
#include <winternl.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QUERY L"2 4 238 230"
#define LOOPS 10
#define BURST 200
#define MAX_CPUS 64

typedef NTSTATUS (WINAPI *pNtQuerySystemInformation)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);
typedef NTSTATUS (WINAPI *pNtQueryInformationProcess)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);

/* VM_COUNTERS_EX as returned for ProcessVmCounters (class 3) */
typedef struct
{
    SIZE_T PeakVirtualSize, VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage, PeakPagefileUsage, PrivateUsage;
} VMC_EX;

/* Counter ids the providers report */
#define C_UPTIME 674
#define C_AVAIL_MB 6490
#define C_COMMITTED 6492
#define C_PROC_IDLE 6
#define C_PROCS 248
#define C_THREADS 250

/* ids seen in the data block, filled by decode() */
static DWORD g_ids[64];
static int g_idIsObject[64];
static int g_idCount;

static void note_id(DWORD id, int isObject)
{
    int i;

    for (i = 0; i < g_idCount; i++)
        if (g_ids[i] == id)
            return;
    if (g_idCount < 64)
    {
        g_ids[g_idCount] = id;
        g_idIsObject[g_idCount++] = isObject;
    }
}

struct decoded
{
    int have[8];
    ULONGLONG uptimeStart, idle, committed;
    DWORD availMB, procs, threads;
    LONGLONG sysPerfTime, sysPerfFreq;      /* PerfTime / PerfFreq of the System object */
    LONGLONG cpuPerfTime, cpuPerfFreq;      /* of the Processor object */
};

struct reference
{
    DWORD availMB, procs, threads, ncpu;
    ULONGLONG uptimeMs, kernel, user, idle;
    ULONGLONG committed;
};

static pNtQuerySystemInformation NtQSI;
static pNtQueryInformationProcess NtQIP;
static BYTE *buffer;
static DWORD bufSize = 8192;

static ULONGLONG ft_to_u64(FILETIME ft) { return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime; }

static void get_reference(struct reference *r)
{
    MEMORYSTATUSEX ms;
    SYSTEM_INFO si;
    HANDLE snap;
    PROCESSENTRY32 pe;
    THREADENTRY32 te;
    SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION perf[MAX_CPUS];
    ULONG ret = 0, i;
    PERFORMANCE_INFORMATION pi;

    memset(r, 0, sizeof(*r));
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    r->availMB = (DWORD)(ms.ullAvailPhys / (1024 * 1024));
    r->uptimeMs = GetTickCount64();
    GetSystemInfo(&si);
    r->ncpu = si.dwNumberOfProcessors;
    if (r->ncpu > MAX_CPUS)
        r->ncpu = MAX_CPUS;

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS | TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE)
    {
        pe.dwSize = sizeof(pe);
        if (Process32First(snap, &pe))
        {
            do { r->procs++; } while (Process32Next(snap, &pe));
        }
        te.dwSize = sizeof(te);
        if (Thread32First(snap, &te))
        {
            do { r->threads++; } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
    }

    if (NtQSI && NT_SUCCESS(NtQSI(SystemProcessorPerformanceInformation, perf, r->ncpu * sizeof(perf[0]), &ret)))
    {
        for (i = 0; i < ret / sizeof(perf[0]); i++)
        {
            r->kernel += perf[i].KernelTime.QuadPart;
            r->user += perf[i].UserTime.QuadPart;
            r->idle += perf[i].IdleTime.QuadPart;
        }
    }

    pi.cb = sizeof(pi);
    if (GetPerformanceInfo(&pi, sizeof(pi)))
        r->committed = (ULONGLONG)pi.CommitTotal * pi.PageSize;
}

static int query(DWORD *cb)
{
    int status;

    *cb = bufSize;
    status = RegQueryValueExW(HKEY_PERFORMANCE_DATA, QUERY, NULL, NULL, buffer, cb);
    while (status == ERROR_MORE_DATA && bufSize < 4 * 1024 * 1024)
    {
        BYTE *bigger = realloc(buffer, bufSize * 2);

        if (!bigger)
            break;
        buffer = bigger;
        bufSize *= 2;
        *cb = bufSize;
        status = RegQueryValueExW(HKEY_PERFORMANCE_DATA, QUERY, NULL, NULL, buffer, cb);
    }
    return status;
}

/* Walk the block with the documented offsets, print every counter, fill `d`. Returns 0 if well formed. */
static int decode(DWORD cb, struct decoded *d, int verbose)
{
    PERF_DATA_BLOCK *pdb = (PERF_DATA_BLOCK *)buffer;
    BYTE *end = buffer + cb;
    PERF_OBJECT_TYPE *pobj;
    int i;

    memset(d, 0, sizeof(*d));
    if (cb < sizeof(*pdb) || memcmp(pdb->Signature, L"PERF", 8) != 0)
        return 1;
    pobj = (PERF_OBJECT_TYPE *)(buffer + pdb->HeaderLength);

    for (i = 0; i < (int)pdb->NumObjectTypes; i++)
    {
        PERF_COUNTER_DEFINITION *def;
        PERF_COUNTER_BLOCK *blk;
        BYTE *objEnd;
        LONG c;

        if ((BYTE *)pobj + sizeof(*pobj) > end || pobj->TotalByteLength < sizeof(*pobj) ||
            (BYTE *)pobj + pobj->TotalByteLength > end)
            return 2;
        objEnd = (BYTE *)pobj + pobj->TotalByteLength;
        if (pobj->NumInstances != PERF_NO_INSTANCES)
        {
            printf("  OBJ %lu: has instances (%ld), not decoded\n", pobj->ObjectNameTitleIndex, pobj->NumInstances);
            pobj = (PERF_OBJECT_TYPE *)objEnd;
            continue;
        }
        if (pobj->DefinitionLength < pobj->HeaderLength ||
            (BYTE *)pobj + pobj->DefinitionLength + sizeof(PERF_COUNTER_BLOCK) > objEnd)
            return 3;
        blk = (PERF_COUNTER_BLOCK *)((BYTE *)pobj + pobj->DefinitionLength);
        if ((BYTE *)blk + blk->ByteLength > objEnd)
            return 4;

        note_id(pobj->ObjectNameTitleIndex, 1);
        if (pobj->ObjectNameTitleIndex == 2)
        {
            d->sysPerfTime = pobj->PerfTime.QuadPart;
            d->sysPerfFreq = pobj->PerfFreq.QuadPart;
        }
        if (pobj->ObjectNameTitleIndex == 238)
        {
            d->cpuPerfTime = pobj->PerfTime.QuadPart;
            d->cpuPerfFreq = pobj->PerfFreq.QuadPart;
        }

        def = (PERF_COUNTER_DEFINITION *)((BYTE *)pobj + pobj->HeaderLength);
        for (c = 0; c < pobj->NumCounters; c++)
        {
            ULONGLONG v = 0;
            BYTE *src;

            if ((BYTE *)def + sizeof(*def) > (BYTE *)blk || def->ByteLength < sizeof(*def))
                return 5;
            if (def->CounterOffset + def->CounterSize > blk->ByteLength || (def->CounterSize != 4 && def->CounterSize != 8))
                return 6;
            src = (BYTE *)blk + def->CounterOffset;
            if (def->CounterSize == 4)
            {
                DWORD x;
                memcpy(&x, src, 4);
                v = x;
            }
            else
                memcpy(&v, src, 8);
            note_id(def->CounterNameTitleIndex, 0);
            if (verbose)
                printf("  CTR obj=%lu id=%lu type=0x%08lx size=%lu offset=%lu value=%llu\n", pobj->ObjectNameTitleIndex,
                       def->CounterNameTitleIndex, def->CounterType, def->CounterSize, def->CounterOffset, v);
            switch (def->CounterNameTitleIndex)
            {
            case C_UPTIME: d->uptimeStart = v; d->have[0] = 1; break;
            case C_AVAIL_MB: d->availMB = (DWORD)v; d->have[1] = 1; break;
            case C_COMMITTED: d->committed = v; d->have[2] = 1; break;
            case C_PROC_IDLE: d->idle = v; d->have[3] = 1; break;
            case C_PROCS: d->procs = (DWORD)v; d->have[4] = 1; break;
            case C_THREADS: d->threads = (DWORD)v; d->have[5] = 1; break;
            }
            def = (PERF_COUNTER_DEFINITION *)((BYTE *)def + def->ByteLength);
        }
        pobj = (PERF_OBJECT_TYPE *)objEnd;
    }
    return 0;
}

static ULONGLONG proc_cpu_100ns(void)
{
    FILETIME c, e, k, u;

    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    return ft_to_u64(k) + ft_to_u64(u);
}

static void vm_counters(VMC_EX *v)
{
    ULONG ret = 0;

    memset(v, 0, sizeof(*v));
    if (NtQIP)
        NtQIP(GetCurrentProcess(), (PROCESSINFOCLASS)3, v, sizeof(*v), &ret);
}

/* Look up "index" in a REG_MULTI_SZ of index, text pairs; returns a pointer into the list or NULL */
static const WCHAR *find_text(const WCHAR *list, DWORD index)
{
    const WCHAR *p = list;

    while (*p)
    {
        const WCHAR *text = p + wcslen(p) + 1;

        if (!*text && !text[1])
            return NULL;
        if (wcstoul(p, NULL, 10) == index)
            return text;
        p = text + wcslen(text) + 1;
    }
    return NULL;
}

/* Read a REG_MULTI_SZ with a growing buffer; key == NULL reads HKEY_PERFORMANCE_DATA with `value` as the query */
static WCHAR *read_multisz(HKEY key, const WCHAR *value)
{
    DWORD cb = 8192, got, type;
    WCHAR *buf = NULL;
    LONG err;

    for (;;)
    {
        WCHAR *bigger = realloc(buf, cb + 8);

        if (!bigger)
        {
            free(buf);
            return NULL;
        }
        buf = bigger;
        got = cb;
        err = RegQueryValueExW(key ? key : HKEY_PERFORMANCE_DATA, value, NULL, &type, (BYTE *)buf, &got);
        if (err != ERROR_MORE_DATA || cb >= 4 * 1024 * 1024)
            break;
        cb *= 2;
    }
    if (err != ERROR_SUCCESS || type != REG_MULTI_SZ)
    {
        printf("  NAMES: read of %ls failed (error %ld, type %lu)\n", value, (long)err, type);
        free(buf);
        return NULL;
    }
    memset((BYTE *)buf + got, 0, 8);
    return buf;
}

static DWORD read_dword(HKEY root, const WCHAR *path, const WCHAR *value, int *found)
{
    HKEY key;
    DWORD v = 0, cb = sizeof(v), type;

    *found = 0;
    if (RegOpenKeyExW(root, path, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS)
    {
        if (RegQueryValueExW(key, value, NULL, &type, (BYTE *)&v, &cb) == ERROR_SUCCESS && type == REG_DWORD)
            *found = 1;
        RegCloseKey(key);
    }
    return v;
}

typedef LONG (WINAPI *pPdhLookup)(LPCWSTR, DWORD, LPWSTR, LPDWORD);

/* Resolve every object and counter id of the data block to a name and help text, three ways */
static void check_names(void)
{
    static const WCHAR perflib[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Perflib";
    WCHAR *names = read_multisz(NULL, L"Counter 009");
    WCHAR *helps = read_multisz(NULL, L"Help 009");
    WCHAR *regnames = NULL;
    HKEY key;
    HMODULE pdh = LoadLibraryW(L"pdh.dll");
    pPdhLookup lookup = pdh ? (pPdhLookup)GetProcAddress(pdh, "PdhLookupPerfNameByIndexW") : NULL;
    int found, i, bad = 0;
    DWORD lastCounter, lastHelp, fc, lc, fh, lh;

    printf("\nNAMES: resolving %d ids of the data block (objects and counters)\n", g_idCount);
    lastCounter = read_dword(HKEY_LOCAL_MACHINE, perflib, L"Last Counter", &found);
    printf("NAMES: Perflib Last Counter = %lu (%s)\n", lastCounter, found ? "present" : "ABSENT");
    lastHelp = read_dword(HKEY_LOCAL_MACHINE, perflib, L"Last Help", &found);
    printf("NAMES: Perflib Last Help = %lu (%s)\n", lastHelp, found ? "present" : "ABSENT");
    {
        static const WCHAR *const prov[] = { L"SYSTEM\\CurrentControlSet\\Services\\ServicesPerfOS\\Performance",
                                             L"SYSTEM\\CurrentControlSet\\Services\\ServicesPerfProc\\Performance" };
        for (i = 0; i < 2; i++)
        {
            int f1, f2, f3, f4;

            fc = read_dword(HKEY_LOCAL_MACHINE, prov[i], L"First Counter", &f1);
            lc = read_dword(HKEY_LOCAL_MACHINE, prov[i], L"Last Counter", &f2);
            fh = read_dword(HKEY_LOCAL_MACHINE, prov[i], L"First Help", &f3);
            lh = read_dword(HKEY_LOCAL_MACHINE, prov[i], L"Last Help", &f4);
            printf("NAMES: provider %d First/Last Counter %lu/%lu First/Last Help %lu/%lu (%s)\n", i, fc, lc, fh, lh,
                   f1 && f2 && f3 && f4 ? "present" : "ABSENT");
        }
    }
    /* the same text read as the Perflib\009 registry value (redirected to HKEY_PERFORMANCE_TEXT) */
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Perflib\\009", 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS)
    {
        regnames = read_multisz(key, L"Counter");
        RegCloseKey(key);
    }
    if (!names || !helps)
    {
        printf("NAMES: FAIL: no Counter 009 / Help 009 text from HKEY_PERFORMANCE_DATA\n");
        return;
    }
    for (i = 0; i < g_idCount; i++)
    {
        DWORD id = g_ids[i];
        const WCHAR *n = find_text(names, id), *h = find_text(helps, id + 1), *rn = regnames ? find_text(regnames, id) : NULL;
        WCHAR pdhName[128] = L"";
        DWORD pdhSize = 128;
        LONG pdhStatus = lookup ? lookup(NULL, id, pdhName, &pdhSize) : -1;

        printf("NAMES: %s id=%lu name=\"%ls\" help=\"%ls\" regvalue=%s pdh=%s%ls%s\n", g_idIsObject[i] ? "object " : "counter", id,
               n ? n : L"(none)", h ? h : L"(none)", rn ? (n && !wcscmp(rn, n) ? "same" : "DIFFERENT") : "(none)",
               pdhStatus == 0 ? "\"" : "status ", pdhStatus == 0 ? pdhName : L"", pdhStatus == 0 ? "\"" : "");
        if (!n || !h || id > lastCounter || id + 1 > lastHelp)
            bad++;
    }
    printf("NAMES: %d of %d ids unresolved or outside Last Counter/Last Help\n", bad, g_idCount);
    free(names);
    free(helps);
    free(regnames);
}

static void busy_wait_ms(DWORD ms)
{
    ULONGLONG t0 = GetTickCount64();
    volatile unsigned x = 0;

    while (GetTickCount64() - t0 < ms)
        x++;
}

int main(void)
{
    struct decoded d, prev, first;
    struct reference ref, prevref;
    LARGE_INTEGER qf, q0, q1;
    VMC_EX vm0, vm1, vm2;
    IO_COUNTERS io0, io1;
    ULONGLONG cpu0, cpu1, cpuLoopTotal = 0;
    DWORD cb;
    int loop, status, bad = 0, havePrev = 0;
    LONGLONG minAvail = 0x7fffffff, maxAvail = 0;
    ULONGLONG minUp = ~0ull, maxUp = 0;
    double minBusy = 1e9, maxBusy = -1e9;
    int idleChanged = 0, upChanged = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    buffer = malloc(bufSize);
    if (!buffer)
        return 1;
    NtQSI = (pNtQuerySystemInformation)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");
    NtQIP = (pNtQueryInformationProcess)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
    QueryPerformanceFrequency(&qf);

    printf("=== Test Performance Counter Consumer: values vs independent reference ===\n");
    printf("Query \"2 4 238 230\" on HKEY_PERFORMANCE_DATA, %d loops (loops 4-6 burn CPU), then a %d-query burst\n\n", LOOPS, BURST);

    vm_counters(&vm0);
    GetProcessIoCounters(GetCurrentProcess(), &io0);
    cpu0 = proc_cpu_100ns();
    memset(&prev, 0, sizeof(prev));
    memset(&prevref, 0, sizeof(prevref));

    for (loop = 0; loop < LOOPS; loop++)
    {
        ULONGLONG c0, c1;

        get_reference(&ref);
        c0 = proc_cpu_100ns();
        status = query(&cb);
        c1 = proc_cpu_100ns();
        cpuLoopTotal += c1 - c0;
        if (status != ERROR_SUCCESS)
        {
            printf("LOOP %d: query failed (%d)\n", loop, status);
            bad++;
            Sleep(1000);
            continue;
        }
        if (decode(cb, &d, 1) != 0)
        {
            printf("LOOP %d: block malformed (decode)\n", loop);
            bad++;
            Sleep(1000);
            continue;
        }
        printf("LOOP %d: %lu bytes, consumer CPU for this query %llu x100ns\n", loop, cb, c1 - c0);

        /* Available MBytes: counter vs GlobalMemoryStatusEx (taken just before the query) */
        printf("  CMP avail_mb: counter=%lu ref=%lu diff=%ld\n", d.availMB, ref.availMB, (long)d.availMB - (long)ref.availMB);
        /* Processes / threads: counter vs toolhelp snapshot */
        printf("  CMP procs: counter=%lu ref=%lu diff=%ld\n", d.procs, ref.procs, (long)d.procs - (long)ref.procs);
        printf("  CMP threads: counter=%lu ref=%lu diff=%ld\n", d.threads, ref.threads, (long)d.threads - (long)ref.threads);
        /* Committed bytes: GetPerformanceInfo (same OS source as the provider: checks the plumbing only) */
        printf("  CMP committed_bytes: counter=%llu ref=%llu\n", d.committed, ref.committed);
        /* Uptime: PERF_ELAPSED_TIME -> (PerfTime - start) / PerfFreq, vs GetTickCount64 */
        if (d.sysPerfFreq > 0)
        {
            double up = (double)(d.sysPerfTime - (LONGLONG)d.uptimeStart) / (double)d.sysPerfFreq;
            printf("  CMP uptime_s: counter=%.3f ref=%.3f diff=%.3f\n", up, ref.uptimeMs / 1000.0, up - ref.uptimeMs / 1000.0);
            if ((ULONGLONG)(up * 1000) < minUp) minUp = (ULONGLONG)(up * 1000);
            if ((ULONGLONG)(up * 1000) > maxUp) maxUp = (ULONGLONG)(up * 1000);
        }
        /* Processor %: two samples. counter: 100*(1 - didle/dtime) with dtime from the object's PerfTime */
        if (havePrev && d.cpuPerfFreq > 0 && d.cpuPerfTime > prev.cpuPerfTime && ref.ncpu)
        {
            double dtime100ns = (double)(d.cpuPerfTime - prev.cpuPerfTime) * 1e7 / (double)d.cpuPerfFreq * ref.ncpu;
            double cbusy = 100.0 * (1.0 - (double)(d.idle - prev.idle) / dtime100ns);
            double dref = (double)((ref.kernel + ref.user) - (prevref.kernel + prevref.user));
            double rbusy = dref > 0 ? 100.0 * (1.0 - (double)(ref.idle - prevref.idle) / dref) : -1;

            printf("  CMP cpu_busy_pct: counter=%.1f ref(NtQSI)=%.1f diff=%.1f\n", cbusy, rbusy, cbusy - rbusy);
            if (cbusy < minBusy) minBusy = cbusy;
            if (cbusy > maxBusy) maxBusy = cbusy;
            if (d.idle != prev.idle) idleChanged++;
        }
        if (havePrev && d.uptimeStart != prev.uptimeStart)
            upChanged++;
        if ((LONGLONG)d.availMB < minAvail) minAvail = d.availMB;
        if ((LONGLONG)d.availMB > maxAvail) maxAvail = d.availMB;
        if (!havePrev)
            first = d;
        prev = d;
        prevref = ref;
        havePrev = 1;

        if (loop < LOOPS - 1)
        {
            if (loop >= 3 && loop <= 5)
                busy_wait_ms(900);   /* make the processor counter move */
            else
                Sleep(1000);
        }
    }

    printf("\nSUMMARY (changes across loops): avail_mb min=%lld max=%lld, uptime_s min=%.3f max=%.3f, cpu_busy_pct min=%.1f max=%.1f, idle counter changed in %d samples\n",
           minAvail, maxAvail, minUp / 1000.0, maxUp / 1000.0, minBusy, maxBusy, idleChanged);
    printf("SUMMARY: uptime start value changed between loops %d times (constant start = correct for PERF_ELAPSED_TIME)\n", upChanged);
    printf("SUMMARY: malformed or failed loops: %d\n", bad);

    /* ---- overhead ---- */
    vm_counters(&vm1);
    QueryPerformanceCounter(&q0);
    cpu1 = proc_cpu_100ns();
    {
        int i, fails = 0;
        for (i = 0; i < BURST; i++)
            if (query(&cb) != ERROR_SUCCESS)
                fails++;
        QueryPerformanceCounter(&q1);
        cpu1 = proc_cpu_100ns() - cpu1;
        printf("OVERHEAD burst: %d queries (%d failed) in %.1f ms wall, consumer CPU %llu x100ns = %.3f ms/query\n", BURST, fails,
               (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)qf.QuadPart, cpu1, cpu1 / 10000.0 / BURST);
    }
    vm_counters(&vm2);
    GetProcessIoCounters(GetCurrentProcess(), &io1);
    printf("OVERHEAD loops: consumer CPU in the %d queries %llu x100ns (timer tick granularity applies)\n", LOOPS, cpuLoopTotal);
    printf("OVERHEAD memory (process, includes the providers loaded in-process): before first query PagefileUsage=%llu WorkingSet=%llu; after %d loops %llu / %llu; after burst %llu / %llu\n",
           (ULONGLONG)vm0.PagefileUsage, (ULONGLONG)vm0.WorkingSetSize, LOOPS, (ULONGLONG)vm1.PagefileUsage,
           (ULONGLONG)vm1.WorkingSetSize, (ULONGLONG)vm2.PagefileUsage, (ULONGLONG)vm2.WorkingSetSize);
    printf("OVERHEAD I/O (whole consumer process, start to end): write ops %llu (bytes %llu), read ops %llu (bytes %llu); total CPU %llu x100ns\n",
           io1.WriteOperationCount - io0.WriteOperationCount, io1.WriteTransferCount - io0.WriteTransferCount,
           io1.ReadOperationCount - io0.ReadOperationCount, io1.ReadTransferCount - io0.ReadTransferCount,
           proc_cpu_100ns() - cpu0);
    check_names();
    (void)first;
    return 0;
}
