/*
 * PROJECT:     LeewayUI
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Theme object: light/dark/high-contrast/classic detection, Aureline palette, system-colour paths
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 * PROVENANCE:  sdk/lib/leewayui/LEEWAYUI-PROVENANCE.md
 */

#include <windows.h>
#include <uxtheme.h>
#include <leewayui/theme.h>

struct ILeewayTheme {
    LeewayThemeMode mode;
    COLORREF colors[LWUI_COLOR_COUNT];
    HBRUSH brushes[LWUI_COLOR_COUNT];
};

static const WCHAR *const g_color_names[LWUI_COLOR_COUNT] = {
    L"STRIP_BG", L"BAND_BG", L"BAND_BG2", L"TAB_TEXT", L"TAB_BORDER", L"GROUP_BORDER",
    L"GROUP_LABEL_BG", L"GROUP_LABEL_TEXT", L"TEXT", L"TEXT_DISABLED", L"HOVER_BG", L"HOVER_BORDER",
    L"PRESSED_BG", L"PRESSED_BORDER", L"PRESSED_TEXT", L"CHECKED_BG", L"CHECKED_BORDER", L"SEPARATOR",
    L"POPUP_BG", L"POPUP_BORDER", L"KEYTIP_BG", L"KEYTIP_TEXT", L"KEYTIP_BORDER", L"APPBUTTON_BG",
    L"APPBUTTON_TEXT", L"FOCUS", L"CONTEXT_TINT", L"PLACEHOLDER_BG", L"PLACEHOLDER_TEXT",
};

/* Aureline light: Aero-inspired blues, our own values (design §9). */
static const COLORREF g_light[LWUI_COLOR_COUNT] = {
    RGB(220, 229, 240), RGB(243, 247, 251), RGB(225, 234, 245), RGB(30, 40, 60), RGB(158, 178, 204), RGB(176, 192, 212),
    RGB(212, 224, 239), RGB(60, 76, 96), RGB(30, 38, 50), RGB(140, 150, 160), RGB(218, 234, 252), RGB(120, 170, 230),
    RGB(180, 208, 240), RGB(80, 130, 200), RGB(30, 38, 50), RGB(200, 222, 246), RGB(100, 150, 210), RGB(190, 204, 220),
    RGB(250, 252, 255), RGB(140, 160, 190), RGB(255, 255, 225), RGB(20, 20, 20), RGB(120, 120, 120), RGB(31, 95, 184),
    RGB(255, 255, 255), RGB(31, 95, 184), RGB(236, 222, 184), RGB(200, 216, 236), RGB(31, 95, 184),
};

static const COLORREF g_dark[LWUI_COLOR_COUNT] = {
    RGB(32, 36, 42), RGB(45, 50, 58), RGB(38, 42, 50), RGB(220, 226, 234), RGB(70, 78, 90), RGB(70, 78, 90),
    RGB(38, 42, 50), RGB(170, 180, 195), RGB(230, 234, 240), RGB(120, 128, 140), RGB(62, 72, 88), RGB(100, 140, 200),
    RGB(50, 80, 120), RGB(90, 140, 210), RGB(230, 234, 240), RGB(44, 70, 104), RGB(90, 140, 210), RGB(70, 78, 90),
    RGB(40, 44, 52), RGB(90, 100, 115), RGB(60, 64, 72), RGB(255, 255, 255), RGB(140, 150, 165), RGB(31, 95, 184),
    RGB(255, 255, 255), RGB(111, 176, 242), RGB(96, 84, 52), RGB(60, 70, 86), RGB(200, 216, 236),
};

static BOOL ReadAppsUseLightTheme(void)
{
    HKEY key;
    DWORD value = 1, size = sizeof(value), type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &key) != ERROR_SUCCESS)
        return TRUE;
    if (RegQueryValueExW(key, L"AppsUseLightTheme", NULL, &type, (LPBYTE)&value, &size) != ERROR_SUCCESS || type != REG_DWORD)
        value = 1;
    RegCloseKey(key);
    return value != 0;
}

static LeewayThemeMode DetectMode(void)
{
    HIGHCONTRASTW hc;
    ZeroMemory(&hc, sizeof(hc));
    hc.cbSize = sizeof(hc);
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON))
        return LEEWAY_THEME_MODE_HIGH_CONTRAST;
    if (!IsThemeActive())
        return LEEWAY_THEME_MODE_CLASSIC;
    return ReadAppsUseLightTheme() ? LEEWAY_THEME_MODE_LIGHT : LEEWAY_THEME_MODE_DARK;
}

/* System-colour path (design §3/§5): no custom fills, state shown by framing. */
static void FillSystemPalette(COLORREF *c)
{
    COLORREF face = GetSysColor(COLOR_BTNFACE), text = GetSysColor(COLOR_BTNTEXT);
    COLORREF hilite = GetSysColor(COLOR_HIGHLIGHT), hitext = GetSysColor(COLOR_HIGHLIGHTTEXT);
    COLORREF gray = GetSysColor(COLOR_GRAYTEXT), shadow = GetSysColor(COLOR_BTNSHADOW);
    COLORREF window = GetSysColor(COLOR_WINDOW), wtext = GetSysColor(COLOR_WINDOWTEXT);
    c[LWUI_COLOR_STRIP_BG] = face;       c[LWUI_COLOR_BAND_BG] = face;        c[LWUI_COLOR_BAND_BG2] = face;
    c[LWUI_COLOR_TAB_TEXT] = text;       c[LWUI_COLOR_TAB_BORDER] = shadow;   c[LWUI_COLOR_GROUP_BORDER] = shadow;
    c[LWUI_COLOR_GROUP_LABEL_BG] = face; c[LWUI_COLOR_GROUP_LABEL_TEXT] = text;
    c[LWUI_COLOR_TEXT] = text;           c[LWUI_COLOR_TEXT_DISABLED] = gray;
    c[LWUI_COLOR_HOVER_BG] = face;       c[LWUI_COLOR_HOVER_BORDER] = hilite;
    c[LWUI_COLOR_PRESSED_BG] = hilite;   c[LWUI_COLOR_PRESSED_BORDER] = hilite; c[LWUI_COLOR_PRESSED_TEXT] = hitext;
    c[LWUI_COLOR_CHECKED_BG] = hilite;   c[LWUI_COLOR_CHECKED_BORDER] = hilite;
    c[LWUI_COLOR_SEPARATOR] = shadow;    c[LWUI_COLOR_POPUP_BG] = window;     c[LWUI_COLOR_POPUP_BORDER] = wtext;
    c[LWUI_COLOR_KEYTIP_BG] = window;    c[LWUI_COLOR_KEYTIP_TEXT] = wtext;   c[LWUI_COLOR_KEYTIP_BORDER] = wtext;
    c[LWUI_COLOR_APPBUTTON_BG] = hilite; c[LWUI_COLOR_APPBUTTON_TEXT] = hitext;
    c[LWUI_COLOR_FOCUS] = text;          c[LWUI_COLOR_CONTEXT_TINT] = hilite;
    c[LWUI_COLOR_PLACEHOLDER_BG] = face; c[LWUI_COLOR_PLACEHOLDER_TEXT] = text;
}

static void DropBrushes(ILeewayTheme *theme)
{
    for (int i = 0; i < LWUI_COLOR_COUNT; i++) {
        if (theme->brushes[i]) {
            DeleteObject(theme->brushes[i]);
            theme->brushes[i] = NULL;
        }
    }
}

static void Reload(ILeewayTheme *theme)
{
    theme->mode = DetectMode();
    switch (theme->mode) {
    case LEEWAY_THEME_MODE_LIGHT:
        CopyMemory(theme->colors, g_light, sizeof(theme->colors));
        break;
    case LEEWAY_THEME_MODE_DARK:
        CopyMemory(theme->colors, g_dark, sizeof(theme->colors));
        break;
    default:
        FillSystemPalette(theme->colors);
        break;
    }
    DropBrushes(theme);
}

HRESULT LeewayThemeCreate(ILeewayTheme **out_theme)
{
    if (!out_theme) {
        return E_INVALIDARG;
    }

    ILeewayTheme *theme = (ILeewayTheme *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ILeewayTheme));
    if (!theme) {
        return E_OUTOFMEMORY;
    }

    Reload(theme);
    *out_theme = theme;
    return S_OK;
}

void LeewayThemeDestroy(ILeewayTheme *theme)
{
    if (!theme) {
        return;
    }
    DropBrushes(theme);
    HeapFree(GetProcessHeap(), 0, theme);
}

LeewayThemeMode LeewayThemeGetMode(ILeewayTheme *theme)
{
    return theme ? theme->mode : LEEWAY_THEME_MODE_LIGHT;
}

COLORREF LeewayThemeGetColor(ILeewayTheme *theme, LeewayThemeColor which)
{
    if (!theme || which < 0 || which >= LWUI_COLOR_COUNT) {
        return RGB(0, 0, 0);
    }
    return theme->colors[which];
}

HBRUSH LeewayThemeGetBrush(ILeewayTheme *theme, const WCHAR *name)
{
    if (!theme || !name) {
        return NULL;
    }
    for (int i = 0; i < LWUI_COLOR_COUNT; i++) {
        if (lstrcmpiW(name, g_color_names[i]) == 0) {
            if (!theme->brushes[i]) {
                theme->brushes[i] = CreateSolidBrush(theme->colors[i]);
            }
            return theme->brushes[i];
        }
    }
    return NULL;
}

BOOL LeewayThemeIsFlat(ILeewayTheme *theme)
{
    return theme && (theme->mode == LEEWAY_THEME_MODE_HIGH_CONTRAST || theme->mode == LEEWAY_THEME_MODE_CLASSIC);
}

void LeewayThemeOnSettingChange(ILeewayTheme *theme)
{
    if (theme) {
        Reload(theme);
    }
}

void LeewayThemeOnThemeChange(ILeewayTheme *theme)
{
    if (theme) {
        Reload(theme);
    }
}
