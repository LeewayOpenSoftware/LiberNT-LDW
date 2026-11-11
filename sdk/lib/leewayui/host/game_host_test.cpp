/* Host test for the G0 game shell (MinGW, real registry/user32, stub ribbon). Prints "game_host_test: ok". */
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>
#include <leewayui/game.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail++; } } while (0)

struct ILeewayRibbon { int dummy; };
extern "C" {
HRESULT WINAPI LeewayRibbonCreate(HWND, const LeewayUICommand *, UINT32, LeewayUICommandExecuteHandler,
    LeewayUICommandStateHandler, void *, ILeewayRibbon **) { return E_FAIL; }
HWND WINAPI LeewayRibbonGetHWND(ILeewayRibbon *) { return NULL; }
UINT32 WINAPI LeewayRibbonGetHeight(ILeewayRibbon *) { return 0; }
HRESULT WINAPI LeewayRibbonSetDPI(ILeewayRibbon *, UINT32) { return S_OK; }
HRESULT WINAPI LeewayRibbonOnThemeChanged(ILeewayRibbon *) { return S_OK; }
HRESULT WINAPI LeewayRibbonDestroy(ILeewayRibbon *) { return S_OK; }
}

#define GAME L"GameHostTest"
static void Cleanup(void)
{
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Leeway\\Games\\" GAME L"\\Stats\\Default");
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Leeway\\Games\\" GAME L"\\Stats\\Hard");
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Leeway\\Games\\" GAME L"\\Stats");
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Leeway\\Games\\" GAME L"\\Save");
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Leeway\\Games\\" GAME);
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Leeway\\Games\\" GAME);
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Leeway\\Games\\" GAME);
}

static void TestStats(void)
{
    LwgStats s, l; WCHAR buf[512];
    LwgStatsClear(&s);
    LwgStatsRecord(&s, TRUE, 300, TRUE, 500);
    LwgStatsRecord(&s, TRUE, 200, TRUE, 400);
    CHECK(s.played == 2 && s.won == 2 && s.best_time == 200 && s.best_score == 500 && s.streak == 2 && s.best_win_streak == 2);
    LwgStatsRecord(&s, FALSE, 50, FALSE, 0);
    CHECK(s.streak == -1 && s.best_win_streak == 2 && s.best_loss_streak == 1 && s.played == 3 && s.total_time == 550);
    LwgStatsRecord(&s, FALSE, 10, FALSE, 0);
    CHECK(s.streak == -2 && s.best_loss_streak == 2);
    LwgStatsRecord(&s, TRUE, 0, FALSE, 0);               /* a win in 0 s does not become the best time */
    CHECK(s.best_time == 200 && s.streak == 1 && s.has_score && s.best_score == 500);
    LwgStatsFormat(&s, buf, 512);
    CHECK(wcsstr(buf, L"Games played: 5") && wcsstr(buf, L"Win rate: 60%") && wcsstr(buf, L"Best time: 3:20") &&
          wcsstr(buf, L"Best score: 500"));
    LwgStatsFormat(&s, buf, 8);                          /* truncates, stays terminated */
    CHECK(wcslen(buf) < 8);
    CHECK(!LwgStatsLoad(GAME, L"Default", &l) && l.played == 0);
    CHECK(LwgStatsSave(GAME, L"Default", &s) && LwgStatsLoad(GAME, L"Default", &l) && !memcmp(&s, &l, sizeof(s)));
    CHECK(!LwgStatsLoad(GAME, L"Hard", &l));              /* variants are separate */
    CHECK(LwgStatsReset(GAME, L"Default") && !LwgStatsLoad(GAME, L"Default", &l));
    CHECK(!LwgStatsSave(NULL, L"x", &s) && !LwgStatsSave(GAME, L"", &s));
}

static void TestSave(void)
{
    BYTE data[100], packed[200], *got = NULL; const void *pl; UINT32 n, psz, ver, gsz, i;
    for (i = 0; i < 100; i++) data[i] = (BYTE)(i * 7);
    n = LwgSavePack(LWG_TAG('S','O','L','1'), 3, data, 100, packed, 200);
    CHECK(n == 100 + LWG_SAVE_HEADER_SIZE && !memcmp(packed, "LWGS", 4));
    CHECK(LwgSavePack(1, 1, data, 100, packed, 100) == 0);                      /* too small */
    CHECK(LwgSaveUnpack(LWG_TAG('S','O','L','1'), 3, packed, n, &pl, &psz, &ver) == LWG_SAVE_OK && psz == 100 && ver == 3 && !memcmp(pl, data, 100));
    CHECK(LwgSaveUnpack(LWG_TAG('S','O','L','1'), 2, packed, n, &pl, &psz, &ver) == LWG_SAVE_NEWER);
    CHECK(LwgSaveUnpack(LWG_TAG('S','P','D','1'), 3, packed, n, &pl, &psz, &ver) == LWG_SAVE_WRONG_GAME);
    CHECK(LwgSaveUnpack(LWG_TAG('S','O','L','1'), 3, packed, n - 1, &pl, &psz, &ver) == LWG_SAVE_TRUNCATED);
    CHECK(LwgSaveUnpack(LWG_TAG('S','O','L','1'), 3, packed, 10, &pl, &psz, &ver) == LWG_SAVE_TRUNCATED);
    packed[50] ^= 1;
    CHECK(LwgSaveUnpack(LWG_TAG('S','O','L','1'), 3, packed, n, &pl, &psz, &ver) == LWG_SAVE_BAD_CRC);
    packed[50] ^= 1; packed[0] = 'X';
    CHECK(LwgSaveUnpack(LWG_TAG('S','O','L','1'), 3, packed, n, &pl, &psz, &ver) == LWG_SAVE_BAD_MAGIC);
    /* registry slot */
    CHECK(LwgSaveRestore(GAME, L"Game", 5, 1, (void **)&got, &gsz, NULL) == LWG_SAVE_TRUNCATED && !got);
    CHECK(LwgSaveStore(GAME, L"Game", 5, 1, data, 100));
    CHECK(LwgSaveRestore(GAME, L"Game", 5, 1, (void **)&got, &gsz, &ver) == LWG_SAVE_OK && gsz == 100 && !memcmp(got, data, 100) && ver == 1);
    LwgFree(got); got = NULL;
    CHECK(LwgSaveRestore(GAME, L"Game", 6, 1, (void **)&got, &gsz, NULL) == LWG_SAVE_WRONG_GAME && !got);
    CHECK(LwgSaveStore(GAME, L"Empty", 5, 1, NULL, 0) && LwgSaveRestore(GAME, L"Empty", 5, 1, (void **)&got, &gsz, NULL) == LWG_SAVE_OK && gsz == 0);
    LwgFree(got); got = NULL;
    CHECK(LwgSaveDiscard(GAME, L"Game") && LwgSaveRestore(GAME, L"Game", 5, 1, (void **)&got, &gsz, NULL) == LWG_SAVE_TRUNCATED);
    CHECK(LwgSaveDiscard(GAME, L"Game"));                                          /* idempotent */
}

static void TestBall(void)
{
    LwgBall b = { 100, 0, 3, 0 }; int i; BOOL alive = TRUE; float maxy = 0;
    for (i = 0; i < 1000 && alive; i++) {
        alive = LwgBallStep(&b, 400, 70, 800, 0.6f, 0.78f);
        if (b.y > maxy) maxy = b.y;
    }
    CHECK(!alive && b.x > 800 && maxy <= 400.0f);      /* left through the right edge, never below the floor */
    {
        LwgBall r = { 100, 400, 0, 0 };                /* resting on the floor keeps moving */
        for (i = 0; i < 5; i++) LwgBallStep(&r, 400, 70, 800, 0.6f, 0.78f);
        CHECK(r.vy < 0 || r.y < 400);
    }
    {
        LwgBall l = { 10, 0, -100, 0 };
        CHECK(!LwgBallStep(&l, 400, 70, 800, 0.6f, 0.78f));   /* left through the left edge */
    }
}

static int g_draws, g_ticks;
static void Draw(HDC, int, int, UINT32, void *) { g_draws++; }
static void Tick(UINT32, void *) { g_ticks++; }

static void TestShell(void)
{
    HWND w = CreateWindowExW(0, L"STATIC", L"t", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 800, 600, NULL, NULL, NULL, NULL);
    LwgShellDesc d; LwgShell *sh = NULL; LwgStats st; LRESULT lr; RECT c = { 0, 0, 800, 500 }; LeewayUIFrameRects r;
    POINT o[2] = { { 100, 100 }, { 200, 100 } }; void *got = NULL; UINT32 gsz; int i;

    ZeroMemory(&d, sizeof(d));
    CHECK(FAILED(LwgShellCreate(&d, &sh)) && !sh);
    d.struct_size = sizeof(d); d.owner = w; d.game_id = GAME; d.title = L"Host"; d.save_tag = 9; d.save_version = 1; d.pill_height = 30;
    CHECK(SUCCEEDED(LwgShellCreate(&d, &sh)) && sh);
    CHECK(LwgShellGetFrame(sh) && LeewayFrameGetChrome(LwgShellGetFrame(sh)) == LWUI_CHROME_PILL);
    LwgShellLayout(sh, &c, &r);
    CHECK(r.work.top >= 30 && r.work.bottom <= 500 && r.status.bottom == 500);
    CHECK(LwgShellScale(sh, 100) >= 100);
    LwgShellSetVariant(sh, L"Hard");
    LwgShellSetTickHandler(sh, Tick, NULL);

    CHECK(!LwgShellIsGameRunning(sh));
    LwgShellGameStart(sh);
    CHECK(LwgShellIsGameRunning(sh));
    CHECK(LwgShellHandleMessage(sh, WM_TIMER, 0x7E51, 0, &lr));
    CHECK(LwgShellHandleMessage(sh, WM_TIMER, 0x7E51, 0, &lr) && LwgShellElapsedSeconds(sh) == 2 && g_ticks == 2);
    CHECK(!LwgShellHandleMessage(sh, WM_SIZE, SIZE_MINIMIZED, 0, &lr));
    LwgShellHandleMessage(sh, WM_TIMER, 0x7E51, 0, &lr);
    CHECK(LwgShellElapsedSeconds(sh) == 2);                     /* paused */
    LwgShellHandleMessage(sh, WM_SIZE, SIZE_RESTORED, 0, &lr);
    CHECK(LwgShellSave(sh, "abc", 3) && LwgShellResume(sh, &got, &gsz) == LWG_SAVE_OK && gsz == 3);
    LwgFree(got);
    LwgShellGameEnd(sh, TRUE, TRUE, 700);
    CHECK(!LwgShellIsGameRunning(sh) && LwgShellResume(sh, &got, &gsz) != LWG_SAVE_OK);   /* save dropped at the end */
    CHECK(LwgStatsLoad(GAME, L"Hard", &st) && st.played == 1 && st.won == 1 && st.best_time == 2 && st.best_score == 700);
    CHECK(!LwgStatsLoad(GAME, L"Default", &st));

    LwgShellGameStart(sh); LwgShellGameAbandon(sh);
    CHECK(LwgStatsLoad(GAME, L"Hard", &st) && st.played == 2 && st.won == 1 && st.streak == -1);
    LwgShellGameAbandon(sh);                                     /* not running: no extra record */
    CHECK(LwgStatsLoad(GAME, L"Hard", &st) && st.played == 2);
    LwgShellGameEnd(sh, TRUE, FALSE, 0);                         /* not running: ignored */
    CHECK(LwgStatsLoad(GAME, L"Hard", &st) && st.played == 2);

    CHECK(!LwgShellStartWin(sh, w, 71, 96, o, 0, Draw, NULL));
    CHECK(LwgShellStartWin(sh, w, 71, 96, o, 2, Draw, NULL));
    for (i = 0; i < 4000; i++) {
        LwgShellHandleMessage(sh, WM_TIMER, 0x7E50, 0, &lr);
        if (g_draws > 50 && i > 5) break;
    }
    CHECK(g_draws > 0);
    LwgShellStopWin(sh); LwgShellStopWin(sh);                    /* idempotent */
    LwgShellDestroy(sh); LwgShellDestroy(NULL);
    DestroyWindow(w);
}

int main(void)
{
    Cleanup();
    TestStats(); TestSave(); TestBall(); TestShell();
    Cleanup();
    if (g_fail) { printf("game_host_test: %d FAILED\n", g_fail); return 1; }
    printf("game_host_test: ok\n");
    return 0;
}
