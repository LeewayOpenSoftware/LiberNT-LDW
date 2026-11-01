/*
 * PROJECT:     LiberNT Update - libservicing
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     ENFORCE verification of a package. The root of trust is manifest.sig
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Decision of the architecture (S1-CONTRACTS.md): the root of trust of Update is the SIGNATURE OF THE
 * PACKAGE MANIFEST, made on the host when the package is built: manifest.sig = RSA-2048 PKCS#1 v1.5
 * over the SHA-256 of manifest.json, with the development key. The boot applier verifies the same
 * file with the same code (S1Rsa2048Sha256Verify of the portable s1core), so nothing here depends on
 * wintrust, and nothing is applied unless the hashes of the payloads match the signed manifest.
 *
 * There is no audit mode (owner condition 2). Order of checks, each one rejecting before anything is
 * written to the target:
 *   1. manifest.json parses and obeys the schema, the white list and the path rules;
 *   2. every payload exists (a package folder, or an object of the store) and its SHA-256 matches;
 *   3. the pinned certificate (trust\ directory of the target) is the certificate named by the
 *      manifest (certificateSha256);
 *   4. manifest.sig verifies over manifest.json with the public key of that certificate;
 *   5. catalog.cat is OPTIONAL and additional (M6: what PnP asks for the payloads of drivers). If it
 *      is present it must verify completely (WinVerifyTrust accepts the signature, the members cover
 *      the manifest and every payload, the signer is the pinned certificate); a package with a driver
 *      payload must carry it.
 *
 * The development certificate is self-signed, so WinVerifyTrust reports an untrusted root
 * (CERT_E_UNTRUSTEDROOT) after it has checked the catalog signature. That result is accepted only
 * because the signer is pinned; any other failure is a rejection.
 */

#include "libservicing.h"
#include "../pending/s1core.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wintrust.h>
#include <softpub.h>
#include <mscat.h>

#ifndef CERT_E_UNTRUSTEDROOT
#define CERT_E_UNTRUSTEDROOT ((LONG)0x800B0109L)
#endif
#ifndef TRUST_E_NOSIGNATURE
#define TRUST_E_NOSIGNATURE ((LONG)0x800B0100L)
#endif

/* ---- the pinned certificate ------------------------------------------------------------------- */

DWORD LnCertFileSha256(const WCHAR *cerPath, char out[65])
{
    BYTE *data;
    size_t len;
    DWORD err = LnReadFileAll(cerPath, &data, &len);

    if (err)
        return err;
    if (len < 16 || data[0] != 0x30)                    /* a DER certificate is an ASN.1 SEQUENCE */
        err = LN_E_CERT_MISMATCH;
    else
        LnSha256Hex(data, len, out);
    free(data);
    return err;
}

/* Minimal DER reader: one TLV at *p. */
static BOOL DerNext(const BYTE **p, const BYTE *end, BYTE *tag, const BYTE **content, size_t *clen)
{
    const BYTE *q = *p;
    size_t len, i;

    if (q + 2 > end)
        return FALSE;
    *tag = *q++;
    if (*q < 0x80)
        len = *q++;
    else
    {
        size_t nb = *q++ & 0x7f;

        if (nb == 0 || nb > 4 || q + nb > end)
            return FALSE;
        for (len = 0, i = 0; i < nb; i++)
            len = (len << 8) | *q++;
    }
    if (len > (size_t)(end - q))
        return FALSE;
    *content = q;
    *clen = len;
    *p = q + len;
    return TRUE;
}

/* RSA modulus (256 bytes, big endian) and exponent from the SubjectPublicKeyInfo of an X.509 DER. */
static BOOL CertRsa2048Key(const BYTE *der, size_t len, BYTE modulus[256], uint32_t *exponent)
{
    const BYTE *p = der, *end = der + len, *cert, *tbs, *c, *spki, *bits, *rsa, *n, *e;
    size_t certLen, tbsLen, cl, spkiLen, bitsLen, rsaLen, nLen, eLen, i;
    BYTE tag;

    if (!DerNext(&p, end, &tag, &cert, &certLen) || tag != 0x30)
        return FALSE;
    p = cert;
    end = cert + certLen;
    if (!DerNext(&p, end, &tag, &tbs, &tbsLen) || tag != 0x30)          /* tbsCertificate */
        return FALSE;
    p = tbs;
    end = tbs + tbsLen;
    if (!DerNext(&p, end, &tag, &c, &cl))
        return FALSE;
    if (tag == 0xA0 && !DerNext(&p, end, &tag, &c, &cl))                /* [0] version, then the serial */
        return FALSE;
    /* the serial number has been read; skip signature, issuer, validity, subject */
    for (i = 0; i < 4; i++)
    {
        if (!DerNext(&p, end, &tag, &c, &cl))
            return FALSE;
    }
    if (!DerNext(&p, end, &tag, &spki, &spkiLen) || tag != 0x30)        /* subjectPublicKeyInfo */
        return FALSE;
    p = spki;
    end = spki + spkiLen;
    if (!DerNext(&p, end, &tag, &c, &cl) || tag != 0x30)                /* algorithm */
        return FALSE;
    if (!DerNext(&p, end, &tag, &bits, &bitsLen) || tag != 0x03 || bitsLen < 2 || bits[0] != 0)
        return FALSE;
    p = bits + 1;
    end = bits + bitsLen;
    if (!DerNext(&p, end, &tag, &rsa, &rsaLen) || tag != 0x30)          /* RSAPublicKey */
        return FALSE;
    p = rsa;
    end = rsa + rsaLen;
    if (!DerNext(&p, end, &tag, &n, &nLen) || tag != 0x02 || !DerNext(&p, end, &tag, &e, &eLen) || tag != 0x02)
        return FALSE;
    while (nLen > 0 && *n == 0)                                         /* leading zero of the positive INTEGER */
    {
        n++;
        nLen--;
    }
    if (nLen != 256 || eLen == 0 || eLen > 4)                           /* the applier core is RSA-2048 */
        return FALSE;
    memcpy(modulus, n, 256);
    *exponent = 0;
    for (i = 0; i < eLen; i++)
        *exponent = (*exponent << 8) | e[i];
    /* the same pinning as the core: a real 2048-bit odd modulus and the public exponent 65537 */
    return *exponent == 65537 && (modulus[0] & 0x80) && (modulus[255] & 1);
}

#ifdef LN_TESTING
BOOL LnTestCertRsa2048Key(const BYTE *der, size_t len, BYTE modulus[256], uint32_t *exponent)
{
    return CertRsa2048Key(der, len, modulus, exponent);
}
#endif

/* ---- manifest.sig ---------------------------------------------------------------------------- */

/* man/cer are the bytes already read ONCE by the caller: the bytes that were hashed and parsed are the
 * bytes whose signature is checked (no second read of a file that could change in between). */
static DWORD VerifyManifestSignatureBytes(const BYTE *man, size_t mlen, const BYTE *sig, size_t slen,
                                          const BYTE *cer, size_t clen)
{
    BYTE modulus[256];
    uint32_t exponent = 0;

    if (slen != 256 || !CertRsa2048Key(cer, clen, modulus, &exponent))
        return LN_E_BAD_SIGNATURE;
    return S1Rsa2048Sha256Verify(modulus, exponent, man, mlen, sig) ? ERROR_SUCCESS : LN_E_BAD_SIGNATURE;
}

static DWORD VerifyManifestSignature(const WCHAR *dir, const BYTE *man, size_t mlen, const BYTE *cer, size_t clen)
{
    WCHAR *sp = LnJoin(dir, L"manifest.sig");
    BYTE *sig = NULL;
    size_t slen = 0;
    DWORD err;

    if (!sp)
        return ERROR_NOT_ENOUGH_MEMORY;
    if (!LnPathExists(sp))
    {
        free(sp);
        return LN_E_UNSIGNED;                           /* a package without manifest.sig is unsigned */
    }
    err = LnReadFileAll(sp, &sig, &slen);
    free(sp);
    if (err)
        return LN_E_BAD_SIGNATURE;                  /* the signature must be readable to be checked */
    err = VerifyManifestSignatureBytes(man, mlen, sig, slen, cer, clen);
    free(sig);
    return err;
}

/* ---- optional catalog ---------------------------------------------------------------------- */

/* Returns the WinVerifyTrust status and, if a signer exists, the SHA-256 of its certificate. */
static LONG TrustCatalogFile(const WCHAR *catPath, char signerHash[65])
{
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_FILE_INFO fi;
    WINTRUST_DATA wd;
    LONG status;

    signerHash[0] = 0;
    memset(&fi, 0, sizeof(fi));
    fi.cbStruct = sizeof(fi);
    fi.pcwszFilePath = catPath;
    memset(&wd, 0, sizeof(wd));
    wd.cbStruct = sizeof(wd);
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.dwProvFlags = WTD_REVOCATION_CHECK_NONE | WTD_CACHE_ONLY_URL_RETRIEVAL;

    status = WinVerifyTrust(INVALID_HANDLE_VALUE, &action, &wd);
    if (wd.hWVTStateData)
    {
        CRYPT_PROVIDER_DATA *pd = WTHelperProvDataFromStateData(wd.hWVTStateData);

        if (pd)
        {
            CRYPT_PROVIDER_SGNR *sg = WTHelperGetProvSignerFromChain(pd, 0, FALSE, 0);
            CRYPT_PROVIDER_CERT *pc = sg ? WTHelperGetProvCertFromChain(sg, 0) : NULL;

            if (pc && pc->pCert && pc->pCert->pbCertEncoded)
                LnSha256Hex(pc->pCert->pbCertEncoded, pc->pCert->cbCertEncoded, signerHash);
        }
    }
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(INVALID_HANDLE_VALUE, &action, &wd);
    return status;
}

/* Collects the SHA-256 digests of every catalog member. */
static DWORD ReadCatalogDigests(const WCHAR *catPath, unsigned char (**digests)[32], size_t *count)
{
    HANDLE cat;
    CRYPTCATMEMBER *mem = NULL;
    unsigned char (*list)[32] = NULL;
    size_t n = 0, cap = 0;
    WCHAR *copy = _wcsdup(catPath);

    *digests = NULL;
    *count = 0;
    if (!copy)
        return ERROR_NOT_ENOUGH_MEMORY;
    cat = CryptCATOpen(copy, CRYPTCAT_OPEN_EXISTING, 0, 0, 0);
    free(copy);
    if (cat == INVALID_HANDLE_VALUE || !cat)
        return LN_E_CATALOG_INVALID;

    while ((mem = CryptCATEnumerateMember(cat, mem)) != NULL)
    {
        if (!mem->pIndirectData || mem->pIndirectData->Digest.cbData != 32 ||
            !mem->pIndirectData->Digest.pbData)
            continue;                                   /* not a SHA-256 member: cannot cover anything */
        if (n == cap)
        {
            size_t ncap = cap ? cap * 2 : 64;
            unsigned char (*p)[32] = realloc(list, ncap * 32);

            if (!p)
            {
                free(list);
                CryptCATClose(cat);
                return ERROR_NOT_ENOUGH_MEMORY;
            }
            list = p;
            cap = ncap;
        }
        memcpy(list[n++], mem->pIndirectData->Digest.pbData, 32);
    }
    CryptCATClose(cat);
    *digests = list;
    *count = n;
    return ERROR_SUCCESS;
}

static BOOL Covered(unsigned char (*digests)[32], size_t count, const char *hex)
{
    char h[65];
    size_t i;

    for (i = 0; i < count; i++)
    {
        LnHexEncode(digests[i], 32, h);
        if (!strcmp(h, hex))
            return TRUE;
    }
    return FALSE;
}

/* The additional check: the catalog, when present, must verify completely. */
static DWORD VerifyCatalog(const WCHAR *catPath, const LN_MANIFEST *m, const char *pinnedHash)
{
    unsigned char (*digests)[32] = NULL;
    size_t nd = 0, i;
    char signer[65];
    LONG trust = TrustCatalogFile(catPath, signer);
    DWORD err;

    if (trust == TRUST_E_NOSIGNATURE)
        return LN_E_UNSIGNED;
    if (trust != 0 && trust != CERT_E_UNTRUSTEDROOT)
        return LN_E_CATALOG_INVALID;
    err = ReadCatalogDigests(catPath, &digests, &nd);
    if (err)
        return err;
    err = ERROR_SUCCESS;
    if (!Covered(digests, nd, m->manifestSha256))
        err = LN_E_CATALOG_INVALID;
    for (i = 0; !err && i < m->itemCount; i++)
    {
        if (!m->items[i].isDelete && !Covered(digests, nd, m->items[i].sha256))
            err = LN_E_CATALOG_INVALID;
    }
    if (!err && (!signer[0] || strcmp(signer, pinnedHash)))
        err = LN_E_CERT_MISMATCH;
    free(digests);
    return err;
}

/* ---- the verification -------------------------------------------------------------------------- */

/* t == NULL: pkgDir is a package folder (payload\<path>). t != NULL: pkgDir is the copy held in the
 * store (packages\<id>\<ver>) and the payloads are the store objects, checked by hash. */
static DWORD VerifyCommon(const LN_TARGET *t, const WCHAR *pkgDir, const WCHAR *cerPath, LN_MANIFEST *out)
{
    LN_MANIFEST m;
    WCHAR *manifestPath, *catPath, *payloadDir;
    BYTE *cerBytes = NULL, *manBytes = NULL;
    size_t cerLen = 0, manLen = 0;
    char pinned[65];
    size_t i;
    BOOL hasDriver = FALSE;
    DWORD err;

    err = LnReadFileAll(cerPath, &cerBytes, &cerLen);                   /* the certificate is read once */
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
        return LN_E_NO_TRUST_ANCHOR;
    if (err)
        return err;
    if (cerLen < 16 || cerBytes[0] != 0x30)
    {
        free(cerBytes);
        return LN_E_CERT_MISMATCH;
    }
    LnSha256Hex(cerBytes, cerLen, pinned);

    manifestPath = LnJoin(pkgDir, L"manifest.json");
    catPath = LnJoin(pkgDir, L"catalog.cat");
    payloadDir = LnJoin(pkgDir, L"payload");
    if (!manifestPath || !catPath || !payloadDir)
    {
        err = ERROR_NOT_ENOUGH_MEMORY;
        goto done_nomanifest;
    }

    /* 1. manifest: read once, parsed and (below) signature-checked from the same bytes */
    err = LnReadFileAll(manifestPath, &manBytes, &manLen);
    if (!err)
        err = LnManifestParse((const char *)manBytes, manLen, &m);
    if (err)
        goto done_nomanifest;

    /* 2. payload hashes */
    for (i = 0; i < m.itemCount; i++)
    {
        WCHAR *p;
        char h[65];

        if (m.items[i].driverStart != -1)
            hasDriver = TRUE;
        if (m.items[i].isDelete)
            continue;
        if (t)
        {
            err = LnStoreObjectPath(t, m.items[i].sha256, &p);
            if (err)
                goto done;
        }
        else
            p = LnJoin(payloadDir, m.items[i].path);
        if (!p)
        {
            err = ERROR_NOT_ENOUGH_MEMORY;
            goto done;
        }
        err = LnSha256FileHex(p, h);
        free(p);
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
        {
            err = LN_E_HASH_MISMATCH;                   /* declared but missing */
            goto done;
        }
        if (err)
            goto done;
        if (strcmp(h, m.items[i].sha256))
        {
            err = LN_E_HASH_MISMATCH;
            goto done;
        }
    }

    /* 3. the signer named by the manifest is the pinned certificate */
    if (strcmp(m.certificateSha256, pinned))
    {
        err = LN_E_CERT_MISMATCH;
        goto done;
    }

    /* 4. the signature of the manifest: the root of trust */
    err = VerifyManifestSignature(pkgDir, manBytes, manLen, cerBytes, cerLen);
    if (err)
        goto done;

    /* 5. the catalog: additional; required for driver payloads; when present it must verify */
    if (LnPathExists(catPath))
        err = VerifyCatalog(catPath, &m, pinned);
    else if (hasDriver)
        err = LN_E_UNSIGNED;                            /* PnP wants a signed catalog for a driver (M6) */

done:
    if (!err && out)
        *out = m;                                       /* ownership moves to the caller */
    else
        LnManifestFree(&m);
done_nomanifest:
    free(cerBytes);
    free(manBytes);
    free(manifestPath);
    free(catPath);
    free(payloadDir);
    return err;
}

DWORD LnServicingVerifyPackage(const WCHAR *pkgDir, const WCHAR *pinnedCerPath, LN_MANIFEST *out)
{
    return VerifyCommon(NULL, pkgDir, pinnedCerPath, out);
}

/* The root-of-trust check alone (manifest.sig over the manifest bytes against the
 * pinned certificate), without the payload pass: a downloader can refuse an
 * untrusted manifest before acting on anything the manifest says. The manifest
 * bytes are the caller's, so verification and parsing run over one read. */
DWORD LnVerifyManifestSignatureMem(const BYTE *man, size_t mlen, const WCHAR *sigDir,
                                   const WCHAR *pinnedCerPath)
{
    WCHAR *sp = LnJoin(sigDir, L"manifest.sig");
    BYTE *sig = NULL, *cerBytes = NULL;
    size_t slen = 0, cerLen = 0;
    DWORD err;

    if (!sp)
        return ERROR_NOT_ENOUGH_MEMORY;
    err = LnReadFileAll(pinnedCerPath, &cerBytes, &cerLen);
    if (err)
    {
        free(sp);
        return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND
                   ? LN_E_NO_TRUST_ANCHOR : err;
    }
    if (!LnPathExists(sp))
    {
        free(sp);
        free(cerBytes);
        return LN_E_UNSIGNED;
    }
    err = LnReadFileAll(sp, &sig, &slen);
    free(sp);
    if (err)
        err = LN_E_BAD_SIGNATURE;
    else
        err = VerifyManifestSignatureBytes(man, mlen, sig, slen, cerBytes, cerLen);
    free(sig);
    free(cerBytes);
    return err;
}

DWORD LnServicingVerifyStored(const LN_TARGET *t, const char *packageId, const char *version, LN_MANIFEST *out)
{
    WCHAR *wid = LnUtf8ToWide(packageId), *wver = LnUtf8ToWide(version), *rel, *dir, *cer;
    LN_MANIFEST m, installed;
    DWORD err;

    if (!wid || !wver)
    {
        free(wid);
        free(wver);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    rel = malloc((wcslen(wid) + wcslen(wver) + 32) * sizeof(WCHAR));
    if (!rel)
    {
        free(wid);
        free(wver);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    wcscpy(rel, L"store\\packages\\");
    wcscat(rel, wid);
    wcscat(rel, L"\\");
    wcscat(rel, wver);
    free(wid);
    free(wver);
    dir = LnTargetPath(t, rel);
    cer = LnTargetPath(t, L"trust\\Leeway-LiberNT-Update-DEV.cer");
    free(rel);
    if (!dir || !cer)
    {
        free(dir);
        free(cer);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    err = VerifyCommon(t, dir, cer, &m);
    free(dir);
    free(cer);
    if (err)
        return err;
    /* the manifest the engine reads (manifests\) must be the very one that was verified (packages\) */
    err = LnStoreLoadManifest(t, packageId, version, &installed);
    if (!err)
    {
        if (strcmp(installed.manifestSha256, m.manifestSha256))
            err = LN_E_BAD_SIGNATURE;
        LnManifestFree(&installed);
    }
    if (!err && out)
        *out = m;
    else
        LnManifestFree(&m);
    return err;
}
