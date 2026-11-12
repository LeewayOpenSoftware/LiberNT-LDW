/*
 * PROJECT:     LeewayUI
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Shared print path: a print-preview frame (ribbon + page canvas)
 *              and thin wrappers over the system print / page-setup dialogs,
 *              built once so WordPad, Paint and future apps share it.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 * PROVENANCE:  sdk/lib/leewayui/LEEWAYUI-PROVENANCE.md
 *
 * Plain C API, same conventions as ribbon.h (own control, own API; every Set*
 * copies what it is given). Clean-room; our own (Aureline) icons.
 *
 * Model (RIBBON-APPS print path):
 *   The preview FRAME is a child window that, while active, covers the app's
 *   client area and shows a ribbon with a single "Print Preview" tab
 *   (Print, Page setup, Zoom, One/Two pages, Previous/Next page, Close) over a
 *   scrollable page canvas. The frame owns navigation (current page, one/two-up,
 *   zoom) and paints the themed paper + margins; the app only answers three
 *   callbacks: how many pages, how big the paper is, and "draw page N into this
 *   HDC". Toolbar actions the app must perform (Print, Page setup, Close) come
 *   back through one on_action callback. PerMonitorV2-aware; light and dark.
 *
 *   The dialog wrappers keep one LeewayPrintSettings (DEVMODE/DEVNAMES +
 *   margins) so Print, Page setup and the preview agree on paper and printer.
 *   They call the system PrintDlgEx / PageSetupDlg today; the Win7-style custom
 *   dialog is a later layer behind the same API.
 */

#pragma once

#include <windows.h>
#include <stdint.h>
#include <leewayui/theme.h>
#include <leewayui/ribbon.h>   /* LeewayUIIconResolver for the frame's ribbon */

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ Shared settings ============================ *
 * One of these lives in the app and is passed to every wrapper so the printer,
 * paper and margins stay consistent across Print / Page setup / Preview.
 * The struct owns its two globals; free it with LeewayPrintSettingsFree. */
typedef struct {
    HGLOBAL dev_mode;      /* DEVMODEW (printer + paper + orientation); may be NULL until first use */
    HGLOBAL dev_names;     /* DEVNAMESW (driver/device/output); may be NULL */
    RECT    margins_cmm;   /* page margins, hundredths of a millimetre (left/top/right/bottom) */
    /* Per-document print scaling (Win7 Paint Page Setup). The frame does not act
     * on these - the app reads them in render_page (and, later, the Win7-style
     * Page setup dialog edits them). WordPad can ignore them (scale 100). */
    UINT32  scale_percent; /* 100 = actual size; 0 is treated as 100 */
    UINT16  fit_pages_x;   /* 0 = use scale_percent; else "fit to X pages across" */
    UINT16  fit_pages_y;   /* 0 = use scale_percent; else "...by Y pages down" */
    BOOL    center_h;      /* centre horizontally on the sheet */
    BOOL    center_v;      /* centre vertically */
} LeewayPrintSettings;

/* Fill s with the default printer and 25.4 mm (1 in) margins. dev_mode/dev_names
 * are allocated from the default printer; S_OK even with no printer (globals NULL,
 * Letter assumed). */
HRESULT WINAPI LeewayPrintSettingsInitDefault(LeewayPrintSettings *s);

/* Release the globals and zero the struct. Safe on an all-zero struct. */
void    WINAPI LeewayPrintSettingsFree(LeewayPrintSettings *s);

/* Create an information or device context for the chosen printer (for measuring
 * or printing). NULL if there is no printer. Caller DeleteDC()s it. */
HDC     WINAPI LeewayPrintSettingsCreateIC(const LeewayPrintSettings *s);  /* information DC (measuring) */
HDC     WINAPI LeewayPrintSettingsCreateDC(const LeewayPrintSettings *s);  /* device DC (printing) */

/* Printable paper geometry derived from the settings, in device px of `ref_dpi`
 * (pass the preview/print DPI). paper = full sheet; printable = inside the
 * hardware margins; margin_box = inside LeewayPrintSettings.margins_cmm.
 * Any out pointer may be NULL. Uses Letter when there is no printer. */
HRESULT WINAPI LeewayPrintSettingsGetPaper(const LeewayPrintSettings *s, UINT32 ref_dpi,
                                           SIZE *paper_px, RECT *printable_px, RECT *margin_box_px);

/* ---- Dialog wrappers ----
 * Both read and write *s in place. Return S_OK if the user accepted (settings
 * updated), S_FALSE if cancelled, or an error HRESULT. */

/* PageSetupDlg wrapper: lets the user pick paper/orientation/margins; writes
 * them back into *s. */
HRESULT WINAPI LeewayPageSetupDialog(HWND owner, LeewayPrintSettings *s);

/* Range + copies the Print dialog collects, for the app's print loop. */
typedef struct {
    UINT32 from_page;      /* 1-based; 0 = "all" selected */
    UINT32 to_page;
    UINT32 min_page;       /* in:  document bounds (1-based) */
    UINT32 max_page;
    UINT32 copies;         /* out: copies (collation already applied by the driver when supported) */
    BOOL   selection;      /* out: user chose "Selection" */
    BOOL   all_pages;      /* out: user chose "All" */
} LeewayPrintRange;

/* PrintDlgEx wrapper. On S_OK, *s is updated and, if print_dc_out != NULL, a
 * ready-to-use device DC is returned (caller DeleteDC()s it); range/copies are
 * written into *range. Pass range->min_page/max_page in. */
HRESULT WINAPI LeewayPrintDialog(HWND owner, LeewayPrintSettings *s,
                                 LeewayPrintRange *range, HDC *print_dc_out);

/* ============================ Preview frame ============================= */

typedef struct ILeewayPrintPreview ILeewayPrintPreview;

/* How many pages the document currently has (>=1). */
typedef UINT32 (*LeewayPreviewPageCountFn)(void *ctx);

/* Draw page `index` (0-based) into hdc. The frame has already filled the sheet
 * white and will draw the margin guides; `paper` is the full sheet rect and
 * `content` is the margin box, both in device px of `dpi`, already scaled for
 * the current zoom and one/two-up layout. The app draws page content clipped to
 * `content` (or to `paper` if it manages its own margins). Return S_OK. */
typedef HRESULT (*LeewayPreviewRenderPageFn)(UINT32 index, HDC hdc, const RECT *paper,
                                             const RECT *content, UINT32 dpi, void *ctx);

/* Toolbar actions the app performs (the frame only raises them). */
typedef enum {
    LWUI_PREVIEW_PRINT = 1,    /* "Print": app calls LeewayPrintDialog + its print loop */
    LWUI_PREVIEW_PAGESETUP,    /* "Page setup": app calls LeewayPageSetupDialog, then Refresh */
    LWUI_PREVIEW_CLOSE,        /* "Close print preview": app hides the frame (Show FALSE) */
} LeewayPreviewAction;
typedef void (*LeewayPreviewActionFn)(LeewayPreviewAction action, void *ctx);

/* config.flags */
#define LWUI_PREVIEW_FIT  0x00000001u   /* reserved: show a "Fit to page" toggle in the tab (lands with the Win7 Page setup layer) */

typedef struct {
    LeewayPreviewPageCountFn   page_count;    /* required */
    LeewayPreviewRenderPageFn  render_page;   /* required */
    LeewayPreviewActionFn      on_action;     /* required */
    void                      *context;
    const LeewayPrintSettings *settings;      /* paper geometry source; NULL = Letter + 1in */
    const WCHAR               *app_name;       /* persist one/two-up + zoom under HKCU\Software\Leeway\<app_name>; NULL = none */
    LeewayThemePref            theme;          /* SYSTEM / LIGHT / DARK */
    UINT32                     flags;          /* LWUI_PREVIEW_* (0 = defaults) */
} LeewayPrintPreviewConfig;

/* Create the frame as a child of `parent` (hidden). The app positions it with
 * MoveWindow like any child and calls Show to enter preview. */
HRESULT WINAPI LeewayPrintPreviewCreate(HWND parent, const LeewayPrintPreviewConfig *cfg,
                                        ILeewayPrintPreview **out);
HRESULT WINAPI LeewayPrintPreviewDestroy(ILeewayPrintPreview *pv);
HWND    WINAPI LeewayPrintPreviewGetHWND(ILeewayPrintPreview *pv);

/* Enter / leave preview: show or hide the frame. On show it re-queries the page
 * count and resets to page 0. The app typically hides its main ribbon + document
 * while active and restores them on LWUI_PREVIEW_CLOSE. */
HRESULT WINAPI LeewayPrintPreviewShow(ILeewayPrintPreview *pv, BOOL show);
BOOL    WINAPI LeewayPrintPreviewIsActive(ILeewayPrintPreview *pv);

/* Re-query page count + paper and repaint (after an edit, printer or page-setup
 * change). */
HRESULT WINAPI LeewayPrintPreviewRefresh(ILeewayPrintPreview *pv);

/* Current page (0-based) the app can read or drive. */
HRESULT WINAPI LeewayPrintPreviewSetPage(ILeewayPrintPreview *pv, UINT32 index);
UINT32  WINAPI LeewayPrintPreviewGetPage(ILeewayPrintPreview *pv);

/* Pages shown side by side: 1 or 2. */
HRESULT WINAPI LeewayPrintPreviewSetPagesShown(ILeewayPrintPreview *pv, UINT32 pages);
UINT32  WINAPI LeewayPrintPreviewGetPagesShown(ILeewayPrintPreview *pv);

/* Give the frame's ribbon the same icon resolver the app uses for its main
 * ribbon (e.g. the stock Aureline resolver), so the Print Preview tab shows
 * real icons. NULL = the placeholder glyphs. */
HRESULT WINAPI LeewayPrintPreviewSetIconResolver(ILeewayPrintPreview *pv,
                                                 LeewayUIIconResolver resolver, void *resolver_context);

/* Forward from the top-level window, same as the ribbon. */
HRESULT WINAPI LeewayPrintPreviewOnThemeChanged(ILeewayPrintPreview *pv);
HRESULT WINAPI LeewayPrintPreviewSetDPI(ILeewayPrintPreview *pv, UINT32 dpi);
HRESULT WINAPI LeewayPrintPreviewSetThemePreference(ILeewayPrintPreview *pv, LeewayThemePref pref);

/* Call from the message loop before TranslateMessage while preview is active
 * (handles the ribbon keytips, PgUp/PgDn, +/- zoom, Esc = close). TRUE if consumed. */
BOOL    WINAPI LeewayPrintPreviewPreTranslateMessage(ILeewayPrintPreview *pv, MSG *msg);

#ifdef __cplusplus
}
#endif
