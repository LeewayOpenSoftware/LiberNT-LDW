/*
 * PROJECT:     LiberNT Update - libservicing
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     pending\plan.json and pending\attempt.json (S1-CONTRACTS.md sections 1 and 5)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * attempt.json: {"schema":1,"tx":"<id>","attempt":N,"healthConfirmed":false,
 *                "failureRecorded":false,"lastResult":"started|applied|failed"}
 *
 * The boot applier calls LnServicingShouldRollback BEFORE LnServicingMarkBootStarted. The mark
 * writes attempt+1 and lastResult=started with a flush before anything is changed, so a power
 * cut still consumes an attempt (a counter written only at the end of a run would never advance
 * after a crash). healthConfirmed is written only by the service (LnServicingConfirmHealth)
 * and resets attempt to 0.
 */

#include "libservicing.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define ATTEMPT_LIMIT 2

DWORD LnServicingReadPendingPlan(const LN_TARGET *t, char **planJson)
{
    WCHAR *path = LnTargetPath(t, L"pending\\plan.json");
    BYTE *data;
    size_t len;
    DWORD err;

    *planJson = NULL;
    if (!path)
        return ERROR_NOT_ENOUGH_MEMORY;
    err = LnReadFileAll(path, &data, &len);
    free(path);
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
        return LN_E_NOT_FOUND;
    if (err)
        return err;
    *planJson = (char *)data;
    return ERROR_SUCCESS;
}

static DWORD PlanTransactionId(const LN_TARGET *t, char txid[65])
{
    char *plan;
    LN_JSON *root;
    const char *id;
    DWORD err = LnServicingReadPendingPlan(t, &plan);

    txid[0] = 0;
    if (err)
        return err;
    err = LnJsonParse(plan, strlen(plan), &root);
    free(plan);
    if (err)
        return LN_E_MANIFEST_INVALID;
    id = LnJsonGetString(root, "transactionId");
    if (id && LnIsSha256Hex(id))
        strcpy(txid, id);
    else
        err = LN_E_MANIFEST_INVALID;
    LnJsonFree(root);
    return err;
}

static DWORD WriteAttempt(const LN_TARGET *t, const LN_ATTEMPT *a)
{
    LN_BUF b;
    WCHAR *path;
    char num[24];
    DWORD err = 0;

    LnBufInit(&b);
    err |= LnBufAppendStr(&b, "{\"schema\":1,\"tx\":");
    err |= LnBufAppendJsonString(&b, a->transactionId);
    LnI64ToStr(a->attempt, num);
    err |= LnBufAppendStr(&b, ",\"attempt\":");
    err |= LnBufAppendStr(&b, num);
    err |= LnBufAppendStr(&b, a->healthConfirmed ? ",\"healthConfirmed\":true" : ",\"healthConfirmed\":false");
    err |= LnBufAppendStr(&b, a->failureRecorded ? ",\"failureRecorded\":true" : ",\"failureRecorded\":false");
    err |= LnBufAppendStr(&b, ",\"lastResult\":");
    err |= LnBufAppendJsonString(&b, a->lastResult);
    err |= LnBufAppendStr(&b, "}\n");
    if (err)
    {
        LnBufFree(&b);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    path = LnTargetPath(t, L"pending\\attempt.json");
    err = path ? LnWriteFileAtomic(path, b.data, b.len) : ERROR_NOT_ENOUGH_MEMORY;
    free(path);
    LnBufFree(&b);
    return err;
}

DWORD LnServicingReadAttempt(const LN_TARGET *t, LN_ATTEMPT *a)
{
    WCHAR *path = LnTargetPath(t, L"pending\\attempt.json");
    BYTE *data;
    size_t len;
    LN_JSON *root;
    DWORD err;
    long long v;
    BOOL bv;
    const char *s;

    memset(a, 0, sizeof(*a));
    if (!path)
        return ERROR_NOT_ENOUGH_MEMORY;
    err = LnReadFileAll(path, &data, &len);
    free(path);
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
        return LN_E_NOT_FOUND;                          /* absent = new plan, attempt 0 */
    if (err)
        return err;
    err = LnJsonParse((const char *)data, len, &root);
    free(data);
    if (err || !root || root->type != LnJsonObject || !LnJsonGetInt(root, "schema", &v) || v != 1)
    {
        LnJsonFree(root);
        return LN_E_MANIFEST_INVALID;
    }
    s = LnJsonGetString(root, "tx");
    if (s && LnIsSha256Hex(s)) strcpy(a->transactionId, s);
    if (LnJsonGetInt(root, "attempt", &v)) a->attempt = v;
    if (LnJsonGetBool(root, "healthConfirmed", &bv)) a->healthConfirmed = bv;
    if (LnJsonGetBool(root, "failureRecorded", &bv)) a->failureRecorded = bv;
    s = LnJsonGetString(root, "lastResult");
    if (s) { strncpy(a->lastResult, s, sizeof(a->lastResult) - 1); }
    LnJsonFree(root);
    return ERROR_SUCCESS;
}

DWORD LnServicingMarkBootStarted(const LN_TARGET *t, LN_ATTEMPT *a)
{
    LN_ATTEMPT cur;
    char planTx[65];
    DWORD err = PlanTransactionId(t, planTx);

    if (err)
        return err;                                     /* no plan: nothing to attempt */
    err = LnServicingReadAttempt(t, &cur);
    if (err == LN_E_NOT_FOUND || (!err && strcmp(cur.transactionId, planTx)))
    {
        memset(&cur, 0, sizeof(cur));                   /* first boot of this plan */
        strcpy(cur.transactionId, planTx);
    }
    else if (err)
        return err;

    strcpy(cur.previousResult, cur.lastResult);         /* "started" here = the last boot never finished */
    cur.attempt++;
    strcpy(cur.lastResult, "started");
    err = WriteAttempt(t, &cur);                        /* flushed before the applier operates */
    if (!err && a)
        *a = cur;
    return err;
}

DWORD LnServicingMarkBootOutcome(const LN_TARGET *t, BOOL verifiedFailure, BOOL completed)
{
    LN_ATTEMPT cur;
    DWORD err = LnServicingReadAttempt(t, &cur);

    if (err)
        return err;
    if (verifiedFailure)
    {
        cur.failureRecorded = TRUE;
        strcpy(cur.lastResult, "failed");
    }
    else if (completed)
        strcpy(cur.lastResult, "applied");
    else
        return ERROR_INVALID_PARAMETER;
    return WriteAttempt(t, &cur);
}

BOOL LnServicingShouldRollback(const LN_ATTEMPT *a, const char **reason)
{
    if (a->healthConfirmed)
    {
        *reason = "health-confirmed";
        return FALSE;
    }
    if (a->attempt >= ATTEMPT_LIMIT)
    {
        *reason = a->failureRecorded ? "verifiable-failure" : "unexpected-reboot";
        return TRUE;
    }
    *reason = "within-limit";
    return FALSE;
}

/* Called by LnServicingConfirmHealth after the final hashes were re-checked: the only writer of
 * healthConfirmed. It resets the attempt counter. */
DWORD LnAttemptSetHealth(const LN_TARGET *t, const char *transactionId)
{
    LN_ATTEMPT cur;
    DWORD err = LnServicingReadAttempt(t, &cur);

    if (err == LN_E_NOT_FOUND)
    {
        memset(&cur, 0, sizeof(cur));
        strcpy(cur.transactionId, transactionId);
    }
    else if (err)
        return err;
    if (strcmp(cur.transactionId, transactionId))
        return ERROR_SUCCESS;                           /* attempt belongs to another transaction */
    cur.healthConfirmed = TRUE;
    cur.failureRecorded = FALSE;
    cur.attempt = 0;
    strcpy(cur.lastResult, "applied");
    err = WriteAttempt(t, &cur);
    if (!err)
    {
        /* B2: tell the boot loader the new set is confirmed, so its boot-try counter is cleared on the next
         * boot (S1-B2-FREELDR-BOOTTRY.md). Harmless when no boot-class transaction armed it. */
        WCHAR *bs = LnTargetPath(t, L"pending\\bootstate");

        if (bs)
        {
            LnWriteFileAtomic(bs, "CLEAR\n", 6);
            free(bs);
        }
    }
    return err;
}

/* The plan is a convenience for the boot applier, not an authority: it must be exactly what the signed
 * package in the store says. Checks the stored package again (catalog, manifest signature, objects),
 * the transaction id, and every operation against the manifest. */
DWORD LnServicingValidatePlan(const LN_TARGET *t, const char *planJson)
{
    LN_JSON *root;
    const LN_JSON *pk, *tg, *ops, *op;
    const char *id, *ver, *digest, *txs, *machine;
    char expect[65];
    LN_MANIFEST m;
    long long schema;
    DWORD err = LnJsonParse(planJson, strlen(planJson), &root);

    if (err)
        return LN_E_MANIFEST_INVALID;
    err = LN_E_MANIFEST_INVALID;
    pk = LnJsonGet(root, "package");
    tg = LnJsonGet(root, "target");
    ops = LnJsonGet(root, "operations");
    txs = LnJsonGetString(root, "transactionId");
    id = LnJsonGetString(pk, "id");
    ver = LnJsonGetString(pk, "version");
    digest = LnJsonGetString(pk, "digest");
    machine = LnJsonGetString(tg, "machineId");
    if (!LnJsonGetInt(root, "schema", &schema) || schema != 1 || !txs || !id || !ver || !digest || !machine ||
        !ops || ops->type != LnJsonArray || !LnJsonGetString(root, "operation") ||
        strcmp(LnJsonGetString(root, "operation"), "install") || strcmp(machine, t->identity) ||
        FALSE)
        goto out;
    err = LnServicingVerifyStored(t, id, ver, &m);
    if (err)
        goto out;
    err = LN_E_MANIFEST_INVALID;
    LnTransactionId(t->identity, id, ver, m.manifestSha256, "install", expect);
    if (strcmp(m.manifestSha256, digest) || strcmp(expect, txs))
        goto outm;
    for (op = ops->child; op; op = op->next)
    {
        const char *type = LnJsonGetString(op, "type"), *target = LnJsonGetString(op, "target");
        const char *sha = LnJsonGetString(op, "sha256"), *source = LnJsonGetString(op, "source");
        const LN_JSON *backup = LnJsonGet(op, "backup");
        WCHAR *wt = (target && target[0] == '\\') ? LnUtf8ToWide(target + 1) : NULL;
        char want[128];
        BOOL found = FALSE;
        size_t i;

        if (!type || !sha || !source || !wt ||
            (strcmp(type, "replace-file") && strcmp(type, "add-file") && strcmp(type, "swap-file")))
        {
            free(wt);
            goto outm;
        }
        strcpy(want, "store/objects/");
        strncat(want, sha, 64);
        for (i = 0; i < m.itemCount; i++)
        {
            if (!m.items[i].isDelete && !_wcsicmp(m.items[i].target, wt) && !strcmp(m.items[i].sha256, sha))
                found = TRUE;
        }
        free(wt);
        if (!found || strcmp(source, want))
            goto outm;
        if ((!strcmp(type, "replace-file") || !strcmp(type, "swap-file")) != (backup && backup->type == LnJsonString))
            goto outm;                         /* replace-file and swap-file have a backup, add-file has none */
    }
    err = ERROR_SUCCESS;
outm:
    LnManifestFree(&m);
out:
    LnJsonFree(root);
    return err;
}

/* A failed health check is a verifiable failure (S1-CONTRACTS.md section 5): failureRecorded=true makes the
 * next rollback decision name it. The transaction itself stays "applied": confirmed is only ever written
 * after a successful check, and a failed check does not rewrite history. */
DWORD LnAttemptRecordFailure(const LN_TARGET *t, const char *transactionId)
{
    LN_ATTEMPT cur;
    DWORD err = LnServicingReadAttempt(t, &cur);

    if (err == LN_E_NOT_FOUND)
    {
        memset(&cur, 0, sizeof(cur));
        strcpy(cur.transactionId, transactionId);
    }
    else if (err)
        return err;
    if (strcmp(cur.transactionId, transactionId))
        return ERROR_SUCCESS;
    cur.failureRecorded = TRUE;
    strcpy(cur.lastResult, "failed");
    return WriteAttempt(t, &cur);
}
