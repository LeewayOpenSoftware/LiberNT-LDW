/*
 * wmiprobe - WMI live-data probe (LiberNT observability, objective C).
 *
 * Connects to ROOT\CIMV2 through the documented client path
 * (CoCreateInstance(CLSID_WbemLocator) -> IWbemLocator::ConnectServer ->
 * IWbemServices::ExecQuery) and, for four milestone classes, prints each key
 * field next to the value obtained from a direct Win32 API call in this same
 * process.  The per-field diff is the evidence that wbemprox returns LIVE
 * system data, not static/hardcoded values.  Read-only; no WMI writes.
 *
 * Classes / cross-checks:
 *   Win32_OperatingSystem : Version/BuildNumber vs RtlGetVersion/GetVersionExW;
 *                           TotalVisibleMemorySize vs GlobalMemoryStatusEx.
 *   Win32_Processor       : NumberOfLogicalProcessors vs GetSystemInfo.
 *   Win32_Process         : row count vs CreateToolhelp32Snapshot.
 *   Win32_Service         : row count vs EnumServicesStatusExW.
 */

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#define INITGUID
#include <initguid.h>
#include <wbemcli.h>
#include <tlhelp32.h>
#include <winsvc.h>
#include <psapi.h>
#include <stdio.h>

/* overhead helpers (god report): current working set in KB, and QPC delta in ms. */
static ULONGLONG ws_kb(void)
{
    PROCESS_MEMORY_COUNTERS pmc; pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return (ULONGLONG)(pmc.WorkingSetSize / 1024);
    return 0;
}
static double qpc_ms(LARGE_INTEGER a, LARGE_INTEGER b, LARGE_INTEGER f)
{
    return (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
}

static ULONGLONG CountQuery(IWbemServices *svc, const WCHAR *wql, const WCHAR *keyprop,
                            WCHAR *sample, int samplelen)
{
    IEnumWbemClassObject *e = NULL;
    HRESULT hr;
    ULONGLONG n = 0;
    BSTR lang = SysAllocString(L"WQL");
    BSTR q = SysAllocString(wql);

    if (sample && samplelen) sample[0] = 0;
    hr = IWbemServices_ExecQuery(svc, lang, q,
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &e);
    SysFreeString(q); SysFreeString(lang);
    if (FAILED(hr)) { wprintf(L"    [ExecQuery failed 0x%08lx]\n", hr); return (ULONGLONG)-1; }

    for (;;)
    {
        IWbemClassObject *obj = NULL;
        ULONG got = 0;
        hr = IEnumWbemClassObject_Next(e, WBEM_INFINITE, 1, &obj, &got);
        if (hr != WBEM_S_NO_ERROR || !got) break;
        if (n == 0 && keyprop && sample)
        {
            VARIANT v; VariantInit(&v);
            if (SUCCEEDED(IWbemClassObject_Get(obj, keyprop, 0, &v, NULL, NULL)) && V_VT(&v) == VT_BSTR && V_BSTR(&v))
                _snwprintf(sample, samplelen - 1, L"%s", V_BSTR(&v));
            VariantClear(&v);
        }
        IWbemClassObject_Release(obj);
        n++;
    }
    IEnumWbemClassObject_Release(e);
    return n;
}

static int WmiGetProp(IWbemServices *svc, const WCHAR *wql, const WCHAR *prop, VARIANT *out)
{
    IEnumWbemClassObject *e = NULL;
    IWbemClassObject *obj = NULL;
    ULONG got = 0;
    HRESULT hr;
    BSTR lang = SysAllocString(L"WQL");
    BSTR q = SysAllocString(wql);
    int ok = 0;

    VariantInit(out);
    hr = IWbemServices_ExecQuery(svc, lang, q,
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &e);
    SysFreeString(q); SysFreeString(lang);
    if (FAILED(hr)) { wprintf(L"    [ExecQuery failed 0x%08lx]\n", hr); return 0; }
    hr = IEnumWbemClassObject_Next(e, WBEM_INFINITE, 1, &obj, &got);
    if (hr == WBEM_S_NO_ERROR && got)
    {
        ok = SUCCEEDED(IWbemClassObject_Get(obj, prop, 0, out, NULL, NULL));
        IWbemClassObject_Release(obj);
    }
    IEnumWbemClassObject_Release(e);
    return ok;
}

static ULONGLONG DirectProcessCount(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe; ULONGLONG n = 0;
    if (snap == INVALID_HANDLE_VALUE) return (ULONGLONG)-1;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) do { n++; } while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    return n;
}

static ULONGLONG DirectServiceCount(void)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE);
    DWORD need = 0, ret = 0, resume = 0;
    ULONGLONG n = 0;
    BYTE *buf;
    if (!scm) return (ULONGLONG)-1;
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32 | SERVICE_DRIVER,
                          SERVICE_STATE_ALL, NULL, 0, &need, &ret, &resume, NULL);
    buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, need ? need : 1);
    if (buf && EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32 | SERVICE_DRIVER,
                                     SERVICE_STATE_ALL, buf, need, &need, &ret, &resume, NULL))
        n = ret;
    if (buf) HeapFree(GetProcessHeap(), 0, buf);
    CloseServiceHandle(scm);
    return n;
}

/* P0-6: direct count of RUNNING Win32 services only (no drivers), for cross-check. */
static ULONGLONG DirectRunningWin32Count(void)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE);
    DWORD need = 0, ret = 0, resume = 0;
    ULONGLONG n = 0;
    BYTE *buf;
    if (!scm) return (ULONGLONG)-1;
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32,
                          SERVICE_ACTIVE, NULL, 0, &need, &ret, &resume, NULL);
    buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, need ? need : 1);
    if (buf && EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32,
                                     SERVICE_ACTIVE, buf, need, &need, &ret, &resume, NULL))
        n = ret;
    if (buf) HeapFree(GetProcessHeap(), 0, buf);
    CloseServiceHandle(scm);
    return n;
}

/* P0-6: count Win32_Service rows (State=Running) and how many are driver-typed
 * (Win32_Service must contain only Win32 services; drivers belong to Win32_SystemDriver). */
static int CheckRunningServiceTypes(IWbemServices *svc, ULONGLONG *total, ULONGLONG *drivers,
                                    WCHAR *firstdrv, int fdlen)
{
    IEnumWbemClassObject *e = NULL; HRESULT hr; ULONGLONG n = 0, d = 0;
    BSTR lang = SysAllocString(L"WQL");
    BSTR q = SysAllocString(L"SELECT Name,ServiceType FROM Win32_Service WHERE State=\"Running\"");
    if (firstdrv && fdlen) firstdrv[0] = 0;
    hr = IWbemServices_ExecQuery(svc, lang, q,
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &e);
    SysFreeString(q); SysFreeString(lang);
    if (FAILED(hr)) { *total = (ULONGLONG)-1; *drivers = 0; return 0; }
    for (;;)
    {
        IWbemClassObject *obj = NULL; ULONG got = 0; VARIANT vt;
        hr = IEnumWbemClassObject_Next(e, WBEM_INFINITE, 1, &obj, &got);
        if (hr != WBEM_S_NO_ERROR || !got) break;
        VariantInit(&vt);
        if (SUCCEEDED(IWbemClassObject_Get(obj, L"ServiceType", 0, &vt, NULL, NULL)) &&
            V_VT(&vt) == VT_BSTR && V_BSTR(&vt) && wcsstr(V_BSTR(&vt), L"Driver"))
        {
            if (d == 0 && firstdrv && fdlen)
            {
                VARIANT vn; VariantInit(&vn);
                if (SUCCEEDED(IWbemClassObject_Get(obj, L"Name", 0, &vn, NULL, NULL)) &&
                    V_VT(&vn) == VT_BSTR && V_BSTR(&vn))
                    _snwprintf(firstdrv, fdlen - 1, L"%s (%s)", V_BSTR(&vn), V_BSTR(&vt));
                VariantClear(&vn);
            }
            d++;
        }
        VariantClear(&vt);
        IWbemClassObject_Release(obj);
        n++;
    }
    IEnumWbemClassObject_Release(e);
    *total = n; *drivers = d;
    return 1;
}

int wmain(void)
{
    HRESULT hr;
    IWbemLocator *loc = NULL;
    IWbemServices *svc = NULL;
    BSTR ns;
    VARIANT v;
    SYSTEM_INFO si;
    MEMORYSTATUSEX ms;
    ULONGLONG wmiN, directN;
    ULONGLONG ws0, ws1;
    WCHAR sample[256];
    int fail = 0;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) { wprintf(L"CoInitializeEx 0x%08lx\n", hr); return 2; }
    CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT,
                         RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);

    ws0 = ws_kb();
    hr = CoCreateInstance(&CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWbemLocator, (void **)&loc);
    if (FAILED(hr)) { wprintf(L"CoCreateInstance(WbemLocator) 0x%08lx\n", hr); CoUninitialize(); return 2; }

    ns = SysAllocString(L"ROOT\\CIMV2");
    hr = IWbemLocator_ConnectServer(loc, ns, NULL, NULL, NULL, 0, NULL, NULL, &svc);
    SysFreeString(ns);
    if (FAILED(hr))
    {
        wprintf(L"ConnectServer(ROOT\\CIMV2) FAILED 0x%08lx\n", hr);
        wprintf(L"  (if this needs the winmgmt/wmisvc service, that is the blocker to report)\n");
        IWbemLocator_Release(loc); CoUninitialize(); return 3;
    }
    ws1 = ws_kb();
    wprintf(L"ConnectServer(ROOT\\CIMV2) OK\n\n");

    /* --- Win32_OperatingSystem: Version vs RtlGetVersion; memory vs GlobalMemoryStatusEx --- */
    wprintf(L"== Win32_OperatingSystem ==\n");
    if (WmiGetProp(svc, L"SELECT Version,BuildNumber,Caption FROM Win32_OperatingSystem", L"Version", &v) && V_VT(&v) == VT_BSTR)
        wprintf(L"  WMI    Version         = %s\n", V_BSTR(&v));
    VariantClear(&v);
    {
        OSVERSIONINFOEXW ovi; ovi.dwOSVersionInfoSize = sizeof(ovi);
        if (GetVersionExW((OSVERSIONINFOW *)&ovi))
            wprintf(L"  direct GetVersionExW    = %lu.%lu.%lu\n", ovi.dwMajorVersion, ovi.dwMinorVersion, ovi.dwBuildNumber);
    }
    ms.dwLength = sizeof(ms); GlobalMemoryStatusEx(&ms);
    if (WmiGetProp(svc, L"SELECT TotalVisibleMemorySize FROM Win32_OperatingSystem", L"TotalVisibleMemorySize", &v))
    {
        VARIANT vs; VariantInit(&vs);
        if (SUCCEEDED(VariantChangeType(&vs, &v, 0, VT_UI8)))
            wprintf(L"  WMI    TotalVisibleKB  = %I64u\n", V_UI8(&vs));
        else if (V_VT(&v) == VT_BSTR) wprintf(L"  WMI    TotalVisibleKB  = %s\n", V_BSTR(&v));
        VariantClear(&vs);
    }
    VariantClear(&v);
    wprintf(L"  direct GlobalMemoryKB   = %I64u\n\n", ms.ullTotalPhys / 1024);

    /* --- Win32_Processor: NumberOfLogicalProcessors vs GetSystemInfo --- */
    wprintf(L"== Win32_Processor ==\n");
    GetSystemInfo(&si);
    if (WmiGetProp(svc, L"SELECT NumberOfLogicalProcessors FROM Win32_Processor", L"NumberOfLogicalProcessors", &v))
    {
        VARIANT vs; VariantInit(&vs);
        if (SUCCEEDED(VariantChangeType(&vs, &v, 0, VT_UI4)))
            wprintf(L"  WMI    LogicalProcs    = %lu\n", V_UI4(&vs));
        VariantClear(&vs);
    }
    VariantClear(&v);
    wprintf(L"  direct GetSystemInfo    = %lu\n\n", si.dwNumberOfProcessors);

    /* --- Win32_Process: row count vs toolhelp --- */
    wprintf(L"== Win32_Process ==\n");
    wmiN = CountQuery(svc, L"SELECT Name FROM Win32_Process", L"Name", sample, 256);
    directN = DirectProcessCount();
    wprintf(L"  WMI    rows            = %I64u (first: %s)\n", wmiN, sample[0] ? sample : L"-");
    wprintf(L"  direct toolhelp procs   = %I64u\n", directN);
    if (wmiN == (ULONGLONG)-1 || directN == (ULONGLONG)-1 || (directN && (wmiN < directN/2 || wmiN > directN*2))) fail++;
    wprintf(L"\n");

    /* --- Win32_Service: row count vs SCM --- */
    wprintf(L"== Win32_Service ==\n");
    wmiN = CountQuery(svc, L"SELECT Name FROM Win32_Service", L"Name", sample, 256);
    directN = DirectServiceCount();
    wprintf(L"  WMI    rows            = %I64u (first: %s)\n", wmiN, sample[0] ? sample : L"-");
    wprintf(L"  direct SCM services     = %I64u\n", directN);
    if (wmiN == (ULONGLONG)-1 || directN == (ULONGLONG)-1 || (directN && (wmiN < directN/2 || wmiN > directN*2))) fail++;
    wprintf(L"\n");

    /* ===== P0-6 regression checks (WT Creed independent wmic cross-check) ===== */
    wprintf(L"== P0-6 checks ==\n");

    /* (1) Win32_Processor.LoadPercentage: was 'Invalid query' (column missing). */
    if (WmiGetProp(svc, L"SELECT LoadPercentage,Name,NumberOfCores FROM Win32_Processor", L"LoadPercentage", &v))
    {
        VARIANT vs; VariantInit(&vs);
        if (SUCCEEDED(VariantChangeType(&vs, &v, 0, VT_UI4)))
            wprintf(L"  [P06.1 PASS] LoadPercentage query accepted, value = %lu\n", V_UI4(&vs));
        else
            wprintf(L"  [P06.1 PASS] LoadPercentage query accepted, vt = %u\n", V_VT(&v));
        VariantClear(&vs);
    }
    else { wprintf(L"  [P06.1 FAIL] LoadPercentage query rejected (still Invalid query)\n"); fail++; }
    VariantClear(&v);

    /* (2) Win32_Process.WorkingSetSize: was 0 for every process. Check our own process > 0. */
    {
        WCHAR wq[128]; ULONGLONG wmiws = 0, directws = ws_kb() * 1024;
        _snwprintf(wq, 128, L"SELECT WorkingSetSize FROM Win32_Process WHERE Handle=\"%lu\"", GetCurrentProcessId());
        if (WmiGetProp(svc, wq, L"WorkingSetSize", &v))
        {
            VARIANT vs; VariantInit(&vs);
            if (SUCCEEDED(VariantChangeType(&vs, &v, 0, VT_UI8))) wmiws = V_UI8(&vs);
            VariantClear(&vs);
        }
        VariantClear(&v);
        wprintf(L"  [P06.2 %s] WorkingSetSize(self): WMI = %I64u, direct = %I64u\n",
                wmiws ? L"PASS" : L"FAIL", wmiws, directws);
        if (!wmiws) fail++;
    }

    /* (3) Win32_Service(State=Running) must contain no kernel/driver-typed rows. */
    {
        ULONGLONG total = 0, drv = 0, directWin32 = DirectRunningWin32Count();
        WCHAR firstdrv[160];
        if (CheckRunningServiceTypes(svc, &total, &drv, firstdrv, 160))
        {
            wprintf(L"  [P06.3 %s] Win32_Service running rows = %I64u, driver-typed = %I64u (direct Win32 running = %I64u)\n",
                    drv ? L"FAIL" : L"PASS", total, drv, directWin32);
            if (drv) { wprintf(L"           first driver leaked into Win32_Service: %s\n", firstdrv[0] ? firstdrv : L"-"); fail++; }
        }
        else { wprintf(L"  [P06.3 FAIL] Win32_Service query failed\n"); fail++; }
    }
    wprintf(L"\n");

    /* --- OVERHEAD (god report): working set + per-query wall time; no disk writes --- */
    {
        LARGE_INTEGER f, t0, t1;
        const WCHAR *cls[4] = { L"Win32_OperatingSystem", L"Win32_Processor", L"Win32_Process", L"Win32_Service" };
        int i; WCHAR wql[64];
        QueryPerformanceFrequency(&f);
        wprintf(L"== overhead ==\n");
        wprintf(L"  WS before ConnectServer = %I64u KB\n", ws0);
        wprintf(L"  WS after  ConnectServer = %I64u KB (delta %I64d)\n", ws1, (LONGLONG)ws1 - (LONGLONG)ws0);
        for (i = 0; i < 4; i++)
        {
            _snwprintf(wql, 64, L"SELECT * FROM %s", cls[i]);
            QueryPerformanceCounter(&t0);
            (void)CountQuery(svc, wql, NULL, NULL, 0);
            QueryPerformanceCounter(&t1);
            wprintf(L"  query %-22s = %.1f ms, WS %I64u KB\n", cls[i], qpc_ms(t0, t1, f), ws_kb());
        }
        wprintf(L"  persistent disk writes  = none (wmiprobe writes only to stdout/serial)\n\n");
    }

    IWbemServices_Release(svc);
    IWbemLocator_Release(loc);
    CoUninitialize();
    wprintf(L"wmiprobe done (%s). Compare WMI vs direct per field: equal/close = LIVE, constant/zero = suspect.\n",
            fail ? L"MISMATCH flagged" : L"counts consistent");
    return fail ? 1 : 0;
}
