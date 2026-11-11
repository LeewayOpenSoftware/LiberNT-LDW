/*
 * PROJECT:     LiberNT Update - libservicing
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Transactions: add-package, apply, rollback, remove-package, confirm, export
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Rules enforced here (owner conditions, DESIGN-v0):
 *  - verification is ENFORCE and happens before anything reaches the store or the target;
 *  - every state transition is journaled (append-only, flushed) before the next step;
 *  - a backup of every target is taken and verified BEFORE the first change, and undo.json
 *    lists it, so a rollback needs only the backup files and this code (not the updated
 *    components);
 *  - nothing is reported applied before the final hash of the target was checked;
 *  - "confirmed" is only written by LnServicingConfirmHealth, after re-checking every hash;
 *  - the same request is idempotent: it resumes or returns the existing result;
 *  - user components and drivers only; the kernel and the boot path are refused (target.c).
 */

#include "libservicing.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define CER_NAME L"trust\\Leeway-LiberNT-Update-DEV.cer"

#ifdef LN_TESTING
BOOL LnTestLockedAsPending = FALSE;
void (*LnTestAfterStageHook)(const WCHAR *sourcePackageDir) = NULL;
#define TEST_LOCKED_AS_PENDING LnTestLockedAsPending
#else
#define TEST_LOCKED_AS_PENDING FALSE
#endif

typedef struct _CTX
{
    const LN_TARGET *t;
    LN_MANIFEST m;
    char txid[65];
    const char *op;
} CTX;

/* ---- journal + log helpers --------------------------------------------- */

static void LogLine(const LN_TARGET *t, const char *txid, const char *text)
{
    WCHAR *wid = LnUtf8ToWide(txid), rel[160], *path;
    HANDLE h;
    char ts[32];
    LN_BUF b;
    DWORD wrote;

    if (!wid)
        return;
    wcscpy(rel, L"logs\\");
    wcscat(rel, wid);
    wcscat(rel, L".log");
    free(wid);
    path = LnTargetPath(t, rel);
    if (!path)
        return;
    {
        WCHAR dir[MAX_PATH], *sl;

        wcscpy(dir, path);
        sl = wcsrchr(dir, L'\\');
        if (sl)
            *sl = 0;
        LnEnsureDir(dir);
    }
    LnNowUtc(ts);
    LnBufInit(&b);
    LnBufAppendStr(&b, ts);
    LnBufAppendStr(&b, " ");
    LnBufAppendStr(&b, text);
    LnBufAppendStr(&b, "\r\n");
    h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE)
    {
        WriteFile(h, b.data, (DWORD)b.len, &wrote, NULL);
        FlushFileBuffers(h);
        CloseHandle(h);
    }
    LnBufFree(&b);
    free(path);
}

static DWORD JournalOp(const CTX *c, const char *phase, const char *result, long long op, const char *error)
{
    LN_JENTRY e;
    DWORD err;

    memset(&e, 0, sizeof(e));
    strcpy(e.transactionId, c->txid);
    strncpy(e.phase, phase, sizeof(e.phase) - 1);
    strncpy(e.result, result, sizeof(e.result) - 1);
    e.op = op;
    if (error)
        strncpy(e.error, error, sizeof(e.error) - 1);
    err = LnJournalAppend(c->t, &e);
    if (!err)
        err = LnJournalWriteState(c->t);
    {
        char line[260];

        strcpy(line, phase);
        strcat(line, " ");
        strcat(line, result);
        strcat(line, " ");
        strncat(line, error ? error : "", 160);
        LogLine(c->t, c->txid, line);
    }
    return err;
}

/* transaction-level event (op = null) */
static DWORD Journal(const CTX *c, const char *phase, const char *result, const char *error)
{
    return JournalOp(c, phase, result, -1, error);
}

/* journal.error: the hex code of the failure */
static const char *ErrText(DWORD err, char *buf)
{
    static const char hex[] = "0123456789ABCDEF";
    int i;

    strcpy(buf, "0x");
    for (i = 0; i < 8; i++)
        buf[2 + i] = hex[(err >> (28 - 4 * i)) & 15];
    buf[10] = 0;
    return buf;
}

/* One transaction per (machine, package, version, manifest digest, "install"): add-package,
 * apply, rollback and confirm are phases of it (S1-CONTRACTS.md sections 3 and 4). */
static DWORD CtxInit(CTX *c, const LN_TARGET *t, const char *id, const char *ver, const char *op)
{
    DWORD err;

    (void)op;
    memset(c, 0, sizeof(*c));
    c->t = t;
    c->op = "install";
    err = LnStoreLoadManifest(t, id, ver, &c->m);
    if (!err)
        LnTransactionId(t->identity, c->m.packageId, c->m.releaseVersion, c->m.manifestSha256, c->op, c->txid);
    return err;
}

static const LN_JENTRY *LastOf(const LN_JOURNAL *j, const char *txid)
{
    return LnJournalLast(j, txid);
}

/* ---- trust anchor ------------------------------------------------------ */

DWORD LnPinnedCertSha256(const LN_TARGET *t, char hash[65])
{
    WCHAR *p = LnTargetPath(t, CER_NAME);
    DWORD err;

    if (!p)
        return ERROR_NOT_ENOUGH_MEMORY;
    err = LnCertFileSha256(p, hash);
    free(p);
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
        return LN_E_NO_TRUST_ANCHOR;
    return err;
}

#define PinnedCert LnPinnedCertSha256

static DWORD VerifyStagedCopy(const LN_TARGET *t, const WCHAR *stage)
{
    WCHAR *cer = LnTargetPath(t, CER_NAME);
    DWORD err = cer ? LnServicingVerifyPackage(stage, cer, NULL) : ERROR_NOT_ENOUGH_MEMORY;

    free(cer);
    return err;
}

/* ---- add-package ------------------------------------------------------- */

/* Copies the package into a private staging folder (inside the protected servicing tree). What the
 * caller names (a user folder, removable media, a share) is read exactly once; everything after
 * this is done on the copy, so changing the source between verify and import changes nothing. */
static DWORD StagePackage(const LN_TARGET *t, const WCHAR *pkgDir, WCHAR **stageDir)
{
    WCHAR leaf[48], *base, *src = NULL, *dst = NULL;
    LARGE_INTEGER pc;
    static const WCHAR *fixed[] = { L"manifest.json", L"manifest.sig", L"catalog.cat" };
    LN_MANIFEST m;
    char num[24];
    DWORD err = ERROR_SUCCESS;
    size_t i, k;

    *stageDir = NULL;
    QueryPerformanceCounter(&pc);
    LnI64ToStr(pc.QuadPart, num);
    wcscpy(leaf, L"staging\\add-");
    for (k = 0; num[k] && k < 30; k++)
    {
        WCHAR one[2] = { (WCHAR)num[k], 0 };

        wcscat(leaf, one);
    }
    base = LnTargetPath(t, leaf);
    if (!base)
        return ERROR_NOT_ENOUGH_MEMORY;
    err = LnEnsureDir(base);
    for (i = 0; !err && i < sizeof(fixed) / sizeof(fixed[0]); i++)
    {
        src = LnJoin(pkgDir, fixed[i]);
        dst = LnJoin(base, fixed[i]);
        if (!src || !dst)
            err = ERROR_NOT_ENOUGH_MEMORY;
        else if (LnPathExists(src))
            err = LnCopyFileFlush(src, dst);
        else if (i == 0)
            err = ERROR_FILE_NOT_FOUND;                  /* a package without a manifest */
        free(src);
        free(dst);
    }
    if (!err)
    {
        WCHAR *mp = LnJoin(base, L"manifest.json");

        err = mp ? LnManifestLoad(mp, &m) : ERROR_NOT_ENOUGH_MEMORY;     /* the COPY decides the file list */
        free(mp);
        if (!err)
        {
            WCHAR *sp = LnJoin(pkgDir, L"payload"), *dp = LnJoin(base, L"payload");

            for (i = 0; !err && sp && dp && i < m.itemCount; i++)
            {
                if (m.items[i].isDelete)
                    continue;
                src = LnJoin(sp, m.items[i].path);
                dst = LnJoin(dp, m.items[i].path);
                err = (src && dst) ? LnCopyFileFlush(src, dst) : ERROR_NOT_ENOUGH_MEMORY;
                if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
                    err = LN_E_HASH_MISMATCH;            /* declared but missing */
                free(src);
                free(dst);
            }
            free(sp);
            free(dp);
            LnManifestFree(&m);
        }
    }
    if (err)
        LnRemoveTree(base);
    else
        *stageDir = base;
    if (err)
        free(base);
    return err;
}

DWORD LnServicingAddPackage(const LN_TARGET *t, const WCHAR *pkgDir, char txId[65])
{
    CTX c;
    LN_JOURNAL j;
    const LN_JENTRY *last;
    WCHAR *stage = NULL, *mp;
    char pinned[65], buf[48];
    DWORD err;

    if (txId)
        txId[0] = 0;
    err = StagePackage(t, pkgDir, &stage);
    if (err)
        return err;
#ifdef LN_TESTING
    if (LnTestAfterStageHook)
        LnTestAfterStageHook(pkgDir);                    /* tests change the SOURCE here: it must not matter */
#endif

    mp = LnJoin(stage, L"manifest.json");
    if (!mp)
    {
        LnRemoveTree(stage);
        free(stage);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    memset(&c, 0, sizeof(c));
    c.t = t;
    c.op = "install";
    err = LnManifestLoad(mp, &c.m);
    free(mp);
    if (err)
        goto cleanup_nomanifest;                         /* no digest to journal against */
    LnTransactionId(t->identity, c.m.packageId, c.m.releaseVersion, c.m.manifestSha256, c.op, c.txid);
    if (txId)
        strcpy(txId, c.txid);

    err = PinnedCert(t, pinned);
    if (!err)
        err = VerifyStagedCopy(t, stage);                            /* only the private copy is verified */
    if (err)
    {
        Journal(&c, "failed", "error", ErrText(err, buf));              /* rejected before staging */
        goto cleanup;
    }
    {
        char why[200];

        err = LnPackageCheckApplicability(t, &c.m, why);        /* requires / supersedes against the store */
        if (err)
        {
            if (why[0])
                LogLine(t, c.txid, why);
            Journal(&c, "failed", "error", ErrText(err, buf));
            goto cleanup;
        }
    }

    err = LnJournalRead(t, &j);
    if (err)
        goto cleanup;
    last = LastOf(&j, c.txid);
    if (last && strcmp(last->phase, "failed"))
    {
        LN_MANIFEST stored;

        /* already verified/staged (or further): make sure the store still has it, else re-import */
        if (!LnStoreLoadManifest(t, c.m.packageId, c.m.releaseVersion, &stored) &&
            !strcmp(stored.manifestSha256, c.m.manifestSha256))
        {
            LnManifestFree(&stored);
            LnJournalFree(&j);
            goto cleanup;
        }
    }
    LnJournalFree(&j);

    err = Journal(&c, "verified", "ok", NULL);
    if (!err)
        err = LnStoreImport(t, stage, &c.m);
    if (!err)
    {
        /* what was stored is what was verified */
        LN_MANIFEST stored;

        err = LnStoreLoadManifest(t, c.m.packageId, c.m.releaseVersion, &stored);
        if (!err)
        {
            if (strcmp(stored.manifestSha256, c.m.manifestSha256))
                err = LN_E_HASH_MISMATCH;
            LnManifestFree(&stored);
        }
    }
    if (err)
        Journal(&c, "failed", "error", ErrText(err, buf));
    else
        err = Journal(&c, "staged", "ok", NULL);
cleanup:
    LnManifestFree(&c.m);
cleanup_nomanifest:
    LnRemoveTree(stage);
    free(stage);
    return err;
}

/* ---- backup / undo ------------------------------------------------------ */

typedef struct _UNDO
{
    char oldSha[65];            /* "" when the target did not exist */
} UNDO;

static WCHAR *BackupDir(const CTX *c)
{
    WCHAR *w = LnUtf8ToWide(c->txid), rel[160], *p;

    if (!w)
        return NULL;
    wcscpy(rel, L"backup\\");
    wcscat(rel, w);
    free(w);
    p = LnTargetPath(c->t, rel);
    return p;
}

static WCHAR *BackupFile(const CTX *c, size_t i)
{
    WCHAR *dir = BackupDir(c), name[16], *p;
    char num[24];
    size_t k, n;

    if (!dir)
        return NULL;
    LnI64ToStr((long long)(i + 1), num);                /* seq is 1-based, 4 digits */
    n = strlen(num);
    for (k = 0; k < 4; k++)
        name[k] = (n + k >= 4) ? (WCHAR)num[n + k - 4] : L'0';
    name[4] = 0;
    p = LnJoin(dir, name);
    free(dir);
    return p;
}

static DWORD WriteUndo(const CTX *c, const UNDO *u)
{
    LN_BUF b;
    WCHAR *dir = BackupDir(c), *path;
    size_t i;
    DWORD err = 0;

    if (!dir)
        return ERROR_NOT_ENOUGH_MEMORY;
    path = LnJoin(dir, L"undo.json");
    free(dir);
    if (!path)
        return ERROR_NOT_ENOUGH_MEMORY;
    LnBufInit(&b);
    err |= LnBufAppendStr(&b, "{\"schemaVersion\":1,\"transactionId\":");
    err |= LnBufAppendJsonString(&b, c->txid);
    err |= LnBufAppendStr(&b, ",\"items\":[");
    for (i = 0; i < c->m.itemCount; i++)
    {
        char *tg = LnWideToUtf8(c->m.items[i].target);

        if (i)
            err |= LnBufAppendStr(&b, ",");
        err |= LnBufAppendStr(&b, "{\"target\":");
        err |= LnBufAppendJsonString(&b, tg ? tg : "");
        err |= LnBufAppendStr(&b, ",\"oldSha256\":");
        err |= LnBufAppendJsonString(&b, u[i].oldSha);
        err |= LnBufAppendStr(&b, "}");
        free(tg);
    }
    err |= LnBufAppendStr(&b, "]}\n");
    if (!err)
        err = LnWriteFileAtomic(path, b.data, b.len);
    else
        err = ERROR_NOT_ENOUGH_MEMORY;
    LnBufFree(&b);
    free(path);
    return err;
}

/* Reads undo.json back; the targets must match the manifest of this transaction. */
static DWORD ReadUndo(const CTX *c, UNDO *u)
{
    WCHAR *dir = BackupDir(c), *path;
    BYTE *data;
    size_t len, i = 0;
    LN_JSON *root;
    const LN_JSON *items, *it;
    DWORD err;

    if (!dir)
        return ERROR_NOT_ENOUGH_MEMORY;
    path = LnJoin(dir, L"undo.json");
    free(dir);
    if (!path)
        return ERROR_NOT_ENOUGH_MEMORY;
    err = LnReadFileAll(path, &data, &len);
    free(path);
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
        return LN_E_NOT_FOUND;
    if (err)
        return err;
    err = LnJsonParse((const char *)data, len, &root);
    free(data);
    if (err)
        return LN_E_MANIFEST_INVALID;
    items = LnJsonGet(root, "items");
    err = LN_E_MANIFEST_INVALID;
    if (items && items->type == LnJsonArray)
    {
        err = ERROR_SUCCESS;
        for (it = items->child; it && i < c->m.itemCount; it = it->next, i++)
        {
            const char *tg = LnJsonGetString(it, "target"), *old = LnJsonGetString(it, "oldSha256");
            WCHAR *wt = tg ? LnUtf8ToWide(tg) : NULL;

            if (!wt || !old || _wcsicmp(wt, c->m.items[i].target) || (*old && !LnIsSha256Hex(old)))
                err = LN_E_MANIFEST_INVALID;
            else
                strcpy(u[i].oldSha, old);
            free(wt);
        }
        if (it || i != c->m.itemCount)
            err = LN_E_MANIFEST_INVALID;
    }
    LnJsonFree(root);
    return err;
}

/* Hash of the current content of a target; "" when it does not exist. */
static DWORD CurrentHash(const WCHAR *path, char out[65])
{
    DWORD attrs = GetFileAttributesW(path), err;

    out[0] = 0;
    if (attrs == INVALID_FILE_ATTRIBUTES)
    {
        err = GetLastError();
        return (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) ? ERROR_SUCCESS : err;
    }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY)
        return LN_E_PRECONDITION;
    return LnSha256FileHex(path, out);
}

static DWORD TakeBackups(const CTX *c, UNDO *u)
{
    size_t i;
    DWORD err;

    for (i = 0; i < c->m.itemCount; i++)
    {
        WCHAR *tp = LnTargetFile(c->t, c->m.items[i].target);
        WCHAR *bp = BackupFile(c, i);
        char h[65];

        if (!tp || !bp)
        {
            free(tp);
            free(bp);
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        err = CurrentHash(tp, u[i].oldSha);
        if (!err && u[i].oldSha[0])
        {
            err = LnCopyFileFlush(tp, bp);
            if (!err)
                err = LnSha256FileHex(bp, h);
            if (!err && strcmp(h, u[i].oldSha))
                err = LN_E_VERIFY_FINAL;                /* the backup is not the file we saw */
        }
        free(tp);
        free(bp);
        if (err)
            return err;
    }
    return WriteUndo(c, u);
}

/* ---- apply one item ----------------------------------------------------- */

/* Sharing/lock violations and a mapped image are "in use". ACCESS_DENIED is what replacing a running
 * executable returns, but it is also what a real permission problem returns: it counts as "in use"
 * only if the file is not read-only and the temporary copy next to it could be created (so the
 * directory is writable and the denial can only come from the open image). */
static BOOL IsLockedError(DWORD e, const WCHAR *target)
{
    if (e == ERROR_SHARING_VIOLATION || e == ERROR_USER_MAPPED_FILE || e == ERROR_LOCK_VIOLATION)
        return TRUE;
    if (e == ERROR_ACCESS_DENIED)
    {
        DWORD attrs = GetFileAttributesW(target);

        return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_READONLY) &&
               !(attrs & FILE_ATTRIBUTE_DIRECTORY);
    }
    return FALSE;
}

/* A virus scanner or an indexer may hold a freshly written file for a moment: a sharing violation on the
 * replacement is retried a few times before it is treated as "in use". */
static BOOL MoveReplace(const LN_TARGET *t, const WCHAR *from, const WCHAR *to)
{
    /* An offline image has no running programs: a scanner is the only thing that can hold the file, so it
     * is worth waiting longer than on the running system, where a denial usually means the file is in use. */
    int attempt, max = (t->mode == LnTargetImage) ? 50 : 10;

    for (attempt = 0; attempt < max; attempt++)
    {
        DWORD e;

        if (MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return TRUE;
        e = GetLastError();
        if (e != ERROR_SHARING_VIOLATION && e != ERROR_LOCK_VIOLATION && e != ERROR_ACCESS_DENIED &&
            e != ERROR_USER_MAPPED_FILE)
            return FALSE;
        Sleep(100);
    }
    return FALSE;
}

/* Replaces under a system directory never apply inline: the old NTFS replace-rename ignores share
 * semantics, so the lock probe cannot be trusted there and the target may be a mapped image. The
 * built-in roots are the ones the target white list allows; LNUPDATE_SYSTEM_DIRS (semicolon-separated)
 * adds roots in production, and only the test build may replace or empty the list. */
static BOOL
IsUnderSystemDir(const WCHAR *target)
{
    static const WCHAR kDefault[] = L"Windows;Program Files;Program Files (x86)";
    WCHAR env[300], combined[RTL_NUMBER_OF(kDefault) + 300];
    const WCHAR *list, *p;
    size_t firstLen;
    DWORD n;

    while (*target == L'\\')
        target++;
    firstLen = wcscspn(target, L"\\");
    if (!firstLen)
        return FALSE;
    SetLastError(ERROR_SUCCESS);
    n = GetEnvironmentVariableW(L"LNUPDATE_SYSTEM_DIRS", env, RTL_NUMBER_OF(env));
    if (n == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND)
        list = kDefault;                     /* unset */
#ifdef LN_TESTING
    else if (n < RTL_NUMBER_OF(env))
        list = env;                          /* the test build gets exact control, incl. an empty list */
#endif
    else if (n == 0 || n >= RTL_NUMBER_OF(env))
        list = kDefault;                     /* set but empty or overlong: the built-in roots stay */
    else
    {
        size_t dl = wcslen(kDefault), el = wcslen(env);

        if (dl + 1 + el < RTL_NUMBER_OF(combined))
        {
            memcpy(combined, kDefault, dl * sizeof(WCHAR));
            combined[dl] = L';';
            memcpy(combined + dl + 1, env, (el + 1) * sizeof(WCHAR));
            list = combined;
        }
        else
            list = kDefault;
    }
    for (p = list; ; )
    {
        const WCHAR *end = wcschr(p, L';');
        size_t len = end ? (size_t)(end - p) : wcslen(p);

        while (len && (p[0] == L' ' || p[0] == L'\t'))
        {
            p++;
            len--;
        }
        while (len && (p[len - 1] == L' ' || p[len - 1] == L'\t'))
            len--;
        if (len && len == firstLen && !_wcsnicmp(p, target, len))
            return TRUE;
        if (!end)
            break;
        p = end + 1;
    }
    return FALSE;
}

/* *pending is set when the target is in use (Online only): the plan takes it at boot. */
static DWORD ApplyItem(const CTX *c, const LN_ITEM *it, BOOL *pending)
{
    WCHAR *tp = LnTargetFile(c->t, it->target), *tmp = NULL, *obj = NULL;
    char cur[65], fin[65];
    DWORD err;

    *pending = FALSE;
    if (!tp)
        return ERROR_NOT_ENOUGH_MEMORY;
    err = LnTargetValidateReal(c->t, it->target);        /* again, immediately before this write */
    if (err)
        goto out;
    err = CurrentHash(tp, cur);
    if (err)
        goto out;

    if (it->isDelete)
    {
        if (!cur[0])
            goto verify;
        if (!DeleteFileW(tp))
        {
            err = GetLastError();
            if (IsLockedError(err, tp))
                err = LN_E_PRECONDITION;                /* a delete of a file in use is not deferred (stage 1) */
            goto out;
        }
        goto verify;
    }

    if (!strcmp(cur, it->sha256))
        goto verify;                                    /* already in place (resumed transaction) */

    /* only an existing file can be a mapped image: a brand-new file (add-file) applies inline */
    if ((c->t->mode == LnTargetOnline || TEST_LOCKED_AS_PENDING) && cur[0] && IsUnderSystemDir(it->target))
    {
        *pending = TRUE;
        goto out;
    }

    err = LnStoreObjectPath(c->t, it->sha256, &obj);
    if (err)
        goto out;
    {
        size_t n = wcslen(tp) + 24;
        WCHAR suffix[24], *parent, *sl;

        tmp = malloc(n * sizeof(WCHAR));
        if (!tmp)
        {
            err = ERROR_NOT_ENOUGH_MEMORY;
            goto out;
        }
        wcscpy(tmp, tp);
        {
            WCHAR *w = LnUtf8ToWide(c->txid);

            wcscpy(suffix, L".lnupd-");
            if (w)
                wcsncat(suffix, w, 8);
            free(w);
        }
        wcscat(tmp, suffix);
        parent = _wcsdup(tp);
        sl = parent ? wcsrchr(parent, L'\\') : NULL;
        if (sl)
        {
            *sl = 0;
            err = LnEnsureDir(parent);
        }
        free(parent);
        if (err)
            goto out;
    }
    err = LnCopyFileFlush(obj, tmp);
    if (!err)
        err = LnSha256FileHex(tmp, fin);
    if (!err && strcmp(fin, it->sha256))
        err = LN_E_HASH_MISMATCH;
    if (err)
    {
        DeleteFileW(tmp);
        goto out;
    }
    if (!MoveReplace(c->t, tmp, tp))
    {
        err = GetLastError();
        DeleteFileW(tmp);
        if ((c->t->mode == LnTargetOnline || TEST_LOCKED_AS_PENDING) && IsLockedError(err, tp))
        {
            *pending = TRUE;
            err = ERROR_SUCCESS;
        }
        goto out;
    }

verify:
    /* not installed until the final state is proven */
    err = CurrentHash(tp, fin);
    if (!err)
    {
        if (it->isDelete ? fin[0] != 0 : strcmp(fin, it->sha256))
            err = LN_E_VERIFY_FINAL;
    }
out:
    free(tp);
    free(tmp);
    free(obj);
    return err;
}

/* ---- restoring from the backup (rollback) ------------------------------- */

static DWORD RestoreItem(const CTX *c, size_t i, const UNDO *u)
{
    WCHAR *tp = LnTargetFile(c->t, c->m.items[i].target), *bp = NULL, *tmp = NULL;
    char cur[65], fin[65];
    DWORD err;

    if (!tp)
        return ERROR_NOT_ENOUGH_MEMORY;
    err = LnTargetValidateReal(c->t, c->m.items[i].target);
    if (err)
        goto out;
    err = CurrentHash(tp, cur);
    if (err)
        goto out;
    if (!strcmp(cur, u[i].oldSha))
        goto out;                                       /* already as it was */

    if (!u[i].oldSha[0])
    {
        if (!DeleteFileW(tp) && GetLastError() != ERROR_FILE_NOT_FOUND)
            err = GetLastError();
    }
    else
    {
        bp = BackupFile(c, i);
        if (!bp)
        {
            err = ERROR_NOT_ENOUGH_MEMORY;
            goto out;
        }
        err = LnSha256FileHex(bp, fin);
        if (!err && strcmp(fin, u[i].oldSha))
            err = LN_E_VERIFY_FINAL;                    /* the backup itself is damaged */
        if (err)
            goto out;
        tmp = malloc((wcslen(tp) + 16) * sizeof(WCHAR));
        if (!tmp)
        {
            err = ERROR_NOT_ENOUGH_MEMORY;
            goto out;
        }
        wcscpy(tmp, tp);
        wcscat(tmp, L".lnundo");
        err = LnCopyFileFlush(bp, tmp);
        if (!err && !MoveReplace(c->t, tmp, tp))
        {
            err = GetLastError();
            DeleteFileW(tmp);
        }
    }
    if (!err)
    {
        err = CurrentHash(tp, fin);
        if (!err && strcmp(fin, u[i].oldSha))
            err = LN_E_VERIFY_FINAL;
    }
out:
    free(tp);
    free(bp);
    free(tmp);
    return err;
}

static DWORD RemovePlanOf(const CTX *c)
{
    WCHAR *p = LnTargetPath(c->t, L"pending\\plan.json");
    BYTE *data;
    size_t len;
    DWORD err = LN_E_NOT_FOUND;

    if (!p)
        return ERROR_NOT_ENOUGH_MEMORY;
    if (!LnReadFileAll(p, &data, &len))
    {
        LN_JSON *root;

        if (!LnJsonParse((const char *)data, len, &root))
        {
            const char *id = LnJsonGetString(root, "transactionId");

            if (id && !strcmp(id, c->txid))
            {
                DeleteFileW(p);
                err = ERROR_SUCCESS;
            }
            LnJsonFree(root);
        }
        free(data);
    }
    if (err == ERROR_SUCCESS)
    {
        WCHAR *a = LnTargetPath(c->t, L"pending\\attempt.json");
        LN_ATTEMPT rec;

        /* the boot counter belongs to one transaction: a pending plan of another
         * package must keep its record */
        if (a && LnServicingReadAttempt(c->t, &rec) == ERROR_SUCCESS &&
            !strcmp(rec.transactionId, c->txid))
            DeleteFileW(a);
        free(a);
    }
    free(p);
    return err;
}

static DWORD DoRollback(CTX *c, const char *why)
{
    UNDO *u = calloc(c->m.itemCount, sizeof(UNDO));
    size_t i;
    DWORD err, first = ERROR_SUCCESS;
    char buf[48];

    if (!u)
        return ERROR_NOT_ENOUGH_MEMORY;
    err = ReadUndo(c, u);
    if (err == LN_E_NOT_FOUND)
    {
        /* nothing was backed up, so nothing was changed */
        err = ERROR_SUCCESS;
        free(u);
        RemovePlanOf(c);
        return err;
    }
    if (err)
    {
        free(u);
        return err;
    }
    Journal(c, "rollback-prepared", "pending", why);
    for (i = c->m.itemCount; i > 0; i--)
    {
        err = RestoreItem(c, i - 1, u);
        if (err && !first)
            first = err;
    }
    free(u);
    if (first)
    {
        Journal(c, "failed", "error", ErrText(first, buf));
        return first;
    }
    RemovePlanOf(c);
    Journal(c, "rolled-back", "ok", NULL);
    return ERROR_SUCCESS;
}

/* ---- plan.json ---------------------------------------------------------- */

/* A deferred plan is only consumable when the boot applier is installed for this
 * target, otherwise the update waits for a boot that will never apply it. Online:
 * the applier exe sits in the running System32 and BootExecute names it. Image:
 * the offline root is checked for the exe; its BootExecute lives in unmounted
 * hives, and an image that carries the exe came from an installed system. */
/* TRUE when a raw BootExecute value (REG_SZ or REG_MULTI_SZ, maybe unterminated or
 * without the double NUL) lists the LiberNT boot applier. `bytes` is the buffer
 * size in bytes. Each entry is matched by its first token (up to a space), with
 * any path and a trailing ".exe" stripped, case-insensitively. */
BOOL
LnBootExecuteListed(const WCHAR *multiSz, size_t bytes)
{
    const WCHAR *end = (const WCHAR *)((const BYTE *)multiSz + bytes);
    const WCHAR *s;

    for (s = multiSz; s < end && *s; s += wcslen(s) + 1)
    {
        WCHAR token[MAX_PATH], *base;
        size_t len = 0;

        while (s + len < end && s[len] && s[len] != L' ' && len < MAX_PATH - 1)
        {
            token[len] = s[len];
            len++;
        }
        token[len] = 0;
        base = wcsrchr(token, L'\\');
        base = base ? base + 1 : token;
        len = wcslen(base);
        if (len > 4 && _wcsicmp(base + len - 4, L".exe") == 0)
            base[len - 4] = 0;
        if (_wcsicmp(base, L"LiberNTUpdatePending") == 0)
            return TRUE;
    }
    return FALSE;
}

static DWORD
RequireBootApplier(const LN_TARGET *t)
{
    if (t->mode == LnTargetImage)
    {
        WCHAR *p = LnTargetFile(t, L"Windows\\System32\\LiberNTUpdatePending.exe");
        DWORD attrs;

        if (!p)
            return ERROR_NOT_ENOUGH_MEMORY;
        attrs = GetFileAttributesW(p);
        free(p);
        return attrs == INVALID_FILE_ATTRIBUTES ? LN_E_NO_BOOT_APPLIER : ERROR_SUCCESS;
    }
    else
    {
        WCHAR exe[MAX_PATH], buf[4096];
        DWORD n, type, bootExecLen = sizeof(buf) - sizeof(WCHAR);
        HKEY key;
        LONG r;

        n = GetSystemDirectoryW(exe, MAX_PATH - 30);
        if (n == 0)
            return ERROR_NOT_ENOUGH_MEMORY;
        wcscat(exe, L"\\LiberNTUpdatePending.exe");
        if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES)
            return LN_E_NO_BOOT_APPLIER;
        r = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                          L"SYSTEM\\CurrentControlSet\\Control\\Session Manager", 0, KEY_READ, &key);
        if (r != ERROR_SUCCESS)
            return LN_E_NO_BOOT_APPLIER;
        memset(buf, 0, sizeof(buf));
        r = RegQueryValueExW(key, L"BootExecute", NULL, &type, (BYTE *)buf, &bootExecLen);
        RegCloseKey(key);
        if (r != ERROR_SUCCESS || (type != REG_MULTI_SZ && type != REG_SZ))
            return LN_E_NO_BOOT_APPLIER;
        return LnBootExecuteListed(buf, bootExecLen) ? ERROR_SUCCESS : LN_E_NO_BOOT_APPLIER;
    }
}

/* TRUE when the route rule will defer at least one existing replace to the boot
 * plan. Lock-driven defers cannot be predicted here and still surface at the late
 * check before the plan is written. */
static BOOL
RouteRuleWouldDefer(const LN_TARGET *t, const LN_MANIFEST *m)
{
    size_t i;
    BOOL enabled = (t->mode == LnTargetOnline) || TEST_LOCKED_AS_PENDING;

    if (!enabled)
        return FALSE;
    for (i = 0; i < m->itemCount; i++)
    {
        WCHAR *tp;
        BOOL defer;

        if (m->items[i].isDelete || !m->items[i].path)
            continue;
        tp = LnTargetFile(t, m->items[i].target);
        if (!tp)
            continue;
        defer = LnPathExists(tp) && IsUnderSystemDir(m->items[i].target);
        free(tp);
        if (defer)
            return TRUE;
    }
    return FALSE;
}

static void
RemoveAttemptRecord(const LN_TARGET *t, const char *txid)
{
    WCHAR *a = LnTargetPath(t, L"pending\\attempt.json");
    LN_ATTEMPT rec;

    if (!a)
        return;
    /* the record belongs to one transaction; a pending plan of another package
     * keeps its boot counter */
    if (LnServicingReadAttempt(t, &rec) == ERROR_SUCCESS && !strcmp(rec.transactionId, txid))
        DeleteFileW(a);
    free(a);
}

static DWORD WritePlan(const CTX *c, const BOOL *pending)
{
    LN_BUF b;
    WCHAR *path = LnTargetPath(c->t, L"pending\\plan.json");
    size_t i, seq = 0;
    DWORD err = 0;

    if (!path)
        return ERROR_NOT_ENOUGH_MEMORY;
    {
        BYTE *old;
        size_t olen;

        if (!LnReadFileAll(path, &old, &olen))
        {
            LN_JSON *root;
            const char *id = NULL;

            if (!LnJsonParse((const char *)old, olen, &root))
                id = LnJsonGetString(root, "transactionId");
            if (!id || strcmp(id, c->txid))
            {
                LnJsonFree(root);
                free(old);
                free(path);
                return LN_E_BAD_STATE;                  /* another transaction owns the plan */
            }
            LnJsonFree(root);
            free(old);
        }
    }

    LnBufInit(&b);
    err |= LnBufAppendStr(&b, "{\"schema\":1,\"transactionId\":");
    err |= LnBufAppendJsonString(&b, c->txid);
    err |= LnBufAppendStr(&b, ",\"target\":{\"machineId\":");
    err |= LnBufAppendJsonString(&b, c->t->identity);
    err |= LnBufAppendStr(&b, ",\"arch\":");
    err |= LnBufAppendJsonString(&b, c->m.architecture);
    err |= LnBufAppendStr(&b, ",\"line\":\"main-nt10\",\"base\":");
    err |= LnBufAppendJsonString(&b, c->m.baseVersion);
    err |= LnBufAppendStr(&b, "},\"package\":{\"id\":");
    err |= LnBufAppendJsonString(&b, c->m.packageId);
    err |= LnBufAppendStr(&b, ",\"version\":");
    err |= LnBufAppendJsonString(&b, c->m.releaseVersion);
    err |= LnBufAppendStr(&b, ",\"digest\":");
    err |= LnBufAppendJsonString(&b, c->m.manifestSha256);
    err |= LnBufAppendStr(&b, "},\"operation\":\"install\",\"requiresReboot\":true,\"operations\":[");
    for (i = 0; i < c->m.itemCount; i++)
    {
        const LN_ITEM *it = &c->m.items[i];
        WCHAR *wt;
        char *tg, num[24], seqs[8], idx[24];
        WCHAR *tp;
        char oldSha[65];
        size_t k, n;
        LN_BUF tgt;

        if (!pending[i])
            continue;
        if (seq++)
            err |= LnBufAppendStr(&b, ",");
        /* seq of the plan operation = position of the item in the manifest (stable, 1-based) */
        LnI64ToStr((long long)(i + 1), num);
        n = strlen(num);
        for (k = 0; k < 4; k++)
            seqs[k] = (n + k >= 4) ? num[n + k - 4] : '0';
        seqs[4] = 0;
        LnI64ToStr((long long)(i + 1), idx);
        oldSha[0] = 0;
        tp = LnTargetFile(c->t, it->target);
        if (tp)
        {
            DWORD HashErr = CurrentHash(tp, oldSha);

            free(tp);
            if (HashErr != ERROR_SUCCESS)
            {
                /* Fail closed (Pam review H1): a target that exists but
                 * cannot be hashed must abort the plan, not be emitted as
                 * an add-file with no backup — the applier would overwrite
                 * it without an undo and a rollback would DELETE it. */
                LnBufFree(&b);
                free(path);
                return HashErr;
            }
        }
        else
        {
            /* LnTargetFile NULL means OOM or an invalid target path; the
             * item must not fall through as an unhashed add-file either
             * (Pam's residual note on the H1 re-check). */
            LnBufFree(&b);
            free(path);
            return ERROR_NOT_ENOUGH_MEMORY;
        }

        err |= LnBufAppendStr(&b, "{\"seq\":");
        err |= LnBufAppendStr(&b, idx);
        err |= LnBufAppendStr(&b, oldSha[0] ? ",\"type\":\"replace-file\"" : ",\"type\":\"add-file\"");
        err |= LnBufAppendStr(&b, ",\"component\":");
        err |= LnBufAppendJsonString(&b, c->m.packageId);
        err |= LnBufAppendStr(&b, ",\"version\":");
        err |= LnBufAppendJsonString(&b, c->m.releaseVersion);
        err |= LnBufAppendStr(&b, ",\"source\":\"store/objects/");
        err |= LnBufAppendStr(&b, it->sha256);
        err |= LnBufAppendStr(&b, "\",\"target\":");
        wt = malloc((wcslen(it->target) + 2) * sizeof(WCHAR));
        if (wt)
        {
            wcscpy(wt, L"\\");
            wcscat(wt, it->target);
            tg = LnWideToUtf8(wt);
            free(wt);
        }
        else
            tg = NULL;
        LnBufInit(&tgt);
        err |= LnBufAppendJsonString(&b, tg ? tg : "");
        free(tg);
        LnBufFree(&tgt);
        err |= LnBufAppendStr(&b, ",\"sha256\":");
        err |= LnBufAppendJsonString(&b, it->sha256);
        err |= LnBufAppendStr(&b, ",\"backup\":");
        if (oldSha[0])
        {
            char bk[200];

            strcpy(bk, "backup/");
            strcat(bk, c->txid);
            strcat(bk, "/");
            strcat(bk, seqs);
            err |= LnBufAppendJsonString(&b, bk);
            /* REQUIRED (owner decision, fail closed): the boot applier
             * re-hashes the backup against this before restoring. */
            err |= LnBufAppendStr(&b, ",\"originalSha256\":");
            err |= LnBufAppendJsonString(&b, oldSha);
        }
        else
            err |= LnBufAppendStr(&b, "null");
        err |= LnBufAppendStr(&b, "}");
    }
    err |= LnBufAppendStr(&b, "]");
    err |= LnBufAppendStr(&b, "}\n");
    if (err)
        err = ERROR_NOT_ENOUGH_MEMORY;
    else
        err = LnWriteFileAtomic(path, b.data, b.len);
    LnBufFree(&b);
    free(path);
    return err;
}

/* ---- apply -------------------------------------------------------------- */

DWORD LnServicingApplyPackage(const LN_TARGET *t, const char *packageId, const char *version,
                              BOOL *rebootRequired, char txId[65])
{
    CTX c;
    LN_JOURNAL j;
    const LN_JENTRY *last;
    BOOL resumable = FALSE;
    UNDO *u = NULL;
    BOOL *pending = NULL, any = FALSE;
    char pinned[65], buf[48];
    size_t i;
    DWORD err;

    if (rebootRequired)
        *rebootRequired = FALSE;
    if (txId)
        txId[0] = 0;
    err = CtxInit(&c, t, packageId, version, "apply");
    if (err)
        return err;
    if (txId)
        strcpy(txId, c.txid);

    err = LnJournalRead(t, &j);
    if (err)
        goto fail;
    last = LastOf(&j, c.txid);
    if (last)
    {
        if (!strcmp(last->phase, "confirmed") || !strcmp(last->phase, "applied"))
        {
            LnJournalFree(&j);
            LnManifestFree(&c.m);
            return ERROR_SUCCESS;                       /* the same request: nothing to do */
        }
        if (!strcmp(last->phase, "pending-first-boot"))
        {
            if (rebootRequired)
                *rebootRequired = TRUE;
            LnJournalFree(&j);
            LnManifestFree(&c.m);
            return ERROR_SUCCESS;
        }
        if (!strcmp(last->phase, "rollback-prepared"))
        {
            LnJournalFree(&j);
            err = LN_E_BAD_STATE;
            goto fail;
        }
        /* read before the journal is released: `last` points into it */
        resumable = !strcmp(last->phase, "prepared") || !strcmp(last->phase, "applying");
    }
    LnJournalFree(&j);

    /* the store must still hold the objects (immutable, but verify before trusting) */
    for (i = 0; i < c.m.itemCount; i++)
    {
        WCHAR *op;
        char h[65];

        if (c.m.items[i].isDelete)
            continue;
        err = LnStoreObjectPath(t, c.m.items[i].sha256, &op);
        if (err)
            goto fail;
        err = LnSha256FileHex(op, h);
        free(op);
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
            err = LN_E_NOT_FOUND;
        else if (!err && strcmp(h, c.m.items[i].sha256))
            err = LN_E_HASH_MISMATCH;
        if (err)
            goto fail;
    }
    err = PinnedCert(t, pinned);
    if (err)
        goto fail;
    /* ENFORCE at the moment of use, not only at add-package: the store copy (manifest, catalog,
     * signature, objects) must still verify against the pinned certificate */
    {
        LN_MANIFEST verified;

        err = LnServicingVerifyStored(t, packageId, version, &verified);
        if (!err)
        {
            /* the manifest that was verified is the very one this transaction was derived from */
            if (strcmp(verified.manifestSha256, c.m.manifestSha256))
                err = LN_E_BAD_SIGNATURE;
            LnManifestFree(&verified);
        }
    }
    if (err)
        goto fail;
    /* every target must be a real, unaliased path: no 8.3 names, no junctions or links in the way */
    for (i = 0; i < c.m.itemCount; i++)
    {
        err = LnTargetValidateReal(t, c.m.items[i].target);
        if (err)
            goto fail;
    }

    u = calloc(c.m.itemCount, sizeof(UNDO));
    pending = calloc(c.m.itemCount, sizeof(BOOL));
    if (!u || !pending)
    {
        err = ERROR_NOT_ENOUGH_MEMORY;
        goto fail;
    }

    /* if the route rule will defer this apply, the boot applier must be installed
     * before anything changes; lock-driven defers still hit the late check below */
    if (RouteRuleWouldDefer(t, &c.m))
    {
        err = RequireBootApplier(t);
        if (err)
        {
            LogLine(t, c.txid, "apply refused: a reboot is required but the boot updater is not installed");
            Journal(&c, "failed", "error", ErrText(err, buf));
            goto fail_logged;
        }
    }

    /* prepared: backups taken and verified, undo.json written, before any change */
    {
        UNDO existing[1];
        BOOL resumed = resumable;

        (void)existing;
        if (resumed)
        {
            err = ReadUndo(&c, u);
            if (err == LN_E_NOT_FOUND)
                resumed = FALSE;                        /* died before undo.json: start over */
            else if (err)
                goto fail;
        }
        if (!resumed)
        {
            err = TakeBackups(&c, u);
            if (err)
            {
                Journal(&c, "failed", "error", ErrText(err, buf));
                goto fail_logged;
            }
            err = Journal(&c, "prepared", "ok", NULL);
            if (err)
                goto fail_logged;
        }
    }

    err = Journal(&c, "applying", "pending", NULL);
    if (err)
        goto fail_logged;
    for (i = 0; i < c.m.itemCount; i++)
    {
        err = ApplyItem(&c, &c.m.items[i], &pending[i]);
        if (err)
        {
            Journal(&c, "failed", "error", ErrText(err, buf));
            DoRollback(&c, "verifiable-failure");
            goto fail_logged;
        }
        if (pending[i])
            any = TRUE;
        else
            JournalOp(&c, "applying", "ok", (long long)(i + 1), NULL);       /* per-operation evidence */
    }

    if (any)
    {
        err = RequireBootApplier(t);
        if (err)
        {
            LogLine(t, c.txid, "apply refused: a reboot is required but the boot updater is not installed");
            Journal(&c, "failed", "error", ErrText(err, buf));
            DoRollback(&c, "boot-applier-missing");
            goto fail_logged;
        }
        err = WritePlan(&c, pending);
        if (err)
        {
            Journal(&c, "failed", "error", ErrText(err, buf));
            DoRollback(&c, "verifiable-failure");
            goto fail_logged;
        }
        err = Journal(&c, "pending-first-boot", "pending", NULL);
        if (rebootRequired)
            *rebootRequired = TRUE;
    }
    else
        err = Journal(&c, "applied", "ok", NULL);
    goto cleanup;

fail:
    if (err && err != LN_E_BAD_STATE)
    {
        /* failed before anything changed: record it, there is nothing to roll back */
        Journal(&c, "failed", "error", ErrText(err, buf));
    }
fail_logged:
cleanup:
    free(u);
    free(pending);
    LnManifestFree(&c.m);
    return err;
}

/* ---- rollback ----------------------------------------------------------- */

DWORD LnServicingRollback(const LN_TARGET *t, const char *packageId, const char *version, char txId[65])
{
    CTX c;
    LN_JOURNAL j;
    const LN_JENTRY *last;
    DWORD err;

    if (txId)
        txId[0] = 0;
    err = CtxInit(&c, t, packageId, version, "apply");
    if (err)
        return err;
    if (txId)
        strcpy(txId, c.txid);
    err = LnJournalRead(t, &j);
    if (err)
    {
        LnManifestFree(&c.m);
        return err;
    }
    last = LastOf(&j, c.txid);
    if (!last)
        err = LN_E_NOT_FOUND;
    else if (!strcmp(last->phase, "rolled-back") || !strcmp(last->phase, "staged"))
        err = ERROR_SUCCESS;                            /* nothing applied, or already reverted */
    LnJournalFree(&j);
    if (!err && last)
    {
        /* re-read the phase: a rolled-back transaction needs nothing more */
        LN_JOURNAL j2;

        err = LnJournalRead(t, &j2);
        if (!err)
        {
            last = LastOf(&j2, c.txid);
            if (last && !strcmp(last->phase, "rolled-back"))
            {
                LnJournalFree(&j2);
                LnManifestFree(&c.m);
                return ERROR_SUCCESS;
            }
            LnJournalFree(&j2);
        }
    }
    if (!err || err == ERROR_SUCCESS)
        err = DoRollback(&c, NULL);
    LnManifestFree(&c.m);
    return err;
}

/* ---- confirm health ------------------------------------------------------ */

DWORD LnServicingConfirmHealth(const LN_TARGET *t, const char *packageId, const char *version)
{
    CTX c;
    LN_JOURNAL j;
    const LN_JENTRY *last;
    size_t i;
    DWORD err;
    BOOL early = FALSE;

    err = CtxInit(&c, t, packageId, version, "apply");
    if (err)
        return err;
    err = LnJournalRead(t, &j);
    if (err)
    {
        LnManifestFree(&c.m);
        return err;
    }
    last = LastOf(&j, c.txid);
    if (!last)
        err = LN_E_NOT_FOUND;
    else if (!strcmp(last->phase, "confirmed"))
        err = ERROR_SUCCESS;
    else if (strcmp(last->phase, "applied") && strcmp(last->phase, "pending-first-boot"))
        err = LN_E_BAD_STATE;
    {
        BOOL already = last && !strcmp(last->phase, "confirmed");

        early = last && !strcmp(last->phase, "pending-first-boot");     /* the boot applier has not run yet */
        LnJournalFree(&j);
        if (err || already)
        {
            LnManifestFree(&c.m);
            return err;
        }
    }

    {
        err = LnServicingVerifyStored(t, packageId, version, NULL);                  /* same rule as apply */
        if (err)
        {
            LogLine(t, c.txid, "confirm refused: the stored package no longer verifies");
            LnAttemptRecordFailure(t, c.txid);
            LnManifestFree(&c.m);
            return err;
        }
    }
    /* the proof: every target is exactly what the manifest says */
    for (i = 0; i < c.m.itemCount && !err; i++)
    {
        WCHAR *tp = LnTargetFile(t, c.m.items[i].target);
        char h[65];

        if (!tp)
        {
            err = ERROR_NOT_ENOUGH_MEMORY;
            break;
        }
        err = CurrentHash(tp, h);
        free(tp);
        if (!err && (c.m.items[i].isDelete ? h[0] != 0 : strcmp(h, c.m.items[i].sha256)))
            err = LN_E_VERIFY_FINAL;
    }
    if (err)
    {
        LogLine(t, c.txid, "confirm refused: a target does not match its final hash");
        if (!early)
            LnAttemptRecordFailure(t, c.txid);          /* too early (plan pending) is not a failure */
        LnManifestFree(&c.m);
        return err;
    }
    err = Journal(&c, "confirmed", "ok", NULL);
    if (!err)
    {
        LnAttemptSetHealth(t, c.txid);
        RemovePlanOf(&c);
        RemoveAttemptRecord(t, c.txid);             /* the transaction is closed; a stale record would count as an attempt for the next tx */
    }
    LnManifestFree(&c.m);
    return err;
}

/* ---- remove-package ------------------------------------------------------ */

DWORD LnServicingRemovePackage(const LN_TARGET *t, const char *packageId, const char *version)
{
    CTX c;
    LN_JOURNAL j;
    const LN_JENTRY *last;
    DWORD err = CtxInit(&c, t, packageId, version, "apply");

    if (err)
        return err;
    err = LnJournalRead(t, &j);
    if (err)
    {
        LnManifestFree(&c.m);
        return err;
    }
    last = LastOf(&j, c.txid);
    if (last && strcmp(last->phase, "rolled-back") && strcmp(last->phase, "staged") && strcmp(last->phase, "failed"))
    {
        /* installed (or half installed): uninstall = the inverse plan from the backup */
        LnJournalFree(&j);
        err = LnServicingRollback(t, packageId, version, NULL);
        if (err)
        {
            LnManifestFree(&c.m);
            return err;
        }
    }
    else
        LnJournalFree(&j);

    err = LnStoreRemovePackage(t, packageId, version);
    RemoveAttemptRecord(t, c.txid);             /* idempotent: close this transaction's leftover record */
    LogLine(t, c.txid, "remove-package: store entry removed (objects are immutable and stay)");
    LnManifestFree(&c.m);
    return err;
}

/* ---- export-log ---------------------------------------------------------- */

static void CopyIfExists(const LN_TARGET *t, const WCHAR *rel, const WCHAR *destDir, const WCHAR *name)
{
    WCHAR *src = LnTargetPath(t, rel), *dst = LnJoin(destDir, name);

    if (src && dst && LnPathExists(src))
        LnCopyFileFlush(src, dst);
    free(src);
    free(dst);
}

DWORD LnServicingExportLog(const LN_TARGET *t, const WCHAR *destDir)
{
    DWORD err = LnEnsureDir(destDir);
    WCHAR *logs, *pat;
    WIN32_FIND_DATAW fd;
    HANDLE h;

    if (err)
        return err;
    CopyIfExists(t, L"journal\\transactions.jsonl", destDir, L"transactions.jsonl");
    CopyIfExists(t, L"journal\\state.json", destDir, L"state.json");
    CopyIfExists(t, L"pending\\plan.json", destDir, L"plan.json");
    CopyIfExists(t, L"pending\\attempt.json", destDir, L"attempt.json");
    CopyIfExists(t, L"target-id", destDir, L"target-id.txt");
    logs = LnTargetPath(t, L"logs");
    pat = logs ? LnJoin(logs, L"*.log") : NULL;
    h = pat ? FindFirstFileW(pat, &fd) : INVALID_HANDLE_VALUE;
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            WCHAR *src = LnJoin(logs, fd.cFileName), *dst = LnJoin(destDir, fd.cFileName);

            if (src && dst)
                LnCopyFileFlush(src, dst);
            free(src);
            free(dst);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    free(pat);
    free(logs);
    return ERROR_SUCCESS;
}
