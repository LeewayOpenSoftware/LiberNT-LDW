/*
 * PROJECT:     LiberNT Update
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Host test for the REG_MULTI_SZ BootExecute helpers (no privileges needed)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "bootstrap.h"

static int g_Failures;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { printf("FAIL: %s (%d)\n", msg, __LINE__); g_Failures++; } \
        else { printf("ok:   %s\n", msg); } \
    } while (0)

static void
ExpectSeq(
    _In_ const WCHAR *Got,
    _In_ const WCHAR *Expected,
    _In_ const char *Msg)
{
    while (*Expected)
    {
        if (wcscmp(Got, Expected) != 0)
        {
            printf("FAIL: %s (got '%ls' expected '%ls')\n", Msg, Got, Expected);
            g_Failures++;
            return;
        }
        Got += wcslen(Got) + 1;
        Expected += wcslen(Expected) + 1;
    }
    if (Got[0] != 0)
    {
        printf("FAIL: %s (trailing entries)\n", Msg);
        g_Failures++;
        return;
    }
    printf("ok:   %s\n", Msg);
}

int
main(void)
{
    WCHAR Out[256];
    WCHAR In1[] = L"autocheck\0autochk *\0";
    WCHAR In2[] = L"autocheck\0autochk *\0something\0";
    WCHAR In3[] = L"onlyapp\0";
    WCHAR Empty[] = L"\0";
    SIZE_T Chars;

    Chars = BsMultiSzAdd(Out, RTL_NUMBER_OF(Out), In1, L"LiberNTUpdatePending");
    CHECK(Chars > 0, "add to autocheck-only list succeeds");
    if (Chars)
        ExpectSeq(Out, L"autocheck\0autochk *\0LiberNTUpdatePending\0", "lands after autochk *");

    Chars = BsMultiSzAdd(Out, RTL_NUMBER_OF(Out), In2, L"LiberNTUpdatePending");
    CHECK(Chars > 0, "add with trailing entry succeeds");
    if (Chars)
        ExpectSeq(Out, L"autocheck\0autochk *\0LiberNTUpdatePending\0something\0",
                  "inserted before first non-autocheck entry");

    Chars = BsMultiSzAdd(Out, RTL_NUMBER_OF(Out), In3, L"LiberNTUpdatePending");
    CHECK(Chars > 0, "add to foreign-only list succeeds");
    if (Chars)
        ExpectSeq(Out, L"LiberNTUpdatePending\0onlyapp\0",
                  "prepended when no autocheck entry exists");

    {
        /* Pam review B6: a foreign entry BEFORE the autocheck block must not
         * push the app ahead of the filesystem checks. */
        WCHAR InForeignFirst[] = L"something\0autocheck\0autochk *\0";
        Chars = BsMultiSzAdd(Out, RTL_NUMBER_OF(Out), InForeignFirst,
                             L"LiberNTUpdatePending");
        CHECK(Chars > 0, "add with foreign first entry succeeds");
        if (Chars)
            ExpectSeq(Out, L"something\0autocheck\0autochk *\0LiberNTUpdatePending\0",
                      "inserted after the last autocheck, not before the first");
    }

    Chars = BsMultiSzAdd(Out, RTL_NUMBER_OF(Out), Empty, L"LiberNTUpdatePending");
    CHECK(Chars > 0, "add to empty value succeeds");
    if (Chars)
        ExpectSeq(Out, L"LiberNTUpdatePending\0", "empty value becomes the single entry");

    Chars = BsMultiSzAdd(Out, RTL_NUMBER_OF(Out), In1, L"LiberNTUpdatePending");
    {
        WCHAR Again[256];
        SIZE_T AgainChars = BsMultiSzAdd(Again, RTL_NUMBER_OF(Again), Out,
                                         L"LiberNTUpdatePending");
        CHECK(AgainChars == Chars, "second add is idempotent (same length)");
        if (AgainChars)
            ExpectSeq(Again, L"autocheck\0autochk *\0LiberNTUpdatePending\0",
                      "no duplicate entry");
    }

    {
        WCHAR Mixed[256];
        WCHAR InMixed[] = L"autocheck\0LiberNTUpdatePending\0autochk *\0";
        Chars = BsMultiSzAdd(Out, RTL_NUMBER_OF(Out), InMixed, L"LiberNTUpdatePending");
        CHECK(Chars > 0, "add to already-containing list succeeds");
        if (Chars)
        {
            memcpy(Mixed, Out, Chars * sizeof(WCHAR));
            ExpectSeq(Mixed, InMixed, "existing entry untouched wherever it is");
        }
    }

    Chars = BsMultiSzRemove(Out, RTL_NUMBER_OF(Out), In1, L"LiberNTUpdatePending");
    CHECK(Chars > 0, "remove absent entry succeeds");
    if (Chars)
        ExpectSeq(Out, In1, "list unchanged when entry absent");

    Chars = BsMultiSzRemove(Out, RTL_NUMBER_OF(Out), In2, L"something");
    CHECK(Chars > 0, "remove trailing entry succeeds");
    if (Chars)
        ExpectSeq(Out, In1, "only the named entry is removed");

    if (g_Failures)
    {
        printf("%d FAILURES\n", g_Failures);
        return 1;
    }
    printf("ALL MULTISZ TESTS PASSED\n");
    return 0;
}
