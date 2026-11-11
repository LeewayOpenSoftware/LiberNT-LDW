/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Record DPI, display-mode and display-notification baselines
 * COPYRIGHT:   Copyright 2026 Leeway Open Software <281492504+LeewayOpenSoftware@users.noreply.github.com>
 */

#include <windows.h>
#include <shellscalingapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define D0_MAX_INDEXED_MODES 512

typedef BOOL (WINAPI *SETPROCESSCONTEXT)(DPI_AWARENESS_CONTEXT);
typedef DPI_AWARENESS_CONTEXT (WINAPI *GETTHREADCONTEXT)(VOID);
typedef DPI_AWARENESS (WINAPI *GETAWARENESS)(DPI_AWARENESS_CONTEXT);
typedef UINT (WINAPI *GETDPIFORSYSTEM)(VOID);
typedef UINT (WINAPI *GETDPIFORWINDOW)(HWND);
typedef BOOL (WINAPI *GETDPIFORMONITORINTERNAL)(HMONITOR, UINT, UINT *, UINT *);
typedef INT (WINAPI *GETMETRICSFORDPI)(INT, UINT);
typedef BOOL (WINAPI *SPIFORDPI)(UINT, UINT, PVOID, UINT, UINT);
typedef HRESULT (WINAPI *GETDPIFORMONITOR)(HMONITOR, MONITOR_DPI_TYPE, UINT *, UINT *);

typedef struct
{
    const char *Variant;
    const char *Phase;
    const char *LogPath;
    DWORD WaitMs;
    LONG TestWidth;
    LONG TestHeight;
    BOOL Apply;
    BOOL Restore;
} OPTIONS;

typedef struct
{
    SETPROCESSCONTEXT SetProcessContext;
    GETTHREADCONTEXT GetThreadContext;
    GETAWARENESS GetAwareness;
    GETDPIFORSYSTEM GetDpiForSystem;
    GETDPIFORWINDOW GetDpiForWindow;
    GETDPIFORMONITORINTERNAL GetDpiForMonitorInternal;
    GETMETRICSFORDPI GetMetricsForDpi;
    SPIFORDPI SpiForDpi;
    GETDPIFORMONITOR GetDpiForMonitor;
} DPI_APIS;

static OPTIONS g_Options;
static DPI_APIS g_Api;
static FILE *g_Log;
static HWND g_Window;

static void
Log(const char *Format, ...)
{
    va_list Args;

    va_start(Args, Format);
    vprintf(Format, Args);
    fflush(stdout);
    if (g_Log != NULL)
    {
        va_list FileArgs;
        va_copy(FileArgs, Args);
        vfprintf(g_Log, Format, FileArgs);
        fflush(g_Log);
        va_end(FileArgs);
    }
    va_end(Args);
}

static BOOL
ParseLong(const char *Text, LONG *Value)
{
    char *End;
    long Parsed;

    if (Text == NULL || *Text == 0)
        return FALSE;
    Parsed = strtol(Text, &End, 10);
    if (*End != 0)
        return FALSE;
    *Value = Parsed;
    return TRUE;
}

static BOOL
ParseOptions(int argc, char **argv)
{
    int i;

    ZeroMemory(&g_Options, sizeof(g_Options));
    g_Options.Variant = "unaware";
    g_Options.Phase = "live";
    g_Options.WaitMs = 0;

    for (i = 1; i < argc; i++)
    {
        if (!strncmp(argv[i], "--variant=", 10))
            g_Options.Variant = argv[i] + 10;
        else if (!strncmp(argv[i], "--phase=", 8))
            g_Options.Phase = argv[i] + 8;
        else if (!strncmp(argv[i], "--log=", 6))
            g_Options.LogPath = argv[i] + 6;
        else if (!strncmp(argv[i], "--wait-ms=", 10))
        {
            LONG Value;
            if (!ParseLong(argv[i] + 10, &Value) || Value < 0)
                return FALSE;
            g_Options.WaitMs = (DWORD)Value;
        }
        else if (!strncmp(argv[i], "--test-width=", 13))
        {
            if (!ParseLong(argv[i] + 13, &g_Options.TestWidth))
                return FALSE;
        }
        else if (!strncmp(argv[i], "--test-height=", 14))
        {
            if (!ParseLong(argv[i] + 14, &g_Options.TestHeight))
                return FALSE;
        }
        else if (!strcmp(argv[i], "--apply"))
            g_Options.Apply = TRUE;
        else if (!strcmp(argv[i], "--restore"))
            g_Options.Restore = TRUE;
        else
            return FALSE;
    }

    if (strcmp(g_Options.Variant, "unaware") &&
        strcmp(g_Options.Variant, "system") &&
        strcmp(g_Options.Variant, "permonitor") &&
        strcmp(g_Options.Variant, "permonitorv2"))
        return FALSE;
    if (strcmp(g_Options.Phase, "cold") && strcmp(g_Options.Phase, "live"))
        return FALSE;
    if (g_Options.LogPath == NULL || *g_Options.LogPath == 0)
        return FALSE;
    if (g_Options.Apply && (g_Options.TestWidth <= 0 || g_Options.TestHeight <= 0))
        return FALSE;
    if (g_Options.Apply != g_Options.Restore)
        return FALSE;
    return TRUE;
}

static void
LoadApis(void)
{
    HMODULE User32 = GetModuleHandleW(L"user32.dll");
    HMODULE ShCore = LoadLibraryW(L"shcore.dll");
    FARPROC Procedure;

#define LOAD_DPI_API(Module, Member, Name) \
    do \
    { \
        Procedure = GetProcAddress(Module, Name); \
        memcpy(&g_Api.Member, &Procedure, sizeof(Procedure)); \
    } while (0)

    if (User32 != NULL)
    {
        LOAD_DPI_API(User32, SetProcessContext, "SetProcessDpiAwarenessContext");
        LOAD_DPI_API(User32, GetThreadContext, "GetThreadDpiAwarenessContext");
        LOAD_DPI_API(User32, GetAwareness, "GetAwarenessFromDpiAwarenessContext");
        LOAD_DPI_API(User32, GetDpiForSystem, "GetDpiForSystem");
        LOAD_DPI_API(User32, GetDpiForWindow, "GetDpiForWindow");
        LOAD_DPI_API(User32, GetDpiForMonitorInternal, "GetDpiForMonitorInternal");
        LOAD_DPI_API(User32, GetMetricsForDpi, "GetSystemMetricsForDpi");
        LOAD_DPI_API(User32, SpiForDpi, "SystemParametersInfoForDpi");
    }
    if (ShCore != NULL)
        LOAD_DPI_API(ShCore, GetDpiForMonitor, "GetDpiForMonitor");

#undef LOAD_DPI_API
}

static DPI_AWARENESS_CONTEXT
RequestedContext(void)
{
    if (!strcmp(g_Options.Variant, "system"))
        return DPI_AWARENESS_CONTEXT_SYSTEM_AWARE;
    if (!strcmp(g_Options.Variant, "permonitor"))
        return DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE;
    if (!strcmp(g_Options.Variant, "permonitorv2"))
        return DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2;
    return DPI_AWARENESS_CONTEXT_UNAWARE;
}

static void
LogRegistryDword(HKEY Root, const char *RootName, const char *Path, const char *ValueName)
{
    DWORD Value = 0;
    DWORD Size = sizeof(Value);
    LSTATUS Status = RegGetValueA(Root, Path, ValueName, RRF_RT_REG_DWORD, NULL, &Value, &Size);

    if (Status == ERROR_SUCCESS)
        Log("REGISTRY root=%s path=\"%s\" value=%s dword=%lu\n", RootName, Path, ValueName, Value);
    else
        Log("REGISTRY root=%s path=\"%s\" value=%s error=%ld\n", RootName, Path, ValueName, Status);
}

static void
LogRegistry(void)
{
    LogRegistryDword(HKEY_LOCAL_MACHINE, "HKLM",
                     "SYSTEM\\CurrentControlSet\\Hardware Profiles\\Current\\Software\\Fonts", "LogPixels");
    LogRegistryDword(HKEY_LOCAL_MACHINE, "HKLM",
                     "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\FontDPI", "LogPixels");
    LogRegistryDword(HKEY_CURRENT_USER, "HKCU",
                     "Control Panel\\Desktop\\WindowMetrics", "AppliedDPI");
}

static void
LogDpi(void)
{
    DPI_AWARENESS_CONTEXT Context = NULL;
    HMONITOR Monitor;
    UINT DpiX, DpiY, Type;

    if (g_Api.GetThreadContext != NULL)
        Context = g_Api.GetThreadContext();
    Log("CONTEXT value=%p awareness=%d process_aware=%d\n",
        Context,
        g_Api.GetAwareness != NULL ? g_Api.GetAwareness(Context) : DPI_AWARENESS_INVALID,
        IsProcessDPIAware());
    if (g_Api.GetDpiForSystem != NULL)
        Log("DPI system=%u\n", g_Api.GetDpiForSystem());
    else
        Log("UNSUPPORTED GetDpiForSystem\n");
    if (g_Api.GetDpiForWindow != NULL)
        Log("DPI window=%u\n", g_Api.GetDpiForWindow(g_Window));
    else
        Log("UNSUPPORTED GetDpiForWindow\n");

    Monitor = MonitorFromWindow(g_Window, MONITOR_DEFAULTTOPRIMARY);
    for (Type = MDT_EFFECTIVE_DPI; Type <= MDT_RAW_DPI; Type++)
    {
        DpiX = DpiY = 0;
        if (g_Api.GetDpiForMonitorInternal != NULL)
        {
            SetLastError(ERROR_SUCCESS);
            if (g_Api.GetDpiForMonitorInternal(Monitor, Type, &DpiX, &DpiY))
                Log("DPI monitor_internal type=%u x=%u y=%u\n", Type, DpiX, DpiY);
            else
                Log("DPI monitor_internal type=%u error=%lu\n", Type, GetLastError());
        }
        else
        {
            Log("UNSUPPORTED GetDpiForMonitorInternal type=%u\n", Type);
        }

        DpiX = DpiY = 0;
        if (g_Api.GetDpiForMonitor != NULL)
        {
            HRESULT Result = g_Api.GetDpiForMonitor(Monitor, (MONITOR_DPI_TYPE)Type, &DpiX, &DpiY);
            Log("DPI shcore_monitor type=%u hr=0x%08lx x=%u y=%u\n", Type, Result, DpiX, DpiY);
        }
        else
        {
            Log("UNSUPPORTED SHCore GetDpiForMonitor type=%u\n", Type);
        }
    }
}

static void
LogDeviceCaps(void)
{
    static const struct
    {
        int Index;
        const char *Name;
    } Caps[] = {
        { LOGPIXELSX, "LOGPIXELSX" },
        { LOGPIXELSY, "LOGPIXELSY" },
        { HORZRES, "HORZRES" },
        { VERTRES, "VERTRES" },
        { DESKTOPHORZRES, "DESKTOPHORZRES" },
        { DESKTOPVERTRES, "DESKTOPVERTRES" },
        { BITSPIXEL, "BITSPIXEL" },
        { NUMCOLORS, "NUMCOLORS" }
    };
    HDC Dc = GetDC(NULL);
    UINT i;

    if (Dc == NULL)
    {
        Log("CAPS error=GetDC_failed\n");
        return;
    }
    for (i = 0; i < ARRAYSIZE(Caps); i++)
        Log("CAPS %s=%d\n", Caps[i].Name, GetDeviceCaps(Dc, Caps[i].Index));
    ReleaseDC(NULL, Dc);
}

static void
LogMetrics(void)
{
    static const struct
    {
        int Index;
        const char *Name;
    } Metrics[] = {
        { SM_CXICON, "SM_CXICON" },
        { SM_CYICON, "SM_CYICON" },
        { SM_CXSMICON, "SM_CXSMICON" },
        { SM_CYSMICON, "SM_CYSMICON" },
        { SM_CXCURSOR, "SM_CXCURSOR" },
        { SM_CYCURSOR, "SM_CYCURSOR" },
        { SM_CXFRAME, "SM_CXFRAME" },
        { SM_CYFRAME, "SM_CYFRAME" },
        { SM_CYCAPTION, "SM_CYCAPTION" },
        { SM_CXVSCROLL, "SM_CXVSCROLL" },
        { SM_CYHSCROLL, "SM_CYHSCROLL" },
        { SM_CXMINIMIZED, "SM_CXMINIMIZED" }
    };
    static const UINT Dpis[] = { 96, 120, 144, 192 };
    UINT i, j;

    for (i = 0; i < ARRAYSIZE(Metrics); i++)
    {
        Log("METRIC %s=%d\n", Metrics[i].Name, GetSystemMetrics(Metrics[i].Index));
        if (g_Api.GetMetricsForDpi != NULL)
        {
            for (j = 0; j < ARRAYSIZE(Dpis); j++)
                Log("METRIC_FOR_DPI %s dpi=%u value=%d\n",
                    Metrics[i].Name, Dpis[j], g_Api.GetMetricsForDpi(Metrics[i].Index, Dpis[j]));
        }
    }
    if (g_Api.GetMetricsForDpi == NULL)
        Log("UNSUPPORTED GetSystemMetricsForDpi\n");
}

static void
LogStockFont(int Stock, const char *Name)
{
    LOGFONTW Font;
    TEXTMETRICW Metrics;
    HDC Dc;
    HFONT OldFont;
    INT Result;

    ZeroMemory(&Font, sizeof(Font));
    Result = GetObjectW(GetStockObject(Stock), sizeof(Font), &Font);
    Log("FONT stock=%s get_object=%d height=%ld width=%ld weight=%ld charset=%u face=\"%ls\"\n",
        Name, Result, Font.lfHeight, Font.lfWidth, Font.lfWeight, Font.lfCharSet, Font.lfFaceName);

    Dc = CreateCompatibleDC(NULL);
    if (Dc == NULL)
        return;
    OldFont = (HFONT)SelectObject(Dc, GetStockObject(Stock));
    ZeroMemory(&Metrics, sizeof(Metrics));
    if (GetTextMetricsW(Dc, &Metrics))
        Log("FONT_METRICS stock=%s height=%ld ascent=%ld descent=%ld external_leading=%ld average_width=%ld\n",
            Name, Metrics.tmHeight, Metrics.tmAscent, Metrics.tmDescent,
            Metrics.tmExternalLeading, Metrics.tmAveCharWidth);
    SelectObject(Dc, OldFont);
    DeleteDC(Dc);
}

static void
LogFonts(void)
{
    static const UINT Dpis[] = { 96, 120, 144, 192 };
    NONCLIENTMETRICSW Metrics;
    LOGFONTW IconFont;
    UINT i;

    LogStockFont(DEFAULT_GUI_FONT, "DEFAULT_GUI_FONT");
    LogStockFont(SYSTEM_FONT, "SYSTEM_FONT");

    if (g_Api.SpiForDpi == NULL)
    {
        Log("UNSUPPORTED SystemParametersInfoForDpi\n");
        return;
    }
    for (i = 0; i < ARRAYSIZE(Dpis); i++)
    {
        ZeroMemory(&Metrics, sizeof(Metrics));
        Metrics.cbSize = sizeof(Metrics);
        if (g_Api.SpiForDpi(SPI_GETNONCLIENTMETRICS, sizeof(Metrics), &Metrics, 0, Dpis[i]))
        {
            Log("NONCLIENT_FOR_DPI dpi=%u caption_height=%ld border=%ld scroll_width=%ld scroll_height=%ld "
                "caption_font_height=%ld caption_font_width=%ld menu_font_height=%ld\n",
                Dpis[i], Metrics.iCaptionHeight, Metrics.iBorderWidth,
                Metrics.iScrollWidth, Metrics.iScrollHeight,
                Metrics.lfCaptionFont.lfHeight, Metrics.lfCaptionFont.lfWidth,
                Metrics.lfMenuFont.lfHeight);
        }
        else
        {
            Log("NONCLIENT_FOR_DPI dpi=%u error=%lu\n", Dpis[i], GetLastError());
        }

        ZeroMemory(&IconFont, sizeof(IconFont));
        if (g_Api.SpiForDpi(SPI_GETICONTITLELOGFONT, sizeof(IconFont), &IconFont, 0, Dpis[i]))
            Log("ICONFONT_FOR_DPI dpi=%u height=%ld width=%ld\n",
                Dpis[i], IconFont.lfHeight, IconFont.lfWidth);
        else
            Log("ICONFONT_FOR_DPI dpi=%u error=%lu\n", Dpis[i], GetLastError());
    }
}

static void
LogMode(const char *Kind, DWORD Index, const DEVMODEW *Mode)
{
    Log("DEVMODE kind=%s index=%lu size=%u fields=0x%08lx width=%lu height=%lu bpp=%lu frequency=%lu "
        "flags=0x%08lx position=%ld,%ld orientation=%lu log_pixels=%u\n",
        Kind, Index, Mode->dmSize, Mode->dmFields, Mode->dmPelsWidth, Mode->dmPelsHeight,
        Mode->dmBitsPerPel, Mode->dmDisplayFrequency, Mode->dmDisplayFlags,
        Mode->dmPosition.x, Mode->dmPosition.y, Mode->dmDisplayOrientation, Mode->dmLogPixels);
}

static BOOL
ReadCurrentMode(DEVMODEW *Mode)
{
    ZeroMemory(Mode, sizeof(*Mode));
    Mode->dmSize = sizeof(*Mode);
    return EnumDisplaySettingsExW(NULL, ENUM_CURRENT_SETTINGS, Mode, 0);
}

static void
LogDevmodes(BOOL IncludeIndexed)
{
    DEVMODEW Mode;
    DWORD Index;

    if (ReadCurrentMode(&Mode))
        LogMode("current", 0, &Mode);
    else
        Log("DEVMODE kind=current error=%lu\n", GetLastError());

    ZeroMemory(&Mode, sizeof(Mode));
    Mode.dmSize = sizeof(Mode);
    if (EnumDisplaySettingsExW(NULL, ENUM_REGISTRY_SETTINGS, &Mode, 0))
        LogMode("registry", 0, &Mode);
    else
        Log("DEVMODE kind=registry error=%lu\n", GetLastError());

    if (!IncludeIndexed)
        return;
    for (Index = 0; Index < D0_MAX_INDEXED_MODES; Index++)
    {
        ZeroMemory(&Mode, sizeof(Mode));
        Mode.dmSize = sizeof(Mode);
        if (!EnumDisplaySettingsExW(NULL, Index, &Mode, 0))
            break;
        LogMode("indexed", Index, &Mode);
    }
    Log("DEVMODE indexed_end index=%lu error=%lu\n", Index, GetLastError());
}

static void
RunModeTest(void)
{
    DEVMODEW Current, Request;
    LONG Result;

    if (!ReadCurrentMode(&Current))
        return;
    Request = Current;
    if (g_Options.TestWidth > 0 && g_Options.TestHeight > 0)
    {
        Request.dmPelsWidth = (DWORD)g_Options.TestWidth;
        Request.dmPelsHeight = (DWORD)g_Options.TestHeight;
        Request.dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT;
    }

    Result = ChangeDisplaySettingsExW(NULL, &Request, NULL, CDS_TEST, NULL);
    Log("CDS_TEST width=%lu height=%lu result=%ld\n",
        Request.dmPelsWidth, Request.dmPelsHeight, Result);
    if (!g_Options.Apply || Result != DISP_CHANGE_SUCCESSFUL)
        return;

    Result = ChangeDisplaySettingsExW(NULL, &Request, NULL, CDS_UPDATEREGISTRY, NULL);
    Log("CDS_APPLY width=%lu height=%lu result=%ld\n",
        Request.dmPelsWidth, Request.dmPelsHeight, Result);
    if (g_Options.Restore)
    {
        Result = ChangeDisplaySettingsExW(NULL, &Current, NULL, CDS_UPDATEREGISTRY, NULL);
        Log("CDS_RESTORE width=%lu height=%lu result=%ld\n",
            Current.dmPelsWidth, Current.dmPelsHeight, Result);
    }
}

static void
PumpMessages(DWORD WaitMs)
{
    DWORD Started = GetTickCount();
    MSG Message;

    for (;;)
    {
        while (PeekMessageW(&Message, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&Message);
            DispatchMessageW(&Message);
        }
        if (WaitMs == 0 || GetTickCount() - Started >= WaitMs)
            break;
        Sleep(20);
    }
}

static LRESULT CALLBACK
ProbeWindowProc(HWND Window, UINT Message, WPARAM wParam, LPARAM lParam)
{
    if (Message == WM_DISPLAYCHANGE)
    {
        Log("MESSAGE WM_DISPLAYCHANGE tick=%lu bpp=%lu width=%u height=%u\n",
            GetTickCount(), (DWORD)wParam, LOWORD(lParam), HIWORD(lParam));
        return 0;
    }
    if (Message == WM_DPICHANGED)
    {
        RECT *Suggested = (RECT *)lParam;
        Log("MESSAGE WM_DPICHANGED tick=%lu dpi_x=%u dpi_y=%u suggested=%ld,%ld,%ld,%ld\n",
            GetTickCount(), LOWORD(wParam), HIWORD(wParam),
            Suggested->left, Suggested->top, Suggested->right, Suggested->bottom);
        return 0;
    }
    if (Message == WM_SETTINGCHANGE)
    {
        Log("MESSAGE WM_SETTINGCHANGE tick=%lu action=%lu area=\"%ls\"\n",
            GetTickCount(), (DWORD)wParam, lParam ? (LPCWSTR)lParam : L"");
        return 0;
    }
    return DefWindowProcW(Window, Message, wParam, lParam);
}

static BOOL
CreateProbeWindow(HINSTANCE Instance)
{
    WNDCLASSW Class;
    ATOM Atom;

    ZeroMemory(&Class, sizeof(Class));
    Class.lpfnWndProc = ProbeWindowProc;
    Class.hInstance = Instance;
    Class.lpszClassName = L"LiberNTD0DpiProbe";
    Class.hCursor = LoadCursor(NULL, IDC_ARROW);
    Class.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    Atom = RegisterClassW(&Class);
    if (Atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return FALSE;

    g_Window = CreateWindowExW(0, Class.lpszClassName, L"D0 DPI/mode probe",
                               WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                               480, 320, NULL, NULL, Instance, NULL);
    if (g_Window == NULL)
        return FALSE;
    ShowWindow(g_Window, SW_SHOWNORMAL);
    UpdateWindow(g_Window);
    return TRUE;
}

static void
LogSnapshot(const char *Name, BOOL IncludeIndexed)
{
    Log("SNAPSHOT name=%s variant=%s phase=%s\n", Name, g_Options.Variant, g_Options.Phase);
    LogRegistry();
    LogDpi();
    LogDeviceCaps();
    LogMetrics();
    LogFonts();
    LogDevmodes(IncludeIndexed);
}

int
main(int argc, char **argv)
{
    BOOL SetResult = FALSE;
    DWORD SetError = ERROR_SUCCESS;

    if (!ParseOptions(argc, argv))
    {
        fprintf(stderr,
                "usage: dpiprobe --variant=unaware|system|permonitor|permonitorv2 "
                "--phase=cold|live --log=PATH [--wait-ms=N] "
                "[--test-width=W --test-height=H [--apply [--restore]]]\n");
        return 2;
    }

    g_Log = fopen(g_Options.LogPath, "w");
    if (g_Log == NULL)
    {
        fprintf(stderr, "cannot open log: %s\n", g_Options.LogPath);
        return 2;
    }

    Log("D0_PROBE_BEGIN variant=%s phase=%s\n", g_Options.Variant, g_Options.Phase);
    LoadApis();
    if (g_Api.SetProcessContext != NULL)
    {
        SetResult = g_Api.SetProcessContext(RequestedContext());
        SetError = GetLastError();
    }
    Log("AWARENESS requested=%s set_result=%d error=%lu\n",
        g_Options.Variant, SetResult, SetError);
    if (g_Api.SetProcessContext == NULL)
    {
        Log("D0_PROBE_END rc=3 reason=SetProcessDpiAwarenessContext_missing\n");
        fclose(g_Log);
        return 3;
    }

    if (!CreateProbeWindow(GetModuleHandleW(NULL)))
    {
        Log("D0_PROBE_END rc=4 reason=CreateWindow_failed error=%lu\n", GetLastError());
        fclose(g_Log);
        return 4;
    }

    LogSnapshot("initial", TRUE);
    RunModeTest();
    PumpMessages(g_Options.WaitMs);
    if (g_Options.WaitMs != 0 || g_Options.Apply)
        LogSnapshot("final", TRUE);

    Log("D0_PROBE_END rc=0\n");
    DestroyWindow(g_Window);
    fclose(g_Log);
    return 0;
}
