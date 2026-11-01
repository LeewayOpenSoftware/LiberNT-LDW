/*
 * PROJECT:     LiberNT Update - libservicing
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Append-only JSONL journal with a hash chain, and the derived state.json
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Format of S1-CONTRACTS.md section 2, one line per event:
 *   {"seq":12,"prev":"<sha256 of line seq-1; genesis 64 zeros>","ts":"...Z","tx":"<id>",
 *    "phase":"applying","op":3,"result":"ok","error":null}
 * Each append opens with FILE_APPEND_DATA, writes the whole line, calls FlushFileBuffers and
 * closes. prev is the SHA-256 of the previous valid line, so a removed or altered entry breaks
 * the chain.
 *
 * A write torn by a power cut leaves a partial LAST line, without its newline. That one line is
 * ignored (tailTruncated is set) and the next append truncates it away. Any other line that does
 * not parse (in the middle, empty, or newline-terminated) is corruption, and so is a line that
 * parses but does not continue the chain.
 *
 * A hash chain without a key cannot tell that the LAST entries were cut or rewritten, so
 * journal\anchor.json keeps (seq, hash) of the last entry, rewritten after every append and
 * compared on every read. The journal may be one entry ahead of the anchor (a crash between the
 * two writes), never behind or different. Both files live in the protected servicing tree.
 */

#include "libservicing.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define ZERO_HASH "0000000000000000000000000000000000000000000000000000000000000000"

void LnTransactionId(const char *machineId, const char *packageId, const char *version,
                     const char *manifestDigest, const char *operation, char out[65])
{
    LN_BUF b;

    LnBufInit(&b);
    LnBufAppendStr(&b, machineId);
    LnBufAppendStr(&b, "\n");
    LnBufAppendStr(&b, packageId);
    LnBufAppendStr(&b, "\n");
    LnBufAppendStr(&b, version);
    LnBufAppendStr(&b, "\n");
    LnBufAppendStr(&b, manifestDigest);
    LnBufAppendStr(&b, "\n");
    LnBufAppendStr(&b, operation);                      /* no trailing newline */
    LnSha256Hex(b.data, b.len, out);
    LnBufFree(&b);
}

static DWORD Serialize(const LN_JENTRY *e, LN_BUF *b)
{
    char num[24];
    DWORD err = 0;

    err |= LnBufAppendStr(b, "{\"seq\":");
    LnI64ToStr(e->sequence, num);
    err |= LnBufAppendStr(b, num);
    err |= LnBufAppendStr(b, ",\"prev\":");
    err |= LnBufAppendJsonString(b, e->previousEntrySha256);
    err |= LnBufAppendStr(b, ",\"ts\":");
    err |= LnBufAppendJsonString(b, e->timestampUtc);
    err |= LnBufAppendStr(b, ",\"tx\":");
    err |= LnBufAppendJsonString(b, e->transactionId);
    err |= LnBufAppendStr(b, ",\"phase\":");
    err |= LnBufAppendJsonString(b, e->phase);
    err |= LnBufAppendStr(b, ",\"op\":");
    if (e->op < 0)
        err |= LnBufAppendStr(b, "null");
    else
    {
        LnI64ToStr(e->op, num);
        err |= LnBufAppendStr(b, num);
    }
    err |= LnBufAppendStr(b, ",\"result\":");
    err |= LnBufAppendJsonString(b, e->result);
    err |= LnBufAppendStr(b, ",\"error\":");
    if (e->error[0])
        err |= LnBufAppendJsonString(b, e->error);
    else
        err |= LnBufAppendStr(b, "null");
    err |= LnBufAppendStr(b, "}");
    return err ? ERROR_NOT_ENOUGH_MEMORY : ERROR_SUCCESS;
}

static void CopyField(char *dst, size_t cap, const char *src)
{
    if (!src)
        src = "";
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
}

static BOOL ParseEntry(const char *line, size_t len, LN_JENTRY *e)
{
    LN_JSON *root = NULL;
    const LN_JSON *op, *er;

    memset(e, 0, sizeof(*e));
    if (LnJsonParse(line, len, &root) || !root || root->type != LnJsonObject)
    {
        LnJsonFree(root);
        return FALSE;
    }
    if (!LnJsonGetInt(root, "seq", &e->sequence) || !LnJsonGetString(root, "tx") ||
        !LnJsonGetString(root, "phase") || !LnJsonGetString(root, "prev"))
    {
        LnJsonFree(root);
        return FALSE;
    }
    CopyField(e->transactionId, sizeof(e->transactionId), LnJsonGetString(root, "tx"));
    CopyField(e->phase, sizeof(e->phase), LnJsonGetString(root, "phase"));
    CopyField(e->result, sizeof(e->result), LnJsonGetString(root, "result"));
    CopyField(e->previousEntrySha256, sizeof(e->previousEntrySha256), LnJsonGetString(root, "prev"));
    CopyField(e->timestampUtc, sizeof(e->timestampUtc), LnJsonGetString(root, "ts"));
    op = LnJsonGet(root, "op");
    e->op = (op && op->type == LnJsonNumber) ? op->num : -1;
    er = LnJsonGet(root, "error");
    if (er && er->type == LnJsonString)
        CopyField(e->error, sizeof(e->error), er->str);
    e->rebootRequired = !strcmp(e->phase, "pending-first-boot");
    e->healthConfirmed = !strcmp(e->phase, "confirmed");
    LnJsonFree(root);
    return TRUE;
}

static WCHAR *JournalPath(const LN_TARGET *t)
{
    return LnTargetPath(t, L"journal\\transactions.jsonl");
}

static DWORD WriteAnchor(const LN_TARGET *t, long long seq, const char *hash)
{
    LN_BUF b;
    char num[24];
    WCHAR *path = LnTargetPath(t, L"journal\\anchor.json");
    DWORD err = 0;

    if (!path)
        return ERROR_NOT_ENOUGH_MEMORY;
    LnBufInit(&b);
    LnI64ToStr(seq, num);
    err |= LnBufAppendStr(&b, "{\"schema\":1,\"seq\":");
    err |= LnBufAppendStr(&b, num);
    err |= LnBufAppendStr(&b, ",\"hash\":");
    err |= LnBufAppendJsonString(&b, hash);
    err |= LnBufAppendStr(&b, "}\n");
    err = err ? ERROR_NOT_ENOUGH_MEMORY : LnWriteFileAtomic(path, b.data, b.len);
    LnBufFree(&b);
    free(path);
    return err;
}

/* found = FALSE when there is no anchor file */
static DWORD ReadAnchor(const LN_TARGET *t, BOOL *found, long long *seq, char hash[65])
{
    WCHAR *path = LnTargetPath(t, L"journal\\anchor.json");
    BYTE *data;
    size_t len;
    LN_JSON *root;
    const char *h;
    DWORD err;

    *found = FALSE;
    if (!path)
        return ERROR_NOT_ENOUGH_MEMORY;
    err = LnReadFileAll(path, &data, &len);
    free(path);
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
        return ERROR_SUCCESS;
    if (err)
        return err;
    err = LnJsonParse((const char *)data, len, &root);
    free(data);
    if (err || !root)
        return LN_E_JOURNAL_CORRUPT;
    h = LnJsonGetString(root, "hash");
    if (!LnJsonGetInt(root, "seq", seq) || !h || !LnIsSha256Hex(h) || *seq < 0)
        err = LN_E_JOURNAL_CORRUPT;
    else
    {
        strcpy(hash, h);
        *found = TRUE;
    }
    LnJsonFree(root);
    return err;
}

void LnJournalFree(LN_JOURNAL *j)
{
    free(j->entries);
    memset(j, 0, sizeof(*j));
}

/* Reads and validates; lastHash/lastSeq describe the last valid entry; needsNewline tells
 * whether the file ends in a partial line. */
static DWORD ReadInternal(const LN_TARGET *t, LN_JOURNAL *j, char lastHash[65], long long *lastSeq, BOOL *needsNewline,
                          size_t *validEnd)
{
    BOOL anchorFound;
    long long anchorSeq = 0;
    char anchorHash[65];
    WCHAR *path = JournalPath(t);
    BYTE *data = NULL;
    size_t len = 0, pos = 0, cap = 0;
    DWORD err;

    memset(j, 0, sizeof(*j));
    strcpy(lastHash, ZERO_HASH);
    *lastSeq = 0;
    *needsNewline = FALSE;
    *validEnd = 0;
    if (!path)
        return ERROR_NOT_ENOUGH_MEMORY;
    err = LnReadFileAll(path, &data, &len);
    free(path);
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
    {
        data = NULL;
        len = 0;                                        /* empty journal */
        err = ERROR_SUCCESS;
    }
    if (err)
        return err;

    while (pos < len)
    {
        size_t end = pos;
        LN_JENTRY e;
        char h[65];

        while (end < len && data[end] != '\n')
            end++;
        if (end == pos)
        {
            free(data);                                 /* an empty line is never written */
            LnJournalFree(j);
            return LN_E_JOURNAL_CORRUPT;
        }
        {
            if (ParseEntry((const char *)data + pos, end - pos, &e))
            {
                if (strcmp(e.previousEntrySha256, lastHash) || e.sequence != *lastSeq + 1)
                {
                    free(data);
                    LnJournalFree(j);
                    return LN_E_JOURNAL_CORRUPT;
                }
                if (j->count == cap)
                {
                    size_t ncap = cap ? cap * 2 : 64;
                    LN_JENTRY *p = realloc(j->entries, ncap * sizeof(*p));

                    if (!p)
                    {
                        free(data);
                        LnJournalFree(j);
                        return ERROR_NOT_ENOUGH_MEMORY;
                    }
                    j->entries = p;
                    cap = ncap;
                }
                j->entries[j->count++] = e;
                LnSha256Hex(data + pos, end - pos, h);
                strcpy(lastHash, h);
                *lastSeq = e.sequence;
                *validEnd = end < len ? end + 1 : len;
                if (end == len)
                    *needsNewline = TRUE;               /* a complete last line that lost only its newline */
            }
            else if (end == len)
                j->tailTruncated = TRUE;                /* the torn LAST line of a power cut: ignored */
            else
            {
                free(data);                             /* anything else that does not parse */
                LnJournalFree(j);
                return LN_E_JOURNAL_CORRUPT;
            }
        }
        pos = end + 1;
    }
    free(data);

    /* the anchor must agree with the tail: the journal is equal to it or exactly one entry ahead */
    err = ReadAnchor(t, &anchorFound, &anchorSeq, anchorHash);
    if (err)
    {
        LnJournalFree(j);
        return err;
    }
    if (anchorFound)
    {
        BOOL ok = FALSE;

        if (anchorSeq == *lastSeq)
            ok = !strcmp(anchorHash, lastHash);
        else if (anchorSeq + 1 == *lastSeq && j->count)
            ok = !strcmp(anchorHash, j->entries[j->count - 1].previousEntrySha256);
        if (!ok)
        {
            LnJournalFree(j);
            return LN_E_JOURNAL_CORRUPT;                /* cut, rewritten or replaced */
        }
    }
    else if (*lastSeq > 1)
    {
        LnJournalFree(j);
        return LN_E_JOURNAL_CORRUPT;                    /* the anchor was removed */
    }
    return ERROR_SUCCESS;
}

DWORD LnJournalRead(const LN_TARGET *t, LN_JOURNAL *j)
{
    char h[65];
    long long seq;
    BOOL nl;
    size_t ve;

    return ReadInternal(t, j, h, &seq, &nl, &ve);
}

DWORD LnJournalAppend(const LN_TARGET *t, LN_JENTRY *e)
{
    LN_JOURNAL j;
    char lastHash[65];
    long long lastSeq;
    BOOL needsNewline, torn;
    LN_BUF line;
    WCHAR *path;
    HANDLE h;
    DWORD err, wrote;
    size_t validEnd;
    char entryHash[65];

    err = ReadInternal(t, &j, lastHash, &lastSeq, &needsNewline, &validEnd);
    torn = j.tailTruncated;
    LnJournalFree(&j);
    if (err)
        return err;

    e->sequence = lastSeq + 1;
    strcpy(e->previousEntrySha256, lastHash);
    LnNowUtc(e->timestampUtc);

    LnBufInit(&line);
    if (needsNewline && !torn)
        LnBufAppendStr(&line, "\n");
    err = Serialize(e, &line);
    if (!err)
    {
        size_t skip = (needsNewline && !torn) ? 1 : 0;

        LnSha256Hex(line.data + skip, line.len - skip, entryHash);    /* hash of the line without its newline */
        err = LnBufAppendStr(&line, "\n");
    }
    if (err)
    {
        LnBufFree(&line);
        return err;
    }

    path = JournalPath(t);
    if (!path)
    {
        LnBufFree(&line);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    {
        WCHAR dir[MAX_PATH], *sl;

        wcscpy(dir, path);
        sl = wcsrchr(dir, L'\\');
        if (sl)
            *sl = 0;
        err = LnEnsureDir(dir);
    }
    if (!err && torn)
    {
        /* cut the partial last line of a power failure; everything before it is intact */
        HANDLE t2 = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

        if (t2 == INVALID_HANDLE_VALUE)
            err = GetLastError();
        else
        {
            LARGE_INTEGER pos;

            pos.QuadPart = (LONGLONG)validEnd;
            if (!SetFilePointerEx(t2, pos, NULL, FILE_BEGIN) || !SetEndOfFile(t2) || !FlushFileBuffers(t2))
                err = GetLastError();
            CloseHandle(t2);
        }
    }
    if (!err)
    {
        h = LnOpenRetry(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_DELETE,
                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH);
        if (h == INVALID_HANDLE_VALUE)
            err = GetLastError();
        else
        {
            if (!WriteFile(h, line.data, (DWORD)line.len, &wrote, NULL) || wrote != (DWORD)line.len)
                err = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
            else if (!FlushFileBuffers(h))
                err = GetLastError();
            CloseHandle(h);
        }
    }
    free(path);
    LnBufFree(&line);
    if (!err)
        err = WriteAnchor(t, e->sequence, entryHash);
    return err;
}

const LN_JENTRY *LnJournalLast(const LN_JOURNAL *j, const char *transactionId)
{
    size_t i;

    for (i = j->count; i > 0; i--)
    {
        if (!strcmp(j->entries[i - 1].transactionId, transactionId))
            return &j->entries[i - 1];
    }
    return NULL;
}

DWORD LnJournalWriteState(const LN_TARGET *t)
{
    LN_JOURNAL j;
    LN_BUF b;
    WCHAR *path;
    size_t i, k;
    char num[24];
    DWORD err = LnJournalRead(t, &j);

    if (err)
        return err;
    LnBufInit(&b);
    LnBufAppendStr(&b, "{\"schema\":1,\"derivedFromJournal\":true,\"machineId\":");
    LnBufAppendJsonString(&b, t->identity);
    LnI64ToStr((long long)j.count, num);
    LnBufAppendStr(&b, ",\"entries\":");
    LnBufAppendStr(&b, num);
    LnBufAppendStr(&b, ",\"transactions\":[");
    for (i = 0, k = 0; i < j.count; i++)
    {
        const LN_JENTRY *e = &j.entries[i];
        size_t later;
        BOOL superseded = FALSE;

        for (later = i + 1; later < j.count; later++)
        {
            if (!strcmp(j.entries[later].transactionId, e->transactionId))
                superseded = TRUE;
        }
        if (superseded)
            continue;                                   /* only the latest phase of each transaction */
        if (k++)
            LnBufAppendStr(&b, ",");
        LnBufAppendStr(&b, "{\"tx\":");
        LnBufAppendJsonString(&b, e->transactionId);
        LnBufAppendStr(&b, ",\"phase\":");
        LnBufAppendJsonString(&b, e->phase);
        LnBufAppendStr(&b, ",\"result\":");
        LnBufAppendJsonString(&b, e->result);
        LnBufAppendStr(&b, ",\"error\":");
        if (e->error[0])
            LnBufAppendJsonString(&b, e->error);
        else
            LnBufAppendStr(&b, "null");
        LnBufAppendStr(&b, "}");
    }
    LnBufAppendStr(&b, "]}\n");
    LnJournalFree(&j);

    path = LnTargetPath(t, L"journal\\state.json");
    err = path ? LnWriteFileAtomic(path, b.data, b.len) : ERROR_NOT_ENOUGH_MEMORY;
    free(path);
    LnBufFree(&b);
    return err;
}
