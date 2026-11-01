/*
 * PROJECT:     LiberNT Update
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Post-install self-test for the bootstrap
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "bootstrap.h"

static DWORD
BsSelfTestFiles(
    _In_ const BS_TARGET *Target,
    _In_ const BS_INVENTORY *Inventory)
{
    ULONG i;

    for (i = 0; i < Inventory->FileCount; i++)
    {
        char Hex[65];

        if (BsHashTargetFile(Target, Inventory->Files[i].Path, Hex) != ERROR_SUCCESS)
            return ERROR_FILE_NOT_FOUND;
        if (_stricmp(Hex, Inventory->Files[i].Sha256) != 0)
            return ERROR_CRC; /* a file on disk differs from the bundle */
    }
    return ERROR_SUCCESS;
}

static DWORD
BsSelfTestPinnedKey(
    _In_ const BS_TARGET *Target)
{
    WCHAR Path[BS_MAX_PATH_BUF];
    HANDLE Handle;
    uint8_t Buffer[S1_RSA2048_SIZE];
    DWORD Read;

    if (BsBuildTargetPath(Path, RTL_NUMBER_OF(Path), Target, BS_DEVKEY_FILE) != ERROR_SUCCESS)
        return ERROR_FILENAME_EXCED_RANGE;
    Handle = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (Handle == INVALID_HANDLE_VALUE)
        return GetLastError();
    if (!ReadFile(Handle, Buffer, sizeof(Buffer), &Read, NULL) || Read != sizeof(Buffer))
    {
        CloseHandle(Handle);
        return ERROR_READ_FAULT;
    }
    CloseHandle(Handle);
    return memcmp(Buffer, BsDevPublicKeyBytes(), S1_RSA2048_SIZE) == 0
               ? ERROR_SUCCESS
               : ERROR_INVALID_DATA;
}

/* Exercises the backup/rename/restore primitives on a scratch file so a
 * failed self-test never touches the real store. */
static DWORD
BsSelfTestSimulatedTransaction(
    _In_ const BS_TARGET *Target)
{
    static const WCHAR *Scratch = L"LiberNT\\Servicing\\bootstrap\\selftest.tmp";
    static const WCHAR *Backup = L"LiberNT\\Servicing\\bootstrap\\selftest.tmp.bak";
    WCHAR Original[] = L"bootstrap-selftest";
    WCHAR ScratchPath[BS_MAX_PATH_BUF], BackupPath[BS_MAX_PATH_BUF];
    HANDLE Handle;
    DWORD Written;
    DWORD Error;

    Error = BsBuildTargetPath(ScratchPath, RTL_NUMBER_OF(ScratchPath), Target, Scratch);
    if (Error != ERROR_SUCCESS)
        return Error;
    Error = BsBuildTargetPath(BackupPath, RTL_NUMBER_OF(BackupPath), Target, Backup);
    if (Error != ERROR_SUCCESS)
        return Error;

    Handle = CreateFileW(ScratchPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, NULL);
    if (Handle == INVALID_HANDLE_VALUE)
        return GetLastError();
    if (!WriteFile(Handle, Original, sizeof(Original), &Written, NULL) ||
        Written != sizeof(Original))
    {
        CloseHandle(Handle);
        return GetLastError();
    }
    CloseHandle(Handle);

    if (!MoveFileW(ScratchPath, BackupPath))
        return GetLastError();
    if (!MoveFileW(BackupPath, ScratchPath))
        return GetLastError();
    if (!DeleteFileW(ScratchPath))
        return GetLastError();
    return ERROR_SUCCESS;
}

/* The inventory must be the record of THIS bundle: its manifest digest is
 * the identity of what was installed (Pam review note). */
static DWORD
BsSelfTestManifest(
    _In_ const BS_BUNDLE *Bundle,
    _In_ const BS_INVENTORY *Inventory)
{
    if (Inventory->ManifestDigest[0] == 0 ||
        _stricmp(Inventory->ManifestDigest, Bundle->ManifestDigest) != 0)
        return ERROR_INVALID_DATA;
    return ERROR_SUCCESS;
}

/* RSA-2048 modulus and exponent from the SubjectPublicKeyInfo of an X.509
 * DER. Mirrors CertRsa2048Key of libservicing/verify.c; keep in sync. */
static BOOL
BsDerNext(
    _Inout_ const uint8_t **Cursor,
    _In_ const uint8_t *End,
    _Out_ uint8_t *Tag,
    _Out_ const uint8_t **Value,
    _Out_ size_t *Length)
{
    size_t Len;
    size_t Header;
    const uint8_t *p = *Cursor;

    if (p + 2 > End)
        return FALSE;
    *Tag = *p++;
    Len = *p++;
    if (Len & 0x80)
    {
        size_t n = Len & 0x7f;
        size_t i;

        if (n == 0 || n > 4 || p + n > End)
            return FALSE;
        Len = 0;
        for (i = 0; i < n; i++)
            Len = (Len << 8) | *p++;
    }
    Header = (size_t)(p - *Cursor);
    if ((size_t)(End - p) < Len)
        return FALSE;
    *Value = p;
    *Length = Len;
    *Cursor = p + Len;
    (void)Header;
    return TRUE;
}

static BOOL
BsCertRsa2048Key(
    _In_ const uint8_t *Der,
    _In_ size_t Len,
    _Out_ uint8_t Modulus[256],
    _Out_ uint32_t *Exponent)
{
    const uint8_t *p = Der, *end = Der + Len, *cert, *tbs, *c, *spki, *bits, *rsa, *n, *e;
    size_t certLen, tbsLen, cl, spkiLen, bitsLen, rsaLen, nLen, eLen, i;
    uint8_t tag;

    if (!BsDerNext(&p, end, &tag, &cert, &certLen) || tag != 0x30)
        return FALSE;
    p = cert;
    end = cert + certLen;
    if (!BsDerNext(&p, end, &tag, &tbs, &tbsLen) || tag != 0x30)
        return FALSE;
    p = tbs;
    end = tbs + tbsLen;
    if (!BsDerNext(&p, end, &tag, &c, &cl))
        return FALSE;
    if (tag == 0xA0 && !BsDerNext(&p, end, &tag, &c, &cl))
        return FALSE;
    for (i = 0; i < 4; i++)
    {
        if (!BsDerNext(&p, end, &tag, &c, &cl))
            return FALSE;
    }
    if (!BsDerNext(&p, end, &tag, &spki, &spkiLen) || tag != 0x30)
        return FALSE;
    p = spki;
    end = spki + spkiLen;
    if (!BsDerNext(&p, end, &tag, &c, &cl) || tag != 0x30)
        return FALSE;
    if (!BsDerNext(&p, end, &tag, &bits, &bitsLen) || tag != 0x03 ||
        bitsLen < 2 || bits[0] != 0)
        return FALSE;
    p = bits + 1;
    end = bits + bitsLen;
    if (!BsDerNext(&p, end, &tag, &rsa, &rsaLen) || tag != 0x30)
        return FALSE;
    p = rsa;
    end = rsa + rsaLen;
    if (!BsDerNext(&p, end, &tag, &n, &nLen) || tag != 0x02 ||
        !BsDerNext(&p, end, &tag, &e, &eLen) || tag != 0x02)
        return FALSE;
    while (nLen > 0 && *n == 0)
    {
        n++;
        nLen--;
    }
    if (nLen != 256 || eLen == 0 || eLen > 4)
        return FALSE;
    memcpy(Modulus, n, 256);
    *Exponent = 0;
    for (i = 0; i < eLen; i++)
        *Exponent = (*Exponent << 8) | e[i];
    return *Exponent == 65537 && (Modulus[0] & 0x80) && (Modulus[255] & 1);
}

/* When the CLI has installed the trusted development certificate, the
 * pinned key must be the key OF THAT CERTIFICATE (Pam review B3). */
static DWORD
BsSelfTestTrustCert(
    _In_ const BS_TARGET *Target)
{
    static const WCHAR CerRel[] = L"trust\\Leeway-LiberNT-Update-DEV.cer";
    WCHAR Path[BS_MAX_PATH_BUF];
    HANDLE Handle;
    LARGE_INTEGER Size;
    uint8_t *Der;
    uint8_t Modulus[256];
    uint32_t Exponent;
    DWORD Read, Error = ERROR_SUCCESS;

    if (BsBuildTargetPath(Path, RTL_NUMBER_OF(Path), Target, CerRel) != ERROR_SUCCESS)
        return ERROR_FILENAME_EXCED_RANGE;
    Handle = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (Handle == INVALID_HANDLE_VALUE)
        return ERROR_FILE_NOT_FOUND; /* init --trust-cer has not run yet */
    if (!GetFileSizeEx(Handle, &Size) || Size.QuadPart <= 0 || Size.QuadPart > 65536)
    {
        CloseHandle(Handle);
        return ERROR_FILE_INVALID;
    }
    Der = malloc((size_t)Size.QuadPart);
    if (!Der)
    {
        CloseHandle(Handle);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    if (!ReadFile(Handle, Der, (DWORD)Size.QuadPart, &Read, NULL) ||
        Read != (DWORD)Size.QuadPart)
        Error = ERROR_READ_FAULT;
    CloseHandle(Handle);
    if (Error == ERROR_SUCCESS &&
        (!BsCertRsa2048Key(Der, (size_t)Size.QuadPart, Modulus, &Exponent) ||
         memcmp(Modulus, BsDevPublicKeyBytes(), S1_RSA2048_SIZE) != 0))
        Error = ERROR_INVALID_DATA;
    free(Der);
    return Error;
}

DWORD
BsSelfTest(
    _In_ const BS_TARGET *Target,
    _In_ const BS_BUNDLE *Bundle,
    _In_ const BS_INVENTORY *Inventory,
    _In_ BOOL OnlineService)
{
    WCHAR Path[BS_MAX_PATH_BUF];
    BOOL Exists;
    DWORD Error;

    Error = BsSelfTestFiles(Target, Inventory);
    if (Error != ERROR_SUCCESS)
        return Error;
    Error = BsSelfTestManifest(Bundle, Inventory);
    if (Error != ERROR_SUCCESS)
        return Error;
    Error = BsBuildTargetPath(Path, RTL_NUMBER_OF(Path), Target, BS_SERVICING_DIR);
    if (Error != ERROR_SUCCESS)
        return Error;
    Error = BsCheckSecure(Path);
    if (Error != ERROR_SUCCESS)
        return Error;
    Error = BsSelfTestPinnedKey(Target);
    if (Error != ERROR_SUCCESS)
        return Error;
    Error = BsSelfTestTrustCert(Target);
    if (Error != ERROR_SUCCESS && Error != ERROR_FILE_NOT_FOUND)
        return Error;
    Error = BsSelfTestSimulatedTransaction(Target);
    if (Error != ERROR_SUCCESS)
        return Error;
    if (OnlineService)
    {
        Error = BsServiceQuery(&Exists);
        if (Error != ERROR_SUCCESS)
            return Error;
        if (!Exists)
            return ERROR_SERVICE_DOES_NOT_EXIST;
    }
    return ERROR_SUCCESS;
}
