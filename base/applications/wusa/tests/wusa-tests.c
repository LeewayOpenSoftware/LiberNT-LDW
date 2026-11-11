/*
 * PROJECT:     LiberNT Update - wusa host tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Argument parsing, KB/id helpers and the container view of wusa.exe
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * usage: wusa-tests <KBxxxx.exe fixture>
 */
#include "../wusa.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int g_checks, g_failed;
#define CHECK(c) do { g_checks++; if (!(c)) { g_failed++; printf("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static DWORD P(WUSA_ARGS *a, int n, ...)
{
    WCHAR *argv[16];
    va_list ap;
    int i;

    argv[0] = L"wusa.exe";
    va_start(ap, n);
    for (i = 1; i <= n; i++)
        argv[i] = va_arg(ap, WCHAR *);
    va_end(ap);
    return WusaParseArgs(n + 1, argv, a);
}

int wmain(int argc, WCHAR **argv)
{
    WUSA_ARGS a;
    WCHAR kb[24];
    char ver[80];
    WUSA_IDENT id;
    const char *q;

    /* install */
    CHECK(P(&a, 1, L"KB1.exe") == 0 && a.mode == WusaInstall && a.hasFile && !wcscmp(a.file, L"KB1.exe") && !a.quiet);
    CHECK(P(&a, 3, L"KB1.exe", L"/quiet", L"/norestart") == 0 && a.quiet && a.norestart && !a.forcerestart);
    CHECK(P(&a, 3, L"/QUIET", L"KB1.exe", L"-NoRestart") == 0 && a.quiet && a.norestart && a.mode == WusaInstall);
    CHECK(P(&a, 2, L"KB1.exe", L"/forcerestart") == 0 && a.forcerestart && !a.norestart);
    CHECK(P(&a, 2, L"KB1.exe", L"/promptrestart") == 0 && !a.forcerestart && !a.norestart);
    CHECK(P(&a, 2, L"KB1.exe", L"/log:c:\\x.log") == 0);
    CHECK(P(&a, 2, L"/q", L"KB1.exe") == 0 && a.quiet);
    /* extract */
    CHECK(P(&a, 2, L"KB1.exe", L"/extract:c:\\out") == 0 && a.mode == WusaExtract && !wcscmp(a.extractDir, L"c:\\out"));
    CHECK(P(&a, 2, L"KB1.exe", L"/extract:") == WUSA_E_USAGE);
    CHECK(P(&a, 1, L"/extract:c:\\out") == WUSA_E_USAGE);                      /* needs the file */
    /* uninstall */
    CHECK(P(&a, 2, L"/uninstall", L"KB1.exe") == 0 && a.mode == WusaUninstall && a.hasFile);
    CHECK(P(&a, 2, L"/uninstall", L"/kb:KB2975719") == 0 && a.mode == WusaUninstall && !a.hasFile && !wcscmp(a.kb, L"KB2975719"));
    CHECK(P(&a, 2, L"/uninstall", L"/kb:2975719") == 0 && !wcscmp(a.kb, L"KB2975719"));
    CHECK(P(&a, 3, L"/uninstall", L"/kb:kb12", L"/quiet") == 0 && !wcscmp(a.kb, L"KB12") && a.quiet);
    CHECK(P(&a, 1, L"/uninstall") == WUSA_E_USAGE);
    CHECK(P(&a, 2, L"/kb:12", L"KB1.exe") == WUSA_E_USAGE);                    /* /kb only with /uninstall */
    CHECK(P(&a, 2, L"/uninstall", L"/kb:12x") == WUSA_E_USAGE);
    CHECK(P(&a, 2, L"/uninstall", L"/kb:") == WUSA_E_USAGE);
    CHECK(P(&a, 3, L"/uninstall", L"KB1.exe", L"/extract:c:\\o") == WUSA_E_USAGE);
    /* usage errors */
    CHECK(P(&a, 0) == WUSA_E_USAGE);
    CHECK(P(&a, 1, L"/quiet") == WUSA_E_USAGE);
    CHECK(P(&a, 2, L"KB1.exe", L"KB2.exe") == WUSA_E_USAGE);
    CHECK(P(&a, 2, L"KB1.exe", L"/bogus") == WUSA_E_USAGE);
    CHECK(P(&a, 3, L"KB1.exe", L"/norestart", L"/forcerestart") == WUSA_E_USAGE);

    /* KB normalization */
    CHECK(WusaNormalizeKb(L"123", kb, 24) && !wcscmp(kb, L"KB123"));
    CHECK(WusaNormalizeKb(L"Kb9999001", kb, 24) && !wcscmp(kb, L"KB9999001"));
    CHECK(!WusaNormalizeKb(L"KB", kb, 24) && !WusaNormalizeKb(L"", kb, 24) && !WusaNormalizeKb(L"12 3", kb, 24));
    CHECK(!WusaNormalizeKb(L"1234567890123", kb, 24) && !WusaNormalizeKb(L"KB1;calc", kb, 24));

    /* ids and versions that reach a command line */
    CHECK(WusaValidToken("KB9999001") && WusaValidToken("ln.fixture") && WusaValidToken("1.0-rc_2"));
    CHECK(!WusaValidToken("") && !WusaValidToken("a b") && !WusaValidToken("a\"b") && !WusaValidToken("a&b") && !WusaValidToken("a|b"));

    /* `libernt-update query` output */
    q = "target: image\nidentity: x\ntrust anchor: sha256 ab\npackages: 3\n  ln.fixture 1.0 : staged (not applied)\n  KB9999001 1.0 : confirmed\n  KB9999001 2.0 : applied\n";
    CHECK(WusaFindInstalled(q, "KB9999001", ver, sizeof(ver)) && !strcmp(ver, "2.0"));
    CHECK(WusaFindInstalled(q, "kb9999001", ver, sizeof(ver)) && !strcmp(ver, "2.0"));
    CHECK(WusaFindInstalled(q, "ln.fixture", ver, sizeof(ver)) && !strcmp(ver, "1.0"));
    CHECK(!WusaFindInstalled(q, "KB999900", ver, sizeof(ver)));                 /* no prefix match */
    CHECK(!WusaFindInstalled(q, "KB1", ver, sizeof(ver)));
    CHECK(!WusaFindInstalled("", "KB1", ver, sizeof(ver)));

    /* the container view of a fixture KB */
    if (argc > 1)
    {
        CHECK(WusaOpenContainer(argv[1]));
        CHECK(WusaReadIdentity(&id));
        CHECK(!strcmp(id.id, "KB9999001") && !strcmp(id.version, "1.0"));
        CHECK(!strcmp(id.kb, "KB9999001") && id.title[0] && id.description[0]);
        WusaCloseContainer();
        CHECK(!WusaOpenContainer(L"wusa-tests.c"));                            /* not a container */
        CHECK(!WusaOpenContainer(L"no-such-file.exe"));
    }
    printf("wusa tests: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
