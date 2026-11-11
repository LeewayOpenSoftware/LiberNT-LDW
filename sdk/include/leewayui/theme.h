/*
 * PROJECT:     LeewayUI
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Theme object: live light/dark/high-contrast/classic state, named colours and brushes
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 * PROVENANCE:  sdk/lib/leewayui/LEEWAYUI-PROVENANCE.md
 */

#pragma once

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ILeewayTheme;
typedef struct ILeewayTheme ILeewayTheme;

/* Theme creation */
HRESULT LeewayThemeCreate(ILeewayTheme **out_theme);
void LeewayThemeDestroy(ILeewayTheme *theme);

/* Theme state queries */
typedef enum {
    LEEWAY_THEME_MODE_LIGHT,
    LEEWAY_THEME_MODE_DARK,
    LEEWAY_THEME_MODE_HIGH_CONTRAST,
    LEEWAY_THEME_MODE_CLASSIC
} LeewayThemeMode;

LeewayThemeMode LeewayThemeGetMode(ILeewayTheme *theme);

/* Named colours (RIBBON-CONTROLS-DESIGN.md §5: the control never hard-codes
 * a colour). High contrast and classic map to the system colours. */
typedef enum {
    LWUI_COLOR_STRIP_BG = 0,        /* tab strip and QAT row */
    LWUI_COLOR_BAND_BG,             /* band, top of the gradient */
    LWUI_COLOR_BAND_BG2,            /* band, bottom of the gradient */
    LWUI_COLOR_TAB_TEXT,
    LWUI_COLOR_TAB_BORDER,          /* active tab outline and band top line */
    LWUI_COLOR_GROUP_BORDER,
    LWUI_COLOR_GROUP_LABEL_BG,
    LWUI_COLOR_GROUP_LABEL_TEXT,
    LWUI_COLOR_TEXT,
    LWUI_COLOR_TEXT_DISABLED,
    LWUI_COLOR_HOVER_BG,
    LWUI_COLOR_HOVER_BORDER,
    LWUI_COLOR_PRESSED_BG,
    LWUI_COLOR_PRESSED_BORDER,
    LWUI_COLOR_PRESSED_TEXT,
    LWUI_COLOR_CHECKED_BG,
    LWUI_COLOR_CHECKED_BORDER,
    LWUI_COLOR_SEPARATOR,
    LWUI_COLOR_POPUP_BG,
    LWUI_COLOR_POPUP_BORDER,
    LWUI_COLOR_KEYTIP_BG,
    LWUI_COLOR_KEYTIP_TEXT,
    LWUI_COLOR_KEYTIP_BORDER,
    LWUI_COLOR_APPBUTTON_BG,
    LWUI_COLOR_APPBUTTON_TEXT,
    LWUI_COLOR_FOCUS,
    LWUI_COLOR_CONTEXT_TINT,        /* contextual tab header */
    LWUI_COLOR_PLACEHOLDER_BG,      /* unresolved icon slot */
    LWUI_COLOR_PLACEHOLDER_TEXT,
    LWUI_COLOR_COUNT
} LeewayThemeColor;

COLORREF LeewayThemeGetColor(ILeewayTheme *theme, LeewayThemeColor which);

/* Named colour brushes (on-demand, cached; owned by the theme). The name is
 * the LWUI_COLOR_* identifier without the prefix, e.g. L"BAND_BG". */
HBRUSH LeewayThemeGetBrush(ILeewayTheme *theme, const WCHAR *name);

/* TRUE when gradients and custom fills must be dropped (high contrast, classic). */
BOOL LeewayThemeIsFlat(ILeewayTheme *theme);

/* Notify theme of system changes (re-reads the system state, drops cached brushes) */
void LeewayThemeOnSettingChange(ILeewayTheme *theme);
void LeewayThemeOnThemeChange(ILeewayTheme *theme);

#ifdef __cplusplus
}
#endif
