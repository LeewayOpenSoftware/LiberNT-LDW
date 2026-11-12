/*
 * PROJECT:     LeewayUI
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Shared print path: preview frame + print/page-setup dialog wrappers.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 * PROVENANCE:  sdk/lib/leewayui/LEEWAYUI-PROVENANCE.md
 *
 * Clean-room. The preview frame is a child window that hosts a LeewayRibbon
 * (single "Print Preview" tab) over a themed, scrollable page canvas; the app
 * answers page_count / render_page and performs Print / Page setup / Close.
 */

#include <windows.h>
#include <commdlg.h>
#include <stdlib.h>
#include <string.h>

#include <leewayui/print.h>
#include <leewayui/ribbon_icons.h>

/* ------------------------------------------------------------------ */
/* Units. Printer and page-setup geometry is kept in hundredths of a   */
/* millimetre (cmm); the canvas works in device px at the frame DPI.    */
/* ------------------------------------------------------------------ */

#define CMM_PER_INCH   2540          /* 25.40 mm */
#define LETTER_W_CMM   (85L * 254 / 10 * 10)   /* 8.5 in  = 21590 cmm */
#define LETTER_H_CMM   (11L * 2540)            /* 11  in  = 27940 cmm */
#define DEFAULT_MARGIN_CMM 2540                /* 1 inch all round */

static int CmmToPx(long cmm, UINT32 dpi) { return (int)MulDiv(cmm, (int)dpi, CMM_PER_INCH); }

/* ================================================================== */
/* Shared settings                                                     */
/* ================================================================== */

HRESULT WINAPI LeewayPrintSettingsInitDefault(LeewayPrintSettings *s)
{
    if (!s) return E_POINTER;
    ZeroMemory(s, sizeof(*s));
    s->margins_cmm.left = s->margins_cmm.top = DEFAULT_MARGIN_CMM;
    s->margins_cmm.right = s->margins_cmm.bottom = DEFAULT_MARGIN_CMM;
    s->scale_percent = 100;   /* actual size; fit_pages_x/y = 0 */

    PRINTDLGW pd;
    ZeroMemory(&pd, sizeof(pd));
    pd.lStructSize = sizeof(pd);
    pd.Flags = PD_RETURNDEFAULT | PD_NOWARNING;
    if (PrintDlgW(&pd)) {
        s->dev_mode = pd.hDevMode;
        s->dev_names = pd.hDevNames;
    }
    return S_OK;   /* no printer is fine: Letter is assumed downstream */
}

void WINAPI LeewayPrintSettingsFree(LeewayPrintSettings *s)
{
    if (!s) return;
    if (s->dev_mode)  { GlobalFree(s->dev_mode);  s->dev_mode = NULL; }
    if (s->dev_names) { GlobalFree(s->dev_names); s->dev_names = NULL; }
    ZeroMemory(s, sizeof(*s));
}

static HDC MakeDC(const LeewayPrintSettings *s, BOOL info)
{
    if (!s || !s->dev_names) return NULL;
    LPDEVNAMES dn = (LPDEVNAMES)GlobalLock(s->dev_names);
    LPDEVMODEW dm = s->dev_mode ? (LPDEVMODEW)GlobalLock(s->dev_mode) : NULL;
    HDC hdc = NULL;
    if (dn) {
        const WCHAR *driver = (const WCHAR *)dn + dn->wDriverOffset;
        const WCHAR *device = (const WCHAR *)dn + dn->wDeviceOffset;
        const WCHAR *output = (const WCHAR *)dn + dn->wOutputOffset;
        hdc = info ? CreateICW(driver, device, output, dm)
                   : CreateDCW(driver, device, output, dm);
    }
    if (dm) GlobalUnlock(s->dev_mode);
    GlobalUnlock(s->dev_names);
    return hdc;
}

HDC WINAPI LeewayPrintSettingsCreateIC(const LeewayPrintSettings *s) { return MakeDC(s, TRUE); }
HDC WINAPI LeewayPrintSettingsCreateDC(const LeewayPrintSettings *s) { return MakeDC(s, FALSE); }

HRESULT WINAPI LeewayPrintSettingsGetPaper(const LeewayPrintSettings *s, UINT32 ref_dpi,
                                           SIZE *paper_px, RECT *printable_px, RECT *margin_box_px)
{
    if (!ref_dpi) ref_dpi = 96;
    long paper_w_cmm = LETTER_W_CMM, paper_h_cmm = LETTER_H_CMM;
    RECT hard = { 0, 0, 0, 0 };   /* hardware unprintable margins, cmm */
    BOOL have_printer = FALSE;

    HDC ic = LeewayPrintSettingsCreateIC(s);
    if (ic) {
        int dpix = GetDeviceCaps(ic, LOGPIXELSX), dpiy = GetDeviceCaps(ic, LOGPIXELSY);
        int phw = GetDeviceCaps(ic, PHYSICALWIDTH),  phh = GetDeviceCaps(ic, PHYSICALHEIGHT);
        int offx = GetDeviceCaps(ic, PHYSICALOFFSETX), offy = GetDeviceCaps(ic, PHYSICALOFFSETY);
        int resx = GetDeviceCaps(ic, HORZRES),        resy = GetDeviceCaps(ic, VERTRES);
        if (dpix > 0 && dpiy > 0 && phw > 0 && phh > 0) {
            paper_w_cmm = MulDiv(phw, CMM_PER_INCH, dpix);
            paper_h_cmm = MulDiv(phh, CMM_PER_INCH, dpiy);
            hard.left   = MulDiv(offx, CMM_PER_INCH, dpix);
            hard.top    = MulDiv(offy, CMM_PER_INCH, dpiy);
            hard.right  = MulDiv(phw - offx - resx, CMM_PER_INCH, dpix);
            hard.bottom = MulDiv(phh - offy - resy, CMM_PER_INCH, dpiy);
            have_printer = TRUE;
        }
        DeleteDC(ic);
    }

    SIZE p = { CmmToPx(paper_w_cmm, ref_dpi), CmmToPx(paper_h_cmm, ref_dpi) };
    if (paper_px) *paper_px = p;
    if (printable_px) {
        printable_px->left   = CmmToPx(hard.left, ref_dpi);
        printable_px->top    = CmmToPx(hard.top, ref_dpi);
        printable_px->right  = p.cx - CmmToPx(hard.right, ref_dpi);
        printable_px->bottom = p.cy - CmmToPx(hard.bottom, ref_dpi);
    }
    if (margin_box_px) {
        RECT m = s ? s->margins_cmm : RECT{ DEFAULT_MARGIN_CMM, DEFAULT_MARGIN_CMM, DEFAULT_MARGIN_CMM, DEFAULT_MARGIN_CMM };
        /* never inside the hardware margin */
        long ml = m.left   > hard.left   ? m.left   : hard.left;
        long mt = m.top    > hard.top    ? m.top    : hard.top;
        long mr = m.right  > hard.right  ? m.right  : hard.right;
        long mb = m.bottom > hard.bottom ? m.bottom : hard.bottom;
        margin_box_px->left   = CmmToPx(ml, ref_dpi);
        margin_box_px->top    = CmmToPx(mt, ref_dpi);
        margin_box_px->right  = p.cx - CmmToPx(mr, ref_dpi);
        margin_box_px->bottom = p.cy - CmmToPx(mb, ref_dpi);
    }
    (void)have_printer;
    return S_OK;
}

/* ---- Dialog wrappers ---- */

HRESULT WINAPI LeewayPageSetupDialog(HWND owner, LeewayPrintSettings *s)
{
    if (!s) return E_POINTER;
    PAGESETUPDLGW ps;
    ZeroMemory(&ps, sizeof(ps));
    ps.lStructSize = sizeof(ps);
    ps.hwndOwner = owner;
    ps.hDevMode = s->dev_mode;
    ps.hDevNames = s->dev_names;
    ps.Flags = PSD_MARGINS | PSD_INHUNDREDTHSOFMILLIMETERS;
    ps.rtMargin = s->margins_cmm;

    if (!PageSetupDlgW(&ps)) {
        DWORD e = CommDlgExtendedError();
        return e ? E_FAIL : S_FALSE;      /* 0 = user cancelled */
    }
    s->dev_mode = ps.hDevMode;
    s->dev_names = ps.hDevNames;
    s->margins_cmm = ps.rtMargin;
    return S_OK;
}

HRESULT WINAPI LeewayPrintDialog(HWND owner, LeewayPrintSettings *s,
                                 LeewayPrintRange *range, HDC *print_dc_out)
{
    if (!s || !range) return E_POINTER;

    PRINTPAGERANGE pr = { range->min_page ? range->min_page : 1,
                          range->max_page ? range->max_page : 1 };
    PRINTDLGEXW pd;
    ZeroMemory(&pd, sizeof(pd));
    pd.lStructSize = sizeof(pd);
    pd.hwndOwner = owner;
    pd.hDevMode = s->dev_mode;
    pd.hDevNames = s->dev_names;
    pd.Flags = PD_RETURNDC | PD_USEDEVMODECOPIESANDCOLLATE | PD_NOCURRENTPAGE;
    pd.nPageRanges = 0;
    pd.nMaxPageRanges = 1;
    pd.lpPageRanges = &pr;
    pd.nMinPage = range->min_page ? range->min_page : 1;
    pd.nMaxPage = range->max_page ? range->max_page : 1;
    pd.nCopies = range->copies ? range->copies : 1;
    pd.nStartPage = START_PAGE_GENERAL;

    HRESULT hr = PrintDlgExW(&pd);
    if (FAILED(hr)) return hr;

    s->dev_mode = pd.hDevMode;
    s->dev_names = pd.hDevNames;

    if (pd.dwResultAction == PD_RESULT_CANCEL) {
        if (pd.hDC) DeleteDC(pd.hDC);
        return S_FALSE;
    }
    /* PD_RESULT_PRINT (and PD_RESULT_APPLY) update the settings; only PRINT carries a DC. */
    range->all_pages  = (pd.Flags & PD_PAGENUMS) ? FALSE : TRUE;
    range->selection  = (pd.Flags & PD_SELECTION) ? TRUE : FALSE;
    range->copies     = pd.nCopies ? pd.nCopies : 1;
    if (pd.nPageRanges >= 1 && (pd.Flags & PD_PAGENUMS)) {
        range->from_page = pr.nFromPage;
        range->to_page   = pr.nToPage;
    } else {
        range->from_page = range->min_page;
        range->to_page   = range->max_page;
    }
    if (print_dc_out) *print_dc_out = pd.hDC;
    else if (pd.hDC)  DeleteDC(pd.hDC);
    return (pd.dwResultAction == PD_RESULT_PRINT) ? S_OK : S_FALSE;
}

/* ================================================================== */
/* Preview frame                                                       */
/* ================================================================== */

/* Frame-internal command ids for the single Print Preview tab. */
enum {
    PVCMD_PRINT = 1, PVCMD_PAGESETUP, PVCMD_ZOOMIN, PVCMD_ZOOMOUT,
    PVCMD_ONEPAGE, PVCMD_TWOPAGES, PVCMD_PREV, PVCMD_NEXT, PVCMD_CLOSE
};

static const WCHAR kFrameClass[] = L"LeewayUIPrintPreview";

/* Per-app Theme override, same key/format as LeewayThemeApplyAppOverride:
 * HKCU\Software\Leeway\<app>\Theme (REG_SZ system|light|dark, or REG_DWORD 0|1|2).
 * Returns SYSTEM when absent/invalid. Used to resolve the effective preference so
 * the frame's canvas theme AND its ribbon honour it, like the ribbon does. */
static LeewayThemePref PvReadAppThemePref(const WCHAR *app_name)
{
    LeewayThemePref pref = LEEWAY_THEME_PREF_SYSTEM;
    if (!app_name || !app_name[0]) return pref;
    WCHAR key[256]; wsprintfW(key, L"Software\\Leeway\\%s", app_name); key[255] = 0;
    HKEY h;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_READ, &h) == ERROR_SUCCESS) {
        DWORD type = 0, cb = 0; WCHAR sv[16]; DWORD dv = 0;
        cb = sizeof(sv);
        if (RegQueryValueExW(h, L"Theme", NULL, &type, (LPBYTE)sv, &cb) == ERROR_SUCCESS) {
            if (type == REG_SZ) {
                sv[15] = 0;
                if (lstrcmpiW(sv, L"light") == 0) pref = LEEWAY_THEME_PREF_LIGHT;
                else if (lstrcmpiW(sv, L"dark") == 0) pref = LEEWAY_THEME_PREF_DARK;
            } else if (type == REG_DWORD) {
                CopyMemory(&dv, sv, sizeof(dv));
                if (dv == 1) pref = LEEWAY_THEME_PREF_LIGHT; else if (dv == 2) pref = LEEWAY_THEME_PREF_DARK;
            }
        }
        RegCloseKey(h);
    }
    return pref;
}

struct ILeewayPrintPreview {
    HWND  hwnd;
    HWND  parent;
    ILeewayRibbon *ribbon;
    ILeewayTheme  *theme;
    LeewayPrintPreviewConfig cfg;
    LeewayUIIconResolver icon_resolver;
    void *icon_ctx;
    UINT32 dpi;
    UINT32 page;          /* 0-based top-left page */
    UINT32 pages_shown;   /* 1 or 2 */
    UINT32 page_count;
    int    zoom;          /* percent; 0 = fit to canvas */
    int    scroll_y, scroll_x;
    int    content_h, content_w;   /* virtual canvas size at current zoom */
};

/* ---- the single-tab ribbon layout (static; ids resolved by the table) ---- */
static const LeewayUICommand kPvTable[] = {
    { PVCMD_PRINT,     L"Print",        L"Print the document",        L"P",  LWUI_AUR_COMMON_PRINT,     TRUE, FALSE, TRUE },
    { PVCMD_PAGESETUP, L"Page setup",   L"Paper, orientation, margins",L"G", LWUI_AUR_COMMON_PAGESETUP, TRUE, FALSE, TRUE },
    { PVCMD_ZOOMIN,    L"Zoom in",      L"Zoom in",                   L"I",  LWUI_AUR_COMMON_ZOOM_IN,   TRUE, FALSE, TRUE },
    { PVCMD_ZOOMOUT,   L"Zoom out",     L"Zoom out",                  L"O",  LWUI_AUR_COMMON_ZOOM_OUT,  TRUE, FALSE, TRUE },
    { PVCMD_ONEPAGE,   L"One page",     L"Show one page",             L"1",  LWUI_ICON_NONE,            TRUE, FALSE, TRUE },
    { PVCMD_TWOPAGES,  L"Two pages",    L"Show two pages",            L"2",  LWUI_AUR_WORDPAD_TWO_PAGES,TRUE, FALSE, TRUE },
    { PVCMD_PREV,      L"Previous",     L"Previous page",             L"B",  LWUI_AUR_VIEWER_PREV,      TRUE, FALSE, TRUE },
    { PVCMD_NEXT,      L"Next",         L"Next page",                 L"N",  LWUI_AUR_VIEWER_NEXT,      TRUE, FALSE, TRUE },
    { PVCMD_CLOSE,     L"Close print preview", L"Close print preview",L"C", LWUI_AUR_COMMON_CLOSE,     TRUE, FALSE, TRUE },
};
static const LeewayUIPlacement kgPrint[]  = { { PVCMD_PRINT, LWUI_CONTROL_BUTTON_LARGE, 0, 0 },
                                              { PVCMD_PAGESETUP, LWUI_CONTROL_BUTTON_LARGE, 0, 0 } };
static const LeewayUIPlacement kgZoom[]   = { { PVCMD_ZOOMIN, LWUI_CONTROL_BUTTON_LARGE, 0, 0 },
                                              { PVCMD_ZOOMOUT, LWUI_CONTROL_BUTTON_SMALL, 0, 0 } };
static const LeewayUIPlacement kgView[]   = { { PVCMD_ONEPAGE, LWUI_CONTROL_TOGGLE_LARGE, 0, 0 },
                                              { PVCMD_TWOPAGES, LWUI_CONTROL_TOGGLE_LARGE, 0, 0 } };
static const LeewayUIPlacement kgNav[]    = { { PVCMD_PREV, LWUI_CONTROL_BUTTON_LARGE, 0, 0 },
                                              { PVCMD_NEXT, LWUI_CONTROL_BUTTON_LARGE, 0, 0 } };
static const LeewayUIPlacement kgClose[]  = { { PVCMD_CLOSE, LWUI_CONTROL_BUTTON_LARGE, 0, 0 } };
static const LeewayUIGroup kPvGroups[] = {
    { L"Print",   NULL, 0, 0, 2, kgPrint, 0 },
    { L"Zoom",    NULL, 0, 0, 2, kgZoom,  0 },
    { L"Page view", NULL, 0, 0, 2, kgView, 0 },
    { L"Navigate", NULL, 0, 0, 2, kgNav,  0 },
    { L"Preview", NULL, 0, 0, 1, kgClose, 0 },
};
static const LeewayUITab kPvTabs[] = { { 1, L"Print preview", L"P", NULL, 5, kPvGroups } };
static const LeewayUILayout kPvLayout = { NULL, 1, kPvTabs, 0, NULL, 0, NULL, 0 };

/* ---- ribbon callbacks ---- */
static void PvRecalc(ILeewayPrintPreview *pv);
static void PvScrollClamp(ILeewayPrintPreview *pv);

static HRESULT PvExecute(UINT32 id, void *ctx)
{
    ILeewayPrintPreview *pv = (ILeewayPrintPreview *)ctx;
    switch (id) {
    case PVCMD_PRINT:     if (pv->cfg.on_action) pv->cfg.on_action(LWUI_PREVIEW_PRINT, pv->cfg.context); break;
    case PVCMD_PAGESETUP: if (pv->cfg.on_action) pv->cfg.on_action(LWUI_PREVIEW_PAGESETUP, pv->cfg.context); break;
    case PVCMD_CLOSE:     if (pv->cfg.on_action) pv->cfg.on_action(LWUI_PREVIEW_CLOSE, pv->cfg.context); break;
    case PVCMD_ZOOMIN:    pv->zoom = (pv->zoom ? pv->zoom : 100) * 5 / 4; if (pv->zoom > 500) pv->zoom = 500; PvRecalc(pv); break;
    case PVCMD_ZOOMOUT:   pv->zoom = (pv->zoom ? pv->zoom : 100) * 4 / 5; if (pv->zoom < 10)  pv->zoom = 10;  PvRecalc(pv); break;
    case PVCMD_ONEPAGE:   pv->pages_shown = 1; PvRecalc(pv); break;
    case PVCMD_TWOPAGES:  pv->pages_shown = 2; PvRecalc(pv); break;
    case PVCMD_PREV:      if (pv->page) { pv->page -= pv->pages_shown; if ((int)pv->page < 0) pv->page = 0; pv->scroll_y = 0; PvRecalc(pv); } break;
    case PVCMD_NEXT:      if (pv->page + pv->pages_shown < pv->page_count) { pv->page += pv->pages_shown; pv->scroll_y = 0; PvRecalc(pv); } break;
    }
    LeewayRibbonInvalidateAll(pv->ribbon);
    return S_OK;
}

static HRESULT PvState(UINT32 id, LeewayUICommandState *st, void *ctx)
{
    ILeewayPrintPreview *pv = (ILeewayPrintPreview *)ctx;
    switch (id) {
    case PVCMD_ONEPAGE:  st->checked = (pv->pages_shown == 1); break;
    case PVCMD_TWOPAGES: st->checked = (pv->pages_shown == 2); break;
    case PVCMD_PREV:     st->enabled = (pv->page > 0); break;
    case PVCMD_NEXT:     st->enabled = (pv->page + pv->pages_shown < pv->page_count); break;
    }
    return S_OK;
}

/* ---- geometry ---- */
static int RibbonH(ILeewayPrintPreview *pv) { return pv->ribbon ? (int)LeewayRibbonGetHeight(pv->ribbon) : 0; }

static void PvCanvasRect(ILeewayPrintPreview *pv, RECT *rc)
{
    GetClientRect(pv->hwnd, rc);
    rc->top += RibbonH(pv);
    if (rc->top > rc->bottom) rc->top = rc->bottom;
}

/* Paper size in px at the current zoom; zoom 0 = fit page height to canvas. */
static void PvPaperPx(ILeewayPrintPreview *pv, int canvas_w, int canvas_h, SIZE *paper, int *gap)
{
    SIZE full; LeewayPrintSettingsGetPaper(pv->cfg.settings, pv->dpi, &full, NULL, NULL);
    if (full.cx <= 0 || full.cy <= 0) { full.cx = CmmToPx(LETTER_W_CMM, pv->dpi); full.cy = CmmToPx(LETTER_H_CMM, pv->dpi); }
    int g = MulDiv(16, (int)pv->dpi, 96);
    int zoom = pv->zoom;
    if (zoom <= 0) {
        /* fit: the block (pages_shown wide) fits canvas width, and one page fits height */
        int margin = MulDiv(24, (int)pv->dpi, 96);
        int avail_w = canvas_w - 2 * margin - (pv->pages_shown - 1) * g;
        int per_w = avail_w / (int)pv->pages_shown;
        int avail_h = canvas_h - 2 * margin;
        int zx = full.cx > 0 ? MulDiv(per_w, 100, full.cx) : 100;
        int zy = full.cy > 0 ? MulDiv(avail_h, 100, full.cy) : 100;
        zoom = zx < zy ? zx : zy;
        if (zoom < 10) zoom = 10;
        if (zoom > 100) zoom = 100;   /* fit never magnifies past 100% */
    }
    paper->cx = MulDiv(full.cx, zoom, 100);
    paper->cy = MulDiv(full.cy, zoom, 100);
    if (gap) *gap = g;
}

static void PvRecalc(ILeewayPrintPreview *pv)
{
    RECT cv; PvCanvasRect(pv, &cv);
    int cw = cv.right - cv.left, ch = cv.bottom - cv.top;
    SIZE paper; int gap; PvPaperPx(pv, cw, ch, &paper, &gap);
    int margin = MulDiv(24, (int)pv->dpi, 96);
    int block_w = paper.cx * (int)pv->pages_shown + gap * ((int)pv->pages_shown - 1);
    pv->content_w = block_w + 2 * margin;
    pv->content_h = paper.cy + 2 * margin;
    PvScrollClamp(pv);
    SCROLLINFO si; ZeroMemory(&si, sizeof(si)); si.cbSize = sizeof(si); si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0; si.nMax = pv->content_h - 1; si.nPage = ch > 0 ? ch : 1; si.nPos = pv->scroll_y;
    SetScrollInfo(pv->hwnd, SB_VERT, &si, TRUE);
    si.nMax = pv->content_w - 1; si.nPage = cw > 0 ? cw : 1; si.nPos = pv->scroll_x;
    SetScrollInfo(pv->hwnd, SB_HORZ, &si, TRUE);
    InvalidateRect(pv->hwnd, NULL, FALSE);
}

static void PvScrollClamp(ILeewayPrintPreview *pv)
{
    RECT cv; PvCanvasRect(pv, &cv);
    int ch = cv.bottom - cv.top, cw = cv.right - cv.left;
    int maxy = pv->content_h - ch; if (maxy < 0) maxy = 0;
    int maxx = pv->content_w - cw; if (maxx < 0) maxx = 0;
    if (pv->scroll_y > maxy) pv->scroll_y = maxy;
    if (pv->scroll_y < 0) pv->scroll_y = 0;
    if (pv->scroll_x > maxx) pv->scroll_x = maxx;
    if (pv->scroll_x < 0) pv->scroll_x = 0;
}

/* ---- painting ---- */
static void PvPaint(ILeewayPrintPreview *pv, HDC hdc)
{
    RECT cv; PvCanvasRect(pv, &cv);
    int cw = cv.right - cv.left, ch = cv.bottom - cv.top;
    if (cw <= 0 || ch <= 0) return;

    /* double buffer */
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, cw, ch);
    HBITMAP ob = (HBITMAP)SelectObject(mem, bmp);
    SetViewportOrgEx(mem, -cv.left, -cv.top, NULL);

    COLORREF bg = pv->theme ? LeewayThemeGetColor(pv->theme, LWUI_COLOR_BAND_BG2) : RGB(100,100,100);
    /* a neutral canvas: darken the band colour a touch so white paper reads */
    int r = GetRValue(bg)*4/5, g = GetGValue(bg)*4/5, b = GetBValue(bg)*4/5;
    HBRUSH bb = CreateSolidBrush(RGB(r,g,b));
    FillRect(mem, &cv, bb);
    DeleteObject(bb);

    SIZE paper; int gap; PvPaperPx(pv, cw, ch, &paper, &gap);
    int margin = MulDiv(24, (int)pv->dpi, 96);
    int block_w = paper.cx * (int)pv->pages_shown + gap * ((int)pv->pages_shown - 1);
    int x0 = cv.left + (cw > block_w ? (cw - block_w) / 2 : margin) - pv->scroll_x;
    int y0 = cv.top + (ch > pv->content_h ? (ch - pv->content_h) / 2 + margin : margin) - pv->scroll_y;

    COLORREF edge = pv->theme ? LeewayThemeGetColor(pv->theme, LWUI_COLOR_GROUP_BORDER) : RGB(60,60,60);
    HPEN mpen = CreatePen(PS_DOT, 1, pv->theme ? LeewayThemeGetColor(pv->theme, LWUI_COLOR_TEXT_DISABLED) : RGB(170,170,170));

    for (UINT32 i = 0; i < pv->pages_shown; i++) {
        UINT32 idx = pv->page + i;
        if (idx >= pv->page_count) break;
        RECT paperRc = { x0 + (int)i * (paper.cx + gap), y0, x0 + (int)i * (paper.cx + gap) + paper.cx, y0 + paper.cy };

        /* drop shadow */
        RECT sh = paperRc; OffsetRect(&sh, MulDiv(3,(int)pv->dpi,96), MulDiv(3,(int)pv->dpi,96));
        HBRUSH shb = CreateSolidBrush(RGB(r*3/4,g*3/4,b*3/4)); FillRect(mem, &sh, shb); DeleteObject(shb);

        /* white sheet + edge */
        HBRUSH white = (HBRUSH)GetStockObject(WHITE_BRUSH);
        FillRect(mem, &paperRc, white);
        HPEN ep = CreatePen(PS_SOLID, 1, edge); HGDIOBJ opn = SelectObject(mem, ep);
        HGDIOBJ obr = SelectObject(mem, GetStockObject(NULL_BRUSH));
        Rectangle(mem, paperRc.left, paperRc.top, paperRc.right, paperRc.bottom);
        SelectObject(mem, obr); SelectObject(mem, opn); DeleteObject(ep);

        /* margin box (scaled from cmm at this zoom) */
        int zoom = pv->zoom > 0 ? pv->zoom : MulDiv(paper.cy, 100, CmmToPx(LETTER_H_CMM, pv->dpi) > 0 ? CmmToPx(LETTER_H_CMM, pv->dpi) : 1);
        RECT content = paperRc;
        {
            SIZE full; LeewayPrintSettingsGetPaper(pv->cfg.settings, pv->dpi, &full, NULL, NULL);
            RECT mb; LeewayPrintSettingsGetPaper(pv->cfg.settings, pv->dpi, NULL, NULL, &mb);
            if (full.cx > 0 && full.cy > 0) {
                content.left   = paperRc.left + MulDiv(mb.left,   paper.cx, full.cx);
                content.top    = paperRc.top  + MulDiv(mb.top,    paper.cy, full.cy);
                content.right  = paperRc.left + MulDiv(mb.right,  paper.cx, full.cx);
                content.bottom = paperRc.top  + MulDiv(mb.bottom, paper.cy, full.cy);
            }
            HGDIOBJ op2 = SelectObject(mem, mpen);
            HGDIOBJ ob2 = SelectObject(mem, GetStockObject(NULL_BRUSH));
            Rectangle(mem, content.left, content.top, content.right, content.bottom);
            SelectObject(mem, ob2); SelectObject(mem, op2);
        }
        (void)zoom;

        /* app content, clipped to the sheet */
        if (pv->cfg.render_page) {
            int sv = SaveDC(mem);
            IntersectClipRect(mem, paperRc.left, paperRc.top, paperRc.right, paperRc.bottom);
            pv->cfg.render_page(idx, mem, &paperRc, &content, pv->dpi, pv->cfg.context);
            RestoreDC(mem, sv);
        }
    }
    DeleteObject(mpen);

    BitBlt(hdc, cv.left, cv.top, cw, ch, mem, cv.left, cv.top, SRCCOPY);
    SelectObject(mem, ob); DeleteObject(bmp); DeleteDC(mem);
}

/* ---- window proc ---- */
static LRESULT CALLBACK PvWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    ILeewayPrintPreview *pv = (ILeewayPrintPreview *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_SIZE:
        if (pv && pv->ribbon) {
            RECT rc; GetClientRect(hwnd, &rc);
            MoveWindow(LeewayRibbonGetHWND(pv->ribbon), 0, 0, rc.right, RibbonH(pv), TRUE);
            PvRecalc(pv);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        if (pv) PvPaint(pv, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_VSCROLL: if (pv) {
        RECT cv; PvCanvasRect(pv, &cv); int ch = cv.bottom - cv.top;
        int step = MulDiv(40,(int)pv->dpi,96);
        switch (LOWORD(wp)) {
        case SB_LINEUP: pv->scroll_y -= step; break;
        case SB_LINEDOWN: pv->scroll_y += step; break;
        case SB_PAGEUP: pv->scroll_y -= ch; break;
        case SB_PAGEDOWN: pv->scroll_y += ch; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: pv->scroll_y = HIWORD(wp); break;
        }
        PvScrollClamp(pv);
        SetScrollPos(hwnd, SB_VERT, pv->scroll_y, TRUE);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0; }
    case WM_HSCROLL: if (pv) {
        int step = MulDiv(40,(int)pv->dpi,96);
        switch (LOWORD(wp)) {
        case SB_LINELEFT: pv->scroll_x -= step; break;
        case SB_LINERIGHT: pv->scroll_x += step; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: pv->scroll_x = HIWORD(wp); break;
        }
        PvScrollClamp(pv); SetScrollPos(hwnd, SB_HORZ, pv->scroll_x, TRUE);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0; }
    case WM_MOUSEWHEEL: if (pv) {
        int d = GET_WHEEL_DELTA_WPARAM(wp);
        pv->scroll_y -= d * MulDiv(40,(int)pv->dpi,96) / WHEEL_DELTA;
        PvScrollClamp(pv); SetScrollPos(hwnd, SB_VERT, pv->scroll_y, TRUE);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0; }
    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)lp;
        if (pv && pv->ribbon && nm->hwndFrom == LeewayRibbonGetHWND(pv->ribbon)) {
            if (nm->code == LWUI_RBN_HEIGHTCHANGED) {
                RECT rc; GetClientRect(hwnd, &rc);
                MoveWindow(LeewayRibbonGetHWND(pv->ribbon), 0, 0, rc.right, RibbonH(pv), TRUE);
                PvRecalc(pv);
            }
            return 0;
        }
        break;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void PvRegisterClass(void)
{
    static BOOL done = FALSE;
    if (done) return;
    WNDCLASSW wc; ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = PvWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = kFrameClass;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassW(&wc);
    done = TRUE;
}

HRESULT WINAPI LeewayPrintPreviewCreate(HWND parent, const LeewayPrintPreviewConfig *cfg,
                                        ILeewayPrintPreview **out)
{
    if (!parent || !cfg || !cfg->page_count || !cfg->render_page || !cfg->on_action || !out)
        return E_INVALIDARG;
    *out = NULL;
    PvRegisterClass();

    ILeewayPrintPreview *pv = (ILeewayPrintPreview *)calloc(1, sizeof(*pv));
    if (!pv) return E_OUTOFMEMORY;
    pv->cfg = *cfg;
    pv->parent = parent;
    pv->pages_shown = 1;
    pv->zoom = 0;            /* fit */
    pv->dpi = 96;

    pv->hwnd = CreateWindowExW(0, kFrameClass, L"", WS_CHILD | WS_CLIPCHILDREN | WS_VSCROLL | WS_HSCROLL,
                               0, 0, 0, 0, parent, NULL, GetModuleHandleW(NULL), NULL);
    if (!pv->hwnd) { free(pv); return HRESULT_FROM_WIN32(GetLastError()); }
    SetWindowLongPtrW(pv->hwnd, GWLP_USERDATA, (LONG_PTR)pv);

    /* Resolve the effective preference: an explicit LIGHT/DARK from the app wins;
     * otherwise honour the HKCU per-app Theme override (like the ribbon), falling
     * back to SYSTEM. Apply the same value to both the canvas theme and the ribbon
     * (the frame's ribbon layout has no app_name, so it cannot auto-apply it). */
    LeewayThemePref eff = pv->cfg.theme;
    if (eff == LEEWAY_THEME_PREF_SYSTEM && pv->cfg.app_name)
        eff = PvReadAppThemePref(pv->cfg.app_name);
    pv->cfg.theme = eff;

    LeewayThemeCreate(&pv->theme);
    if (pv->theme) LeewayThemeSetPreference(pv->theme, eff);

    HRESULT hr = LeewayRibbonCreate(pv->hwnd, kPvTable, (UINT32)(sizeof(kPvTable)/sizeof(kPvTable[0])),
                                    PvExecute, PvState, pv, &pv->ribbon);
    if (FAILED(hr)) { DestroyWindow(pv->hwnd); LeewayThemeDestroy(pv->theme); free(pv); return hr; }
    LeewayRibbonSetLayout(pv->ribbon, &kPvLayout);
    LeewayRibbonSetThemePreference(pv->ribbon, eff);
    /* match the ribbon's DPI source (GetDeviceCaps on the window); SetDPI keeps it in sync later */
    { HDC dc = GetDC(pv->hwnd); if (dc) { int d = GetDeviceCaps(dc, LOGPIXELSX); if (d > 0) pv->dpi = (UINT32)d; ReleaseDC(pv->hwnd, dc); } }

    *out = pv;
    return S_OK;
}

HRESULT WINAPI LeewayPrintPreviewDestroy(ILeewayPrintPreview *pv)
{
    if (!pv) return E_POINTER;
    if (pv->ribbon) LeewayRibbonDestroy(pv->ribbon);
    if (pv->theme)  LeewayThemeDestroy(pv->theme);
    if (pv->hwnd)   DestroyWindow(pv->hwnd);
    free(pv);
    return S_OK;
}

HWND WINAPI LeewayPrintPreviewGetHWND(ILeewayPrintPreview *pv) { return pv ? pv->hwnd : NULL; }

HRESULT WINAPI LeewayPrintPreviewShow(ILeewayPrintPreview *pv, BOOL show)
{
    if (!pv) return E_POINTER;
    if (show) {
        pv->page_count = pv->cfg.page_count ? pv->cfg.page_count(pv->cfg.context) : 1;
        if (pv->page_count < 1) pv->page_count = 1;
        pv->page = 0; pv->scroll_x = pv->scroll_y = 0;
        ShowWindow(pv->hwnd, SW_SHOW);
        PvRecalc(pv);
        LeewayRibbonInvalidateAll(pv->ribbon);
    } else {
        ShowWindow(pv->hwnd, SW_HIDE);
    }
    return S_OK;
}

BOOL WINAPI LeewayPrintPreviewIsActive(ILeewayPrintPreview *pv)
{
    return pv && IsWindowVisible(pv->hwnd);
}

HRESULT WINAPI LeewayPrintPreviewRefresh(ILeewayPrintPreview *pv)
{
    if (!pv) return E_POINTER;
    pv->page_count = pv->cfg.page_count ? pv->cfg.page_count(pv->cfg.context) : 1;
    if (pv->page_count < 1) pv->page_count = 1;
    if (pv->page >= pv->page_count) pv->page = pv->page_count - 1;
    PvRecalc(pv);
    LeewayRibbonInvalidateAll(pv->ribbon);
    return S_OK;
}

HRESULT WINAPI LeewayPrintPreviewSetPage(ILeewayPrintPreview *pv, UINT32 index)
{
    if (!pv) return E_POINTER;
    if (index >= pv->page_count) index = pv->page_count ? pv->page_count - 1 : 0;
    pv->page = index; pv->scroll_y = 0; PvRecalc(pv);
    LeewayRibbonInvalidateAll(pv->ribbon);
    return S_OK;
}
UINT32 WINAPI LeewayPrintPreviewGetPage(ILeewayPrintPreview *pv) { return pv ? pv->page : 0; }

HRESULT WINAPI LeewayPrintPreviewSetPagesShown(ILeewayPrintPreview *pv, UINT32 pages)
{
    if (!pv) return E_POINTER;
    pv->pages_shown = (pages >= 2) ? 2 : 1;
    PvRecalc(pv); LeewayRibbonInvalidateAll(pv->ribbon);
    return S_OK;
}
UINT32 WINAPI LeewayPrintPreviewGetPagesShown(ILeewayPrintPreview *pv) { return pv ? pv->pages_shown : 1; }

HRESULT WINAPI LeewayPrintPreviewSetIconResolver(ILeewayPrintPreview *pv,
                                                 LeewayUIIconResolver resolver, void *resolver_context)
{
    if (!pv) return E_POINTER;
    pv->icon_resolver = resolver; pv->icon_ctx = resolver_context;
    if (pv->ribbon) LeewayRibbonSetIconResolver(pv->ribbon, resolver, resolver_context);
    return S_OK;
}

HRESULT WINAPI LeewayPrintPreviewOnThemeChanged(ILeewayPrintPreview *pv)
{
    if (!pv) return E_POINTER;
    if (pv->theme) LeewayThemeOnThemeChange(pv->theme);
    if (pv->ribbon) LeewayRibbonOnThemeChanged(pv->ribbon);
    InvalidateRect(pv->hwnd, NULL, FALSE);
    return S_OK;
}

HRESULT WINAPI LeewayPrintPreviewSetDPI(ILeewayPrintPreview *pv, UINT32 dpi)
{
    if (!pv) return E_POINTER;
    if (!dpi) dpi = 96;
    pv->dpi = dpi;
    if (pv->ribbon) LeewayRibbonSetDPI(pv->ribbon, dpi);
    PvRecalc(pv);
    return S_OK;
}

HRESULT WINAPI LeewayPrintPreviewSetThemePreference(ILeewayPrintPreview *pv, LeewayThemePref pref)
{
    if (!pv) return E_POINTER;
    pv->cfg.theme = pref;
    if (pv->theme) LeewayThemeSetPreference(pv->theme, pref);
    if (pv->ribbon) LeewayRibbonSetThemePreference(pv->ribbon, pref);
    InvalidateRect(pv->hwnd, NULL, FALSE);
    return S_OK;
}

BOOL WINAPI LeewayPrintPreviewPreTranslateMessage(ILeewayPrintPreview *pv, MSG *msg)
{
    if (!pv || !msg) return FALSE;
    if (pv->ribbon && LeewayRibbonPreTranslateMessage(pv->ribbon, msg)) return TRUE;
    if (msg->message == WM_KEYDOWN && IsWindowVisible(pv->hwnd)) {
        switch (msg->wParam) {
        case VK_NEXT:  PvExecute(PVCMD_NEXT, pv); return TRUE;
        case VK_PRIOR: PvExecute(PVCMD_PREV, pv); return TRUE;
        case VK_ESCAPE: PvExecute(PVCMD_CLOSE, pv); return TRUE;
        case VK_OEM_PLUS: case VK_ADD: PvExecute(PVCMD_ZOOMIN, pv); return TRUE;
        case VK_OEM_MINUS: case VK_SUBTRACT: PvExecute(PVCMD_ZOOMOUT, pv); return TRUE;
        }
    }
    return FALSE;
}
