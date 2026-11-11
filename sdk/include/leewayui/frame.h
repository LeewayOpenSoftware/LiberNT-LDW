/*
 * PROJECT:     LeewayUI
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     G7 frame template: chrome (ribbon | pill), status bar, zoom slider, layout
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 * PROVENANCE:  sdk/lib/leewayui/LEEWAYUI-PROVENANCE.md
 *
 * The frame owns the status bar and its zoom slider, decides which chrome the
 * window shows, and computes the three rectangles the app lays out around:
 *
 *      +--------------------------------+
 *      | chrome  (ribbon or pill band)  |   LeewayUIFrameRects.chrome
 *      +--------------------------------+
 *      | work area (the app's content)  |   .work
 *      +--------------------------------+
 *      | status bar  [info | zoom% |-o-] |   .status
 *      +--------------------------------+
 *
 * Chrome:
 *   ribbon  the frame creates the ribbon (ribbon.h) and owns it.
 *   pill    the app keeps its own toolbar; the frame only reserves
 *           `pill_height` pixels and returns that rect in .chrome.
 *   The registry decides, HKCU\Software\Leeway\<app_name>\ChromeStyle (REG_SZ
 *   "ribbon" or "pill"); the descriptor's default_chrome applies when the
 *   value is absent or unreadable. If the ribbon cannot be created the frame
 *   falls back to pill and LeewayFrameGetChrome reports the effective style.
 *
 * Status bar visibility persists as HKCU\Software\Leeway\<app_name>\StatusBar
 * (REG_DWORD, 0 hidden, default 1).
 *
 * Zoom slider: a trackbar whose positions are the descriptor's zoom_levels
 * (percent values, ascending). Dragging reports the new percent through
 * on_zoom; the app does the zoom and calls LeewayFrameSetZoom to confirm.
 * LeewayFrameSetZoom never calls on_zoom (no feedback loop) and snaps the
 * thumb to the nearest level.
 *
 * Message routing: the app forwards WM_HSCROLL, WM_NOTIFY (ribbon height
 * changes), theme and DPI messages to LeewayFrameHandleMessage and returns the result if it says
 * TRUE. No subclassing, no hidden hooks.
 */

#pragma once

#include <windows.h>
#include <leewayui/ribbon.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LeewayUIFrame LeewayUIFrame;

typedef enum {
    LWUI_CHROME_PILL = 0,
    LWUI_CHROME_RIBBON = 1,
} LeewayUIChromeStyle;

/* Descriptor flags */
#define LWUI_FRAME_STATUSBAR     0x0001   /* create a status bar */
#define LWUI_FRAME_ZOOM_SLIDER   0x0002   /* ... with a zoom slider (needs zoom_levels) */

/* Zoom request from the slider. percent is one of zoom_levels. */
typedef void (*LeewayUIFrameZoomHandler)(UINT32 percent, void *app_context);

typedef struct {
    RECT chrome;                        /* ribbon or pill band, client coordinates */
    RECT work;                          /* what is left for the app */
    RECT status;                        /* empty when the status bar is hidden */
} LeewayUIFrameRects;

/* The frame re-ran the layout by itself (the ribbon changed height: minimise,
 * QAT below, DPI). Move the app's content to rects->work. */
typedef void (*LeewayUIFrameLayoutHandler)(const LeewayUIFrameRects *rects, void *app_context);

typedef struct {
    UINT32 struct_size;                 /* sizeof(LeewayUIFrameDesc) */
    HWND owner;                         /* top-level window that hosts the chrome */
    HINSTANCE instance;
    const WCHAR *app_name;              /* registry key name, e.g. L"PhotoViewer" */
    UINT32 flags;                       /* LWUI_FRAME_* */

    LeewayUIChromeStyle default_chrome; /* used when the registry has no value */
    UINT32 pill_height;                 /* logical px reserved in pill mode (app toolbar) */

    /* ribbon (used when the effective chrome is ribbon) */
    const LeewayUICommand *commands;
    UINT32 command_count;
    LeewayUICommandExecuteHandler execute;
    LeewayUICommandStateHandler state;
    ILeewayRibbon *ribbon;              /* optional: a ribbon the app already built; borrowed,
                                           the frame lays it out but never destroys it */

    /* zoom slider */
    const UINT32 *zoom_levels;          /* ascending percents, at most 32 */
    UINT32 zoom_level_count;
    LeewayUIFrameZoomHandler on_zoom;
    LeewayUIFrameLayoutHandler on_layout;   /* optional, see above */

    void *app_context;                  /* passed to execute, state, on_zoom */
} LeewayUIFrameDesc;

HRESULT WINAPI LeewayFrameCreate(const LeewayUIFrameDesc *desc, LeewayUIFrame **out);
void WINAPI LeewayFrameDestroy(LeewayUIFrame *frame);

/* Effective chrome (after registry and fallback). */
LeewayUIChromeStyle WINAPI LeewayFrameGetChrome(const LeewayUIFrame *frame);

/* NULL in pill mode. Borrowed: the frame owns it. */
ILeewayRibbon * WINAPI LeewayFrameGetRibbon(LeewayUIFrame *frame);

/* Registry helpers, usable before a frame exists. */
LeewayUIChromeStyle WINAPI LeewayFrameReadChromeStyle(const WCHAR *app_name, LeewayUIChromeStyle default_chrome);
BOOL WINAPI LeewayFrameWriteChromeStyle(const WCHAR *app_name, LeewayUIChromeStyle style);

/* Positions the ribbon (ribbon mode), the status bar and the slider inside
 * `client` and returns the rectangles. Moves only the frame's own windows. */
void WINAPI LeewayFrameLayout(LeewayUIFrame *frame, const RECT *client, LeewayUIFrameRects *out);

/* Status bar. Part 0 is the info text (image size, etc.), part 1 the zoom
 * percent text which SetZoom maintains. */
BOOL WINAPI LeewayFrameShowStatusBar(LeewayUIFrame *frame, BOOL show, BOOL persist);
BOOL WINAPI LeewayFrameIsStatusBarShown(const LeewayUIFrame *frame);
void WINAPI LeewayFrameSetStatusText(LeewayUIFrame *frame, UINT32 part, const WCHAR *text);
HWND WINAPI LeewayFrameGetStatusWindow(const LeewayUIFrame *frame);

/* Zoom slider sync. SetZoom accepts any percent and snaps the thumb. */
void WINAPI LeewayFrameSetZoom(LeewayUIFrame *frame, UINT32 percent);
UINT32 WINAPI LeewayFrameGetZoom(const LeewayUIFrame *frame);
void WINAPI LeewayFrameEnableZoom(LeewayUIFrame *frame, BOOL enable);

/* Forward WM_HSCROLL (slider), WM_THEMECHANGED, WM_SETTINGCHANGE and
 * WM_DPICHANGED here. Returns TRUE if handled, with the window procedure's
 * result in *result. */
BOOL WINAPI LeewayFrameHandleMessage(LeewayUIFrame *frame, UINT msg, WPARAM wparam, LPARAM lparam, LRESULT *result);

#ifdef __cplusplus
}
#endif
