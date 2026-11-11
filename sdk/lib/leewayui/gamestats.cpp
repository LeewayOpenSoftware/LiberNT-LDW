/*
 * PROJECT:     LeewayUI
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     G0 game statistics: pure counters + per-game registry persistence
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <windows.h>
#include <stdio.h>
#include <leewayui/game.h>

void LwgStatsClear(LwgStats *s)
{
    if (s)
        ZeroMemory(s, sizeof(*s));
}

void LwgStatsRecord(LwgStats *s, BOOL won, UINT32 seconds, BOOL use_score, INT32 score)
{
    if (!s)
        return;
    s->played++;
    s->total_time += seconds;
    if (won)
    {
        s->won++;
        if (seconds && (!s->best_time || seconds < s->best_time))
            s->best_time = seconds;
        if (use_score && (!s->has_score || score > s->best_score))
        {
            s->best_score = score;
            s->has_score = TRUE;
        }
        s->streak = s->streak > 0 ? s->streak + 1 : 1;
        if ((UINT32)s->streak > s->best_win_streak)
            s->best_win_streak = (UINT32)s->streak;
    }
    else
    {
        s->streak = s->streak < 0 ? s->streak - 1 : -1;
        if ((UINT32)(-s->streak) > s->best_loss_streak)
            s->best_loss_streak = (UINT32)(-s->streak);
    }
}

static void AppendTime(WCHAR *out, size_t cch, const WCHAR *label, UINT32 secs)
{
    size_t n = wcslen(out);
    if (n + 1 >= cch)
        return;
    _snwprintf(out + n, cch - n, L"%ls: %u:%02u\r\n", label, secs / 60, secs % 60);
    out[cch - 1] = 0;
}

static void AppendNum(WCHAR *out, size_t cch, const WCHAR *label, long long v)
{
    size_t n = wcslen(out);
    if (n + 1 >= cch)
        return;
    _snwprintf(out + n, cch - n, L"%ls: %lld\r\n", label, v);
    out[cch - 1] = 0;
}

void LwgStatsFormat(const LwgStats *s, WCHAR *out, UINT32 cch)
{
    UINT32 pct;

    if (!out || !cch)
        return;
    out[0] = 0;
    if (!s)
        return;
    pct = s->played ? (UINT32)(((UINT64)s->won * 100 + s->played / 2) / s->played) : 0;
    AppendNum(out, cch, L"Games played", s->played);
    AppendNum(out, cch, L"Games won", s->won);
    {
        size_t n = wcslen(out);
        if (n + 1 < cch)
        {
            _snwprintf(out + n, cch - n, L"Win rate: %u%%\r\n", pct);
            out[cch - 1] = 0;
        }
    }
    if (s->best_time)
        AppendTime(out, cch, L"Best time", s->best_time);
    if (s->has_score)
        AppendNum(out, cch, L"Best score", s->best_score);
    if (s->streak > 0)
        AppendNum(out, cch, L"Current streak (wins)", s->streak);
    else if (s->streak < 0)
        AppendNum(out, cch, L"Current streak (losses)", -(long long)s->streak);
    AppendNum(out, cch, L"Longest winning streak", s->best_win_streak);
    AppendNum(out, cch, L"Longest losing streak", s->best_loss_streak);
    if (s->played)
        AppendTime(out, cch, L"Average time", (UINT32)(s->total_time / s->played));
}

/* ---- registry ---- */

static BOOL KeyPath(const WCHAR *game, const WCHAR *variant, WCHAR *out, size_t cch)
{
    if (!game || !*game || !variant || !*variant)
        return FALSE;
    return _snwprintf(out, cch, L"Software\\Leeway\\Games\\%ls\\Stats\\%ls", game, variant) > 0;
}

BOOL LwgStatsLoad(const WCHAR *game, const WCHAR *variant, LwgStats *out)
{
    WCHAR path[260];
    HKEY k;
    DWORD type = 0, size;
    BOOL ok = FALSE;

    if (!out)
        return FALSE;
    LwgStatsClear(out);
    if (!KeyPath(game, variant, path, 260))
        return FALSE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return FALSE;
    size = sizeof(*out);
    {
        LwgStats tmp;
        ZeroMemory(&tmp, sizeof(tmp));
        if (RegQueryValueExW(k, L"Stats", NULL, &type, (LPBYTE)&tmp, &size) == ERROR_SUCCESS &&
            type == REG_BINARY && size == sizeof(tmp))
        {
            *out = tmp;
            ok = TRUE;
        }
    }
    RegCloseKey(k);
    return ok;
}

BOOL LwgStatsSave(const WCHAR *game, const WCHAR *variant, const LwgStats *s)
{
    WCHAR path[260];
    HKEY k;
    BOOL ok;

    if (!s || !KeyPath(game, variant, path, 260))
        return FALSE;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS)
        return FALSE;
    ok = RegSetValueExW(k, L"Stats", 0, REG_BINARY, (const BYTE *)s, sizeof(*s)) == ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}

BOOL LwgStatsReset(const WCHAR *game, const WCHAR *variant)
{
    WCHAR path[260];

    if (!KeyPath(game, variant, path, 260))
        return FALSE;
    RegDeleteKeyW(HKEY_CURRENT_USER, path);
    return TRUE;
}
