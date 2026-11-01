/*
 * PROJECT:     LiberNT Update
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Pure REG_MULTI_SZ editing helpers (host-testable, no Win32 calls)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "bootstrap.h"

static SIZE_T
BsMultiSzLength(
    _In_ const WCHAR *In,
    _In_ size_t InCch)
{
    SIZE_T i = 0;

    while (i + 1 < InCch && (In[i] || In[i + 1]))
        i++;
    return i + 2 <= InCch ? i + 2 : 0;
}

SIZE_T
BsMultiSzAdd(
    _Out_ WCHAR *Out,
    _In_ size_t OutCch,
    _In_ const WCHAR *In,
    _In_ PCWSTR App)
{
    SIZE_T InLen = BsMultiSzLength(In, OutCch);
    const WCHAR *Cursor = In;
    WCHAR *Write = Out;
    WCHAR *AfterAutoChk = Out;

    if (InLen == 0)
    {
        /* Empty or single empty string: start fresh. */
        if (OutCch < wcslen(App) + 2)
            return 0;
        wcscpy(Out, App);
        Out[wcslen(App) + 1] = 0;
        return wcslen(App) + 2;
    }

    while (*Cursor)
    {
        BOOL IsAutoChk = _wcsicmp(Cursor, L"autocheck") == 0 ||
                         _wcsnicmp(Cursor, L"autocheck ", 10) == 0 ||
                         _wcsicmp(Cursor, L"autochk") == 0 ||
                         _wcsnicmp(Cursor, L"autochk ", 8) == 0;

        if (wcscmp(Cursor, App) == 0)
        {
            /* Already registered: idempotent, and Out still receives the
             * full value so callers can write it back unconditionally. */
            if (InLen > OutCch)
                return 0;
            memmove(Out, In, InLen * sizeof(WCHAR));
            return InLen;
        }
        {
            size_t Len = wcslen(Cursor) + 1;
            if ((size_t)(Write - Out) + Len > OutCch)
                return 0;
            memmove(Write, Cursor, Len * sizeof(WCHAR));
            Write += Len;
        }
        Cursor += wcslen(Cursor) + 1;
        if (IsAutoChk)
            AfterAutoChk = Write; /* insert after the LAST autocheck entry */
    }

    /* Insert after the last autocheck/autochk entry, never before the first
     * one: SMSS must run the filesystem checks first (Pam review B6). */
    {
        size_t Tail = (size_t)(Write - AfterAutoChk);
        size_t AppLen = wcslen(App) + 1;

        if ((size_t)(AfterAutoChk - Out) + AppLen + Tail + 1 > OutCch)
            return 0;
        if (Tail)
            memmove(AfterAutoChk + AppLen, AfterAutoChk, Tail * sizeof(WCHAR));
        wcscpy(AfterAutoChk, App);
        Write = AfterAutoChk + AppLen + Tail;
    }
    *Write = 0;
    return (SIZE_T)(Write - Out) + 1;
}

SIZE_T
BsMultiSzRemove(
    _Out_ WCHAR *Out,
    _In_ size_t OutCch,
    _In_ const WCHAR *In,
    _In_ PCWSTR App)
{
    const WCHAR *Cursor = In;
    WCHAR *Write = Out;

    while (*Cursor)
    {
        size_t Len = wcslen(Cursor) + 1;
        if (wcscmp(Cursor, App) != 0)
        {
            if ((size_t)(Write - Out) + Len > OutCch)
                return 0;
            memmove(Write, Cursor, Len * sizeof(WCHAR));
            Write += Len;
        }
        Cursor += Len;
    }
    if ((size_t)(Write - Out) + 1 > OutCch)
        return 0;
    *Write = 0;
    return (SIZE_T)(Write - Out) + 1;
}
