/*
 * PROJECT:     LeewayUI
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     G0 common game shell: statistics, save/resume, win animation, frame + DPI wiring
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 * PROVENANCE:  sdk/lib/leewayui/LEEWAYUI-PROVENANCE.md
 *
 * Four independent parts, all optional, all reusable by every Leeway game:
 *
 *   Stats     LwgStats + LwgStatsRecord (pure), persisted per game and variant under
 *             HKCU\Software\Leeway\Games\<game>\Stats\<variant>.
 *   Save      LwgSavePack/Unpack (pure, versioned, CRC-32 checked blob) and a registry slot
 *             store, HKCU\Software\Leeway\Games\<game>\Save\<slot>.
 *   Anim      LwgBall physics (pure) and LwgWinAnim, the card cascade shown on a win. The game
 *             draws one card through a callback; the animation owns the timing.
 *   Shell     LwgShell glues the above to LeewayFrame (pill chrome + status bar), counts game
 *             time, records every finished or abandoned game, and scales metrics with the DPI.
 *
 * The shell owns no game logic and draws no cards. A game keeps its CardWindow (or its own
 * window), forwards WM_TIMER / WM_COMMAND / WM_DPICHANGED / WM_SETTINGCHANGE / WM_THEMECHANGED to
 * LwgShellHandleMessage, and tells the shell when a game starts and ends.
 */

#pragma once

#include <windows.h>
#include <leewayui/frame.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- standard command ids of the Game menu (the game's menu uses these) ---- */
#define LWG_IDM_NEW        0x7E01
#define LWG_IDM_UNDO       0x7E02
#define LWG_IDM_OPTIONS    0x7E03
#define LWG_IDM_STATS      0x7E04
#define LWG_IDM_HINT       0x7E05
#define LWG_IDM_EXIT       0x7E06

/* ================================ statistics ================================ */

typedef struct {
    UINT32 played;          /* games started and finished or abandoned */
    UINT32 won;
    UINT32 best_time;       /* seconds of the fastest win, 0 = none yet */
    INT32  best_score;      /* highest score of a win; valid when has_score */
    UINT32 has_score;       /* TRUE once a win with a score was recorded */
    INT32  streak;          /* > 0 wins in a row, < 0 losses in a row, 0 = none */
    UINT32 best_win_streak;
    UINT32 best_loss_streak;
    UINT64 total_time;      /* seconds over all recorded games */
} LwgStats;

/* Adds one finished game. `score` is ignored unless `use_score`. */
void LwgStatsRecord(LwgStats *s, BOOL won, UINT32 seconds, BOOL use_score, INT32 score);
void LwgStatsClear(LwgStats *s);
/* Human readable block (one line per fact, \r\n separated), at most cch-1 characters. */
void LwgStatsFormat(const LwgStats *s, WCHAR *out, UINT32 cch);

BOOL LwgStatsLoad(const WCHAR *game, const WCHAR *variant, LwgStats *out);   /* FALSE: none stored, *out cleared */
BOOL LwgStatsSave(const WCHAR *game, const WCHAR *variant, const LwgStats *s);
BOOL LwgStatsReset(const WCHAR *game, const WCHAR *variant);

/* ============================== save and resume ============================== */

#define LWG_SAVE_HEADER_SIZE 24

/* Packs a game blob: magic, format version, game tag (four characters of the game id), size,
 * CRC-32. out_cap >= size + LWG_SAVE_HEADER_SIZE. Returns the packed size, 0 on error. */
UINT32 LwgSavePack(UINT32 game_tag, UINT32 version, const void *data, UINT32 size, void *out, UINT32 out_cap);

typedef enum {
    LWG_SAVE_OK = 0,
    LWG_SAVE_TRUNCATED,     /* shorter than the header or than the size field says */
    LWG_SAVE_BAD_MAGIC,
    LWG_SAVE_WRONG_GAME,
    LWG_SAVE_NEWER,         /* written by a newer format version than max_version */
    LWG_SAVE_BAD_CRC
} LwgSaveStatus;

/* Checks a packed blob; on LWG_SAVE_OK *payload / *payload_size / *version point into `packed`. */
LwgSaveStatus LwgSaveUnpack(UINT32 game_tag, UINT32 max_version, const void *packed, UINT32 packed_size,
                            const void **payload, UINT32 *payload_size, UINT32 *version);

#define LWG_TAG(a, b, c, d) ((UINT32)(BYTE)(a) | ((UINT32)(BYTE)(b) << 8) | ((UINT32)(BYTE)(c) << 16) | ((UINT32)(BYTE)(d) << 24))

BOOL LwgSaveStore(const WCHAR *game, const WCHAR *slot, UINT32 game_tag, UINT32 version, const void *data, UINT32 size);
/* Loads and checks. On LWG_SAVE_OK the caller owns *out (LwgFree). LWG_SAVE_TRUNCATED when absent. */
LwgSaveStatus LwgSaveRestore(const WCHAR *game, const WCHAR *slot, UINT32 game_tag, UINT32 max_version,
                             void **out, UINT32 *out_size, UINT32 *version);
BOOL LwgSaveDiscard(const WCHAR *game, const WCHAR *slot);
void LwgFree(void *p);

/* ============================== win animation ============================== */

typedef struct {
    float x, y, vx, vy;
} LwgBall;

/* One physics step: gravity pulls down, the ball bounces on `floor_y` (the bottom of the card)
 * losing `1 - damping` of its speed. Returns FALSE once the ball has left the window sideways
 * (x < -card_w or x > client_w), which is when the next card is launched. */
BOOL LwgBallStep(LwgBall *b, int floor_y, int card_w, int client_w, float gravity, float damping);

typedef struct LwgWinAnim LwgWinAnim;
/* Draws card `index` (0 .. count-1) with its top-left corner at x,y. */
typedef void (*LwgWinDrawProc)(HDC dc, int x, int y, UINT32 index, void *context);

/* Cards are launched from `origins[i]` in the given order. The animation draws straight onto
 * the window (the previous frames stay: that is the classic trail) and stops on its own when
 * all cards are gone or on the first key press, click or LwgWinAnimStop. */
LwgWinAnim * WINAPI LwgWinAnimStart(HWND window, UINT32 card_w, UINT32 card_h, const POINT *origins, UINT32 count,
                                    LwgWinDrawProc draw, void *context);
BOOL WINAPI LwgWinAnimOnTimer(LwgWinAnim *a, UINT_PTR timer_id);   /* FALSE: finished, the animation is freed */
BOOL WINAPI LwgWinAnimIsTimer(const LwgWinAnim *a, UINT_PTR timer_id);
void WINAPI LwgWinAnimStop(LwgWinAnim *a);                         /* safe with NULL */

/* ================================== shell ================================== */

typedef struct LwgShell LwgShell;

typedef struct {
    UINT32 struct_size;                 /* sizeof(LwgShellDesc) */
    HWND owner;
    HINSTANCE instance;
    const WCHAR *game_id;               /* registry key part, e.g. L"Solitaire" */
    const WCHAR *title;                 /* shown in the statistics prompt */
    UINT32 save_tag;                    /* LWG_TAG of the game, for save/resume */
    UINT32 save_version;                /* newest save format the game reads */
    UINT32 pill_height;                 /* logical px the game reserves above the table, 0 = none */
} LwgShellDesc;

HRESULT WINAPI LwgShellCreate(const LwgShellDesc *desc, LwgShell **out);
void WINAPI LwgShellDestroy(LwgShell *sh);

LeewayUIFrame * WINAPI LwgShellGetFrame(LwgShell *sh);
void WINAPI LwgShellLayout(LwgShell *sh, const RECT *client, LeewayUIFrameRects *out);

/* The variant the stats and the save slot use now (draw one / draw three, easy / hard ...). */
void WINAPI LwgShellSetVariant(LwgShell *sh, const WCHAR *variant);

/* Game life cycle. Start when the first move is made; End when the game is won or lost.
 * Abandon counts a started game as lost (call it before dealing a new one). Time runs between
 * Start and End and is paused while the window is minimised. */
void WINAPI LwgShellGameStart(LwgShell *sh);
void WINAPI LwgShellGameEnd(LwgShell *sh, BOOL won, BOOL use_score, INT32 score);
void WINAPI LwgShellGameAbandon(LwgShell *sh);
BOOL WINAPI LwgShellIsGameRunning(const LwgShell *sh);
UINT32 WINAPI LwgShellElapsedSeconds(const LwgShell *sh);
void WINAPI LwgShellSetElapsedSeconds(LwgShell *sh, UINT32 seconds);   /* after a resume */

/* Modal statistics window: `text` (CR LF separated lines) in a read-only box, a Reset and a Close
 * button. Returns TRUE when the player pressed Reset (and confirmed); the caller clears the stats. */
BOOL WINAPI LwgStatsDialog(HWND owner, const WCHAR *title, const WCHAR *text);

/* Shows the statistics of the current variant in LwgStatsDialog and resets them on request. */
void WINAPI LwgShellShowStats(LwgShell *sh);

/* Save/resume of the current game, slot "Game". */
BOOL WINAPI LwgShellSave(LwgShell *sh, const void *data, UINT32 size);
LwgSaveStatus WINAPI LwgShellResume(LwgShell *sh, void **data, UINT32 *size);   /* free with LwgFree */
void WINAPI LwgShellDiscardSave(LwgShell *sh);

/* Win animation through the shell (it forwards the timer). */
BOOL WINAPI LwgShellStartWin(LwgShell *sh, HWND table, UINT32 card_w, UINT32 card_h, const POINT *origins,
                             UINT32 count, LwgWinDrawProc draw, void *context);
void WINAPI LwgShellStopWin(LwgShell *sh);

/* DPI: logical (96 dpi) to device pixels, and the current dpi. */
int WINAPI LwgShellScale(const LwgShell *sh, int logical);
UINT32 WINAPI LwgShellGetDpi(const LwgShell *sh);

/* Forward WM_TIMER, WM_COMMAND (LWG_IDM_STATS), WM_HSCROLL and the other frame messages,
 * WM_DPICHANGED, WM_SETTINGCHANGE, WM_THEMECHANGED and WM_SIZE (a minimised window pauses the
 * clock; WM_SIZE is never consumed). Returns TRUE when handled, with the window procedure's
 * result in *result. */
BOOL WINAPI LwgShellHandleMessage(LwgShell *sh, UINT msg, WPARAM wparam, LPARAM lparam, LRESULT *result);

/* Called once a second while a game runs; the game updates its status text there. */
typedef void (*LwgShellTickHandler)(UINT32 seconds, void *context);
void WINAPI LwgShellSetTickHandler(LwgShell *sh, LwgShellTickHandler tick, void *context);

#ifdef __cplusplus
}
#endif
