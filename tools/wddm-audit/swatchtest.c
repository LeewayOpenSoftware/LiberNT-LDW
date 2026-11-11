/*
 * PROJECT:     LiberNT WDDM audit - swatch/ramp/alpha reference test
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Paints known RGB values, a grey ramp, and a layered alpha
 *              window, then reads its own client area back from both the
 *              window DC and the desktop DC. Separates "software drew a
 *              wrong colour" from "the display path shows a wrong colour"
 *              when Lab03 screenshots are compared against the readback.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */
#include <windows.h>
#include <stdio.h>

#define WIN_W   960
#define WIN_H   640
#define SWATCH  60
#define ROWS    2

static const struct { int r, g, b; const char *name; } kColors[] = {
    {255,   0,   0, "red"},
    {  0, 255,   0, "green"},
    {  0,   0, 255, "blue"},
    {255, 255, 255, "white"},
    {  0,   0,   0, "black"},
    {128, 128, 128, "grey50"},
    {255, 255,   0, "yellow"},
    {  0, 255, 255, "cyan"},
    {255,   0, 255, "magenta"},
    { 30,  60,  90, "darkslate-ish"},
    {200, 120,  40, "orange-ish"},
    { 60,  40, 120, "violet-ish"},
};
#define NCOLORS (sizeof(kColors)/sizeof(kColors[0]))

static HWND g_hwnd;
static HWND g_layered;

static void
PaintSwatches(HDC hdc)
{
    RECT rc;
    int i;

    for (i = 0; (ULONG)i < NCOLORS; i++)
    {
        HBRUSH br = CreateSolidBrush(RGB(kColors[i].r, kColors[i].g, kColors[i].b));
        rc.left   = 20 + (i % 6) * (SWATCH + 12);
        rc.top    = 20 + (i / 6) * (SWATCH + 40);
        rc.right  = rc.left + SWATCH;
        rc.bottom = rc.top + SWATCH;
        FillRect(hdc, &rc, br);
        DeleteObject(br);
    }

    /* Grey ramp: 16 steps, 40px wide, full row. */
    for (i = 0; i < 16; i++)
    {
        int v = i * 17;
        HBRUSH br = CreateSolidBrush(RGB(v, v, v));
        rc.left   = 20 + i * 46;
        rc.top    = 20 + 2 * (SWATCH + 40);
        rc.right  = rc.left + 40;
        rc.bottom = rc.top + 60;
        FillRect(hdc, &rc, br);
        DeleteObject(br);
    }

    /* Text label to catch glyph/tint confusion. */
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    TextOutW(hdc, 20, 20 + 2 * (SWATCH + 40) + 70, L"WDDM-1X-AUDIT swatches: red green blue white black grey50 yellow cyan magenta", 76);
}

static void
DumpPixels(void)
{
    FILE *f = fopen("C:\\swatch-readback.txt", "w");
    HDC wdc, ddc;
    int i;

    if (!f)
        return;
    fprintf(f, "swatch readback: window-DC then desktop-DC (GetPixel)\n");

    wdc = GetDC(g_hwnd);
    ddc = GetDC(NULL);
    for (i = 0; (ULONG)i < NCOLORS; i++)
    {
        int x = 20 + (i % 6) * (SWATCH + 12) + SWATCH / 2;
        int y = 20 + (i / 6) * (SWATCH + 40) + SWATCH / 2;
        COLORREF wc = GetPixel(wdc, x, y);
        COLORREF dc = ddc ? GetPixel(ddc, x, y) : 0xFFFFFFFF;
        fprintf(f, "%-14s expected=(%3d,%3d,%3d) window=(%3d,%3d,%3d) desktop=(%3d,%3d,%3d)\n",
                kColors[i].name,
                kColors[i].r, kColors[i].g, kColors[i].b,
                GetRValue(wc), GetGValue(wc), GetBValue(wc),
                dc == 0xFFFFFFFF ? -1 : GetRValue(dc),
                dc == 0xFFFFFFFF ? -1 : GetGValue(dc),
                dc == 0xFFFFFFFF ? -1 : GetBValue(dc));
    }
    if (ddc) ReleaseDC(NULL, ddc);
    ReleaseDC(g_hwnd, wdc);
    fclose(f);
}

static LRESULT CALLBACK
WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            PaintSwatches(hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER:
            DumpPixels();
            if (g_layered) DestroyWindow(g_layered);
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI
wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show)
{
    WNDCLASSW wc = {0};
    MSG msg;

    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hbrBackground = (HBRUSH)GetStockObject(DKGRAY_BRUSH);
    wc.lpszClassName = L"WddmAuditSwatch";
    RegisterClassW(&wc);

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"WDDM audit swatches",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                             40, 40, WIN_W, WIN_H,
                             NULL, NULL, inst, NULL);
    if (!g_hwnd)
        return 1;

    /* Layered 60%-opaque blue window over the swatches: alpha/premultiply test. */
    g_layered = CreateWindowExW(WS_EX_LAYERED, wc.lpszClassName, L"alpha probe",
                                WS_POPUP | WS_VISIBLE,
                                200, 300, 300, 120,
                                g_hwnd, NULL, inst, NULL);
    if (g_layered)
    {
        SetLayeredWindowAttributes(g_layered, 0, 155, LWA_ALPHA);
    }

    SetTimer(g_hwnd, 1, 8000, NULL);

    while (GetMessageW(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
