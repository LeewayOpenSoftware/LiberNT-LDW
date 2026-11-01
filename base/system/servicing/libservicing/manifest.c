/*
 * PROJECT:     LiberNT Update - libservicing
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Package manifest: parse and validate (schema v1, S1-CORE.md)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * manifest.json:
 *   { "schemaVersion":1, "packageId":"...", "releaseVersion":"...", "type":"system-component",
 *     "targetFamily":"LiberNT", "architecture":"amd64", "certificateSha256":"<64 hex>",
 *     "payloads":[ {"path":"a.dll","target":"Windows\\System32\\a.dll","sha256":"<64 hex>"},
 *                  {"action":"delete","target":"Windows\\System32\\old.dll"} ] }
 */

#include "libservicing.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static BOOL IsIdChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '.' || c == '_' || c == '-';
}

static BOOL IsId(const char *s, size_t max)
{
    size_t n = s ? strlen(s) : 0, i;
    WCHAR wide[80];

    if (n == 0 || n > max)
        return FALSE;
    if (s[0] == '.')                                    /* "." and ".." are never ids */
        return FALSE;
    for (i = 0; i < n; i++)
    {
        if (!IsIdChar(s[i]))
            return FALSE;
        wide[i] = (WCHAR)(unsigned char)s[i];
        if (s[i] == '~')
            return FALSE;
    }
    wide[n] = 0;
    if (LnIsReservedName(wide, n))                      /* ids become directory names in the store */
        return FALSE;
    return TRUE;
}

void LnManifestFree(LN_MANIFEST *m)
{
    size_t i;

    for (i = 0; i < m->itemCount; i++)
    {
        free(m->items[i].path);
        free(m->items[i].target);
    }
    free(m->items);
    memset(m, 0, sizeof(*m));
}

DWORD LnManifestParse(const char *text, size_t len, LN_MANIFEST *m)
{
    LN_JSON *root = NULL, *it;
    const LN_JSON *payloads;
    long long schema;
    const char *s;
    size_t n = 0, i;
    DWORD err;

    memset(m, 0, sizeof(*m));
    LnSha256Hex(text, len, m->manifestSha256);
    if (LnJsonParse(text, len, &root) || !root || root->type != LnJsonObject)
    {
        err = LN_E_MANIFEST_INVALID;
        goto fail;
    }
    if (!LnJsonGetInt(root, "schemaVersion", &schema) || schema != 1)
        goto bad;
    s = LnJsonGetString(root, "packageId");
    if (!IsId(s, 64)) goto bad;
    strcpy(m->packageId, s);
    s = LnJsonGetString(root, "releaseVersion");
    if (!IsId(s, 64)) goto bad;
    strcpy(m->releaseVersion, s);
    s = LnJsonGetString(root, "type");
    if (!s || (strcmp(s, "system-component") && strcmp(s, "driver") && strcmp(s, "tool")))
        goto bad;                                       /* "engine" is not part of stage 1 */
    strcpy(m->type, s);
    s = LnJsonGetString(root, "targetFamily");
    if (!s || strcmp(s, "LiberNT")) goto bad;
    s = LnJsonGetString(root, "architecture");
    if (!s || (strcmp(s, "amd64") && strcmp(s, "x86"))) goto bad;
    strcpy(m->architecture, s);
    s = LnJsonGetString(root, "baseVersion");
    if (s && IsId(s, 31))
        strcpy(m->baseVersion, s);
    else if (s)
        goto bad;
    else
        strcpy(m->baseVersion, "0");
    s = LnJsonGetString(root, "certificateSha256");
    if (!s || !LnIsSha256Hex(s)) goto bad;
    strcpy(m->certificateSha256, s);

    payloads = LnJsonGet(root, "payloads");
    if (!payloads || payloads->type != LnJsonArray)
        goto bad;
    for (it = payloads->child; it; it = it->next)
        n++;
    if (n == 0 || n > LN_MAX_ITEMS)
        goto bad;
    m->items = calloc(n, sizeof(LN_ITEM));
    if (!m->items)
    {
        err = ERROR_NOT_ENOUGH_MEMORY;
        goto fail;
    }

    for (it = payloads->child; it; it = it->next)
    {
        LN_ITEM *item = &m->items[m->itemCount];
        const char *action = LnJsonGetString(it, "action");
        const char *path = LnJsonGetString(it, "path");
        const char *target = LnJsonGetString(it, "target");
        const char *sha = LnJsonGetString(it, "sha256");

        if (it->type != LnJsonObject || !target)
            goto bad;
        item->isDelete = action && !strcmp(action, "delete");
        if (action && !item->isDelete && strcmp(action, "replace"))
            goto bad;
        {
            long long ds;

            item->driverStart = -1;
            if (LnJsonGetInt(it, "driverStart", &ds))
            {
                if (ds < 0 || ds > 4)
                    goto bad;
                item->driverStart = (int)ds;
            }
        }
        item->target = LnUtf8ToWide(target);
        m->itemCount++;                                 /* owned from here, freed on failure */
        if (!item->target || !LnTargetItemAllowed(item->target, item->driverStart))
        {
            err = LN_E_FORBIDDEN_TARGET;
            goto fail;
        }
        if (!item->isDelete)
        {
            if (!path || !sha || !LnIsSha256Hex(sha))
                goto bad;
            item->path = LnUtf8ToWide(path);
            if (!item->path || !LnIsSafeRelativePath(item->path))
                goto bad;
            strcpy(item->sha256, sha);
        }
    }

    /* the same target twice (case-insensitively) would make the result order-dependent */
    for (i = 0; i < m->itemCount; i++)
    {
        size_t j;

        for (j = i + 1; j < m->itemCount; j++)
        {
            if (!_wcsicmp(m->items[i].target, m->items[j].target))
                goto bad;
        }
    }
    LnJsonFree(root);
    return ERROR_SUCCESS;

bad:
    err = LN_E_MANIFEST_INVALID;
fail:
    LnJsonFree(root);
    LnManifestFree(m);
    return err;
}

DWORD LnManifestLoad(const WCHAR *path, LN_MANIFEST *m)
{
    BYTE *data;
    size_t len;
    DWORD err = LnReadFileAll(path, &data, &len);

    if (err)
        return err;
    err = LnManifestParse((const char *)data, len, m);
    free(data);
    return err;
}
