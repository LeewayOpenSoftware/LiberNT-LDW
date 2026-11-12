/*
 * Wordpad implementation - Printing and print preview functions
 *
 * Copyright 2007-2008 by Alexander N. Sørnes <alex@thehandofagony.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <windows.h>
#include <richedit.h>
#include <commctrl.h>
#include <commdlg.h>

#include "wordpad.h"
#include <leewayui/print.h>

static HGLOBAL devMode;
static HGLOBAL devNames;

static RECT margins;

static ILeewayPrintPreview *g_preview;
static LeewayPrintSettings g_print_settings;
static BOOL g_print_settings_ready;
static int *g_pageEnds;
static UINT32 g_pageEndsCount, g_pageEndsCap;

static UINT32 preview_measure_pages(void);

void registry_set_pagemargins(HKEY hKey)
{
    RegSetValueExW(hKey, L"PageMargin", 0, REG_BINARY, (LPBYTE)&margins, sizeof(RECT));
}

void registry_read_pagemargins(HKEY hKey)
{
    DWORD size = sizeof(RECT);

    if(!hKey || RegQueryValueExW(hKey, L"PageMargin", 0, NULL, (LPBYTE)&margins,
                     &size) != ERROR_SUCCESS || size != sizeof(RECT))
        SetRect(&margins, 1757, 1417, 1757, 1417);
}

static HDC make_dc(void)
{
    if(devNames && devMode)
    {
        LPDEVNAMES dn = GlobalLock(devNames);
        LPDEVMODEW dm = GlobalLock(devMode);
        HDC ret;

        ret = CreateDCW((LPWSTR)dn + dn->wDriverOffset,
                         (LPWSTR)dn + dn->wDeviceOffset, 0, dm);

        GlobalUnlock(dn);
        GlobalUnlock(dm);

        return ret;
    } else
    {
        return 0;
    }
}

static LONG twips_to_centmm(int twips)
{
    return MulDiv(twips, CENTMM_PER_INCH, TWIPS_PER_INCH);
}

static LONG centmm_to_twips(int mm)
{
    return MulDiv(mm, TWIPS_PER_INCH, CENTMM_PER_INCH);
}

static LONG twips_to_pixels(int twips, int dpi)
{
    return MulDiv(twips, dpi, TWIPS_PER_INCH);
}

static LONG devunits_to_twips(int units, int dpi)
{
    return MulDiv(units, TWIPS_PER_INCH, dpi);
}


static RECT get_print_rect(HDC hdc)
{
    RECT rc;
    int width, height;

    if(hdc)
    {
        int dpiY = GetDeviceCaps(hdc, LOGPIXELSY);
        int dpiX = GetDeviceCaps(hdc, LOGPIXELSX);
        width = devunits_to_twips(GetDeviceCaps(hdc, PHYSICALWIDTH), dpiX);
        height = devunits_to_twips(GetDeviceCaps(hdc, PHYSICALHEIGHT), dpiY);
    } else
    {
        width = centmm_to_twips(18500);
        height = centmm_to_twips(27000);
    }

    SetRect(&rc, margins.left, margins.top, width - margins.right, height - margins.bottom);

    return rc;
}

void target_device(HWND hMainWnd, DWORD wordWrap)
{
    HWND hEditorWnd = GetDlgItem(hMainWnd, IDC_EDITOR);

    if(wordWrap == ID_WORDWRAP_MARGIN)
    {
        int width = 0;
        LRESULT result;
        HDC hdc = make_dc();
        RECT rc = get_print_rect(hdc);

        width = rc.right - rc.left;
        if(!hdc)
        {
            HDC hMaindc = GetDC(hMainWnd);
            hdc = CreateCompatibleDC(hMaindc);
            ReleaseDC(hMainWnd, hMaindc);
        }
        result = SendMessageW(hEditorWnd, EM_SETTARGETDEVICE, (WPARAM)hdc, width);
        DeleteDC(hdc);
        if (result)
            return;
        /* otherwise EM_SETTARGETDEVICE failed, so fall back on wrapping
         * to window using the NULL DC. */
    }

    if (wordWrap != ID_WORDWRAP_NONE) {
        SendMessageW(hEditorWnd, EM_SETTARGETDEVICE, 0, 0);
    } else {
        SendMessageW(hEditorWnd, EM_SETTARGETDEVICE, 0, 1);
    }

}

static void char_from_pagenum(HWND hEditorWnd, FORMATRANGE *fr, int page)
{
    int i;

    fr->chrg.cpMin = 0;

    for(i = 1; i < page; i++)
    {
        int bottom = fr->rc.bottom;
        fr->chrg.cpMin = SendMessageW(hEditorWnd, EM_FORMATRANGE, FALSE, (LPARAM)fr);
        fr->rc.bottom = bottom;
    }
}

static HWND get_ruler_wnd(HWND hMainWnd)
{
    return GetDlgItem(hMainWnd, IDC_RULER);
}

void redraw_ruler(HWND hRulerWnd)
{
    RECT rc;

    GetClientRect(hRulerWnd, &rc);
    InvalidateRect(hRulerWnd, &rc, TRUE);
}

int get_ruler_unit(void)
{
    return (int)registry_get_ruler_unit();
}

void set_ruler_unit(int unit)
{
    registry_set_ruler_unit((DWORD)unit);
}

static void update_ruler(HWND hRulerWnd)
{
     SendMessageW(hRulerWnd, WM_USER, 0, 0);
     redraw_ruler(hRulerWnd);
}

static void add_ruler_units(HDC hdcRuler, RECT* drawRect, BOOL NewMetrics, LONG EditLeftmost)
{
    static HDC hdc;
    static int cachedUnit = -1;

    if(NewMetrics || cachedUnit != get_ruler_unit())
    {
        static HBITMAP hBitmap;
        int i, s, x, y, RulerTextEnd;
        int UnitPixels, SubPixels, SubCount;
        LONG UnitTwips;
        HFONT hFont;
        int unit = get_ruler_unit();

        if(hdc)
        {
            DeleteDC(hdc);
            DeleteObject(hBitmap);
        }

        hdc = CreateCompatibleDC(0);

        switch (unit)
        {
        default:
        case UNIT_CM:
            UnitTwips = centmm_to_twips(1000);
            SubCount = 4;
            break;
        case UNIT_INCH:
            UnitTwips = 1440;
            SubCount = 8;
            break;
        case UNIT_PT:
            UnitTwips = 720;
            SubCount = 12;
            break;
        case UNIT_PICA:
            UnitTwips = 1440;
            SubCount = 6;
            break;
        }

        UnitPixels = twips_to_pixels(UnitTwips, GetDeviceCaps(hdc, LOGPIXELSX));
        SubPixels = (int)((float)UnitPixels / (float)SubCount);
        if (SubPixels < 1)
            SubPixels = 1;

        hBitmap = CreateCompatibleBitmap(hdc, drawRect->right, drawRect->bottom);
        SelectObject(hdc, hBitmap);
        FillRect(hdc, drawRect, GetStockObject(WHITE_BRUSH));

        hFont = CreateFontW(10, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, L"MS Sans Serif");

        SelectObject(hdc, hFont);
        SetBkMode(hdc, TRANSPARENT);
        SetTextAlign(hdc, TA_CENTER);
        y = (int)(((float)drawRect->bottom - (float)drawRect->top) / 2.0) + 1;
        RulerTextEnd = drawRect->right - EditLeftmost + 1;
        for(i = 1, x = EditLeftmost; x < RulerTextEnd; i++)
        {
            WCHAR str[6];
            int x2 = x;

            for (s = 1; s < SubCount; s++)
            {
                x2 = x + SubPixels * s;
                if (x2 > RulerTextEnd)
                    break;

                if (s == SubCount / 2)
                {
                    MoveToEx(hdc, x2, y - 3, NULL);
                    LineTo(hdc, x2, y + 3);
                } else
                {
                    MoveToEx(hdc, x2, y, NULL);
                    LineTo(hdc, x2, y + 2);
                }
            }

            x += UnitPixels;
            if(x > RulerTextEnd)
                break;

            if (unit == UNIT_PT)
                wsprintfW(str, L"%d", i * 36);
            else if (unit == UNIT_PICA)
                wsprintfW(str, L"%d", i * 6);
            else
                wsprintfW(str, L"%d", i);
            TextOutW(hdc, x, 5, str, lstrlenW(str));
        }
        DeleteObject(hFont);
        cachedUnit = unit;
    }

    BitBlt(hdcRuler, 0, 0, drawRect->right, drawRect->bottom, hdc, 0, 0, SRCAND);
}

static void paint_ruler(HWND hWnd, LONG EditLeftmost, BOOL NewMetrics)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hWnd, &ps);
    HDC hdcPrint = make_dc();
    RECT printRect = get_print_rect(hdcPrint);
    RECT drawRect;

    GetClientRect(hWnd, &drawRect);
    FillRect(hdc, &drawRect, GetSysColorBrush(COLOR_MENU));

    InflateRect(&drawRect, 0, -3);
    drawRect.left = EditLeftmost;
    drawRect.right = twips_to_pixels(printRect.right - margins.left, GetDeviceCaps(hdc, LOGPIXELSX));
    FillRect(hdc, &drawRect, GetStockObject(WHITE_BRUSH));

    drawRect.top--;
    drawRect.bottom++;
    DrawEdge(hdc, &drawRect, EDGE_SUNKEN, BF_RECT);

    drawRect.left = drawRect.right - 1;
    drawRect.right = twips_to_pixels(printRect.right + margins.right - margins.left, GetDeviceCaps(hdc, LOGPIXELSX));
    DrawEdge(hdc, &drawRect, EDGE_ETCHED, BF_RECT);

    drawRect.left = 0;
    drawRect.top = 0;
    add_ruler_units(hdc, &drawRect, NewMetrics, EditLeftmost);

    SelectObject(hdc, GetStockObject(BLACK_BRUSH));
    DeleteDC(hdcPrint);
    EndPaint(hWnd, &ps);
}

LRESULT CALLBACK ruler_proc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    static WNDPROC pPrevRulerProc;
    static LONG EditLeftmost;
    static BOOL NewMetrics;

    switch(msg)
    {
        case WM_USER:
            if(wParam)
            {
                EditLeftmost = ((POINTL*)wParam)->x;
                pPrevRulerProc = (WNDPROC)lParam;
            }
            NewMetrics = TRUE;
            break;

        case WM_PAINT:
            paint_ruler(hWnd, EditLeftmost, NewMetrics);
            break;

        default:
            return CallWindowProcW(pPrevRulerProc, hWnd, msg, wParam, lParam);
    }

    return 0;
}

static void print_pages(HDC hdc, const LeewayPrintRange *range, LPWSTR wszFileName)
{
    FORMATRANGE fr;
    DOCINFOW di;
    GETTEXTLENGTHEX gt = {GTL_DEFAULT, 1200};
    UINT32 printedPages = 0;

    ZeroMemory(&di, sizeof(di));
    di.cbSize = sizeof(di);
    di.lpszDocName = wszFileName;

    ZeroMemory(&fr, sizeof(fr));
    fr.hdc = hdc;
    fr.hdcTarget = hdc;
    fr.rc = get_print_rect(hdc);
    SetRect(&fr.rcPage, 0, 0, fr.rc.right + margins.right, fr.rc.bottom + margins.bottom);

    if(range->selection)
    {
        SendMessageW(hEditorWnd, EM_EXGETSEL, 0, (LPARAM)&fr.chrg);
    } else
    {
        fr.chrg.cpMin = 0;
        fr.chrg.cpMax = SendMessageW(hEditorWnd, EM_GETTEXTLENGTHEX, (WPARAM)&gt, 0);

        if(range->from_page > 1)
            char_from_pagenum(hEditorWnd, &fr, range->from_page);
    }

    if(StartDocW(hdc, &di) <= 0)
        return;
    while(fr.chrg.cpMin < fr.chrg.cpMax)
    {
        LONG cp;

        if(StartPage(hdc) <= 0)
            break;
        cp = SendMessageW(hEditorWnd, EM_FORMATRANGE, TRUE, (LPARAM)&fr);
        if(EndPage(hdc) <= 0)
            break;

        printedPages++;
        if(cp <= fr.chrg.cpMin)
            break;
        fr.chrg.cpMin = cp;
        if(range->to_page && printedPages >= range->to_page - range->from_page + 1)
            break;
    }
    EndDoc(hdc);
    SendMessageW(hEditorWnd, EM_FORMATRANGE, FALSE, 0);
}

static void ensure_print_settings(void)
{
    if(g_print_settings_ready)
        return;
    LeewayPrintSettingsInitDefault(&g_print_settings);
    if(devMode)
    {
        if(g_print_settings.dev_mode) GlobalFree(g_print_settings.dev_mode);
        g_print_settings.dev_mode = devMode;
    }
    if(devNames)
    {
        if(g_print_settings.dev_names) GlobalFree(g_print_settings.dev_names);
        g_print_settings.dev_names = devNames;
    }
    SetRect(&g_print_settings.margins_cmm,
            twips_to_centmm(margins.left), twips_to_centmm(margins.top),
            twips_to_centmm(margins.right), twips_to_centmm(margins.bottom));
    g_print_settings_ready = TRUE;
}

static void print_settings_write_back(void)
{
    devMode = g_print_settings.dev_mode;
    devNames = g_print_settings.dev_names;
    SetRect(&margins,
            centmm_to_twips(g_print_settings.margins_cmm.left),
            centmm_to_twips(g_print_settings.margins_cmm.top),
            centmm_to_twips(g_print_settings.margins_cmm.right),
            centmm_to_twips(g_print_settings.margins_cmm.bottom));
}

void dialog_printsetup(HWND hMainWnd)
{
    ensure_print_settings();
    if(LeewayPageSetupDialog(hMainWnd, &g_print_settings) == S_OK)
    {
        print_settings_write_back();
        update_ruler(get_ruler_wnd(hMainWnd));
    }
}

void get_default_printer_opts(void)
{
    PRINTDLGW pd;
    ZeroMemory(&pd, sizeof(pd));

    ZeroMemory(&pd, sizeof(pd));
    pd.lStructSize = sizeof(pd);
    pd.Flags = PD_RETURNDC | PD_RETURNDEFAULT;
    pd.hDevMode = devMode;

    PrintDlgW(&pd);

    devMode = pd.hDevMode;
    devNames = pd.hDevNames;
}

void print_quick(HWND hMainWnd, LPWSTR wszFileName)
{
    LeewayPrintRange range;
    HDC dc;

    ensure_print_settings();
    dc = LeewayPrintSettingsCreateDC(&g_print_settings);
    if(!dc)
        return;

    ZeroMemory(&range, sizeof(range));
    range.all_pages = TRUE;
    print_pages(dc, &range, wszFileName);
    DeleteDC(dc);
}

void dialog_print(HWND hMainWnd, LPWSTR wszFileName)
{
    LeewayPrintRange range;
    HDC dc = NULL;
    HRESULT hr;

    ensure_print_settings();
    ZeroMemory(&range, sizeof(range));
    range.min_page = 1;
    range.max_page = preview_measure_pages();

    hr = LeewayPrintDialog(hMainWnd, &g_print_settings, &range, &dc);
    if(hr != S_OK || !dc)
        return;

    print_settings_write_back();
    print_pages(dc, &range, wszFileName);
    DeleteDC(dc);
    update_ruler(get_ruler_wnd(hMainWnd));
}

static UINT32 preview_measure_pages(void)
{
    FORMATRANGE fr;
    GETTEXTLENGTHEX gt = {GTL_DEFAULT, 1200};
    HDC hdcTarget, hdc;
    HBITMAP bm;
    LONG cp, cpMax;
    UINT32 pages = 1;

    hdcTarget = LeewayPrintSettingsCreateIC(&g_print_settings);
    if (!hdcTarget)
        return 1;
    hdc = CreateCompatibleDC(NULL);
    bm = CreateCompatibleBitmap(hdcTarget, 1, 1);
    SelectObject(hdc, bm);

    ZeroMemory(&fr, sizeof(fr));
    fr.hdc = hdc;
    fr.hdcTarget = hdcTarget;
    fr.rc = get_print_rect(hdcTarget);
    SetRect(&fr.rcPage, 0, 0, fr.rc.right + margins.right, fr.rc.bottom + margins.bottom);
    cpMax = SendMessageW(hEditorWnd, EM_GETTEXTLENGTHEX, (WPARAM)&gt, 0);
    fr.chrg.cpMin = 0;
    fr.chrg.cpMax = cpMax;

    g_pageEndsCount = 0;
    for (;;)
    {
        cp = SendMessageW(hEditorWnd, EM_FORMATRANGE, FALSE, (LPARAM)&fr);
        if (cp <= fr.chrg.cpMin || cp >= cpMax)
            break;
        if (g_pageEndsCount == g_pageEndsCap)
        {
            UINT32 newcap = g_pageEndsCap ? g_pageEndsCap * 2 : 32;
            int *ne = realloc(g_pageEnds, sizeof(int) * newcap);
            if (!ne) break;
            g_pageEnds = ne;
            g_pageEndsCap = newcap;
        }
        g_pageEnds[g_pageEndsCount++] = cp;
        pages++;
        fr.chrg.cpMin = cp;
    }
    SendMessageW(hEditorWnd, EM_FORMATRANGE, FALSE, 0);
    DeleteDC(hdc);
    DeleteDC(hdcTarget);
    DeleteObject(bm);
    return pages;
}

static UINT32 preview_page_count(void *ctx)
{
    (void)ctx;
    return preview_measure_pages();
}

static HRESULT preview_render_page(UINT32 index, HDC hdc, const RECT *paper,
                                   const RECT *content, UINT32 dpi, void *ctx)
{
    FORMATRANGE fr;
    HDC hdcTarget;
    RECT rcPage, rc;
    int dpix, dpiy, pw, ph;
    int saved;

    (void)content;
    (void)dpi;
    (void)ctx;
    hdcTarget = LeewayPrintSettingsCreateIC(&g_print_settings);
    if (!hdcTarget)
        return E_FAIL;

    dpix = GetDeviceCaps(hdcTarget, LOGPIXELSX);
    dpiy = GetDeviceCaps(hdcTarget, LOGPIXELSY);
    pw = GetDeviceCaps(hdcTarget, PHYSICALWIDTH);
    ph = GetDeviceCaps(hdcTarget, PHYSICALHEIGHT);
    SetRect(&rcPage, 0, 0, devunits_to_twips(pw, dpix), devunits_to_twips(ph, dpiy));
    SetRect(&rc, margins.left, margins.top,
            rcPage.right - margins.right, rcPage.bottom - margins.bottom);

    /* EM_FORMATRANGE draws at printer-pixel scale; map the printer page onto
     * the frame's zoom-scaled canvas rect. */
    saved = SaveDC(hdc);
    SetMapMode(hdc, MM_ANISOTROPIC);
    SetWindowExtEx(hdc, pw, ph, NULL);
    SetViewportExtEx(hdc, paper->right - paper->left, paper->bottom - paper->top, NULL);
    SetViewportOrgEx(hdc, paper->left, paper->top, NULL);

    ZeroMemory(&fr, sizeof(fr));
    fr.hdc = hdc;
    fr.hdcTarget = hdcTarget;
    fr.rc = rc;
    fr.rcPage = rcPage;
    fr.chrg.cpMin = 0;
    if (index > 0)
    {
        if (index - 1 >= g_pageEndsCount)
        {
            RestoreDC(hdc, saved);
            DeleteDC(hdcTarget);
            return S_OK;
        }
        fr.chrg.cpMin = g_pageEnds[index - 1];
    }
    fr.chrg.cpMax = -1;
    SendMessageW(hEditorWnd, EM_FORMATRANGE, TRUE, (LPARAM)&fr);
    SendMessageW(hEditorWnd, EM_FORMATRANGE, FALSE, 0);
    RestoreDC(hdc, saved);
    DeleteDC(hdcTarget);
    return S_OK;
}

static void preview_on_action(LeewayPreviewAction action, void *ctx)
{
    (void)ctx;
    switch (action)
    {
        case LWUI_PREVIEW_PRINT:
            dialog_print(hMainWnd, NULL);
            break;
        case LWUI_PREVIEW_PAGESETUP:
            if (LeewayPageSetupDialog(hMainWnd, &g_print_settings) == S_OK)
            {
                print_settings_write_back();
                LeewayPrintPreviewRefresh(g_preview);
            }
            break;
        case LWUI_PREVIEW_CLOSE:
            preview_exit(hMainWnd);
            break;
    }
}

void init_preview(HWND hMainWnd, LPWSTR wszFileName)
{
    LeewayPrintPreviewConfig cfg;

    (void)wszFileName;
    ensure_print_settings();
    if (!g_preview)
    {
        ZeroMemory(&cfg, sizeof(cfg));
        cfg.page_count = preview_page_count;
        cfg.render_page = preview_render_page;
        cfg.on_action = preview_on_action;
        cfg.settings = &g_print_settings;
        cfg.app_name = L"WordPad";
        cfg.theme = (LeewayThemePref)ribbon_theme_pref();
        if (FAILED(LeewayPrintPreviewCreate(hMainWnd, &cfg, &g_preview)))
            g_preview = NULL;
        if (g_preview)
            ribbon_install_preview_icons(g_preview);
    }
    if (g_preview)
    {
        LeewayPrintPreviewShow(g_preview, TRUE);
        ShowWindow(GetDlgItem(hMainWnd, IDC_EDITOR), SW_HIDE);
        ShowWindow(ribbon_hwnd(), SW_HIDE);
        {
            RECT rc;
            GetClientRect(hMainWnd, &rc);
            SendMessageW(hMainWnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right, rc.bottom));
        }
    }
}

void close_preview(HWND hMainWnd)
{
    (void)hMainWnd;
    if (g_preview)
        LeewayPrintPreviewShow(g_preview, FALSE);
    ShowWindow(GetDlgItem(hMainWnd, IDC_EDITOR), SW_SHOW);
    ShowWindow(ribbon_hwnd(), SW_SHOW);
}

BOOL preview_isactive(void)
{
    return g_preview && LeewayPrintPreviewIsActive(g_preview);
}

HWND preview_hwnd(void)
{
    return g_preview ? LeewayPrintPreviewGetHWND(g_preview) : NULL;
}

void preview_destroy(void)
{
    if (g_preview)
    {
        LeewayPrintPreviewDestroy(g_preview);
        g_preview = NULL;
    }
}

void preview_set_dpi(UINT dpi)
{
    if (g_preview)
        LeewayPrintPreviewSetDPI(g_preview, dpi);
}

void preview_on_theme_changed(void)
{
    if (g_preview)
        LeewayPrintPreviewOnThemeChanged(g_preview);
}

BOOL preview_pretranslate(MSG *msg)
{
    return g_preview && LeewayPrintPreviewIsActive(g_preview) &&
           LeewayPrintPreviewPreTranslateMessage(g_preview, msg);
}
