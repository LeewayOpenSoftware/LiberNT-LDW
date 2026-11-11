/*
 * PROJECT:     LeewayUI
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     G0 game shell: LeewayFrame (pill + status bar), game clock, stats, save, DPI, win animation
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <windows.h>
#include <stdio.h>
#include <leewayui/game.h>
#include <leewayui/dpi.h>

#define SHELL_TIMER_ID 0x7E51
#define SAVE_SLOT      L"Game"

struct LwgShell {
    LwgShellDesc desc;
    WCHAR game[64];
    WCHAR title[64];
    WCHAR variant[32];
    LeewayUIFrame *frame;
    BOOL running;
    BOOL paused;
    BOOL timer_on;
    UINT32 seconds;
    UINT32 dpi;
    LwgWinAnim *win;
    LwgShellTickHandler tick;
    void *tick_ctx;
};

static void CopyStr(WCHAR *dst, size_t cch, const WCHAR *src)
{
    lstrcpynW(dst, src ? src : L"", (int)cch);
}

HRESULT WINAPI LwgShellCreate(const LwgShellDesc *desc, LwgShell **out)
{
    LwgShell *sh;
    LeewayUIFrameDesc fd;
    WCHAR app[96];
    HRESULT hr;

    if (!out)
        return E_POINTER;
    *out = NULL;
    if (!desc || desc->struct_size != sizeof(*desc) || !desc->owner || !desc->game_id || !*desc->game_id)
        return E_INVALIDARG;
    sh = (LwgShell *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*sh));
    if (!sh)
        return E_OUTOFMEMORY;
    sh->desc = *desc;
    CopyStr(sh->game, 64, desc->game_id);
    CopyStr(sh->title, 64, desc->title ? desc->title : desc->game_id);
    CopyStr(sh->variant, 32, L"Default");
    sh->desc.game_id = sh->game;
    sh->desc.title = sh->title;
    sh->dpi = LeewayDpiGetForWindow(desc->owner);

    ZeroMemory(&fd, sizeof(fd));
    _snwprintf(app, 96, L"Games\\%ls", sh->game);
    app[95] = 0;
    fd.struct_size = sizeof(fd);
    fd.owner = desc->owner;
    fd.instance = desc->instance;
    fd.app_name = app;
    fd.flags = LWUI_FRAME_STATUSBAR;
    fd.default_chrome = LWUI_CHROME_PILL;       /* the game keeps its menu bar; no ribbon commands to give */
    fd.pill_height = desc->pill_height;
    hr = LeewayFrameCreate(&fd, &sh->frame);
    if (FAILED(hr))
    {
        HeapFree(GetProcessHeap(), 0, sh);
        return hr;
    }
    *out = sh;
    return S_OK;
}

static void StopClock(LwgShell *sh)
{
    if (sh->timer_on)
    {
        KillTimer(sh->desc.owner, SHELL_TIMER_ID);
        sh->timer_on = FALSE;
    }
}

void WINAPI LwgShellDestroy(LwgShell *sh)
{
    if (!sh)
        return;
    LwgShellStopWin(sh);
    StopClock(sh);
    LeewayFrameDestroy(sh->frame);
    HeapFree(GetProcessHeap(), 0, sh);
}

LeewayUIFrame * WINAPI LwgShellGetFrame(LwgShell *sh)
{
    return sh ? sh->frame : NULL;
}

void WINAPI LwgShellLayout(LwgShell *sh, const RECT *client, LeewayUIFrameRects *out)
{
    if (sh && client && out)
        LeewayFrameLayout(sh->frame, client, out);
}

void WINAPI LwgShellSetVariant(LwgShell *sh, const WCHAR *variant)
{
    if (sh && variant && *variant)
        CopyStr(sh->variant, 32, variant);
}

void WINAPI LwgShellSetTickHandler(LwgShell *sh, LwgShellTickHandler tick, void *context)
{
    if (sh)
    {
        sh->tick = tick;
        sh->tick_ctx = context;
    }
}

void WINAPI LwgShellGameStart(LwgShell *sh)
{
    if (!sh || sh->running)
        return;
    sh->running = TRUE;
    if (!sh->paused && !sh->timer_on)
        sh->timer_on = SetTimer(sh->desc.owner, SHELL_TIMER_ID, 1000, NULL) != 0;
}

static void Record(LwgShell *sh, BOOL won, BOOL use_score, INT32 score)
{
    LwgStats st;

    LwgStatsLoad(sh->game, sh->variant, &st);
    LwgStatsRecord(&st, won, sh->seconds, use_score, score);
    LwgStatsSave(sh->game, sh->variant, &st);
}

void WINAPI LwgShellGameEnd(LwgShell *sh, BOOL won, BOOL use_score, INT32 score)
{
    if (!sh || !sh->running)
        return;
    sh->running = FALSE;
    StopClock(sh);
    Record(sh, won, use_score, score);
    LwgShellDiscardSave(sh);                    /* a finished game is not resumable */
}

void WINAPI LwgShellGameAbandon(LwgShell *sh)
{
    if (!sh || !sh->running)
    {
        if (sh)
            sh->seconds = 0;
        return;
    }
    sh->running = FALSE;
    StopClock(sh);
    Record(sh, FALSE, FALSE, 0);
    sh->seconds = 0;
    LwgShellDiscardSave(sh);
}

BOOL WINAPI LwgShellIsGameRunning(const LwgShell *sh)
{
    return sh && sh->running;
}

UINT32 WINAPI LwgShellElapsedSeconds(const LwgShell *sh)
{
    return sh ? sh->seconds : 0;
}

void WINAPI LwgShellSetElapsedSeconds(LwgShell *sh, UINT32 seconds)
{
    if (sh)
        sh->seconds = seconds;
}

#define IDC_STATS_TEXT  100
#define IDC_STATS_RESET 101

/* in-memory dialog template builder (DLGTEMPLATE + DLGITEMTEMPLATEs, all WORD aligned) */
struct TplBuf {
    WORD w[512];
    UINT n;
};

static void PutW(TplBuf *t, WORD v)
{
    if (t->n < 512)
        t->w[t->n++] = v;
}

static void PutD(TplBuf *t, DWORD v)
{
    PutW(t, LOWORD(v));
    PutW(t, HIWORD(v));
}

static void PutS(TplBuf *t, const WCHAR *s)
{
    do
        PutW(t, (WORD)*s);
    while (*s++);
}

static void Align(TplBuf *t)
{
    if (t->n & 1)
        PutW(t, 0);
}

static void PutItem(TplBuf *t, DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls, const WCHAR *text)
{
    Align(t);
    PutD(t, style | WS_CHILD | WS_VISIBLE);
    PutD(t, 0);
    PutW(t, (WORD)x); PutW(t, (WORD)y); PutW(t, (WORD)cx); PutW(t, (WORD)cy);
    PutW(t, id);
    PutW(t, 0xFFFF); PutW(t, cls);              /* 0x0080 button, 0x0081 edit, 0x0082 static */
    PutS(t, text);
    PutW(t, 0);                                 /* no creation data */
}

struct StatsDlgData {
    const WCHAR *title;
    const WCHAR *text;
};

static INT_PTR CALLBACK StatsDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        const StatsDlgData *d = (const StatsDlgData *)lp;
        SetWindowTextW(dlg, d->title);
        SetDlgItemTextW(dlg, IDC_STATS_TEXT, d->text);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_STATS_RESET)
        {
            if (MessageBoxW(dlg, L"Reset these statistics? This cannot be undone.", L"Statistics",
                            MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) == IDYES)
                EndDialog(dlg, 1);
            return TRUE;
        }
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)
        {
            EndDialog(dlg, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

BOOL WINAPI LwgStatsDialog(HWND owner, const WCHAR *title, const WCHAR *text)
{
    TplBuf t;
    StatsDlgData d;
    INT_PTR r;

    ZeroMemory(&t, sizeof(t));
    d.title = title ? title : L"Statistics";
    d.text = text ? text : L"";
    PutD(&t, DS_MODALFRAME | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU);
    PutD(&t, 0);
    PutW(&t, 3);                                /* item count */
    PutW(&t, 0); PutW(&t, 0); PutW(&t, 190); PutW(&t, 150);
    PutW(&t, 0); PutW(&t, 0);                   /* no menu, default class */
    PutS(&t, L"");                              /* title set in WM_INITDIALOG */
    PutW(&t, 8);
    PutS(&t, L"MS Shell Dlg");
    PutItem(&t, ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_BORDER | WS_VSCROLL | WS_TABSTOP, 7, 7, 176, 112,
            IDC_STATS_TEXT, 0x0081, L"");
    PutItem(&t, BS_PUSHBUTTON | WS_TABSTOP, 7, 128, 60, 14, IDC_STATS_RESET, 0x0080, L"&Reset...");
    PutItem(&t, BS_DEFPUSHBUTTON | WS_TABSTOP, 123, 128, 60, 14, IDOK, 0x0080, L"&Close");
    r = DialogBoxIndirectParamW(GetModuleHandleW(NULL), (const DLGTEMPLATE *)t.w, owner, StatsDlgProc, (LPARAM)&d);
    return r == 1;
}

void WINAPI LwgShellShowStats(LwgShell *sh)
{
    LwgStats st;
    WCHAR text[640], cap[128];

    if (!sh)
        return;
    LwgStatsLoad(sh->game, sh->variant, &st);
    LwgStatsFormat(&st, text, 640);
    _snwprintf(cap, 128, L"%ls statistics (%ls)", sh->title, sh->variant);
    cap[127] = 0;
    if (LwgStatsDialog(sh->desc.owner, cap, text))
        LwgStatsReset(sh->game, sh->variant);
}

BOOL WINAPI LwgShellSave(LwgShell *sh, const void *data, UINT32 size)
{
    return sh && LwgSaveStore(sh->game, SAVE_SLOT, sh->desc.save_tag, sh->desc.save_version, data, size);
}

LwgSaveStatus WINAPI LwgShellResume(LwgShell *sh, void **data, UINT32 *size)
{
    if (!sh)
        return LWG_SAVE_TRUNCATED;
    return LwgSaveRestore(sh->game, SAVE_SLOT, sh->desc.save_tag, sh->desc.save_version, data, size, NULL);
}

void WINAPI LwgShellDiscardSave(LwgShell *sh)
{
    if (sh)
        LwgSaveDiscard(sh->game, SAVE_SLOT);
}

BOOL WINAPI LwgShellStartWin(LwgShell *sh, HWND table, UINT32 card_w, UINT32 card_h, const POINT *origins,
                             UINT32 count, LwgWinDrawProc draw, void *context)
{
    if (!sh)
        return FALSE;
    LwgShellStopWin(sh);
    sh->win = LwgWinAnimStart(table, card_w, card_h, origins, count, draw, context);
    return sh->win != NULL;
}

void WINAPI LwgShellStopWin(LwgShell *sh)
{
    if (sh && sh->win)
    {
        LwgWinAnimStop(sh->win);
        sh->win = NULL;
    }
}

int WINAPI LwgShellScale(const LwgShell *sh, int logical)
{
    return LeewayDpiScale(sh ? sh->dpi : 96, logical);
}

UINT32 WINAPI LwgShellGetDpi(const LwgShell *sh)
{
    return sh ? sh->dpi : 96;
}

BOOL WINAPI LwgShellHandleMessage(LwgShell *sh, UINT msg, WPARAM wparam, LPARAM lparam, LRESULT *result)
{
    LRESULT r = 0;

    if (!sh)
        return FALSE;
    switch (msg)
    {
    case WM_TIMER:
        if (sh->win && LwgWinAnimIsTimer(sh->win, wparam))
        {
            if (!LwgWinAnimOnTimer(sh->win, wparam))
                sh->win = NULL;                 /* it freed itself */
            if (result)
                *result = 0;
            return TRUE;
        }
        if (wparam == SHELL_TIMER_ID)
        {
            if (sh->running && !sh->paused)
            {
                sh->seconds++;
                if (sh->tick)
                    sh->tick(sh->seconds, sh->tick_ctx);
            }
            if (result)
                *result = 0;
            return TRUE;
        }
        return FALSE;

    case WM_SIZE:                               /* pause the clock while minimised; never consumed */
        if (wparam == SIZE_MINIMIZED)
            sh->paused = TRUE;
        else if (sh->paused)
        {
            sh->paused = FALSE;
            if (sh->running && !sh->timer_on)
                sh->timer_on = SetTimer(sh->desc.owner, SHELL_TIMER_ID, 1000, NULL) != 0;
        }
        return FALSE;

    case WM_COMMAND:
        if (LOWORD(wparam) == LWG_IDM_STATS)
        {
            LwgShellShowStats(sh);
            if (result)
                *result = 0;
            return TRUE;
        }
        return FALSE;

    case WM_DPICHANGED:
        sh->dpi = LOWORD(wparam) ? LOWORD(wparam) : 96;
        /* fall through: the frame re-lays itself out too */
    default:
        break;
    }
    if (LeewayFrameHandleMessage(sh->frame, msg, wparam, lparam, &r))
    {
        if (result)
            *result = r;
        return TRUE;
    }
    return FALSE;
}
