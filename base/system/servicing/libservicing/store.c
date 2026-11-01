/*
 * PROJECT:     LiberNT Update - libservicing
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Content store: immutable objects by SHA-256, manifests, package records
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Layout (S1-CONTRACT): store\objects\<sha256>\payload.bin,
 * store\manifests\<id>\<version>\manifest.json, store\packages\<id>\<version>\{manifest.json,catalog.cat}.
 * An object that already exists with the right hash is never rewritten.
 */

#include "libservicing.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static WCHAR *W(const char *s)
{
    return LnUtf8ToWide(s);
}

DWORD LnStoreObjectPath(const LN_TARGET *t, const char *sha256, WCHAR **path)
{
    WCHAR rel[160], *ws;

    *path = NULL;
    if (!LnIsSha256Hex(sha256))
        return LN_E_MANIFEST_INVALID;
    ws = W(sha256);
    if (!ws)
        return ERROR_NOT_ENOUGH_MEMORY;
    wcscpy(rel, L"store\\objects\\");
    wcscat(rel, ws);
    free(ws);
    *path = LnTargetPath(t, rel);
    return *path ? ERROR_SUCCESS : ERROR_NOT_ENOUGH_MEMORY;
}

static WCHAR *PackageRel(const wchar_t *kind, const char *id, const char *ver, const wchar_t *leaf)
{
    WCHAR *wid = W(id), *wver = W(ver), *r = NULL;

    if (wid && wver)
    {
        size_t n = wcslen(kind) + wcslen(wid) + wcslen(wver) + wcslen(leaf) + 24;

        r = malloc(n * sizeof(WCHAR));
        if (r)
        {
            wcscpy(r, L"store\\");
            wcscat(r, kind);
            wcscat(r, L"\\");
            wcscat(r, wid);
            wcscat(r, L"\\");
            wcscat(r, wver);
            if (*leaf)
            {
                wcscat(r, L"\\");
                wcscat(r, leaf);
            }
        }
    }
    free(wid);
    free(wver);
    return r;
}

DWORD LnStoreImport(const LN_TARGET *t, const WCHAR *pkgDir, const LN_MANIFEST *m)
{
    WCHAR *payloadDir = LnJoin(pkgDir, L"payload");
    size_t i;
    DWORD err = ERROR_SUCCESS;

    if (!payloadDir)
        return ERROR_NOT_ENOUGH_MEMORY;
    for (i = 0; i < m->itemCount && !err; i++)
    {
        WCHAR *src, *dst = NULL;
        char h[65];

        if (m->items[i].isDelete)
            continue;
        err = LnStoreObjectPath(t, m->items[i].sha256, &dst);
        if (err)
            break;
        if (!LnSha256FileHex(dst, h) && !strcmp(h, m->items[i].sha256))
        {
            free(dst);
            continue;                                   /* already there and correct */
        }
        src = LnJoin(payloadDir, m->items[i].path);
        if (!src)
            err = ERROR_NOT_ENOUGH_MEMORY;
        else
        {
            err = LnCopyFileFlush(src, dst);
            if (!err)
                err = LnSha256FileHex(dst, h);
            if (!err && strcmp(h, m->items[i].sha256))
            {
                DeleteFileW(dst);
                err = LN_E_HASH_MISMATCH;
            }
        }
        free(src);
        free(dst);
    }
    free(payloadDir);

    if (!err)
    {
        WCHAR *srcManifest = LnJoin(pkgDir, L"manifest.json");
        WCHAR *srcCat = LnJoin(pkgDir, L"catalog.cat");
        WCHAR *srcSig = LnJoin(pkgDir, L"manifest.sig");
        WCHAR *relS = PackageRel(L"manifests", m->packageId, m->releaseVersion, L"manifest.sig");
        WCHAR *relS2 = PackageRel(L"packages", m->packageId, m->releaseVersion, L"manifest.sig");
        WCHAR *relM = PackageRel(L"manifests", m->packageId, m->releaseVersion, L"manifest.json");
        WCHAR *relP = PackageRel(L"packages", m->packageId, m->releaseVersion, L"manifest.json");
        WCHAR *relC = PackageRel(L"packages", m->packageId, m->releaseVersion, L"catalog.cat");

        if (srcManifest && srcCat && relM && relP && relC)
        {
            WCHAR *dM = LnTargetPath(t, relM), *dP = LnTargetPath(t, relP), *dC = LnTargetPath(t, relC);
            WCHAR *dS = relS ? LnTargetPath(t, relS) : NULL;
            WCHAR *dS2 = relS2 ? LnTargetPath(t, relS2) : NULL;

            if (dM && dP && dC)
            {
                err = LnCopyFileFlush(srcManifest, dM);
                if (!err) err = LnCopyFileFlush(srcManifest, dP);
                if (!err && LnPathExists(srcCat)) err = LnCopyFileFlush(srcCat, dC);   /* optional, additional */
                if (!err && srcSig && dS) err = LnCopyFileFlush(srcSig, dS);
                if (!err && srcSig && dS2) err = LnCopyFileFlush(srcSig, dS2);
            }
            else
                err = ERROR_NOT_ENOUGH_MEMORY;
            free(dM);
            free(dP);
            free(dC);
            free(dS);
            free(dS2);
        }
        else
            err = ERROR_NOT_ENOUGH_MEMORY;
        free(srcManifest);
        free(srcCat);
        free(srcSig);
        free(relS);
        free(relS2);
        free(relM);
        free(relP);
        free(relC);
    }
    return err;
}

DWORD LnStoreLoadManifest(const LN_TARGET *t, const char *packageId, const char *version, LN_MANIFEST *m)
{
    WCHAR *rel = PackageRel(L"manifests", packageId, version, L"manifest.json"), *path;
    DWORD err;

    if (!rel)
        return ERROR_NOT_ENOUGH_MEMORY;
    path = LnTargetPath(t, rel);
    free(rel);
    if (!path)
        return ERROR_NOT_ENOUGH_MEMORY;
    err = LnManifestLoad(path, m);
    free(path);
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
        return LN_E_NOT_FOUND;
    if (!err && (strcmp(m->packageId, packageId) || strcmp(m->releaseVersion, version)))
    {
        LnManifestFree(m);
        return LN_E_MANIFEST_INVALID;
    }
    return err;
}

DWORD LnStoreRemovePackage(const LN_TARGET *t, const char *packageId, const char *version)
{
    WCHAR *r1 = PackageRel(L"manifests", packageId, version, L""), *r2 = PackageRel(L"packages", packageId, version, L"");
    DWORD err = ERROR_SUCCESS;

    if (r1 && r2)
    {
        WCHAR *p1 = LnTargetPath(t, r1), *p2 = LnTargetPath(t, r2);

        if (p1 && p2)
        {
            if (!LnPathExists(p1) && !LnPathExists(p2))
                err = LN_E_NOT_FOUND;
            LnRemoveTree(p1);
            LnRemoveTree(p2);
        }
        else
            err = ERROR_NOT_ENOUGH_MEMORY;
        free(p1);
        free(p2);
    }
    else
        err = ERROR_NOT_ENOUGH_MEMORY;
    free(r1);
    free(r2);
    return err;
}

DWORD LnStoreList(const LN_TARGET *t, char (**ids)[130], size_t *count)
{
    WCHAR *base = LnTargetPath(t, L"store\\manifests"), *pat;
    WIN32_FIND_DATAW fd;
    HANDLE h;
    char (*list)[130] = NULL;
    size_t n = 0, cap = 0;

    *ids = NULL;
    *count = 0;
    if (!base)
        return ERROR_NOT_ENOUGH_MEMORY;
    pat = LnJoin(base, L"*");
    h = pat ? FindFirstFileW(pat, &fd) : INVALID_HANDLE_VALUE;
    free(pat);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            WCHAR *sub, *pat2;
            WIN32_FIND_DATAW fd2;
            HANDLE h2;

            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.')
                continue;
            sub = LnJoin(base, fd.cFileName);
            pat2 = sub ? LnJoin(sub, L"*") : NULL;
            h2 = pat2 ? FindFirstFileW(pat2, &fd2) : INVALID_HANDLE_VALUE;
            if (h2 != INVALID_HANDLE_VALUE)
            {
                do
                {
                    char *a, *b;

                    if (!(fd2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd2.cFileName[0] == L'.')
                        continue;
                    a = LnWideToUtf8(fd.cFileName);
                    b = LnWideToUtf8(fd2.cFileName);
                    if (a && b && strlen(a) <= 64 && strlen(b) <= 64)
                    {
                        if (n == cap)
                        {
                            char (*p)[130] = realloc(list, (cap ? cap * 2 : 16) * sizeof(*list));

                            if (!p)
                            {
                                free(a); free(b);
                                break;
                            }
                            list = p;
                            cap = cap ? cap * 2 : 16;
                        }
                        strcpy(list[n], a);
                        strcat(list[n], "\\");
                        strcat(list[n], b);
                        n++;
                    }
                    free(a);
                    free(b);
                } while (FindNextFileW(h2, &fd2));
                FindClose(h2);
            }
            free(sub);
            free(pat2);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    free(base);
    *ids = list;
    *count = n;
    return ERROR_SUCCESS;
}
