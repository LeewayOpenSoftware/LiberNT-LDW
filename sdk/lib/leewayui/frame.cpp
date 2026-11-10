/*
 * PROJECT:     LeewayUI
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     G7 frame template: chrome (ribbon | pill), status bar, zoom slider, layout
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 * PROVENANCE:  sdk/lib/leewayui/LEEWAYUI-PROVENANCE.md
 */

#include <windows.h>
#include <commctrl.h>
#include <strsafe.h>
#include <leewayui/frame.h>
#include <leewayui/dpi.h>

static inline int FrMin(int a, int b) { return a < b ? a : b; }
static inline int FrMax(int a, int b) { return a > b ? a : b; }

#define FRAME_MAX_ZOOM_LEVELS   32
#define FRAME_STATUS_H          22      /* logical px */
#define FRAME_ZOOMTEXT_W        56
#define FRAME_SLIDER_W          140
#define FRAME_GRIP_W            16
#define FRAME_RIBBON_FALLBACK_H 120     /* only if the ribbon reports height 0 */

struct LeewayUIFrame {
    LeewayUIFrameDesc desc;
    UINT32 levels[FRAME_MAX_ZOOM_LEVELS];
    LeewayUIChromeStyle chrome;
    ILeewayRibbon *ribbon;
    HWND status;
    HWND slider;
    BOOL shown;
    BOOL syncing;               /* SetZoom in progress: do not call on_zoom */
    UINT32 zoom;
    UINT32 dpi;
};

/* ---- the only functions that call into ribbon.h ---- */

static ILeewayRibbon *RibbonCreate(const LeewayUIFrameDesc *d)
{
    ILeewayRibbon *r = NULL;
    if (!d->commands || !d->command_count || !d->execute || !d->state)
        return NULL;
    if (FAILED(LeewayRibbonCreate(d->owner, d->commands, d->command_count,
                                  d->execute, d->state, d->app_context, &r)))
        return NULL;
    return r;
}

static HWND RibbonWindow(ILeewayRibbon *r)
{
    return r ? LeewayRibbonGetHWND(r) : NULL;
}

static void RibbonDestroy(ILeewayRibbon *r)
{
    if (r)
        LeewayRibbonDestroy(r);
}

static int RibbonHeight(ILeewayRibbon *r, UINT32 dpi)
{
    UINT32 h = r ? LeewayRibbonGetHeight(r) : 0;
    return h ? (int)h : LeewayDpiScale(dpi, FRAME_RIBBON_FALLBACK_H);
}

/* ---- registry ---- */

static BOOL KeyPath(const WCHAR *app, WCHAR *out, size_t cch)
{
    if (!app || !*app)
        return FALSE;
    return SUCCEEDED(StringCchPrintfW(out, cch, L"Software\\Leeway\\%s", app));
}

LeewayUIChromeStyle WINAPI LeewayFrameReadChromeStyle(const WCHAR *app, LeewayUIChromeStyle def)
{
    WCHAR path[256], val[16];
    DWORD type = 0, cb = sizeof(val) - sizeof(WCHAR);
    HKEY k;
    LeewayUIChromeStyle r = def;

    if (!KeyPath(app, path, 256))
        return def;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return def;
    ZeroMemory(val, sizeof(val));
    if (RegQueryValueExW(k, L"ChromeStyle", NULL, &type, (LPBYTE)val, &cb) == ERROR_SUCCESS && type == REG_SZ)
    {
        if (!lstrcmpiW(val, L"ribbon"))
            r = LWUI_CHROME_RIBBON;
        else if (!lstrcmpiW(val, L"pill"))
            r = LWUI_CHROME_PILL;
    }
    RegCloseKey(k);
    return r;
}

BOOL WINAPI LeewayFrameWriteChromeStyle(const WCHAR *app, LeewayUIChromeStyle style)
{
    WCHAR path[256];
    const WCHAR *s = (style == LWUI_CHROME_RIBBON) ? L"ribbon" : L"pill";
    HKEY k;
    LONG e;

    if (!KeyPath(app, path, 256))
        return FALSE;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS)
        return FALSE;
    e = RegSetValueExW(k, L"ChromeStyle", 0, REG_SZ, (const BYTE *)s, (lstrlenW(s) + 1) * sizeof(WCHAR));
    RegCloseKey(k);
    return e == ERROR_SUCCESS;
}

static BOOL ReadStatusShown(const WCHAR *app)
{
    WCHAR path[256];
    DWORD v = 1, cb = sizeof(v), type = 0;
    HKEY k;

    if (!KeyPath(app, path, 256))
        return TRUE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, &k) == ERROR_SUCCESS)
    {
        if (RegQueryValueExW(k, L"StatusBar", NULL, &type, (LPBYTE)&v, &cb) != ERROR_SUCCESS || type != REG_DWORD)
            v = 1;
        RegCloseKey(k);
    }
    return v != 0;
}

static void WriteStatusShown(const WCHAR *app, BOOL shown)
{
    WCHAR path[256];
    DWORD v = shown ? 1 : 0;
    HKEY k;

    if (!KeyPath(app, path, 256))
        return;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS)
    {
        RegSetValueExW(k, L"StatusBar", 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
        RegCloseKey(k);
    }
}

/* ---- zoom helpers ---- */

static UINT32 NearestLevel(const LeewayUIFrame *f, UINT32 percent)
{
    UINT32 i, best = 0, bd = 0xFFFFFFFF;
    for (i = 0; i < f->desc.zoom_level_count; ++i)
    {
        UINT32 l = f->levels[i];
        UINT32 d = l > percent ? l - percent : percent - l;
        if (d < bd)
        {
            bd = d;
            best = i;
        }
    }
    return best;
}

static void UpdateZoomText(LeewayUIFrame *f)
{
    WCHAR t[16];
    if (!f->status)
        return;
    StringCchPrintfW(t, 16, L"%u%%", f->zoom);
    SendMessageW(f->status, SB_SETTEXTW, 1, (LPARAM)t);
}

/* ---- create / destroy ---- */

static BOOL ValidDesc(const LeewayUIFrameDesc *d)
{
    UINT32 i;
    if (!d || d->struct_size != sizeof(*d) || !d->owner || !d->app_name || !*d->app_name)
        return FALSE;
    if (d->flags & LWUI_FRAME_ZOOM_SLIDER)
    {
        if (!(d->flags & LWUI_FRAME_STATUSBAR) || !d->zoom_levels ||
            !d->zoom_level_count || d->zoom_level_count > FRAME_MAX_ZOOM_LEVELS)
            return FALSE;
        for (i = 1; i < d->zoom_level_count; ++i)
            if (d->zoom_levels[i] <= d->zoom_levels[i - 1])
                return FALSE;
    }
    return TRUE;
}

HRESULT WINAPI LeewayFrameCreate(const LeewayUIFrameDesc *desc, LeewayUIFrame **out)
{
    LeewayUIFrame *f;
    INITCOMMONCONTROLSEX icc;
    UINT32 i;

    if (!out)
        return E_POINTER;
    *out = NULL;
    if (!ValidDesc(desc))
        return E_INVALIDARG;

    f = (LeewayUIFrame *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*f));
    if (!f)
        return E_OUTOFMEMORY;
    f->desc = *desc;
    for (i = 0; i < desc->zoom_level_count; ++i)
        f->levels[i] = desc->zoom_levels[i];
    f->desc.zoom_levels = f->levels;    /* own a copy */
    f->dpi = LeewayDpiGetForWindow(desc->owner);
    f->zoom = desc->zoom_level_count ? 100 : 0;

    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icc);

    f->chrome = LeewayFrameReadChromeStyle(desc->app_name, desc->default_chrome);
    if (f->chrome == LWUI_CHROME_RIBBON)
    {
        f->ribbon = RibbonCreate(&f->desc);
        if (!f->ribbon)
            f->chrome = LWUI_CHROME_PILL;       /* fall back, app keeps its toolbar */
    }

    f->shown = ReadStatusShown(desc->app_name);
    if (desc->flags & LWUI_FRAME_STATUSBAR)
    {
        f->status = CreateWindowExW(0, STATUSCLASSNAMEW, NULL, WS_CHILD | SBARS_SIZEGRIP | CCS_NOPARENTALIGN |
                                    CCS_NORESIZE | WS_CLIPSIBLINGS | (f->shown ? WS_VISIBLE : 0),
                                    0, 0, 0, 0, desc->owner, NULL, desc->instance, NULL);
        if (!f->status)
        {
            RibbonDestroy(f->ribbon);
            HeapFree(GetProcessHeap(), 0, f);
            return HRESULT_FROM_WIN32(GetLastError());
        }
        if (desc->flags & LWUI_FRAME_ZOOM_SLIDER)
        {
            f->slider = CreateWindowExW(0, TRACKBAR_CLASSW, NULL,
                                        WS_CHILD | TBS_HORZ | TBS_AUTOTICKS | WS_CLIPSIBLINGS |
                                        (f->shown ? WS_VISIBLE : 0),
                                        0, 0, 0, 0, desc->owner, NULL, desc->instance, NULL);
            if (f->slider)
            {
                SendMessageW(f->slider, TBM_SETRANGE, TRUE, MAKELONG(0, desc->zoom_level_count - 1));
                SendMessageW(f->slider, TBM_SETPAGESIZE, 0, 1);
                SendMessageW(f->slider, TBM_SETLINESIZE, 0, 1);
                SendMessageW(f->slider, TBM_SETTICFREQ, 1, 0);
            }
        }
    }
    LeewayFrameSetZoom(f, f->zoom);
    *out = f;
    return S_OK;
}

void WINAPI LeewayFrameDestroy(LeewayUIFrame *f)
{
    if (!f)
        return;
    if (f->slider)
        DestroyWindow(f->slider);
    if (f->status)
        DestroyWindow(f->status);
    RibbonDestroy(f->ribbon);
    HeapFree(GetProcessHeap(), 0, f);
}

LeewayUIChromeStyle WINAPI LeewayFrameGetChrome(const LeewayUIFrame *f)
{
    return f ? f->chrome : LWUI_CHROME_PILL;
}

ILeewayRibbon * WINAPI LeewayFrameGetRibbon(LeewayUIFrame *f)
{
    return f ? f->ribbon : NULL;
}

/* ---- layout ---- */

void WINAPI LeewayFrameLayout(LeewayUIFrame *f, const RECT *client, LeewayUIFrameRects *out)
{
    RECT c, chrome, status, work;
    int h;

    if (!f || !client || !out)
        return;
    c = *client;
    SetRectEmpty(&status);

    /* chrome */
    chrome = c;
    h = (f->chrome == LWUI_CHROME_RIBBON) ? RibbonHeight(f->ribbon, f->dpi)
                                          : LeewayDpiScale(f->dpi, (int)f->desc.pill_height);
    chrome.bottom = FrMin(c.bottom, c.top + h);
    if (f->chrome == LWUI_CHROME_RIBBON)
    {
        HWND rw = RibbonWindow(f->ribbon);
        if (rw)
            SetWindowPos(rw, NULL, chrome.left, chrome.top, chrome.right - chrome.left,
                         chrome.bottom - chrome.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    /* status bar and slider */
    work = c;
    work.top = chrome.bottom;
    if (f->status)
    {
        if (f->shown)
        {
            int sh = LeewayDpiScale(f->dpi, FRAME_STATUS_H);
            int zw = LeewayDpiScale(f->dpi, FRAME_ZOOMTEXT_W);
            int sw = LeewayDpiScale(f->dpi, FRAME_SLIDER_W);
            int gw = LeewayDpiScale(f->dpi, FRAME_GRIP_W);
            int sliderLeft = FrMax(c.left, c.right - gw - sw);
            int edges[3];

            status = c;
            status.top = FrMax(chrome.bottom, c.bottom - sh);
            work.bottom = status.top;
            SetWindowPos(f->status, HWND_TOP, status.left, status.top, status.right - status.left,
                         status.bottom - status.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
            edges[1] = FrMax(0, sliderLeft - c.left - 4);
            edges[0] = FrMax(0, edges[1] - zw);
            edges[2] = -1;
            if (!f->slider)
                edges[1] = -1;
            SendMessageW(f->status, SB_SETPARTS, f->slider ? 3 : 2, (LPARAM)edges);
            UpdateZoomText(f);
            if (f->slider)
                SetWindowPos(f->slider, HWND_TOP, sliderLeft, status.top, sw, status.bottom - status.top,
                             SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        else
        {
            ShowWindow(f->status, SW_HIDE);
            if (f->slider)
                ShowWindow(f->slider, SW_HIDE);
        }
    }
    if (work.bottom < work.top)
        work.bottom = work.top;
    out->chrome = chrome;
    out->status = status;
    out->work = work;
}

/* ---- status bar ---- */

BOOL WINAPI LeewayFrameShowStatusBar(LeewayUIFrame *f, BOOL show, BOOL persist)
{
    if (!f || !f->status)
        return FALSE;
    show = show ? TRUE : FALSE;
    if (persist)
        WriteStatusShown(f->desc.app_name, show);
    if (f->shown == show)
        return TRUE;
    f->shown = show;
    ShowWindow(f->status, show ? SW_SHOW : SW_HIDE);
    if (f->slider)
        ShowWindow(f->slider, show ? SW_SHOW : SW_HIDE);
    return TRUE;   /* caller re-runs LeewayFrameLayout */
}

BOOL WINAPI LeewayFrameIsStatusBarShown(const LeewayUIFrame *f)
{
    return f && f->status && f->shown;
}

void WINAPI LeewayFrameSetStatusText(LeewayUIFrame *f, UINT32 part, const WCHAR *text)
{
    if (f && f->status && part < 2)
        SendMessageW(f->status, SB_SETTEXTW, part, (LPARAM)(text ? text : L""));
}

HWND WINAPI LeewayFrameGetStatusWindow(const LeewayUIFrame *f)
{
    return f ? f->status : NULL;
}

/* ---- zoom ---- */

void WINAPI LeewayFrameSetZoom(LeewayUIFrame *f, UINT32 percent)
{
    if (!f)
        return;
    f->zoom = percent;
    f->syncing = TRUE;
    if (f->slider && f->desc.zoom_level_count)
        SendMessageW(f->slider, TBM_SETPOS, TRUE, NearestLevel(f, percent));
    UpdateZoomText(f);
    f->syncing = FALSE;
}

UINT32 WINAPI LeewayFrameGetZoom(const LeewayUIFrame *f)
{
    return f ? f->zoom : 0;
}

void WINAPI LeewayFrameEnableZoom(LeewayUIFrame *f, BOOL enable)
{
    if (f && f->slider)
        EnableWindow(f->slider, enable);
}

/* ---- messages ---- */

BOOL WINAPI LeewayFrameHandleMessage(LeewayUIFrame *f, UINT msg, WPARAM wp, LPARAM lp, LRESULT *result)
{
    if (!f)
        return FALSE;
    if (result)
        *result = 0;

    switch (msg)
    {
    case WM_HSCROLL:
        if (f->slider && (HWND)lp == f->slider && !f->syncing)
        {
            UINT32 idx = (UINT32)SendMessageW(f->slider, TBM_GETPOS, 0, 0);
            if (idx >= f->desc.zoom_level_count)
                idx = f->desc.zoom_level_count - 1;
            if (f->levels[idx] != f->zoom)
            {
                f->zoom = f->levels[idx];
                UpdateZoomText(f);
                if (f->desc.on_zoom)
                    f->desc.on_zoom(f->zoom, f->desc.app_context);
            }
            return TRUE;
        }
        return FALSE;

    case WM_DPICHANGED:
        f->dpi = HIWORD(wp);
        if (f->ribbon)
            LeewayRibbonSetDPI(f->ribbon, f->dpi);
        return FALSE;   /* the app still applies the suggested window rect */

    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE:
        if (f->ribbon)
        {
            HWND rw = RibbonWindow(f->ribbon);
            if (rw)
                InvalidateRect(rw, NULL, TRUE);
        }
        if (f->status)
            InvalidateRect(f->status, NULL, TRUE);
        return FALSE;
    }
    return FALSE;
}
