/*
 * PROJECT:     ReactOS Picture and Fax Viewer
 * LICENSE:     GPL-2.0 (https://spdx.org/licenses/GPL-2.0)
 * PURPOSE:     Standalone native demo of the viewer chrome: the real viewer model
 *              (vribbon_model.c, vribbon.cpp), LeewayUI ribbon and LeewayUI frame
 *              around a small GDI+ image view. Builds with MSYS2 clang64, see build.sh.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Usage: viewer_demo.exe [image]   Ctrl+O opens, arrows page, Ctrl+wheel zooms.
 * Not the viewer: no scrolling, no slideshow, no delete/print/save.
 */

#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <algorithm>
#include <string>
#include <vector>
#include <leewayui/frame.h>
#include "../resource.h"
#include "../vribbon.h"

static HINSTANCE g_inst;
static HWND g_hwnd;
static VRB_RIBBON *g_rib;
static LeewayUIFrame *g_frame;
static RECT g_work;

static Gdiplus::Image *g_img;
static std::vector<std::wstring> g_files;
static size_t g_idx;
static UINT g_zoom = 100;
static bool g_fit = true;

/* ---- image ---- */

static std::wstring Base(const std::wstring &p)
{
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? p : p.substr(i + 1);
}

static bool IsImageName(const std::wstring &n)
{
    static const wchar_t *ext[] = { L".png", L".jpg", L".jpeg", L".bmp", L".gif", L".tif", L".tiff", L".ico" };
    std::wstring l = n;
    for (auto &c : l)
        c = (wchar_t)towlower(c);
    for (auto e : ext)
    {
        size_t el = wcslen(e);
        if (l.size() >= el && !l.compare(l.size() - el, el, e))
            return true;
    }
    return false;
}

static void FitZoom()
{
    if (!g_img)
        return;
    double w = (double)g_img->GetWidth(), h = (double)g_img->GetHeight();
    double aw = g_work.right - g_work.left - 16, ah = g_work.bottom - g_work.top - 16;
    if (w < 1 || h < 1 || aw < 1 || ah < 1)
        return;
    double z = std::min(aw / w, ah / h) * 100.0;
    g_zoom = (UINT)std::max(1.0, std::min(z, 100.0));      /* fit never enlarges */
}

static void StatusInfo()
{
    wchar_t t[512] = L"";
    if (g_img && !g_files.empty())
        wsprintfW(t, L"%s   %u x %u   (%u of %u)", Base(g_files[g_idx]).c_str(), g_img->GetWidth(),
                  g_img->GetHeight(), (UINT)g_idx + 1, (UINT)g_files.size());
    LeewayFrameSetStatusText(g_frame, 0, t);
}

static void Refresh()
{
    if (g_fit)
        FitZoom();
    LeewayFrameSetZoom(g_frame, g_zoom);
    StatusInfo();
    VRB_RibbonInvalidateAll(g_rib);
    InvalidateRect(g_hwnd, &g_work, FALSE);
}

static void LoadAt(size_t i)
{
    Gdiplus::Image *n = Gdiplus::Image::FromFile(g_files[i].c_str());
    if (!n || n->GetLastStatus() != Gdiplus::Ok)
    {
        delete n;
        MessageBoxW(g_hwnd, L"Cannot open this image.", L"Photo Viewer demo", MB_ICONWARNING);
        return;
    }
    delete g_img;
    g_img = n;
    g_idx = i;
    g_fit = true;
    SetWindowTextW(g_hwnd, (Base(g_files[i]) + L" - Photo Viewer (ribbon demo)").c_str());
    Refresh();
}

static void OpenFile(const std::wstring &path)
{
    std::wstring dir = path;
    size_t s = dir.find_last_of(L"\\/");
    dir = s == std::wstring::npos ? L"." : dir.substr(0, s);
    g_files.clear();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && IsImageName(fd.cFileName))
                g_files.push_back(dir + L"\\" + fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(g_files.begin(), g_files.end(), [](const std::wstring &a, const std::wstring &b)
              { return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN; });
    size_t at = 0;
    bool found = false;
    for (size_t i = 0; i < g_files.size(); ++i)
        if (!lstrcmpiW(g_files[i].c_str(), path.c_str()))
        {
            at = i;
            found = true;
        }
    if (!found)
    {
        g_files.push_back(path);
        at = g_files.size() - 1;
    }
    LoadAt(at);
}

static void OpenDialog()
{
    wchar_t buf[MAX_PATH] = L"";
    OPENFILENAMEW o = {};
    o.lStructSize = sizeof(o);
    o.hwndOwner = g_hwnd;
    o.lpstrFile = buf;
    o.nMaxFile = MAX_PATH;
    o.lpstrFilter = L"Pictures\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff;*.ico\0All files\0*.*\0";
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&o))
        OpenFile(buf);
}

static void SetZoomPercent(UINT p)
{
    g_fit = false;
    g_zoom = std::max(1u, std::min(p, 1600u));
    Refresh();
}

static void StepZoom(int dir)
{
    UINT cur = g_zoom;
    if (dir > 0)
    {
        for (UINT i = 0; i < VRB_ZOOM_LEVELS; ++i)
            if (g_VrbZoomPercents[i] > cur)
            {
                SetZoomPercent(g_VrbZoomPercents[i]);
                return;
            }
    }
    else
    {
        for (int i = VRB_ZOOM_LEVELS - 1; i >= 0; --i)
            if (g_VrbZoomPercents[i] < cur)
            {
                SetZoomPercent(g_VrbZoomPercents[i]);
                return;
            }
    }
}

static void Rotate(Gdiplus::RotateFlipType t)
{
    if (!g_img)
        return;
    g_img->RotateFlip(t);
    Refresh();
}

static void Page(int d)
{
    if (g_files.size() < 2)
        return;
    LoadAt((g_idx + g_files.size() + d) % g_files.size());
}

/* ---- host of the viewer model ---- */

static BOOL HostPredicate(void *, VRB_PRED p)
{
    switch (p)
    {
        case VRB_P_ALWAYS:          return TRUE;
        case VRB_P_HAS_IMAGE:       return g_img != NULL;
        case VRB_P_HAS_LIST:        return g_files.size() > 1;
        case VRB_P_CAN_ZOOM_IN:     return g_img && g_zoom < 1600;
        case VRB_P_CAN_ZOOM_OUT:    return g_img && g_zoom > 10;
        case VRB_P_IS_FIT:          return g_img && g_fit;
        case VRB_P_IS_ACTUAL:       return g_img && !g_fit && g_zoom == 100;
        case VRB_P_STATUSBAR_SHOWN: return LeewayFrameIsStatusBarShown(g_frame);
        case VRB_P_RIBBON_MINIMIZED:return LeewayRibbonIsMinimized(VRB_RibbonFramework(g_rib));
        default:                    return FALSE;       /* slideshow, save, delete, print... not in the demo */
    }
}

static HRESULT HostExecute(void *, UINT id)
{
    SendMessageW(g_hwnd, WM_COMMAND, id, 0);
    return S_OK;
}

static UINT HostZoom(void *) { return g_zoom; }

static void OnFrameZoom(UINT32 p, void *) { SetZoomPercent(p); }

/* ---- window ---- */

static void Layout()
{
    RECT rc;
    LeewayUIFrameRects fr;
    GetClientRect(g_hwnd, &rc);
    LeewayFrameLayout(g_frame, &rc, &fr);
    g_work = fr.work;
    if (g_fit)
        FitZoom(), LeewayFrameSetZoom(g_frame, g_zoom);
    InvalidateRect(g_hwnd, &g_work, FALSE);
}

static void Paint(HDC dc)
{
    int w = g_work.right - g_work.left, h = g_work.bottom - g_work.top;
    if (w <= 0 || h <= 0)
        return;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    RECT r = { 0, 0, w, h };
    HBRUSH br = CreateSolidBrush(RGB(32, 32, 34));
    FillRect(mem, &r, br);
    DeleteObject(br);
    if (g_img)
    {
        Gdiplus::Graphics g(mem);
        g.SetInterpolationMode(g_zoom < 100 ? Gdiplus::InterpolationModeHighQualityBicubic
                                            : Gdiplus::InterpolationModeNearestNeighbor);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        int iw = (int)(g_img->GetWidth() * g_zoom / 100.0), ih = (int)(g_img->GetHeight() * g_zoom / 100.0);
        g.DrawImage(g_img, (w - iw) / 2, (h - ih) / 2, std::max(iw, 1), std::max(ih, 1));
    }
    else
    {
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(190, 190, 195));
        DrawTextW(mem, L"Ctrl+O to open a picture (or drop one here)", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    BitBlt(dc, g_work.left, g_work.top, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT lr = 0;

    if (g_frame && LeewayFrameHandleMessage(g_frame, msg, wp, lp, &lr))
        return lr;

    switch (msg)
    {
        case WM_COMMAND:
        {
            UINT id = LOWORD(wp), pct;
            if (VRB_ZoomPercentOfCommand(id, &pct))
            {
                SetZoomPercent(pct);
                return 0;
            }
            switch (id)
            {
                case VRB_CMD_OPEN:      OpenDialog(); break;
                case VRB_CMD_EXIT:      DestroyWindow(hwnd); break;
                case IDC_PREV_PIC:      Page(-1); break;
                case IDC_NEXT_PIC:      Page(1); break;
                case IDC_BEST_FIT:
                case VRB_CMD_ZOOM_FIT:  g_fit = true; Refresh(); break;
                case IDC_REAL_SIZE:
                case VRB_CMD_ZOOM_ACTUAL: SetZoomPercent(100); break;
                case IDC_ZOOM_IN:       StepZoom(1); break;
                case IDC_ZOOM_OUT:      StepZoom(-1); break;
                case IDC_ROT_CLOCKW:    Rotate(Gdiplus::Rotate90FlipNone); break;
                case IDC_ROT_COUNCW:    Rotate(Gdiplus::Rotate270FlipNone); break;
                case VRB_LWUI_STATUSBAR_TOGGLE:
                    LeewayFrameShowStatusBar(g_frame, !LeewayFrameIsStatusBarShown(g_frame), TRUE);
                    Layout();
                    VRB_RibbonInvalidate(g_rib, id);
                    break;
                case VRB_LWUI_RIBBON_MINIMIZE:
                    LeewayRibbonMinimizeRibbon(VRB_RibbonFramework(g_rib), !LeewayRibbonIsMinimized(VRB_RibbonFramework(g_rib)));
                    Layout();
                    VRB_RibbonInvalidate(g_rib, id);
                    break;
                case VRB_LWUI_HELP:
                    MessageBoxW(hwnd, L"Photo Viewer ribbon demo.\nThe chrome (ribbon, status bar, zoom slider) is the real "
                                      L"LeewayUI code; the image view here is minimal.",
                                L"Photo Viewer", MB_OK | MB_ICONINFORMATION);
                    break;
            }
            return 0;
        }
        case WM_SIZE:
            if (g_frame)
                Layout();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            Paint(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEWHEEL:
            if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL)
                StepZoom(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1);
            else
                Page(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1);
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_LEFT) Page(-1);
            else if (wp == VK_RIGHT) Page(1);
            else if (wp == VK_ADD || wp == VK_OEM_PLUS) StepZoom(1);
            else if (wp == VK_SUBTRACT || wp == VK_OEM_MINUS) StepZoom(-1);
            else if (wp == 'O' && (GetKeyState(VK_CONTROL) & 0x8000)) OpenDialog();
            return 0;
        case WM_DROPFILES:
        {
            wchar_t f[MAX_PATH];
            if (DragQueryFileW((HDROP)wp, 0, f, MAX_PATH))
                OpenFile(f);
            DragFinish((HDROP)wp);
            return 0;
        }
        case WM_DESTROY:
            LeewayFrameDestroy(g_frame);
            g_frame = NULL;
            VRB_RibbonDestroy(g_rib);
            g_rib = NULL;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    g_inst = inst;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES | ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);
    Gdiplus::GdiplusStartupInput gi;
    ULONG_PTR tok;
    Gdiplus::GdiplusStartup(&tok, &gi, NULL);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"LwuiViewerDemo";
    RegisterClassW(&wc);
    g_hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"Photo Viewer (ribbon demo)", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1100, 760, NULL, NULL, inst, NULL);

    VRB_HOST host = { NULL, HostPredicate, HostExecute, HostZoom };
    if (FAILED(VRB_RibbonCreate(g_hwnd, inst, &host, &g_rib)))
    {
        MessageBoxW(NULL, L"Could not create the ribbon.", L"Photo Viewer demo", MB_ICONERROR);
        return 1;
    }
    LeewayUIFrameDesc d = {};
    d.struct_size = sizeof(d);
    d.owner = g_hwnd;
    d.instance = inst;
    d.app_name = L"PhotoViewerDemo";
    d.flags = LWUI_FRAME_STATUSBAR | LWUI_FRAME_ZOOM_SLIDER;
    d.default_chrome = LWUI_CHROME_RIBBON;
    d.ribbon = VRB_RibbonFramework(g_rib);
    d.zoom_levels = g_VrbZoomPercents;
    d.zoom_level_count = VRB_ZOOM_LEVELS;
    d.on_zoom = OnFrameZoom;
    if (FAILED(LeewayFrameCreate(&d, &g_frame)))
    {
        MessageBoxW(NULL, L"Could not create the frame.", L"Photo Viewer demo", MB_ICONERROR);
        return 1;
    }
    Layout();
    Refresh();
    ShowWindow(g_hwnd, show);

    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1)
        OpenFile(argv[1]);
    LocalFree(argv);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0)
    {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    Gdiplus::GdiplusShutdown(tok);
    return 0;
}
