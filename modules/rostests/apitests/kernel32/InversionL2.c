/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tramo D, Frente 1, lot 2: kernel32 exports forwarded to kernelbase
 */

#include "precomp.h"

static const char *const g_Forwarded[] =
{
    "SetThreadGroupAffinity", "GetThreadContext", "SetThreadContext",
    "QueueUserWorkItem", "GetPackagePathByFullName", "GetPackagesByPackageFamily",
    "SleepConditionVariableCS", "SleepConditionVariableSRW",
    "TrySubmitThreadpoolCallback", "SetThreadDescription", "CallbackMayRunLong",
};

static void test_ForwardsResolveToKernelBase(void)
{
    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    HMODULE hKB = GetModuleHandleW(L"kernelbase.dll");
    size_t i;

    if (!hK32 || !hKB)
    {
        skip("kernel32/kernelbase not loaded\n");
        return;
    }
    for (i = 0; i < ARRAYSIZE(g_Forwarded); i++)
    {
        FARPROC p32 = GetProcAddress(hK32, g_Forwarded[i]);
        FARPROC pKB = GetProcAddress(hKB, g_Forwarded[i]);

        ok(p32 != NULL, "kernel32!%s not exported\n", g_Forwarded[i]);
        ok(p32 == pKB, "kernel32!%s (%p) is not the kernelbase body (%p)\n", g_Forwarded[i], p32, pKB);
    }
}

static DWORD WINAPI WorkItem(LPVOID ctx)
{
    SetEvent((HANDLE)ctx);
    return 0;
}

static void test_Behavior(void)
{
    CONDITION_VARIABLE cv;
    SRWLOCK lock;
    CRITICAL_SECTION cs;
    CONTEXT ctx;
    HANDLE hEvt;
    UINT32 count = 5, len = 5;
    WCHAR *desc;
    HRESULT hr;
    BOOL ret;

    /* condition variables: timeout reports FALSE + ERROR_TIMEOUT */
    InitializeConditionVariable(&cv);
    InitializeSRWLock(&lock);
    InitializeCriticalSection(&cs);
    AcquireSRWLockExclusive(&lock);
    SetLastError(0xdeadbeef);
    ret = SleepConditionVariableSRW(&cv, &lock, 10, 0);
    ok(!ret && GetLastError() == ERROR_TIMEOUT, "SRW: ret %d, error %lu\n", ret, GetLastError());
    ReleaseSRWLockExclusive(&lock);
    EnterCriticalSection(&cs);
    SetLastError(0xdeadbeef);
    ret = SleepConditionVariableCS(&cv, &cs, 10);
    ok(!ret && GetLastError() == ERROR_TIMEOUT, "CS: ret %d, error %lu\n", ret, GetLastError());
    LeaveCriticalSection(&cs);
    DeleteCriticalSection(&cs);

    /* work item runs */
    hEvt = CreateEventW(NULL, TRUE, FALSE, NULL);
    ok(QueueUserWorkItem(WorkItem, hEvt, WT_EXECUTEDEFAULT), "QueueUserWorkItem failed %lu\n", GetLastError());
    ok(WaitForSingleObject(hEvt, 5000) == WAIT_OBJECT_0, "work item did not run\n");
    CloseHandle(hEvt);

    /* context of a suspended-less foreign handle: invalid handle fails with an error */
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_CONTROL;
    SetLastError(0xdeadbeef);
    ret = GetThreadContext(NULL, &ctx);
    ok(!ret && GetLastError() != 0xdeadbeef, "GetThreadContext(NULL): ret %d error %lu\n", ret, GetLastError());
    SetLastError(0xdeadbeef);
    ret = SetThreadContext(NULL, &ctx);
    ok(!ret && GetLastError() != 0xdeadbeef, "SetThreadContext(NULL): ret %d error %lu\n", ret, GetLastError());

    /* thread description round trip */
    hr = SetThreadDescription(GetCurrentThread(), L"InversionL2");
    if (SUCCEEDED(hr))
    {
        desc = NULL;
        hr = GetThreadDescription(GetCurrentThread(), &desc);
        ok(SUCCEEDED(hr) && desc && !wcscmp(desc, L"InversionL2"), "description %S hr %lx\n", desc ? desc : L"(null)", hr);
        if (desc)
            LocalFree(desc);
    }
    else
        trace("SetThreadDescription -> %lx\n", hr);

    /* packages: invalid arguments, then "no packages" */
    {
        typedef LONG (WINAPI *PFN_PathByFullName)(PCWSTR, UINT32 *, PWSTR);
        typedef LONG (WINAPI *PFN_ByFamily)(PCWSTR, UINT32 *, PWSTR *, UINT32 *, PWSTR);
        HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
        PFN_PathByFullName pPath = (PFN_PathByFullName)GetProcAddress(hK32, "GetPackagePathByFullName");
        PFN_ByFamily pFamily = (PFN_ByFamily)GetProcAddress(hK32, "GetPackagesByPackageFamily");

        if (pPath && pFamily)
        {
            ok(pPath(NULL, &len, NULL) == ERROR_INVALID_PARAMETER, "GetPackagePathByFullName(NULL)\n");
            ok(pFamily(L"x", NULL, NULL, &len, NULL) == ERROR_INVALID_PARAMETER, "GetPackagesByPackageFamily(NULL)\n");
            ok(pFamily(L"x", &count, NULL, &len, NULL) == ERROR_SUCCESS && count == 0 && len == 0, "no packages expected\n");
        }
        else
            skip("package APIs not exported\n");
    }
}

START_TEST(InversionL2)
{
    test_ForwardsResolveToKernelBase();
    test_Behavior();
}
