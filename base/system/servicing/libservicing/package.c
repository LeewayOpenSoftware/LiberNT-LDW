/*
 * PROJECT:     LiberNT Update - libservicing
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Standalone package .exe (LNPK container) intake, and package applicability
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * add-package --exe <file>: the container is read from the end (trailer, then the table of contents),
 * every entry is checked against its table hash while it is copied into inbox\<id> under the protected
 * servicing tree, and the directory then goes through the ordinary add-package (manifest.sig against the
 * pinned certificate, payload hashes, white list, store import). The stub that carries the overlay is
 * never trusted and never runs here. The inbox copy is removed whatever the outcome.
 */
#include "libservicing.h"
#include "lnpk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define LNPK_IO_CHUNK   65536
#define LNPK_MAX_ENTRY  (1024ULL * 1024ULL * 1024ULL)      /* one entry: 1 GiB */

typedef struct _LNPK_ENTRY
{
    uint64_t offset, size;
    uint8_t sha256[32];
    WCHAR *name;                                           /* relative, backslashes */
} LNPK_ENTRY;

typedef struct _LNPK_FILE
{
    HANDLE h;
    uint64_t fileSize;
    LNPK_TRAILER trailer;
    LNPK_ENTRY *entries;
    size_t count;
} LNPK_FILE;

static void LnpkClose(LNPK_FILE *f)
{
    size_t i;

    if (f->h && f->h != INVALID_HANDLE_VALUE)
        CloseHandle(f->h);
    for (i = 0; i < f->count; i++)
        free(f->entries[i].name);
    free(f->entries);
    memset(f, 0, sizeof(*f));
}

static DWORD LnpkReadAt(HANDLE h, uint64_t offset, void *buf, DWORD len)
{
    LARGE_INTEGER li;
    DWORD got = 0;

    li.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN))
        return GetLastError();
    if (!ReadFile(h, buf, len, &got, NULL))
        return GetLastError();
    return got == len ? ERROR_SUCCESS : LN_E_PACKAGE_INVALID;
}

/* The entry names the container may carry: the S1 package directory and nothing else. */
static BOOL LnpkNameAllowed(const WCHAR *name)
{
    if (!LnIsSafeRelativePath(name))
        return FALSE;
    if (!_wcsicmp(name, L"manifest.json") || !_wcsicmp(name, L"manifest.sig") || !_wcsicmp(name, L"catalog.cat"))
        return TRUE;
    return _wcsnicmp(name, L"payload\\", 8) == 0 && name[8] != 0;
}

static DWORD LnpkOpen(const WCHAR *exePath, LNPK_FILE *f)
{
    LARGE_INTEGER size;
    BYTE *toc = NULL;
    uint8_t digest[32];
    size_t tocLen, pos, i;
    DWORD err;
    BOOL manifest = FALSE;

    memset(f, 0, sizeof(*f));
    f->h = CreateFileW(exePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f->h == INVALID_HANDLE_VALUE)
        return GetLastError();
    if (!GetFileSizeEx(f->h, &size))
    {
        err = GetLastError();
        goto fail;
    }
    f->fileSize = (uint64_t)size.QuadPart;
    if (f->fileSize < LNPK_TRAILER_SIZE + sizeof(LNPK_ENTRY_HEAD))
    {
        err = LN_E_PACKAGE_INVALID;
        goto fail;
    }
    err = LnpkReadAt(f->h, f->fileSize - LNPK_TRAILER_SIZE, &f->trailer, LNPK_TRAILER_SIZE);
    if (err)
        goto fail;
    if (memcmp(f->trailer.magic, LNPK_MAGIC, 4) || f->trailer.version != LNPK_VERSION || f->trailer.reserved ||
        f->trailer.tocCount == 0 || f->trailer.tocCount > LNPK_MAX_ENTRIES ||
        f->trailer.tocOffset >= f->fileSize - LNPK_TRAILER_SIZE ||
        f->trailer.overlayOffset > f->trailer.tocOffset)
    {
        err = LN_E_PACKAGE_INVALID;
        goto fail;
    }
    tocLen = (size_t)(f->fileSize - LNPK_TRAILER_SIZE - f->trailer.tocOffset);
    if (tocLen > LNPK_MAX_TOC || tocLen < sizeof(LNPK_ENTRY_HEAD))
    {
        err = LN_E_PACKAGE_INVALID;
        goto fail;
    }
    toc = malloc(tocLen);
    if (!toc)
    {
        err = ERROR_NOT_ENOUGH_MEMORY;
        goto fail;
    }
    err = LnpkReadAt(f->h, f->trailer.tocOffset, toc, (DWORD)tocLen);
    if (err)
        goto fail;
    LnSha256(toc, tocLen, digest);
    if (memcmp(digest, f->trailer.tocSha256, 32))
    {
        err = LN_E_PACKAGE_INVALID;                        /* table does not match the trailer */
        goto fail;
    }
    f->entries = calloc(f->trailer.tocCount, sizeof(LNPK_ENTRY));
    if (!f->entries)
    {
        err = ERROR_NOT_ENOUGH_MEMORY;
        goto fail;
    }
    pos = 0;
    for (i = 0; i < f->trailer.tocCount; i++)
    {
        LNPK_ENTRY_HEAD head;
        char *name8;
        LNPK_ENTRY *e = &f->entries[i];

        if (pos + sizeof(head) > tocLen)
        {
            err = LN_E_PACKAGE_INVALID;
            goto fail;
        }
        memcpy(&head, toc + pos, sizeof(head));
        pos += sizeof(head);
        if (head.nameLength == 0 || head.nameLength > LNPK_MAX_NAME || pos + head.nameLength > tocLen ||
            head.size > LNPK_MAX_ENTRY || head.offset < f->trailer.overlayOffset ||
            head.offset > f->trailer.tocOffset || head.size > f->trailer.tocOffset - head.offset)
        {
            err = LN_E_PACKAGE_INVALID;                    /* an entry outside the overlay, or a cut name */
            goto fail;
        }
        name8 = malloc(head.nameLength + 1u);
        if (!name8)
        {
            err = ERROR_NOT_ENOUGH_MEMORY;
            goto fail;
        }
        memcpy(name8, toc + pos, head.nameLength);
        name8[head.nameLength] = 0;
        pos += head.nameLength;
        e->name = LnUtf8ToWide(name8);
        free(name8);
        if (!e->name)
        {
            err = ERROR_NOT_ENOUGH_MEMORY;
            goto fail;
        }
        e->offset = head.offset;
        e->size = head.size;
        memcpy(e->sha256, head.sha256, 32);
        f->count++;
        if (!LnpkNameAllowed(e->name))
        {
            err = LN_E_PACKAGE_INVALID;
            goto fail;
        }
        if (!_wcsicmp(e->name, L"manifest.json"))
            manifest = TRUE;
    }
    if (pos != tocLen || !manifest)
    {
        err = LN_E_PACKAGE_INVALID;                        /* trailing bytes in the table, or no manifest */
        goto fail;
    }
    free(toc);
    return ERROR_SUCCESS;

fail:
    free(toc);
    LnpkClose(f);
    return err;
}

/* Copy one entry to destPath, hashing the bytes as they are read; a hash mismatch removes the file. */
static DWORD LnpkExtractEntry(const LNPK_FILE *f, const LNPK_ENTRY *e, const WCHAR *destPath)
{
    LN_SHA256_CTX ctx;
    uint8_t digest[32];
    BYTE *buf;
    HANDLE out;
    uint64_t left = e->size, offset = e->offset;
    DWORD err = ERROR_SUCCESS;

    buf = malloc(LNPK_IO_CHUNK);
    if (!buf)
        return ERROR_NOT_ENOUGH_MEMORY;
    out = CreateFileW(destPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE)
    {
        free(buf);
        return GetLastError();
    }
    LnSha256Init(&ctx);
    while (left && !err)
    {
        DWORD n = left > LNPK_IO_CHUNK ? LNPK_IO_CHUNK : (DWORD)left, wrote = 0;

        err = LnpkReadAt(f->h, offset, buf, n);
        if (err)
            break;
        LnSha256Update(&ctx, buf, n);
        if (!WriteFile(out, buf, n, &wrote, NULL) || wrote != n)
            err = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
        offset += n;
        left -= n;
    }
    if (!err)
    {
        LnSha256Final(&ctx, digest);
        if (memcmp(digest, e->sha256, 32))
            err = LN_E_PACKAGE_INVALID;                    /* bytes differ from the table: damaged container */
        else if (!FlushFileBuffers(out))
            err = GetLastError();
    }
    CloseHandle(out);
    free(buf);
    if (err)
        DeleteFileW(destPath);
    return err;
}

static DWORD LnpkExtractAll(const LNPK_FILE *f, const WCHAR *destDir)
{
    size_t i;
    DWORD err = LnEnsureDir(destDir);

    for (i = 0; i < f->count && !err; i++)
    {
        WCHAR *path = LnJoin(destDir, f->entries[i].name), *slash;

        if (!path)
            return ERROR_NOT_ENOUGH_MEMORY;
        slash = wcsrchr(path, L'\\');
        if (slash)
        {
            *slash = 0;
            err = LnEnsureDir(path);
            *slash = L'\\';
        }
        if (!err)
            err = LnpkExtractEntry(f, &f->entries[i], path);
        free(path);
    }
    return err;
}

DWORD LnPackageExeInspect(const WCHAR *exePath, char **manifestJson, size_t *len)
{
    LNPK_FILE f;
    size_t i;
    DWORD err;

    *manifestJson = NULL;
    *len = 0;
    err = LnpkOpen(exePath, &f);
    if (err)
        return err;
    err = LN_E_PACKAGE_INVALID;
    for (i = 0; i < f.count; i++)
    {
        const LNPK_ENTRY *e = &f.entries[i];
        uint8_t digest[32];
        char *text;

        if (_wcsicmp(e->name, L"manifest.json") || e->size > 16u * 1024u * 1024u)
            continue;
        text = malloc((size_t)e->size + 1);
        if (!text)
        {
            err = ERROR_NOT_ENOUGH_MEMORY;
            break;
        }
        err = LnpkReadAt(f.h, e->offset, text, (DWORD)e->size);
        if (!err)
        {
            LnSha256(text, (size_t)e->size, digest);
            if (memcmp(digest, e->sha256, 32))
                err = LN_E_PACKAGE_INVALID;
        }
        if (err)
        {
            free(text);
            break;
        }
        text[e->size] = 0;
        *manifestJson = text;
        *len = (size_t)e->size;
        break;
    }
    LnpkClose(&f);
    return err;
}

DWORD LnServicingAddPackageExe(const LN_TARGET *t, const WCHAR *exePath, char txId[65])
{
    LNPK_FILE f;
    WCHAR rel[80], *dir;
    char hex[65];
    DWORD err;

    if (txId)
        txId[0] = 0;
    err = LnpkOpen(exePath, &f);
    if (err)
        return err;
    /* inbox\<table hash>: a stale copy of the same container is replaced, two containers never collide */
    LnHexEncode(f.trailer.tocSha256, 32, hex);
    wcscpy(rel, L"inbox\\");
    {
        size_t q;

        for (q = 0; q < 16; q++)
            rel[6 + q] = (WCHAR)hex[q];
        rel[22] = 0;
    }
    dir = LnTargetPath(t, rel);
    if (!dir)
    {
        LnpkClose(&f);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    LnRemoveTree(dir);
    err = LnpkExtractAll(&f, dir);
    LnpkClose(&f);
    if (!err)
        err = LnServicingAddPackage(t, dir, txId);
    LnRemoveTree(dir);
    free(dir);
    return err;
}

/* ---- applicability ------------------------------------------------------- */

/* dotted numeric compare, then plain compare for what is left ("1.10" > "1.9", "1.0a" vs "1.0b" by text) */
int LnVersionCompare(const char *a, const char *b)
{
    for (;;)
    {
        const char *pa = a, *pb = b;
        unsigned long long va = 0, vb = 0;
        BOOL na = FALSE, nb = FALSE;

        while (*pa >= '0' && *pa <= '9')
        {
            va = va * 10 + (unsigned)(*pa++ - '0');
            na = TRUE;
        }
        while (*pb >= '0' && *pb <= '9')
        {
            vb = vb * 10 + (unsigned)(*pb++ - '0');
            nb = TRUE;
        }
        if (na && nb)
        {
            if (va != vb)
                return va < vb ? -1 : 1;
        }
        else
        {
            int c = strcmp(a, b);

            return c < 0 ? -1 : (c > 0 ? 1 : 0);
        }
        if (*pa == '.' && *pb == '.')
        {
            a = pa + 1;
            b = pb + 1;
            continue;
        }
        if (*pa == 0 && *pb == 0)
            return 0;
        if (*pa == 0 || *pb == 0)
            return *pa == 0 ? -1 : 1;                      /* "1.0" < "1.0.1" */
        {
            int c = strcmp(pa, pb);

            return c < 0 ? -1 : (c > 0 ? 1 : 0);
        }
    }
}

/* the journal phase that says a stored package is on the target (applied, confirmed, or waiting for its boot) */
static BOOL PackageInstalled(const LN_TARGET *t, const LN_JOURNAL *j, const char *packageId, const char *version)
{
    LN_MANIFEST m;
    char txid[65];
    const LN_JENTRY *last;
    BOOL installed = FALSE;

    if (LnStoreLoadManifest(t, packageId, version, &m))
        return FALSE;
    LnTransactionId(t->identity, m.packageId, m.releaseVersion, m.manifestSha256, "install", txid);
    LnManifestFree(&m);
    last = LnJournalLast(j, txid);
    if (last && (!strcmp(last->phase, "applied") || !strcmp(last->phase, "confirmed") ||
                 !strcmp(last->phase, "pending-first-boot")))
        installed = TRUE;
    return installed;
}

DWORD LnPackageCheckApplicability(const LN_TARGET *t, const LN_MANIFEST *m, char why[200])
{
    char (*ids)[130] = NULL;
    size_t count = 0, i, k;
    LN_JOURNAL j;
    DWORD err;

    why[0] = 0;
    if (m->requiresCount == 0 && m->supersedesCount == 0)
    {
        /* still refused when an installed package supersedes this one: needs the store walk below */
    }
    err = LnStoreList(t, &ids, &count);
    if (err == ERROR_PATH_NOT_FOUND || err == ERROR_FILE_NOT_FOUND)
    {
        err = ERROR_SUCCESS;
        count = 0;
    }
    if (err)
        return err;
    err = LnJournalRead(t, &j);
    if (err)
    {
        free(ids);
        return err;
    }

    /* requires: each named package must be installed at or above the named version */
    for (i = 0; i < m->requiresCount && !err; i++)
    {
        BOOL met = FALSE;

        for (k = 0; k < count && !met; k++)
        {
            char id[65], ver[65];
            const char *sep = strchr(ids[k], '\\');

            if (!sep || (size_t)(sep - ids[k]) > 64 || strlen(sep + 1) > 64)
                continue;
            memcpy(id, ids[k], (size_t)(sep - ids[k]));
            id[sep - ids[k]] = 0;
            strcpy(ver, sep + 1);
            if (strcmp(id, m->requires[i].packageId))
                continue;
            if (m->requires[i].minVersion[0] && LnVersionCompare(ver, m->requires[i].minVersion) < 0)
                continue;
            met = PackageInstalled(t, &j, id, ver);
        }
        if (!met)
        {
            snprintf(why, 200, "requires %s %s%s, which is not installed", m->requires[i].packageId,
                     m->requires[i].minVersion[0] ? ">= " : "", m->requires[i].minVersion);
            err = LN_E_NOT_APPLICABLE;
        }
    }

    /* superseded: an installed package that lists this id (any version, or up to a version) refuses it */
    for (k = 0; k < count && !err; k++)
    {
        char id[65], ver[65];
        const char *sep = strchr(ids[k], '\\');
        LN_MANIFEST other;

        if (!sep || (size_t)(sep - ids[k]) > 64 || strlen(sep + 1) > 64)
            continue;
        memcpy(id, ids[k], (size_t)(sep - ids[k]));
        id[sep - ids[k]] = 0;
        strcpy(ver, sep + 1);
        if (!strcmp(id, m->packageId) && !strcmp(ver, m->releaseVersion))
            continue;                                      /* the same package again: idempotent add */
        if (LnStoreLoadManifest(t, id, ver, &other))
            continue;
        for (i = 0; i < other.supersedesCount; i++)
        {
            if (strcmp(other.supersedes[i].packageId, m->packageId))
                continue;
            if (other.supersedes[i].maxVersion[0] &&
                LnVersionCompare(m->releaseVersion, other.supersedes[i].maxVersion) > 0)
                continue;
            if (PackageInstalled(t, &j, id, ver))
            {
                snprintf(why, 200, "superseded by %s %s, which is installed", id, ver);
                err = LN_E_SUPERSEDED;
                break;
            }
        }
        LnManifestFree(&other);
    }
    LnJournalFree(&j);
    free(ids);
    return err;
}
