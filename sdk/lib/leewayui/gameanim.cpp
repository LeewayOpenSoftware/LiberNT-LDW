/*
 * PROJECT:     LeewayUI
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     G0 win animation: bouncing card cascade (physics is pure, drawing is the game's)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <windows.h>
#include <leewayui/game.h>

#define WIN_TIMER_ID    0x7E50
#define WIN_INTERVAL_MS 16
#define WIN_GRAVITY     0.6f
#define WIN_DAMPING     0.78f
#define WIN_STEPS_PER_TICK 2

BOOL LwgBallStep(LwgBall *b, int floor_y, int card_w, int client_w, float gravity, float damping)
{
    if (!b)
        return FALSE;
    b->vy += gravity;
    b->x += b->vx;
    b->y += b->vy;
    if (b->y > (float)floor_y && b->vy > 0)
    {
        b->y = (float)floor_y;
        b->vy = -b->vy * damping;
        if (b->vy > -1.0f && b->vy < 0)             /* came to rest: kick it so it keeps moving */
            b->vy = -3.0f;
    }
    if (b->x < (float)-card_w || b->x > (float)client_w)
        return FALSE;
    return TRUE;
}

struct LwgWinAnim {
    HWND window;
    UINT32 card_w, card_h, count, next;     /* next card to launch */
    POINT *origins;
    LwgWinDrawProc draw;
    void *context;
    LwgBall ball;
    BOOL active;                            /* a ball is in flight */
    UINT32 seed;
};

static float Rand(LwgWinAnim *a)    /* 0..1, xorshift: the animation must not disturb the game's rand() */
{
    a->seed ^= a->seed << 13;
    a->seed ^= a->seed >> 17;
    a->seed ^= a->seed << 5;
    return (float)(a->seed & 0xFFFF) / 65535.0f;
}

static void Launch(LwgWinAnim *a)
{
    float speed = 2.0f + Rand(a) * 4.0f;

    a->ball.x = (float)a->origins[a->next].x;
    a->ball.y = (float)a->origins[a->next].y;
    a->ball.vx = (Rand(a) < 0.5f) ? -speed : speed;
    a->ball.vy = -Rand(a) * 6.0f;
    a->active = TRUE;
}

LwgWinAnim * WINAPI LwgWinAnimStart(HWND window, UINT32 card_w, UINT32 card_h, const POINT *origins, UINT32 count,
                                    LwgWinDrawProc draw, void *context)
{
    LwgWinAnim *a;

    if (!window || !origins || !count || !draw || !card_w || !card_h)
        return NULL;
    a = (LwgWinAnim *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*a));
    if (!a)
        return NULL;
    a->origins = (POINT *)HeapAlloc(GetProcessHeap(), 0, count * sizeof(POINT));
    if (!a->origins)
    {
        HeapFree(GetProcessHeap(), 0, a);
        return NULL;
    }
    CopyMemory(a->origins, origins, count * sizeof(POINT));
    a->window = window;
    a->card_w = card_w;
    a->card_h = card_h;
    a->count = count;
    a->draw = draw;
    a->context = context;
    a->seed = GetTickCount() | 1;
    if (!SetTimer(window, WIN_TIMER_ID, WIN_INTERVAL_MS, NULL))
    {
        HeapFree(GetProcessHeap(), 0, a->origins);
        HeapFree(GetProcessHeap(), 0, a);
        return NULL;
    }
    Launch(a);
    return a;
}

BOOL WINAPI LwgWinAnimIsTimer(const LwgWinAnim *a, UINT_PTR timer_id)
{
    return a && timer_id == WIN_TIMER_ID;
}

static void Release(LwgWinAnim *a, BOOL wipe)
{
    KillTimer(a->window, WIN_TIMER_ID);
    if (wipe)
        InvalidateRect(a->window, NULL, TRUE);      /* the game repaints the table over the trail */
    HeapFree(GetProcessHeap(), 0, a->origins);
    HeapFree(GetProcessHeap(), 0, a);
}

void WINAPI LwgWinAnimStop(LwgWinAnim *a)
{
    if (a)
        Release(a, TRUE);
}

BOOL WINAPI LwgWinAnimOnTimer(LwgWinAnim *a, UINT_PTR timer_id)
{
    RECT rc;
    HDC dc;
    int step;

    if (!a || timer_id != WIN_TIMER_ID)
        return TRUE;
    /* any key or mouse button ends the show */
    if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ||
        (GetAsyncKeyState(VK_ESCAPE) & 0x8000) || (GetAsyncKeyState(VK_RETURN) & 0x8000) ||
        (GetAsyncKeyState(VK_SPACE) & 0x8000))
    {
        LwgWinAnimStop(a);
        return FALSE;
    }
    GetClientRect(a->window, &rc);
    dc = GetDC(a->window);
    if (!dc)
        return TRUE;
    for (step = 0; step < WIN_STEPS_PER_TICK && a->active; ++step)
    {
        a->active = LwgBallStep(&a->ball, rc.bottom - (int)a->card_h, (int)a->card_w, rc.right,
                                WIN_GRAVITY, WIN_DAMPING);
        if (a->active)
            a->draw(dc, (int)a->ball.x, (int)a->ball.y, a->next, a->context);
        if (!a->active)
        {
            a->next++;
            if (a->next >= a->count)
            {
                ReleaseDC(a->window, dc);
                Release(a, FALSE);                  /* finished by itself: the last trail stays */
                return FALSE;
            }
            Launch(a);
        }
    }
    ReleaseDC(a->window, dc);
    return TRUE;
}
