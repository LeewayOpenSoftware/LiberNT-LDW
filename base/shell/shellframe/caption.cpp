/*
 * PROJECT:     LiberNT Shell Frame
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Minimize, maximize and close buttons drawn with the active visual style
 *
 * A window that extends its frame into the client area leaves the caption
 * buttons to the compositor. Without a compositor nothing draws them, so the
 * frame draws them itself, with the same theme parts as the system caption,
 * and falls back to the classic frame controls when there is no theme.
 */

#include "shellframe.h"
#include <uxtheme.h>
#include <vsstyle.h>

namespace ShellFrame
{

static int
ScaleFor(int value, int dpi)
{
    return MulDiv(value, dpi ? dpi : 96, 96);
}

SIZE
CaptionButtonSize(int dpi)
{
    SIZE size;

    size.cx = GetSystemMetricsForDpi(SM_CXSIZE, dpi);
    size.cy = GetSystemMetricsForDpi(SM_CYSIZE, dpi);
    if (size.cx < ScaleFor(24, dpi))
        size.cx = ScaleFor(24, dpi);
    if (size.cy < ScaleFor(18, dpi))
        size.cy = ScaleFor(18, dpi);
    return size;
}

int
CaptionStripWidth(int dpi)
{
    return CaptionButtonSize(dpi).cx * 3 + ScaleFor(2, dpi) * 2 + ScaleFor(8, dpi);
}

RECT
CaptionButtonBounds(const RECT &client, CaptionButton button, int dpi)
{
    SIZE size = CaptionButtonSize(dpi);
    int index = button == CaptionButton::Close ? 0 : button == CaptionButton::Maximize ? 1 : 2;
    int gap = ScaleFor(2, dpi);
    RECT rc;

    rc.right = client.right - ScaleFor(8, dpi) - index * (size.cx + gap);
    rc.left = rc.right - size.cx;
    rc.top = ScaleFor(2, dpi);
    rc.bottom = rc.top + size.cy;
    return rc;
}

/* The compositor and the system caption draw the buttons through this uxtheme entry point, so
 * the frame buttons look like those of every other window. Index order: close, maximize, minimize. */
typedef BOOL (WINAPI *PFN_THEME_DRAW_CAPTION_BUTTON)(HDC, const RECT *, UINT, ULONG, BOOL, BOOL);

static PFN_THEME_DRAW_CAPTION_BUTTON
GetThemeDrawCaptionButton()
{
    static PFN_THEME_DRAW_CAPTION_BUTTON fn;
    static bool loaded;

    if (!loaded)
    {
        HMODULE module = GetModuleHandleW(L"uxtheme.dll");

        if (!module)
            module = LoadLibraryW(L"uxtheme.dll");
        if (module)
            fn = (PFN_THEME_DRAW_CAPTION_BUTTON)GetProcAddress(module, "ThemeDwmDrawCaptionButton");
        loaded = true;
    }
    return fn;
}

void
DrawCaptionButton(HWND hwnd, HDC hdc, const RECT &rc, CaptionButton button, bool hot, bool pressed, bool active,
                  bool zoomed, bool dark)
{
    int part, state, frame;
    BOOL drawn = FALSE;
    HTHEME theme;
    RECT draw = rc;
    PFN_THEME_DRAW_CAPTION_BUTTON themeDraw = GetThemeDrawCaptionButton();

    switch (button)
    {
        case CaptionButton::Minimize:
            part = WP_MINBUTTON;
            frame = DFCS_CAPTIONMIN;
            break;
        case CaptionButton::Maximize:
            part = zoomed ? WP_RESTOREBUTTON : WP_MAXBUTTON;
            frame = zoomed ? DFCS_CAPTIONRESTORE : DFCS_CAPTIONMAX;
            break;
        case CaptionButton::Close:
            part = WP_CLOSEBUTTON;
            frame = DFCS_CAPTIONCLOSE;
            break;
        default:
            return;
    }

    if (themeDraw && IsAppThemed())
    {
        UINT index = button == CaptionButton::Close ? 0 : button == CaptionButton::Maximize ? 1 : 2;
        ULONG bits = (hot ? (1u << index) : 0) | (pressed ? (1u << (index + 3)) : 0) | (zoomed ? 0x200u : 0);

        /* A dark frame gets the dark buttons the system caption draws: the light visual style sprites
         * scale into a squashed glyph on a red button */
        drawn = themeDraw(hdc, &draw, index, bits, active, dark);
    }

    /* The states of every caption button part are numbered alike */
    state = pressed ? 3 : hot ? 2 : active ? 1 : 5;

    /* No window handle: opening the theme for one would attach it to the window as its own */
    theme = (!drawn && IsAppThemed()) ? OpenThemeDataEx(NULL, L"WINDOW", OTD_NONCLIENT) : NULL;
    if (theme)
    {
        if (IsThemePartDefined(theme, part, state) || IsThemePartDefined(theme, part, 1))
        {
            if (!IsThemePartDefined(theme, part, state))
                state = 1;
            drawn = SUCCEEDED(DrawThemeBackground(theme, hdc, part, state, &draw, NULL));
        }
        CloseThemeData(theme);
    }
    if (!drawn)
        DrawFrameControl(hdc, &draw, DFC_CAPTION, frame | (pressed ? DFCS_PUSHED : 0));
}

}
