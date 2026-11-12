/*
 * PROJECT:     LeewayUI  -  print-preview frame demo
 * LICENSE:     LGPL-2.1-or-later
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Exercises the shared print-preview frame with a dummy multi-page document.
 *   print_demo.exe [--theme light|dark] [--two] [--dpi N] [--shot out.bmp]
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <leewayui/print.h>
#include <leewayui/aureline_icons.h>
#include <leewayui/ribbon_icons.h>

static ILeewayPrintPreview *g_pv;
static LeewayPrintSettings  g_settings;
static int g_pages = 3;

static UINT32 PageCount(void *c) { (void)c; return (UINT32)g_pages; }

static HRESULT RenderPage(UINT32 index, HDC hdc, const RECT *paper, const RECT *content,
                          UINT32 dpi, void *c)
{
    (void)c; (void)paper;
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(20, 20, 20));
    LOGFONTW lf; ZeroMemory(&lf, sizeof lf);
    lf.lfHeight = -MulDiv(16, (int)dpi, 72);
    lstrcpyW(lf.lfFaceName, L"Segoe UI");
    HFONT f = CreateFontIndirectW(&lf); HGDIOBJ of = SelectObject(hdc, f);

    WCHAR hdr[64]; wsprintfW(hdr, L"Page %u of %d", index + 1, g_pages);
    RECT t = *content; t.bottom = t.top + MulDiv(30, (int)dpi, 72);
    DrawTextW(hdc, hdr, -1, &t, DT_LEFT | DT_TOP | DT_SINGLELINE);

    lf.lfHeight = -MulDiv(11, (int)dpi, 72);
    HFONT f2 = CreateFontIndirectW(&lf); SelectObject(hdc, f2);
    int y = t.bottom + MulDiv(12, (int)dpi, 72);
    int lh = MulDiv(15, (int)dpi, 72);
    for (int i = 0; i < 14; i++) {
        RECT r = { content->left, y, content->right, y + lh };
        WCHAR line[96];
        wsprintfW(line, L"The quick brown fox jumps over the lazy dog  (line %d)", i + 1);
        DrawTextW(hdc, line, -1, &r, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += lh;
    }
    /* a framed box to make the content area obvious */
    HPEN p = CreatePen(PS_SOLID, MulDiv(1,(int)dpi,96), RGB(120,140,200));
    HGDIOBJ opn = SelectObject(hdc, p); HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    int by = y + MulDiv(10,(int)dpi,72);
    Rectangle(hdc, content->left, by, content->right, content->bottom);
    SelectObject(hdc, ob); SelectObject(hdc, opn); DeleteObject(p);

    SelectObject(hdc, of); DeleteObject(f); DeleteObject(f2);
    return S_OK;
}

static void OnAction(LeewayPreviewAction a, void *c)
{
    (void)c;
    HWND top = GetAncestor(LeewayPrintPreviewGetHWND(g_pv), GA_ROOT);
    switch (a) {
    case LWUI_PREVIEW_PRINT: {
        LeewayPrintRange r; ZeroMemory(&r, sizeof r); r.min_page = 1; r.max_page = g_pages; r.copies = 1;
        HDC dc = NULL;
        if (LeewayPrintDialog(top, &g_settings, &r, &dc) == S_OK && dc) DeleteDC(dc);
        break; }
    case LWUI_PREVIEW_PAGESETUP:
        if (LeewayPageSetupDialog(top, &g_settings) == S_OK) LeewayPrintPreviewRefresh(g_pv);
        break;
    case LWUI_PREVIEW_CLOSE:
        PostMessageW(top, WM_CLOSE, 0, 0);
        break;
    }
}

static LRESULT CALLBACK TopProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_SIZE:
        if (g_pv) { RECT rc; GetClientRect(h, &rc); MoveWindow(LeewayPrintPreviewGetHWND(g_pv), 0, 0, rc.right, rc.bottom, TRUE); }
        return 0;
    case WM_DPICHANGED:
        if (g_pv) LeewayPrintPreviewSetDPI(g_pv, HIWORD(w));
        { RECT *r = (RECT *)l; SetWindowPos(h, NULL, r->left, r->top, r->right-r->left, r->bottom-r->top, SWP_NOZORDER|SWP_NOACTIVATE); }
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void SaveBmp(HWND h, const WCHAR *path)
{
    RECT rc; GetClientRect(h, &rc); int w = rc.right, ht = rc.bottom;
    HDC hdc = GetDC(h), mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, ht); SelectObject(mem, bmp);
    PrintWindow(h, mem, PW_CLIENTONLY);
    BITMAPFILEHEADER bf; BITMAPINFOHEADER bi; ZeroMemory(&bi, sizeof bi);
    bi.biSize = sizeof bi; bi.biWidth = w; bi.biHeight = -ht; bi.biPlanes = 1; bi.biBitCount = 24; bi.biCompression = BI_RGB;
    int stride = ((w*3+3)/4)*4, sz = stride*ht; BYTE *buf = (BYTE*)malloc(sz);
    GetDIBits(mem, bmp, 0, ht, buf, (BITMAPINFO*)&bi, DIB_RGB_COLORS);
    ZeroMemory(&bf, sizeof bf); bf.bfType = 0x4D42; bf.bfOffBits = sizeof bf + sizeof bi; bf.bfSize = bf.bfOffBits + sz;
    FILE *fp = _wfopen(path, L"wb"); if (fp) { fwrite(&bf,1,sizeof bf,fp); fwrite(&bi,1,sizeof bi,fp); fwrite(buf,1,sz,fp); fclose(fp); }
    free(buf); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(h, hdc);
}

int wmain(int argc, wchar_t **argv)
{
    LeewayThemePref theme = LEEWAY_THEME_PREF_SYSTEM;   /* default: honour system / per-app override */
    const WCHAR *shot = NULL; BOOL two = FALSE; int dpi = 0;
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--theme") && i+1 < argc) { i++;
            theme = !wcscmp(argv[i], L"dark") ? LEEWAY_THEME_PREF_DARK
                  : !wcscmp(argv[i], L"light") ? LEEWAY_THEME_PREF_LIGHT
                  : LEEWAY_THEME_PREF_SYSTEM; }
        else if (!wcscmp(argv[i], L"--two")) two = TRUE;
        else if (!wcscmp(argv[i], L"--dpi") && i+1 < argc) dpi = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--shot") && i+1 < argc) shot = argv[++i];
    }

    HMODULE u = GetModuleHandleW(L"user32");
    typedef BOOL (WINAPI *SPDAC)(HANDLE);
    SPDAC spdac = (SPDAC)GetProcAddress(u, "SetProcessDpiAwarenessContext");
    if (spdac) spdac((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */); else SetProcessDPIAware();

    WNDCLASSW wc; ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = TopProc; wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW); wc.lpszClassName = L"LwPrintDemoTop";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    RegisterClassW(&wc);
    HWND top = CreateWindowW(wc.lpszClassName, L"LeewayUI Print Preview demo",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1000, 760, 0, 0, wc.hInstance, 0);

    LeewayPrintSettingsInitDefault(&g_settings);

    LeewayPrintPreviewConfig cfg; ZeroMemory(&cfg, sizeof cfg);
    cfg.page_count = PageCount; cfg.render_page = RenderPage; cfg.on_action = OnAction;
    cfg.settings = &g_settings; cfg.app_name = L"PrintDemo"; cfg.theme = theme;
    LeewayPrintPreviewCreate(top, &cfg, &g_pv);

    static LeewayAurelineConfig aur = { L"../icons", TRUE };
    LeewayPrintPreviewSetIconResolver(g_pv, LeewayRibbonAurelineResolver, &aur);
    if (two) LeewayPrintPreviewSetPagesShown(g_pv, 2);

    RECT rc; GetClientRect(top, &rc);
    MoveWindow(LeewayPrintPreviewGetHWND(g_pv), 0, 0, rc.right, rc.bottom, TRUE);
    LeewayPrintPreviewShow(g_pv, TRUE);
    ShowWindow(top, SW_SHOW); UpdateWindow(top);
    if (dpi > 0) LeewayPrintPreviewSetDPI(g_pv, (UINT32)dpi);

    if (shot) {
        MSG m; for (int i = 0; i < 8; i++) { while (PeekMessageW(&m,0,0,0,PM_REMOVE)) { TranslateMessage(&m); DispatchMessage(&m); } Sleep(120); }
        SaveBmp(top, shot);
        LeewayPrintPreviewDestroy(g_pv); LeewayPrintSettingsFree(&g_settings);
        return 0;
    }

    MSG msg;
    while (GetMessageW(&msg, 0, 0, 0)) {
        if (g_pv && LeewayPrintPreviewPreTranslateMessage(g_pv, &msg)) continue;
        TranslateMessage(&msg); DispatchMessage(&msg);
    }
    LeewayPrintPreviewDestroy(g_pv); LeewayPrintSettingsFree(&g_settings);
    return 0;
}
