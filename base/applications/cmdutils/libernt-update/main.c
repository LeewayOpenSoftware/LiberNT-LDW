/*
 * PROJECT:     LiberNT Update
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     libernt-update.exe: command line over libservicing (stage 1 core)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Usage: libernt-update [--image <root>] <command> [arguments]
 *   init [--trust-cer <file.cer>]     create the servicing tree; install the trusted dev certificate
 *   query                              target, trust anchor, packages and their state
 *   verify <package-dir>               enforce verification only; writes nothing
 *   add-package <package-dir>          verify (enforce) and stage into the store
 *   apply <id> <version> [--confirm]   apply a staged package (idempotent)
 *   confirm <id> <version>             re-check every final hash, then confirm health
 *   rollback <id> <version>            restore from the backup
 *   remove-package <id> <version>      roll back if installed, then drop from the store
 *   pending                            pending plan, boot attempts and the rollback decision
 *   export-log <dest-dir>              copy journal, state, plan, attempt and logs
 * Exit code: 0 success, 1 usage, otherwise the error code (Win32 or 0xE00000xx).
 */

#include "../../../system/servicing/libservicing/libservicing.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static void Usage(void)
{
    fputs("usage: libernt-update [--image <root>] <command> [arguments]\n"
          "  init [--trust-cer <file.cer>] | query | verify <dir> | add-package <dir>\n"
          "  add-package --from <url>   fetch the package over HTTP, then verify and stage it\n"
          "  add-package --exe <file>   verify and stage a standalone package .exe (its signed contents)\n"
          "  apply <id> <version> [--confirm] | confirm <id> <version> | rollback <id> <version>\n"
          "  remove-package <id> <version> | pending | export-log <dest-dir>\n", stderr);
}

static const char *ErrName(DWORD e)
{
    switch (e)
    {
        case LN_E_MANIFEST_INVALID: return "manifest-invalid";
        case LN_E_HASH_MISMATCH: return "hash-mismatch";
        case LN_E_UNSIGNED: return "unsigned";
        case LN_E_CATALOG_INVALID: return "catalog-invalid";
        case LN_E_BAD_SIGNATURE: return "bad-manifest-signature";
        case LN_E_INSECURE_TARGET: return "insecure-servicing-tree";
        case LN_E_CERT_MISMATCH: return "certificate-mismatch";
        case LN_E_JOURNAL_CORRUPT: return "journal-corrupt";
        case LN_E_PRECONDITION: return "precondition-failed";
        case LN_E_NOT_FOUND: return "not-found";
        case LN_E_FORBIDDEN_TARGET: return "forbidden-target";
        case LN_E_BAD_STATE: return "bad-state";
        case LN_E_VERIFY_FINAL: return "final-hash-mismatch";
        case LN_E_NO_TRUST_ANCHOR: return "no-trust-anchor";
        case LN_E_NO_BOOT_APPLIER: return "boot-updater-not-installed";
        case LN_E_PACKAGE_INVALID: return "package-exe-damaged";
        case LN_E_NOT_APPLICABLE: return "not-applicable";
        case LN_E_SUPERSEDED: return "superseded";
        default: return "win32-error";
    }
}

static int Fail(const char *what, DWORD err)
{
    fprintf(stderr, "libernt-update: %s failed: %s (0x%08lx)\n", what, ErrName(err), (unsigned long)err);
    return (int)(err ? err : 1);
}

static char *U8(const WCHAR *w)
{
    return LnWideToUtf8(w);
}

static int CmdInit(LN_TARGET *t, int argc, WCHAR **argv)
{
    int i;

    for (i = 0; i < argc; i++)
    {
        if (!wcscmp(argv[i], L"--trust-cer") && i + 1 < argc)
        {
            char h[65];
            WCHAR *dst = LnTargetPath(t, L"trust\\Leeway-LiberNT-Update-DEV.cer");
            DWORD err = LnCertFileSha256(argv[i + 1], h);

            if (!err && dst)
                err = LnCopyFileFlush(argv[i + 1], dst);
            free(dst);
            if (err)
                return Fail("install trust certificate", err);
            printf("trust certificate installed, sha256 %s\n", h);
            i++;
        }
        else
            return (Usage(), 1);
    }
    printf("target %s, identity %s\n", t->mode == LnTargetOnline ? "online" : "image", t->identity);
    return 0;
}

static int CmdQuery(const LN_TARGET *t)
{
    char (*ids)[130];
    size_t n, i;
    LN_JOURNAL j;
    DWORD err;
    WCHAR *cer = LnTargetPath(t, L"trust\\Leeway-LiberNT-Update-DEV.cer");
    char h[65];

    printf("target: %s\nidentity: %s\n", t->mode == LnTargetOnline ? "online" : "image", t->identity);
    if (cer && !LnCertFileSha256(cer, h))
        printf("trust anchor: sha256 %s\n", h);
    else
        printf("trust anchor: none (run init --trust-cer)\n");
    free(cer);
    err = LnStoreList(t, &ids, &n);
    if (err)
        return Fail("list store", err);
    err = LnJournalRead(t, &j);
    if (err)
        return Fail("read journal", err);
    if (j.tailTruncated)
        printf("journal: a torn last line was ignored\n");
    printf("packages: %u\n", (unsigned)n);
    for (i = 0; i < n; i++)
    {
        char id[65], ver[65], txid[65];
        LN_MANIFEST m;
        char *bs = strchr(ids[i], '\\');
        const LN_JENTRY *last = NULL;

        if (!bs)
            continue;
        memcpy(id, ids[i], (size_t)(bs - ids[i]));
        id[bs - ids[i]] = 0;
        strcpy(ver, bs + 1);
        if (!LnStoreLoadManifest(t, id, ver, &m))
        {
            LnTransactionId(t->identity, id, ver, m.manifestSha256, "install", txid);
            last = LnJournalLast(&j, txid);
            LnManifestFree(&m);
        }
        printf("  %s %s : %s\n", id, ver, last ? last->phase : "staged (not applied)");
    }
    LnJournalFree(&j);
    free(ids);
    return 0;
}

static int CmdPending(const LN_TARGET *t)
{
    char *plan;
    LN_ATTEMPT a;
    DWORD err = LnServicingReadPendingPlan(t, &plan);

    if (err == LN_E_NOT_FOUND)
    {
        puts("no pending plan");
        return 0;
    }
    if (err)
        return Fail("read plan", err);
    printf("plan:\n%s", plan);
    err = LnServicingValidatePlan(t, plan);
    if (err)
        printf("plan validation: REJECTED (%s, 0x%08lx): it does not match the signed package in the store\n", ErrName(err), (unsigned long)err);
    else
        puts("plan validation: matches the signed package in the store");
    free(plan);
    err = LnServicingReadAttempt(t, &a);
    if (err == LN_E_NOT_FOUND)
        puts("attempt: none (the boot applier has not started yet)");
    else if (err)
        return Fail("read attempt", err);
    else
    {
        const char *why;
        BOOL rb = LnServicingShouldRollback(&a, &why);

        printf("attempt: %lld, last result %s, failure recorded %s, health %s\n", a.attempt, a.lastResult,
               a.failureRecorded ? "yes" : "no", a.healthConfirmed ? "confirmed" : "not confirmed");
        printf("rollback decision at the next boot: %s (%s)\n", rb ? "ROLL BACK" : "keep", why);
    }
    return 0;
}

int wmain(int argc, WCHAR **argv)
{
    LN_TARGET t;
    LN_TARGET_MODE mode = LnTargetOnline;
    const WCHAR *image = NULL;
    const WCHAR *cmd;
    DWORD err;
#ifdef LN_TESTING
    {
        /* host tests only (the CLI is compiled with -DLN_TESTING by Build-And-Test.ps1): accept the account
         * running the test next to SYSTEM/Administrators, as libservicing-tests does, so an Image target can
         * be driven unelevated */
        HANDLE tok;
        DWORD need = 0;
        TOKEN_USER *tu;

        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
        {
            GetTokenInformation(tok, TokenUser, NULL, 0, &need);
            tu = malloc(need);
            if (tu && GetTokenInformation(tok, TokenUser, tu, need, &need))
                LnTestExtraSid = tu->User.Sid;
            CloseHandle(tok);
        }
    }
#endif
    int a = 1;
    char *id = NULL, *ver = NULL;
    int rc = 0;

    if (a + 1 < argc && !wcscmp(argv[a], L"--image"))
    {
        mode = LnTargetImage;
        image = argv[a + 1];
        a += 2;
    }
    if (a >= argc)
        return (Usage(), 1);
    cmd = argv[a++];

    err = LnTargetOpen(&t, mode, image, !wcscmp(cmd, L"init") || !wcscmp(cmd, L"add-package"));
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
    {
        if (!wcscmp(cmd, L"verify"))
        {
            memset(&t, 0, sizeof(t));                   /* verify needs no initialized target */
            err = 0;
        }
        else
        {
            fputs("libernt-update: the target has no servicing tree; run init first\n", stderr);
            return 2;
        }
    }
    if (err)
        return Fail("open target", err);

    if (!wcscmp(cmd, L"init"))
        return CmdInit(&t, argc - a, argv + a);
    if (!wcscmp(cmd, L"query"))
        return CmdQuery(&t);
    if (!wcscmp(cmd, L"pending"))
        return CmdPending(&t);

    if (!wcscmp(cmd, L"verify") || !wcscmp(cmd, L"add-package"))
    {
        char txid[65];

        if (a >= argc)
            return (Usage(), 1);
        if (!wcscmp(cmd, L"verify"))
        {
            WCHAR *cer = NULL;
            const WCHAR *cerPath = NULL;
            LN_MANIFEST m;

            /* verify against the certificate named by --trust-cer, or the one installed in the target */
            if (a + 2 < argc && !wcscmp(argv[a + 1], L"--trust-cer"))
                cerPath = argv[a + 2];
            else if (t.servicing[0])
            {
                cer = LnTargetPath(&t, L"trust\\Leeway-LiberNT-Update-DEV.cer");
                cerPath = cer;
            }
            err = cerPath ? LnServicingVerifyPackage(argv[a], cerPath, &m) : LN_E_NO_TRUST_ANCHOR;
            free(cer);
            if (err)
                return Fail("verify", err);
            printf("verified: %s %s (%u payloads), manifest.sig valid for the pinned certificate\n", m.packageId, m.releaseVersion, (unsigned)m.itemCount);
            LnManifestFree(&m);
            return 0;
        }
        if (a + 1 < argc && !wcscmp(argv[a], L"--exe"))
        {
            err = LnServicingAddPackageExe(&t, argv[a + 1], txid);
            if (err)
                return Fail("add-package --exe", err);
            printf("staged, transaction %s\n", txid);
            return 0;
        }
        if (a + 1 < argc && !wcscmp(argv[a], L"--from"))
        {
            const WCHAR *url = argv[a + 1];
            WCHAR tmpBase[MAX_PATH], tmpDir[MAX_PATH];
            UINT unique;

            if (GetTempPathW(MAX_PATH, tmpBase) == 0)
                return Fail("fetch", GetLastError());
            unique = GetTempFileNameW(tmpBase, L"lnup", 0, tmpDir);
            if (unique == 0)
                return Fail("fetch", GetLastError());
            DeleteFileW(tmpDir);                    /* GetTempFileName made a file; we want a dir */
            err = LnEnsureDir(tmpDir);
            if (err)
                return Fail("fetch", err);
            err = LnFetchPackage(&t, url, tmpDir);
            if (err == ERROR_SUCCESS)
                err = LnServicingAddPackage(&t, tmpDir, txid);
            LnRemoveTree(tmpDir);
            if (err)
                return Fail(err == ERROR_INVALID_DATA ? "fetch (bad URL or HTTP status)" : "add-package --from", err);
            printf("staged, transaction %s\n", txid);
            return 0;
        }
        err = LnServicingAddPackage(&t, argv[a], txid);
        if (err)
            return Fail("add-package", err);
        printf("staged, transaction %s\n", txid);
        return 0;
    }

    if (!wcscmp(cmd, L"export-log"))
    {
        if (a >= argc)
            return (Usage(), 1);
        err = LnServicingExportLog(&t, argv[a]);
        return err ? Fail("export-log", err) : 0;
    }

    /* the remaining commands take <id> <version> */
    if (a + 1 >= argc)
        return (Usage(), 1);
    id = U8(argv[a]);
    ver = U8(argv[a + 1]);
    if (!id || !ver)
        return Fail("arguments", ERROR_INVALID_PARAMETER);

    if (!wcscmp(cmd, L"apply"))
    {
        BOOL reboot = FALSE, confirm = (a + 2 < argc && !wcscmp(argv[a + 2], L"--confirm"));
        char txid[65];

        err = LnServicingApplyPackage(&t, id, ver, &reboot, txid);
        if (err)
            rc = Fail("apply", err);
        else if (reboot)
            printf("pending-first-boot: restart to finish (transaction %s)\n", txid);
        else
        {
            printf("applied, every final hash verified (transaction %s)\n", txid);
            if (confirm)
            {
                err = LnServicingConfirmHealth(&t, id, ver);
                if (err)
                    rc = Fail("confirm", err);
                else
                    puts("confirmed");
            }
        }
    }
    else if (!wcscmp(cmd, L"confirm"))
    {
        err = LnServicingConfirmHealth(&t, id, ver);
        if (err)
            rc = Fail("confirm", err);
        else
            puts("confirmed");
    }
    else if (!wcscmp(cmd, L"rollback"))
    {
        char txid[65];

        err = LnServicingRollback(&t, id, ver, txid);
        if (err)
            rc = Fail("rollback", err);
        else
            printf("rolled back (transaction %s)\n", txid);
    }
    else if (!wcscmp(cmd, L"remove-package"))
    {
        err = LnServicingRemovePackage(&t, id, ver);
        if (err)
            rc = Fail("remove-package", err);
        else
            puts("removed");
    }
    else
        rc = (Usage(), 1);
    free(id);
    free(ver);
    return rc;
}
