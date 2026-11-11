/*
 * PROJECT:     LeewayUI
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test window: the Photo Viewer ribbon from RIBBON-APPS-SPEC.md §1 on the F1 framework
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 * PROVENANCE:  sdk/lib/leewayui/LEEWAYUI-PROVENANCE.md
 *
 * Exercises: layout tree with two tabs and a contextual tab, every F1 control
 * kind, collections (zoom gallery, rotate and slideshow split menus), QAT with
 * persistence (HKCU\Software\Leeway\LeewayUIRibbonTest\QAT), application
 * menu, keytips (Alt, H, L rotates left; Alt, F, O opens; Alt, V, F toggles
 * full screen), tooltips with accelerator text, group variants (narrow the
 * window), minimise (Ctrl+F1, double-click a tab), theme and DPI changes.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <stdio.h>
#include <leewayui/ribbon.h>
#include <leewayui/theme.h>
#include <leewayui/dpi.h>

/* Viewer command ids (shimgvw resource.h values; new ids per spec §1) */
enum {
    IDC_PREV_PIC = 500, IDC_NEXT_PIC, IDC_BEST_FIT, IDC_REAL_SIZE, IDC_SLIDE_SHOW, IDC_ZOOM_IN, IDC_ZOOM_OUT,
    IDC_ROT_CLOCKW, IDC_ROT_COUNCW, IDC_ROT_CWSAVE, IDC_ROT_CCWSAVE, IDC_DELETE, IDC_PRINT, IDC_SAVEAS,
    IDC_MODIFY, IDC_HELP_TOC,
    IDC_TOGGLEFULLSCREEN = 600, IDC_INCTIMER, IDC_DECTIMER,
    ID_OPEN = 700, ID_WALLPAPER, ID_EXIT, ID_ZOOM_GALLERY = 710,
    ID_ZOOM_10 = 711, ID_ZOOM_25, ID_ZOOM_50, ID_ZOOM_75, ID_ZOOM_100, ID_ZOOM_150, ID_ZOOM_200, ID_ZOOM_400, ID_ZOOM_800, ID_ZOOM_1600,
    ID_ANIM_PLAY = 750, ID_ANIM_PREV, ID_ANIM_NEXT,
    ID_DEMO_ANIMATED = 790,     /* demo only: toggles the "Animation" context */
};
enum { TAB_HOME = 1, TAB_VIEW = 2, TAB_ANIMATION = 3 };

struct AppState {
    HWND main_window;
    ILeewayRibbon *ribbon;
    HWND client_area;
    HACCEL accel;
    int zoom;               /* percent */
    BOOL fit, actual, fullscreen, slideshow, animated, statusbar, has_image;
    int image_index, image_count;
    WCHAR last[160];
};

static AppState g_app;

static const LeewayUICommand g_commands[] = {
    /* id, label, tooltip, keytip, icon, enabled, checked, visible */
    {ID_OPEN, L"Open...", L"Open a picture", L"O", 1, TRUE, FALSE, TRUE},
    {IDC_SAVEAS, L"Save as...", L"Save a copy of the picture", L"A", 2, TRUE, FALSE, TRUE},
    {IDC_PRINT, L"Print", L"Print the picture", L"P", 3, TRUE, FALSE, TRUE},
    {IDC_MODIFY, L"Edit", L"Open the picture in an editor", L"E", 4, TRUE, FALSE, TRUE},
    {ID_WALLPAPER, L"Set as desktop background", NULL, L"B", 5, TRUE, FALSE, TRUE},
    {IDC_HELP_TOC, L"Help", L"Help topics", L"H", 6, TRUE, FALSE, TRUE},
    {ID_EXIT, L"Exit", L"Close the viewer", L"X", 7, TRUE, FALSE, TRUE},
    {IDC_PREV_PIC, L"Previous", L"Previous picture", L"P", 8, TRUE, FALSE, TRUE},
    {IDC_NEXT_PIC, L"Next", L"Next picture", L"N", 9, TRUE, FALSE, TRUE},
    {IDC_BEST_FIT, L"Fit", L"Fit the picture to the window", L"F", 10, TRUE, FALSE, TRUE},
    {IDC_REAL_SIZE, L"Actual size", L"Show the picture at 100 %", L"A", 11, TRUE, FALSE, TRUE},
    {IDC_ZOOM_IN, L"Zoom in", NULL, L"I", 12, TRUE, FALSE, TRUE},
    {IDC_ZOOM_OUT, L"Zoom out", NULL, L"O", 13, TRUE, FALSE, TRUE},
    {ID_ZOOM_GALLERY, L"Zoom level", L"Choose a zoom level", L"Z", 14, TRUE, FALSE, TRUE},
    {ID_ZOOM_10, L"10 %", NULL, NULL, 0, TRUE, FALSE, TRUE}, {ID_ZOOM_25, L"25 %", NULL, NULL, 0, TRUE, FALSE, TRUE},
    {ID_ZOOM_50, L"50 %", NULL, NULL, 0, TRUE, FALSE, TRUE}, {ID_ZOOM_75, L"75 %", NULL, NULL, 0, TRUE, FALSE, TRUE},
    {ID_ZOOM_100, L"100 %", NULL, NULL, 0, TRUE, FALSE, TRUE}, {ID_ZOOM_150, L"150 %", NULL, NULL, 0, TRUE, FALSE, TRUE},
    {ID_ZOOM_200, L"200 %", NULL, NULL, 0, TRUE, FALSE, TRUE}, {ID_ZOOM_400, L"400 %", NULL, NULL, 0, TRUE, FALSE, TRUE},
    {ID_ZOOM_800, L"800 %", NULL, NULL, 0, TRUE, FALSE, TRUE}, {ID_ZOOM_1600, L"1600 %", NULL, NULL, 0, TRUE, FALSE, TRUE},
    {IDC_ROT_COUNCW, L"Rotate left", L"Rotate the view 90 degrees left", L"L", 15, TRUE, FALSE, TRUE},
    {IDC_ROT_CLOCKW, L"Rotate right", L"Rotate the view 90 degrees right", L"R", 16, TRUE, FALSE, TRUE},
    {IDC_ROT_CCWSAVE, L"Rotate left and save", NULL, L"L", 15, TRUE, FALSE, TRUE},
    {IDC_ROT_CWSAVE, L"Rotate right and save", NULL, L"R", 16, TRUE, FALSE, TRUE},
    {IDC_SLIDE_SHOW, L"Slideshow", L"Start or stop the slideshow", L"S", 17, TRUE, FALSE, TRUE},
    {IDC_INCTIMER, L"Faster", L"Shorter interval between pictures", L"F", 0, TRUE, FALSE, TRUE},
    {IDC_DECTIMER, L"Slower", L"Longer interval between pictures", L"S", 0, TRUE, FALSE, TRUE},
    {IDC_DELETE, L"Delete", L"Move the picture to the Recycle Bin", L"D", 18, TRUE, FALSE, TRUE},
    {IDC_TOGGLEFULLSCREEN, L"Full screen", L"Show the picture full screen", L"F", 19, TRUE, FALSE, TRUE},
    {LWUI_STATUSBAR_TOGGLE, L"Status bar", L"Show or hide the status bar", L"S", 20, TRUE, TRUE, TRUE},
    {LWUI_RIBBON_MINIMIZE, L"Minimize ribbon", L"Show only the tab names", L"M", 21, TRUE, FALSE, TRUE},
    {ID_ANIM_PLAY, L"Play", L"Play or pause the animation", L"P", 0, TRUE, FALSE, TRUE},
    {ID_ANIM_PREV, L"Previous frame", NULL, L"R", 0, TRUE, FALSE, TRUE},
    {ID_ANIM_NEXT, L"Next frame", NULL, L"N", 0, TRUE, FALSE, TRUE},
    {ID_DEMO_ANIMATED, L"Animated picture", L"Demo: pretend the picture is animated (shows the contextual tab)", L"G", LWUI_ICON_GLYPH | 22, TRUE, FALSE, TRUE},
};

/* ---- layout (RIBBON-APPS-SPEC.md §1.3-1.7) ---- */

static const LeewayUIPlacement g_navigate[] = {
    {IDC_PREV_PIC, LWUI_CONTROL_BUTTON_LARGE, NULL},
    {IDC_NEXT_PIC, LWUI_CONTROL_BUTTON_LARGE, NULL},
};
static const LeewayUIPlacement g_zoom[] = {
    {IDC_BEST_FIT, LWUI_CONTROL_TOGGLE_LARGE, NULL},
    {IDC_REAL_SIZE, LWUI_CONTROL_TOGGLE_LARGE, NULL},
    {IDC_ZOOM_IN, LWUI_CONTROL_BUTTON_SMALL, NULL},
    {IDC_ZOOM_OUT, LWUI_CONTROL_BUTTON_SMALL, NULL},
    {ID_ZOOM_GALLERY, LWUI_CONTROL_GALLERY, NULL},
};
/* View tab copy of the Zoom group: Full screen takes F there (spec §1.6, Alt,V,F), so Fit gets B (Ctrl+B) */
static const LeewayUIPlacement g_zoom_view[] = {
    {IDC_BEST_FIT, LWUI_CONTROL_TOGGLE_LARGE, L"B"},
    {IDC_REAL_SIZE, LWUI_CONTROL_TOGGLE_LARGE, NULL},
    {IDC_ZOOM_IN, LWUI_CONTROL_BUTTON_SMALL, NULL},
    {IDC_ZOOM_OUT, LWUI_CONTROL_BUTTON_SMALL, NULL},
    {ID_ZOOM_GALLERY, LWUI_CONTROL_GALLERY, NULL},
};
static const LeewayUIPlacement g_rotate[] = {
    {IDC_ROT_COUNCW, LWUI_CONTROL_SPLIT_LARGE, NULL},
    {IDC_ROT_CLOCKW, LWUI_CONTROL_SPLIT_LARGE, NULL},
};
static const LeewayUIPlacement g_slideshow[] = {
    {IDC_SLIDE_SHOW, LWUI_CONTROL_SPLIT_LARGE, NULL},
};
static const LeewayUIPlacement g_file[] = {
    {IDC_DELETE, LWUI_CONTROL_BUTTON_LARGE, NULL},
    {IDC_SAVEAS, LWUI_CONTROL_BUTTON_SMALL, L"V"},    /* per-placement keytip: A in the app menu, V here */
    {IDC_PRINT, LWUI_CONTROL_BUTTON_SMALL, L"T"},
    {IDC_MODIFY, LWUI_CONTROL_BUTTON_SMALL, NULL},
};
static const LeewayUIPlacement g_display[] = {
    {IDC_TOGGLEFULLSCREEN, LWUI_CONTROL_TOGGLE_LARGE, NULL},
    {LWUI_STATUSBAR_TOGGLE, LWUI_CONTROL_TOGGLE_SMALL, NULL},
    {LWUI_RIBBON_MINIMIZE, LWUI_CONTROL_TOGGLE_SMALL, NULL},
    {ID_DEMO_ANIMATED, LWUI_CONTROL_TOGGLE_SMALL, NULL},
};
static const LeewayUIPlacement g_help[] = {
    {IDC_HELP_TOC, LWUI_CONTROL_BUTTON_SMALL, NULL},
};
static const LeewayUIPlacement g_anim[] = {
    {ID_ANIM_PLAY, LWUI_CONTROL_TOGGLE_LARGE, NULL},
    {ID_ANIM_PREV, LWUI_CONTROL_BUTTON_SMALL, NULL},
    {ID_ANIM_NEXT, LWUI_CONTROL_BUTTON_SMALL, NULL},
};

#define ORDER_ALL LWUI_VARIANT_ORDER(LWUI_VARIANT_LARGE, LWUI_VARIANT_MIXED, LWUI_VARIANT_SMALL, LWUI_VARIANT_COLLAPSED)

static const LeewayUIGroup g_home_groups[] = {
    {L"Navigate", L"GN", 0, 0, 2, g_navigate},
    {L"Zoom", L"GZ", ORDER_ALL, 0, 5, g_zoom},
    {L"Rotate", L"GR", 0, 0, 2, g_rotate},
    {L"Slideshow", L"GS", 0, 0, 1, g_slideshow},
    {L"File", L"GF", LWUI_VARIANT_ORDER(LWUI_VARIANT_MIXED, LWUI_VARIANT_SMALL, LWUI_VARIANT_COLLAPSED, 0), 0, 4, g_file},
};
static const LeewayUIGroup g_view_groups[] = {
    {L"Zoom", L"GZ", ORDER_ALL, 0, 5, g_zoom_view},
    {L"Display", L"GD", LWUI_VARIANT_ORDER(LWUI_VARIANT_LARGE, LWUI_VARIANT_SMALL, LWUI_VARIANT_COLLAPSED, 0), 0, 4, g_display},
    {L"Help", L"GH", LWUI_VARIANT_ORDER(LWUI_VARIANT_SMALL, 0, 0, 0), 0, 1, g_help},
};
static const LeewayUIGroup g_anim_groups[] = {
    {L"Frames", L"GA", 0, 0, 3, g_anim},
};
static const LeewayUITab g_tabs[] = {
    {TAB_HOME, L"Home", L"H", NULL, 5, g_home_groups},
    {TAB_VIEW, L"View", L"V", NULL, 3, g_view_groups},
    {TAB_ANIMATION, L"Animation", L"A", L"IsAnimated", 1, g_anim_groups},
};
static const UINT32 g_appmenu[] = {ID_OPEN, IDC_SAVEAS, IDC_PRINT, IDC_MODIFY, ID_WALLPAPER, IDC_HELP_TOC, ID_EXIT};
static const UINT32 g_qat_default[] = {IDC_PREV_PIC, IDC_NEXT_PIC, IDC_ROT_COUNCW, IDC_ROT_CLOCKW};

static const LeewayUILayout g_layout = {
    L"LeewayUIRibbonTest", 3, g_tabs, 7, g_appmenu, 4, g_qat_default, IDC_HELP_TOC,
};

static const LeewayUICollectionItem g_zoom_levels[] = {
    {ID_ZOOM_10, L"10 %", 0}, {ID_ZOOM_25, L"25 %", 0}, {ID_ZOOM_50, L"50 %", 0}, {ID_ZOOM_75, L"75 %", 0},
    {ID_ZOOM_100, L"100 %", 0}, {ID_ZOOM_150, L"150 %", 0}, {ID_ZOOM_200, L"200 %", 0}, {ID_ZOOM_400, L"400 %", 0},
    {ID_ZOOM_800, L"800 %", 0}, {ID_ZOOM_1600, L"1600 %", 0}, {IDC_BEST_FIT, L"Fit", 10}, {IDC_REAL_SIZE, L"Actual size", 11},
};
static const LeewayUICollectionItem g_rot_left_menu[] = {{IDC_ROT_CCWSAVE, L"Rotate left and save", 15}};
static const LeewayUICollectionItem g_rot_right_menu[] = {{IDC_ROT_CWSAVE, L"Rotate right and save", 16}};
static const LeewayUICollectionItem g_slideshow_menu[] = {{IDC_INCTIMER, L"Faster", 0}, {IDC_DECTIMER, L"Slower", 0}};

static const int g_levels[] = {10, 25, 50, 75, 100, 150, 200, 400, 800, 1600};

static void Note(const WCHAR *what)
{
    lstrcpynW(g_app.last, what, 160);
    if (g_app.client_area) SetWindowTextW(g_app.client_area, g_app.last);
}

static void Relayout(HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    int h = g_app.ribbon ? (int)LeewayRibbonGetHeight(g_app.ribbon) : 0;
    if (g_app.ribbon) SetWindowPos(LeewayRibbonGetHWND(g_app.ribbon), NULL, 0, 0, rc.right, h, SWP_NOZORDER | SWP_NOACTIVATE);
    if (g_app.client_area) SetWindowPos(g_app.client_area, NULL, 0, h, rc.right, rc.bottom - h, SWP_NOZORDER | SWP_NOACTIVATE);
}

/* ---- the two callbacks ---- */

static HRESULT OnExecute(UINT32 id, void *)
{
    WCHAR msg[160];
    switch (id) {
    case IDC_BEST_FIT: g_app.fit = TRUE; g_app.actual = FALSE; g_app.zoom = 100; break;
    case IDC_REAL_SIZE: g_app.actual = TRUE; g_app.fit = FALSE; g_app.zoom = 100; break;
    case IDC_ZOOM_IN: g_app.fit = g_app.actual = FALSE; if (g_app.zoom < 1600) g_app.zoom = g_app.zoom < 100 ? g_app.zoom + 25 : g_app.zoom * 2; break;
    case IDC_ZOOM_OUT: g_app.fit = g_app.actual = FALSE; if (g_app.zoom > 10) g_app.zoom = g_app.zoom <= 100 ? g_app.zoom - 25 : g_app.zoom / 2; if (g_app.zoom < 10) g_app.zoom = 10; break;
    case IDC_TOGGLEFULLSCREEN: g_app.fullscreen = !g_app.fullscreen; break;
    case IDC_SLIDE_SHOW: g_app.slideshow = !g_app.slideshow; break;
    case LWUI_STATUSBAR_TOGGLE: g_app.statusbar = !g_app.statusbar; break;
    case ID_DEMO_ANIMATED:
        g_app.animated = !g_app.animated;
        LeewayRibbonSetContext(g_app.ribbon, L"IsAnimated", g_app.animated);
        break;
    case IDC_PREV_PIC: if (g_app.image_index > 0) g_app.image_index--; break;
    case IDC_NEXT_PIC: if (g_app.image_index + 1 < g_app.image_count) g_app.image_index++; break;
    case ID_EXIT: PostMessageW(g_app.main_window, WM_CLOSE, 0, 0); break;
    case LWUI_HELP: case IDC_HELP_TOC: break;
    default:
        if (id >= ID_ZOOM_10 && id <= ID_ZOOM_1600) {
            g_app.zoom = g_levels[id - ID_ZOOM_10];
            g_app.fit = g_app.actual = FALSE;
        }
        break;
    }
    const LeewayUICommand *c = NULL;
    for (UINT32 i = 0; i < sizeof(g_commands) / sizeof(g_commands[0]); i++) if (g_commands[i].id == id) c = &g_commands[i];
    wsprintfW(msg, L"execute(%u) %s   zoom %d %%, picture %d of %d", id, c ? c->label : L"", g_app.zoom, g_app.image_index + 1, g_app.image_count);
    Note(msg);
    /* the app owns state: tell the ribbon to re-read it (spec G3) */
    LeewayRibbonInvalidateAll(g_app.ribbon);
    return S_OK;
}

static HRESULT OnQueryState(UINT32 id, LeewayUICommandState *st, void *)
{
    static WCHAR zoom_label[16];
    BOOL has_list = g_app.image_count > 1;
    switch (id) {
    case IDC_PREV_PIC: st->enabled = has_list && g_app.image_index > 0; break;
    case IDC_NEXT_PIC: st->enabled = has_list && g_app.image_index + 1 < g_app.image_count; break;
    case IDC_BEST_FIT: st->checked = g_app.fit; break;
    case IDC_REAL_SIZE: st->checked = g_app.actual; break;
    case IDC_ZOOM_IN: st->enabled = g_app.zoom < 1600; break;
    case IDC_ZOOM_OUT: st->enabled = g_app.zoom > 10; break;
    case ID_ZOOM_GALLERY: wsprintfW(zoom_label, L"%d %%", g_app.zoom); st->label = zoom_label; break;
    case IDC_TOGGLEFULLSCREEN: st->checked = g_app.fullscreen; break;
    case IDC_SLIDE_SHOW: st->checked = g_app.slideshow; break;
    case LWUI_STATUSBAR_TOGGLE: st->checked = g_app.statusbar; break;
    case ID_DEMO_ANIMATED: st->checked = g_app.animated; break;
    case ID_ANIM_PLAY: st->checked = g_app.slideshow; break;
    default: break;
    }
    return S_OK;
}

/* Demo icon resolver (G10): slots 1-22 draw as coloured tiles so the
 * placeholder path and the resolver path can both be seen; 0 = placeholder. */
static HICON ResolveIcon(UINT32 icon_id, UINT32 size, LeewayUIIconVariant variant, COLORREF fg, COLORREF bg, void *)
{
    if (icon_id == 0 || icon_id > 22 || (icon_id % 3) == 0) return NULL;   /* every third slot stays a placeholder */
    HDC screen = GetDC(NULL);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP color = CreateCompatibleBitmap(screen, size, size);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, NULL);
    HGDIOBJ old = SelectObject(mem, color);
    RECT rc = {0, 0, (LONG)size, (LONG)size};
    HBRUSH b = CreateSolidBrush(bg);
    FillRect(mem, &rc, b);
    DeleteObject(b);
    COLORREF tint = variant == LWUI_ICON_VARIANT_MONO ? fg : RGB(60 + (icon_id * 37) % 160, 110 + (icon_id * 53) % 120, 200);
    HBRUSH t = CreateSolidBrush(tint);
    HPEN p = CreatePen(PS_SOLID, 1, variant == LWUI_ICON_VARIANT_MONO ? fg : RGB(23, 63, 122));
    HGDIOBJ ob = SelectObject(mem, variant == LWUI_ICON_VARIANT_MONO ? GetStockObject(NULL_BRUSH) : t), op = SelectObject(mem, p);
    Ellipse(mem, 2, 2, size - 2, size - 2);
    SelectObject(mem, ob);
    SelectObject(mem, op);
    DeleteObject(t);
    DeleteObject(p);
    SelectObject(mem, mask);
    HBRUSH white = (HBRUSH)GetStockObject(WHITE_BRUSH);
    FillRect(mem, &rc, white);
    HGDIOBJ ob2 = SelectObject(mem, GetStockObject(BLACK_BRUSH)), op2 = SelectObject(mem, GetStockObject(BLACK_PEN));
    Ellipse(mem, 2, 2, size - 2, size - 2);
    SelectObject(mem, ob2);
    SelectObject(mem, op2);
    SelectObject(mem, old);
    ICONINFO ii;
    ii.fIcon = TRUE;
    ii.xHotspot = ii.yHotspot = 0;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
    return icon;
}

static LRESULT CALLBACK MainWindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_CREATE: {
        g_app.main_window = hwnd;
        g_app.zoom = 100;
        g_app.fit = TRUE;
        g_app.statusbar = TRUE;
        g_app.has_image = TRUE;
        g_app.image_count = 5;

        g_app.client_area = CreateWindowW(L"STATIC", L"LeewayUI F1: the Photo Viewer ribbon. Alt shows keytips; narrow the window to see groups collapse; Ctrl+F1 minimises.",
                                          WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE, 0, 0, 10, 10, hwnd, NULL, GetModuleHandleW(NULL), NULL);

        if (FAILED(LeewayRibbonCreate(hwnd, g_commands, sizeof(g_commands) / sizeof(g_commands[0]), OnExecute, OnQueryState, &g_app, &g_app.ribbon))) {
            Note(L"LeewayRibbonCreate failed");
            return 0;
        }
        LeewayRibbonSetIconResolver(g_app.ribbon, ResolveIcon, NULL);
        LeewayRibbonSetAccelerators(g_app.ribbon, g_app.accel);
        LeewayRibbonSetCollection(g_app.ribbon, ID_ZOOM_GALLERY, g_zoom_levels, 12, 0);
        LeewayRibbonSetCollection(g_app.ribbon, IDC_ROT_COUNCW, g_rot_left_menu, 1, 0);     /* primary never changes (spec §1.5) */
        LeewayRibbonSetCollection(g_app.ribbon, IDC_ROT_CLOCKW, g_rot_right_menu, 1, 0);
        LeewayRibbonSetCollection(g_app.ribbon, IDC_SLIDE_SHOW, g_slideshow_menu, 2, 0);
        HRESULT hr = LeewayRibbonSetLayout(g_app.ribbon, &g_layout);
        if (FAILED(hr)) {
            WCHAR m[64];
            wsprintfW(m, L"SetLayout failed: 0x%08X", (unsigned)hr);
            Note(m);
        }
        Relayout(hwnd);
        return 0;
    }
    case WM_SIZE:
        Relayout(hwnd);
        return 0;
    case WM_NOTIFY: {
        const NMHDR *nm = (const NMHDR *)lparam;
        if (g_app.ribbon && nm->hwndFrom == LeewayRibbonGetHWND(g_app.ribbon)) {
            if (nm->code == LWUI_RBN_HEIGHTCHANGED) Relayout(hwnd);
            else if (nm->code == LWUI_RBN_QATCHANGED) Note(L"QAT customised and persisted");
        }
        return 0;
    }
    case WM_COMMAND:
        /* accelerators route through the same execute handler as the ribbon */
        if (HIWORD(wparam) == 1) OnExecute(LOWORD(wparam), &g_app);
        return 0;
    case WM_SYSCOMMAND:
        if ((wparam & 0xFFF0) == SC_KEYMENU && g_app.ribbon) return 0;   /* Alt handled by the ribbon's keytips */
        break;
    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
        if (g_app.ribbon) LeewayRibbonOnThemeChanged(g_app.ribbon);
        break;
    case WM_DPICHANGED: {
        if (g_app.ribbon) LeewayRibbonSetDPI(g_app.ribbon, HIWORD(wparam));
        const RECT *rect = (const RECT *)lparam;
        SetWindowPos(hwnd, NULL, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_KEYDOWN:
        if (wparam == VK_F6 && g_app.ribbon) SetFocus(LeewayRibbonGetHWND(g_app.ribbon));   /* the ribbon is one Tab stop */
        return 0;
    case WM_DESTROY:
        if (g_app.ribbon) {
            LeewayRibbonDestroy(g_app.ribbon);
            g_app.ribbon = NULL;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int)
{
    static const ACCEL accels[] = {
        {FCONTROL | FVIRTKEY, 'O', ID_OPEN}, {FCONTROL | FVIRTKEY, 'S', IDC_SAVEAS}, {FCONTROL | FVIRTKEY, 'P', IDC_PRINT},
        {FCONTROL | FVIRTKEY, 'E', IDC_MODIFY}, {FCONTROL | FVIRTKEY, 'B', IDC_BEST_FIT}, {FCONTROL | FVIRTKEY, 'A', IDC_REAL_SIZE},
        {FCONTROL | FVIRTKEY, 'K', IDC_ROT_COUNCW}, {FCONTROL | FVIRTKEY, 'L', IDC_ROT_CLOCKW}, {FVIRTKEY, VK_F11, IDC_TOGGLEFULLSCREEN},
        {FVIRTKEY, VK_LEFT, IDC_PREV_PIC}, {FVIRTKEY, VK_RIGHT, IDC_NEXT_PIC}, {FVIRTKEY, VK_DELETE, IDC_DELETE},
        {FVIRTKEY, VK_ADD, IDC_ZOOM_IN}, {FVIRTKEY, VK_SUBTRACT, IDC_ZOOM_OUT}, {FVIRTKEY, VK_F1, IDC_HELP_TOC},
    };
    g_app.accel = CreateAcceleratorTableW((LPACCEL)accels, sizeof(accels) / sizeof(accels[0]));

    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = MainWindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"LeewayUIRibbonTestWindow";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, L"LeewayUIRibbonTestWindow", L"LeewayUI F1: Photo Viewer ribbon",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 900, 520,
                                NULL, NULL, instance, NULL);
    if (!hwnd) return 1;

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (g_app.ribbon && LeewayRibbonPreTranslateMessage(g_app.ribbon, &msg)) continue;
        if (g_app.accel && TranslateAcceleratorW(hwnd, g_app.accel, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_app.accel) DestroyAcceleratorTable(g_app.accel);
    return 0;
}
