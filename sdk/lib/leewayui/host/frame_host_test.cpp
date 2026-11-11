/* Host test for frame.cpp (MinGW, real comctl32, stub ribbon). Prints "frame_host_test: ok". */
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <leewayui/frame.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail++; } } while (0)

/* stub ribbon: link-time replacement for the ribbon.h free functions */
static HWND g_rw; static int g_dpi_calls, g_destroyed, g_ribbon_fail; static UINT32 g_h = 100;
struct ILeewayRibbon { int dummy; };
static ILeewayRibbon g_stub;
extern "C" {
HRESULT WINAPI LeewayRibbonCreate(HWND parent, const LeewayUICommand *, UINT32, LeewayUICommandExecuteHandler,
    LeewayUICommandStateHandler, void *, ILeewayRibbon **out)
{
    if (g_ribbon_fail) return E_FAIL;
    g_rw = CreateWindowExW(0, L"STATIC", NULL, WS_CHILD | WS_VISIBLE, 0, 0, 10, 100, parent, NULL, NULL, NULL);
    *out = &g_stub; return S_OK;
}
HWND WINAPI LeewayRibbonGetHWND(ILeewayRibbon *) { return g_rw; }
UINT32 WINAPI LeewayRibbonGetHeight(ILeewayRibbon *) { return g_h; }
HRESULT WINAPI LeewayRibbonSetDPI(ILeewayRibbon *, UINT32) { g_dpi_calls++; return S_OK; }
static int g_theme_calls;
HRESULT WINAPI LeewayRibbonOnThemeChanged(ILeewayRibbon *) { g_theme_calls++; return S_OK; }
HRESULT WINAPI LeewayRibbonDestroy(ILeewayRibbon *) { g_destroyed++; return S_OK; }
}

static UINT32 g_zoomed, g_zoomcalls;
static int g_layouts; static LONG g_lastWorkTop;
static void OnLayout(const LeewayUIFrameRects *r, void *) { ++g_layouts; g_lastWorkTop = r->work.top; }
static void OnZoom(UINT32 p, void *) { g_zoomed = p; g_zoomcalls++; }
static HRESULT Exec(UINT32, void *) { return S_OK; }
static HRESULT State(UINT32, LeewayUICommandState *, void *) { return S_OK; }
static const UINT32 kLevels[] = { 10, 25, 50, 75, 100, 150, 200, 400, 800, 1600 };
static LeewayUICommand kCmd[1] = { { 1, (WCHAR*)L"x", (WCHAR*)L"", (WCHAR*)L"", 0, TRUE, FALSE, TRUE } };

#define TESTKEY L"Software\\Leeway\\FrameTest"
static void DelKey(void) { RegDeleteKeyW(HKEY_CURRENT_USER, TESTKEY); }

int main(void)
{
    HWND w = CreateWindowExW(0, L"STATIC", L"t", WS_OVERLAPPEDWINDOW, 0, 0, 800, 600, NULL, NULL, NULL, NULL);
    LeewayUIFrameDesc d; LeewayUIFrame *f = NULL; LeewayUIFrameRects r; RECT c = { 0, 0, 800, 500 }; LRESULT lr;
    DelKey();
    ZeroMemory(&d, sizeof(d));
    d.struct_size = sizeof(d); d.owner = w; d.app_name = L"FrameTest";
    d.flags = LWUI_FRAME_STATUSBAR | LWUI_FRAME_ZOOM_SLIDER;
    d.default_chrome = LWUI_CHROME_PILL; d.pill_height = 40;
    d.commands = kCmd; d.command_count = 1; d.execute = Exec; d.state = State;
    d.zoom_levels = kLevels; d.zoom_level_count = 10; d.on_zoom = OnZoom; d.on_layout = OnLayout;

    /* validation */
    { LeewayUIFrameDesc bad = d; LeewayUIFrame *x = NULL; bad.struct_size = 1; CHECK(LeewayFrameCreate(&bad, &x) == E_INVALIDARG); }
    { LeewayUIFrameDesc bad = d; LeewayUIFrame *x = NULL; UINT32 desc[3] = { 10, 10, 20 }; bad.zoom_levels = desc; bad.zoom_level_count = 3; CHECK(LeewayFrameCreate(&bad, &x) == E_INVALIDARG); }
    { LeewayUIFrameDesc bad = d; LeewayUIFrame *x = NULL; bad.flags = LWUI_FRAME_ZOOM_SLIDER; CHECK(LeewayFrameCreate(&bad, &x) == E_INVALIDARG); }

    /* pill (default), registry default */
    CHECK(LeewayFrameCreate(&d, &f) == S_OK);
    CHECK(LeewayFrameGetChrome(f) == LWUI_CHROME_PILL && !LeewayFrameGetRibbon(f));
    LeewayFrameLayout(f, &c, &r);
    CHECK(r.chrome.bottom - r.chrome.top == 40);
    CHECK(r.status.bottom == 500 && r.status.bottom - r.status.top == 22);
    CHECK(r.work.top == 40 && r.work.bottom == r.status.top);
    CHECK(IsWindowVisible(LeewayFrameGetStatusWindow(f)) || !IsWindowVisible(w)); /* owner hidden: just no crash */

    /* zoom: SetZoom snaps, no callback; drag calls back */
    LeewayFrameSetZoom(f, 100);
    CHECK(LeewayFrameGetZoom(f) == 100 && g_zoomcalls == 0);
    LeewayFrameSetZoom(f, 130);               /* nearest = 150 for slider, text shows 130 */
    { HWND s = FindWindowExW(w, NULL, L"msctls_trackbar32", NULL);
      CHECK(s && SendMessageW(s, TBM_GETPOS, 0, 0) == 5);
      SendMessageW(s, TBM_SETPOS, TRUE, 7);
      CHECK(LeewayFrameHandleMessage(f, WM_HSCROLL, TB_THUMBTRACK, (LPARAM)s, &lr));
      CHECK(g_zoomed == 400 && g_zoomcalls == 1 && LeewayFrameGetZoom(f) == 400);
      CHECK(LeewayFrameHandleMessage(f, WM_HSCROLL, TB_ENDTRACK, (LPARAM)s, &lr) && g_zoomcalls == 1); /* same level: no dup */
      CHECK(!LeewayFrameHandleMessage(f, WM_HSCROLL, 0, (LPARAM)w, &lr)); /* not ours */
      LeewayFrameEnableZoom(f, FALSE); CHECK(!IsWindowEnabled(s)); LeewayFrameEnableZoom(f, TRUE); }

    /* status text + hide/persist */
    LeewayFrameSetStatusText(f, 0, L"640 x 480");
    { WCHAR t[32]; const WCHAR *p = (const WCHAR *)SendMessageW(LeewayFrameGetStatusWindow(f), SB_GETTEXTW, 0, (LPARAM)t);
      CHECK(p && !lstrcmpW(p, L"640 x 480"));
      /* owner draw: the frame paints the part in the theme's colours */
      HDC dc = GetDC(w); DRAWITEMSTRUCT di = {}; di.hwndItem = LeewayFrameGetStatusWindow(f); di.hDC = dc;
      di.rcItem.right = 100; di.rcItem.bottom = 20; di.itemData = (ULONG_PTR)p;
      CHECK(LeewayFrameHandleMessage(f, WM_DRAWITEM, 0, (LPARAM)&di, &lr) && lr == TRUE);
      di.hwndItem = w; CHECK(!LeewayFrameHandleMessage(f, WM_DRAWITEM, 0, (LPARAM)&di, &lr));
      /* the slider background comes from the theme */
      HWND sl = FindWindowExW(w, NULL, L"msctls_trackbar32", NULL);
      CHECK(LeewayFrameHandleMessage(f, WM_CTLCOLORSTATIC, (WPARAM)dc, (LPARAM)sl, &lr) && lr != 0);
      CHECK(!LeewayFrameHandleMessage(f, WM_CTLCOLORSTATIC, (WPARAM)dc, (LPARAM)w, &lr));
      ReleaseDC(w, dc); }
    CHECK(LeewayFrameShowStatusBar(f, FALSE, TRUE));
    LeewayFrameLayout(f, &c, &r);
    CHECK(IsRectEmpty(&r.status) && r.work.bottom == 500);
    LeewayFrameDestroy(f); f = NULL;
    CHECK(LeewayFrameCreate(&d, &f) == S_OK && !LeewayFrameIsStatusBarShown(f));  /* persisted */
    LeewayFrameDestroy(f); f = NULL;

    /* ribbon via registry */
    CHECK(LeewayFrameWriteChromeStyle(L"FrameTest", LWUI_CHROME_RIBBON));
    CHECK(LeewayFrameReadChromeStyle(L"FrameTest", LWUI_CHROME_PILL) == LWUI_CHROME_RIBBON);
    CHECK(LeewayFrameCreate(&d, &f) == S_OK && LeewayFrameGetChrome(f) == LWUI_CHROME_RIBBON && LeewayFrameGetRibbon(f));
    LeewayFrameLayout(f, &c, &r);
    CHECK(r.chrome.bottom - r.chrome.top == 100);          /* ribbon window height */
    { RECT rc; GetWindowRect(g_rw, &rc); CHECK(rc.right - rc.left == 800); }
    g_h = 60; LeewayFrameLayout(f, &c, &r); CHECK(r.chrome.bottom - r.chrome.top == 60);   /* minimized height */
    g_h = 100;
    CHECK(!LeewayFrameHandleMessage(f, WM_DPICHANGED, MAKEWPARAM(144, 144), 0, &lr) && g_dpi_calls == 1);
    LeewayFrameDestroy(f); f = NULL;
    CHECK(g_destroyed == 1);

    /* a ribbon the app built is borrowed: used, never destroyed */
    d.ribbon = &g_stub; g_destroyed = 0;
    CHECK(LeewayFrameCreate(&d, &f) == S_OK && LeewayFrameGetChrome(f) == LWUI_CHROME_RIBBON && LeewayFrameGetRibbon(f) == &g_stub);
    /* theme change goes to the ribbon; the ribbon's height change re-lays out and tells the app */
    CHECK(!LeewayFrameHandleMessage(f, WM_THEMECHANGED, 0, 0, &lr) && g_theme_calls == 1);
    CHECK(!LeewayFrameHandleMessage(f, WM_SYSCOLORCHANGE, 0, 0, &lr) && g_theme_calls == 2);
    g_layouts = 0;
    {
        NMHDR nm; nm.hwndFrom = g_rw; nm.idFrom = 0x4C57; nm.code = LWUI_RBN_HEIGHTCHANGED;
        g_h = 48;
        CHECK(LeewayFrameHandleMessage(f, WM_NOTIFY, 0x4C57, (LPARAM)&nm, &lr) && g_layouts == 1 && g_lastWorkTop == 48);
        nm.hwndFrom = w;                                   /* someone else's notify */
        CHECK(!LeewayFrameHandleMessage(f, WM_NOTIFY, 0, (LPARAM)&nm, &lr) && g_layouts == 1);
        g_h = 100;
    }
    LeewayFrameDestroy(f); f = NULL;
    CHECK(g_destroyed == 0);
    d.ribbon = NULL;

    /* ribbon creation failure falls back to pill */
    g_ribbon_fail = 1;
    CHECK(LeewayFrameCreate(&d, &f) == S_OK && LeewayFrameGetChrome(f) == LWUI_CHROME_PILL);
    LeewayFrameLayout(f, &c, &r); CHECK(r.chrome.bottom - r.chrome.top == 40);
    LeewayFrameDestroy(f);

    /* garbage registry value -> default */
    { HKEY k; RegCreateKeyExW(HKEY_CURRENT_USER, TESTKEY, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL);
      RegSetValueExW(k, L"ChromeStyle", 0, REG_SZ, (const BYTE *)L"bogus", 12); RegCloseKey(k);
      CHECK(LeewayFrameReadChromeStyle(L"FrameTest", LWUI_CHROME_RIBBON) == LWUI_CHROME_RIBBON); }

    DelKey(); DestroyWindow(w);
    if (!g_fail) printf("frame_host_test: ok\n");
    return g_fail ? 1 : 0;
}
