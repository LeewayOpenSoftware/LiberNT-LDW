/*
 * PROJECT:     LiberNT kernel32 API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Child program of the Utf8ActiveCodePage test. Built twice: once with an
 *              activeCodePage UTF-8 manifest, once without. The first argument says which
 *              one it is ("utf8" or "legacy"); the exit code is 0 when the process behaves as
 *              the published documentation describes
 */

#include <windows.h>
#include <string.h>

static
int
CheckUtf8(void)
{
    /* U+00E9 as UTF-8 */
    static const char Utf8[] = { (char)0xC3, (char)0xA9 };
    WCHAR Wide[4] = { 0 };
    char Narrow[8] = { 0 };
    int Count;

    if (GetACP() != CP_UTF8)
        return 10;

    /* CP_ACP equates to CP_UTF8 when the manifest asks for it */
    Count = MultiByteToWideChar(CP_ACP, 0, Utf8, sizeof(Utf8), Wide, 4);
    if (Count != 1 || Wide[0] != 0x00E9)
        return 11;

    Count = WideCharToMultiByte(CP_ACP, 0, L"\x00E9", 1, Narrow, sizeof(Narrow), NULL, NULL);
    if (Count != 2 || Narrow[0] != (char)0xC3 || Narrow[1] != (char)0xA9)
        return 12;

    return 0;
}

static
int
CheckLegacy(void)
{
    /* Without the manifest the system code page is kept */
    if (GetACP() == CP_UTF8)
        return 20;

    return 0;
}

int
main(int argc, char **argv)
{
    if (argc < 2)
        return 2;

    if (!strcmp(argv[1], "utf8"))
        return CheckUtf8();
    if (!strcmp(argv[1], "legacy"))
        return CheckLegacy();

    return 3;
}
