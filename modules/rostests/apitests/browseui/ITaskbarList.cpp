/* ReactOS API tests. LGPL-2.1-or-later. */
#include <apitest.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>

static UINT s_ButtonCreatedMsg;
static BOOL s_ButtonCreated;

static LRESULT CALLBACK TestWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == s_ButtonCreatedMsg && s_ButtonCreatedMsg)
    {
        s_ButtonCreated = TRUE;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void PumpFor(DWORD ms)
{
    DWORD start = GetTickCount();
    MSG msg;
    for (;;)
    {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        DWORD elapsed = GetTickCount() - start;
        if (elapsed >= ms)
            break;
        MsgWaitForMultipleObjects(0, NULL, FALSE, ms - elapsed, QS_ALLINPUT);
    }
}

static BOOL WaitButtonCreated(DWORD ms)
{
    DWORD start = GetTickCount();
    while (!s_ButtonCreated && GetTickCount() - start < ms)
        PumpFor(50);
    return s_ButtonCreated;
}

/* TASKBARLIST_HOLD_MS=<n> keeps each visible state on screen for n ms, so a
   lab run can capture the button. The default is a short pause. */
static void Hold(const char *what)
{
    static DWORD s_Hold = (DWORD)-1;
    if (s_Hold == (DWORD)-1)
    {
        const char *env = getenv("TASKBARLIST_HOLD_MS");
        s_Hold = env ? strtoul(env, NULL, 10) : 200;
    }
    trace("STATE %s\n", what);
    PumpFor(s_Hold);
}

static void TestDecorations(ITaskbarList4 *taskbar)
{
    WNDCLASSW wc = { 0 };
    HWND hwnd, owned;
    HRESULT hr;
    HICON icon;

    if (!FindWindowW(L"Shell_TrayWnd", NULL))
    {
        skip("no taskbar\n");
        return;
    }
    s_ButtonCreatedMsg = RegisterWindowMessageW(L"TaskbarButtonCreated");
    ok(s_ButtonCreatedMsg != 0, "RegisterWindowMessage failed\n");
    wc.lpfnWndProc = TestWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"ITaskbarListTestWnd";
    RegisterClassW(&wc);
    hwnd = CreateWindowExW(0, wc.lpszClassName, L"ITaskbarList test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           100, 100, 320, 200, NULL, NULL, wc.hInstance, NULL);
    ok(hwnd != NULL, "CreateWindow failed %lu\n", GetLastError());
    if (!hwnd)
        return;

    ok(WaitButtonCreated(5000), "no TaskbarButtonCreated within 5 s\n");

    hr = taskbar->HrInit();
    ok(hr == S_OK, "HrInit: %#lx\n", hr);

    hr = taskbar->SetProgressState(hwnd, TBPF_NORMAL);
    ok(hr == S_OK, "SetProgressState NORMAL: %#lx\n", hr);
    hr = taskbar->SetProgressValue(hwnd, 25, 100);
    ok(hr == S_OK, "SetProgressValue 25/100: %#lx\n", hr);
    Hold("normal 25%");
    hr = taskbar->SetProgressValue(hwnd, 3ULL << 40, 4ULL << 40);
    ok(hr == S_OK, "SetProgressValue 64-bit 75%%: %#lx\n", hr);
    Hold("normal 75% (64-bit total)");
    hr = taskbar->SetProgressValue(hwnd, 200, 100);
    ok(hr == S_OK, "SetProgressValue completed > total: %#lx\n", hr);
    Hold("normal 100% (clamped)");
    hr = taskbar->SetProgressState(hwnd, TBPF_ERROR);
    ok(hr == S_OK, "SetProgressState ERROR: %#lx\n", hr);
    hr = taskbar->SetProgressValue(hwnd, 50, 100);
    ok(hr == S_OK, "SetProgressValue 50/100: %#lx\n", hr);
    Hold("error 50% (red)");
    hr = taskbar->SetProgressState(hwnd, TBPF_PAUSED);
    ok(hr == S_OK, "SetProgressState PAUSED: %#lx\n", hr);
    Hold("paused 50% (yellow)");
    hr = taskbar->SetProgressState(hwnd, TBPF_INDETERMINATE);
    ok(hr == S_OK, "SetProgressState INDETERMINATE: %#lx\n", hr);
    Hold("indeterminate (moving band)");
    hr = taskbar->SetProgressState(hwnd, (TBPFLAG)3);
    ok(hr == E_INVALIDARG, "SetProgressState invalid: %#lx\n", hr);
    hr = taskbar->SetProgressState(NULL, TBPF_NORMAL);
    ok(hr == E_INVALIDARG, "SetProgressState NULL hwnd: %#lx\n", hr);
    hr = taskbar->SetProgressValue(NULL, 1, 2);
    ok(hr == E_INVALIDARG, "SetProgressValue NULL hwnd: %#lx\n", hr);
    hr = taskbar->SetProgressState(hwnd, TBPF_NOPROGRESS);
    ok(hr == S_OK, "SetProgressState NOPROGRESS: %#lx\n", hr);
    Hold("no progress");

    /* The caller's icon can be destroyed at once: Explorer keeps a copy. */
    icon = (HICON)CopyImage(LoadIconW(NULL, (LPCWSTR)IDI_WARNING), IMAGE_ICON, 0, 0, 0);
    ok(icon != NULL, "CopyImage failed\n");
    hr = taskbar->SetOverlayIcon(hwnd, icon, L"Warning");
    ok(hr == S_OK, "SetOverlayIcon: %#lx\n", hr);
    if (icon)
        DestroyIcon(icon);
    Hold("overlay (warning icon, large icons only)");
    hr = taskbar->SetOverlayIcon(hwnd, NULL, NULL);
    ok(hr == S_OK, "SetOverlayIcon NULL: %#lx\n", hr);
    Hold("overlay cleared");

    /* An owned window without a button decorates its owner's button, and its
       state goes when it does. */
    owned = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"owned", WS_POPUP | WS_CAPTION | WS_VISIBLE,
                            150, 150, 200, 100, hwnd, NULL, wc.hInstance, NULL);
    ok(owned != NULL, "owned CreateWindow failed %lu\n", GetLastError());
    if (owned)
    {
        hr = taskbar->SetProgressValue(owned, 60, 100);
        ok(hr == S_OK, "owned SetProgressValue: %#lx\n", hr);
        Hold("owner button 60% from the owned window");
        DestroyWindow(owned);
        hr = taskbar->SetProgressState(owned, TBPF_NOPROGRESS);
        ok(hr == S_OK, "destroyed owned SetProgressState NOPROGRESS: %#lx\n", hr);
        Hold("owner button cleared after the owned window died");
    }

    hr = taskbar->SetActiveAlt(hwnd);
    ok(hr == S_OK, "SetActiveAlt: %#lx\n", hr);
    hr = taskbar->ActivateTab(hwnd);
    ok(hr == S_OK, "ActivateTab: %#lx\n", hr);
    hr = taskbar->MarkFullscreenWindow(hwnd, FALSE);
    ok(hr == S_OK, "MarkFullscreenWindow: %#lx\n", hr);
    hr = taskbar->ThumbBarAddButtons(hwnd, 0, NULL);
    ok(hr == E_NOTIMPL, "ThumbBarAddButtons: %#lx\n", hr);
    hr = taskbar->SetThumbnailClip(hwnd, NULL);
    ok(hr == E_NOTIMPL, "SetThumbnailClip: %#lx\n", hr);

    hr = taskbar->DeleteTab(hwnd);
    ok(hr == S_OK, "DeleteTab: %#lx\n", hr);
    hr = taskbar->SetProgressState(hwnd, TBPF_NORMAL);
    ok(hr == E_FAIL, "SetProgressState without a button: %#lx\n", hr);
    s_ButtonCreated = FALSE;
    hr = taskbar->AddTab(hwnd);
    ok(hr == S_OK, "AddTab: %#lx\n", hr);
    ok(WaitButtonCreated(5000), "no TaskbarButtonCreated after AddTab\n");

    DestroyWindow(hwnd);
    PumpFor(100);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}

START_TEST(ITaskbarList)
{
    ITaskbarList4 *taskbar = NULL;
    HRESULT hr = CoInitialize(NULL);
    ok(SUCCEEDED(hr), "CoInitialize: %#lx\n", hr);
    if (FAILED(hr)) return;
    hr = CoCreateInstance(CLSID_TaskbarList, NULL, CLSCTX_INPROC_SERVER,
                          IID_ITaskbarList4, (void **)&taskbar);
    ok(hr == S_OK, "ITaskbarList4 activation: %#lx\n", hr);
    if (SUCCEEDED(hr))
    {
        const IID *iids[] = { &IID_IUnknown, &IID_ITaskbarList, &IID_ITaskbarList2,
                             &IID_ITaskbarList3, &IID_ITaskbarList4 };
        IUnknown *identity = NULL;
        taskbar->QueryInterface(IID_IUnknown, (void **)&identity);
        for (unsigned i = 0; i < _countof(iids); ++i)
        {
            IUnknown *iface = NULL, *unknown = NULL;
            hr = taskbar->QueryInterface(*iids[i], (void **)&iface);
            ok(hr == S_OK, "interface %u: %#lx\n", i, hr);
            if (!iface) continue;
            hr = iface->QueryInterface(IID_IUnknown, (void **)&unknown);
            ok(hr == S_OK && unknown == identity, "inconsistent COM identity\n");
            if (unknown) unknown->Release();
            iface->Release();
        }
        if (identity) identity->Release();
        IUnknown *unknown = (IUnknown *)0xdeadbeef;
        hr = taskbar->QueryInterface(IID_IClassFactory, (void **)&unknown);
        ok(hr == E_NOINTERFACE && !unknown, "unsupported IID: %#lx, %p\n", hr, unknown);
        TestDecorations(taskbar);
        taskbar->Release();
    }
    CoUninitialize();
}
