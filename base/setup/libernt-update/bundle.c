/*
 * PROJECT:     LiberNT Update
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Bundle manifest loading and signature verification for the bootstrap
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "bootstrap.h"

/* Development public key, identical to the one embedded in the boot applier
 * (base/system/servicing/pending/pending.c); pinned by S1-CONTRACTS.md 1b. */
static const uint8_t BsDevPublicKey[S1_RSA2048_SIZE] = {
0xb9,0x0b,0xa9,0xbc,0xc2,0x0c,0x37,0x11,0x12,0xf6,0x46,0x48,0xdc,0x9e,0x77,0x0e,
0x82,0x59,0xc0,0x06,0x84,0x81,0x0a,0xbf,0xb7,0x39,0x7c,0xdc,0x5c,0x40,0xe7,0x94,
0xac,0x76,0xee,0x29,0x42,0x07,0x91,0x62,0x95,0x01,0x08,0x68,0xf2,0xd9,0xf2,0x4f,
0x8c,0x26,0x97,0x00,0x8e,0x3a,0x44,0xef,0x60,0xea,0x5c,0x7a,0x21,0x4b,0x20,0x2b,
0x70,0x5c,0xea,0xa5,0xf0,0x1d,0xd8,0xa7,0x4b,0xdf,0xd6,0x7d,0xc3,0xf6,0x91,0x85,
0x40,0x04,0xac,0x79,0xca,0x25,0xe8,0xb5,0x58,0x62,0xde,0xc0,0xc2,0xfb,0x05,0xf7,
0xa9,0xce,0x81,0xe4,0x7e,0x87,0xd3,0xa4,0x5a,0x74,0xc2,0x54,0xf3,0x9a,0xb3,0x89,
0x1b,0x88,0x1f,0xd6,0x6d,0x9c,0xb0,0x81,0x9d,0xd5,0x24,0xfb,0x96,0x08,0xdf,0x46,
0x92,0x86,0x91,0xc2,0x64,0x58,0x58,0x2a,0xea,0x31,0x37,0x53,0x27,0x23,0xd1,0xc5,
0x5d,0x2b,0x8d,0xb1,0xeb,0xea,0xe7,0x1f,0x46,0xee,0xa4,0x6a,0xaf,0x03,0x48,0xe2,
0x75,0x02,0x94,0x7d,0x70,0x5a,0xbf,0xb0,0xa7,0xde,0xb2,0xbb,0x81,0x6f,0x5a,0x63,
0xe4,0xff,0x7f,0x58,0x21,0x8c,0x0b,0xf8,0x99,0x3b,0x00,0x58,0xdb,0x25,0xac,0xe6,
0x68,0xc9,0x3e,0xda,0xdc,0xfc,0xf1,0x1b,0x7a,0x88,0xd6,0x9d,0xef,0x13,0x57,0x78,
0x24,0xc3,0x2e,0xdb,0xdb,0xda,0x47,0xd3,0x1a,0x46,0x82,0x75,0x9a,0xfd,0x9e,0x31,
0xa6,0xb6,0xe6,0x69,0xe4,0x0e,0x70,0x31,0x52,0xef,0x3b,0x9d,0x6d,0x65,0xc5,0x22,
0xc8,0xd7,0x6f,0x49,0x18,0x66,0x30,0xb5,0x2c,0x17,0x50,0x6f,0xea,0x27,0xeb,0x65,
};

const uint8_t *
BsDevPublicKeyBytes(VOID)
{
    return BsDevPublicKey;
}

static BOOL
BsReadWholeFile(
    _In_ PCWSTR Path,
    _Out_ uint8_t **Data,
    _Out_ ULONG *Size)
{
    HANDLE Handle;
    LARGE_INTEGER FileSize;
    DWORD Read;
    uint8_t *Buffer;

    *Data = NULL;
    *Size = 0;
    Handle = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (Handle == INVALID_HANDLE_VALUE)
        return FALSE;
    if (!GetFileSizeEx(Handle, &FileSize) || FileSize.QuadPart <= 0 ||
        FileSize.QuadPart > 16 * 1024 * 1024)
    {
        CloseHandle(Handle);
        SetLastError(ERROR_FILE_INVALID);
        return FALSE;
    }
    Buffer = malloc((size_t)FileSize.QuadPart);
    if (!Buffer)
    {
        CloseHandle(Handle);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    if (!ReadFile(Handle, Buffer, (DWORD)FileSize.QuadPart, &Read, NULL) ||
        Read != (DWORD)FileSize.QuadPart)
    {
        free(Buffer);
        CloseHandle(Handle);
        SetLastError(ERROR_READ_FAULT);
        return FALSE;
    }
    CloseHandle(Handle);
    *Data = Buffer;
    *Size = (ULONG)FileSize.QuadPart;
    return TRUE;
}

static BOOL
BsCopyNarrow(
    _Out_ void *Dst,
    _In_ size_t DstSize,
    _In_ const char *Value,
    _In_ size_t ValueLength,
    _In_ BOOL ToWide)
{
    if (ValueLength + 1 > DstSize)
        return FALSE;
    if (ToWide)
    {
        size_t i;
        WCHAR *Wide = Dst;
        for (i = 0; i < ValueLength; i++)
            Wide[i] = (WCHAR)(unsigned char)Value[i];
        Wide[ValueLength] = 0;
    }
    else
    {
        memcpy(Dst, Value, ValueLength);
        ((char *)Dst)[ValueLength] = 0;
    }
    return TRUE;
}

static BOOL
BsParseFiles(
    _Inout_ BS_BUNDLE *Bundle,
    _In_ const char *Json,
    _In_ ULONG Size)
{
    S1_JSON *Root;
    size_t Count, i;

    if (!S1JsonParse(Json, Size, &Root))
        return FALSE;

    Count = S1JsonGetArrayCount(Root, "binaries");
    if (Count == 0 || Count > RTL_NUMBER_OF(Bundle->Files))
    {
        S1JsonFree(Root);
        return FALSE;
    }

    for (i = 0; i < Count; i++)
    {
        BS_BUNDLE_FILE *File = &Bundle->Files[i];
        char Path[64];
        const char *Value;
        size_t ValueLength;
        S1Bool BoolValue;

        snprintf(Path, sizeof(Path), "binaries.%lu.source", (unsigned long)i);
        if (!S1JsonGetString(Root, Path, &Value, &ValueLength) ||
            !BsCopyNarrow(File->Source, sizeof(File->Source), Value, ValueLength, TRUE))
            goto fail;
        snprintf(Path, sizeof(Path), "binaries.%lu.dest", (unsigned long)i);
        if (!S1JsonGetString(Root, Path, &Value, &ValueLength) ||
            !BsCopyNarrow(File->Dest, sizeof(File->Dest), Value, ValueLength, TRUE))
            goto fail;
        snprintf(Path, sizeof(Path), "binaries.%lu.sha256", (unsigned long)i);
        if (!S1JsonGetString(Root, Path, &Value, &ValueLength) ||
            ValueLength != 64 ||
            !BsCopyNarrow(File->Sha256, sizeof(File->Sha256), Value, ValueLength, FALSE))
            goto fail;
        snprintf(Path, sizeof(Path), "binaries.%lu.required", (unsigned long)i);
        if (S1JsonGetBool(Root, Path, &BoolValue) == S1_FALSE)
            File->Required = TRUE;
        else
            File->Required = BoolValue != 0;
        snprintf(Path, sizeof(Path), "binaries.%lu.service", (unsigned long)i);
        if (S1JsonGetBool(Root, Path, &BoolValue) != S1_FALSE)
            File->IsService = BoolValue != 0;
        Bundle->FileCount++;
    }

    /* Optional Start Menu shortcuts; absent array means none. */
    Count = S1JsonGetArrayCount(Root, "shortcuts");
    if (Count > RTL_NUMBER_OF(Bundle->Shortcuts))
        goto fail;
    for (i = 0; i < Count; i++)
    {
        BS_BUNDLE_SHORTCUT *Shortcut = &Bundle->Shortcuts[i];
        char Path[64];
        const char *Value;
        size_t ValueLength;

        snprintf(Path, sizeof(Path), "shortcuts.%lu.name", (unsigned long)i);
        if (!S1JsonGetString(Root, Path, &Value, &ValueLength) ||
            !BsCopyNarrow(Shortcut->Name, sizeof(Shortcut->Name), Value, ValueLength, TRUE))
            goto fail;
        snprintf(Path, sizeof(Path), "shortcuts.%lu.target", (unsigned long)i);
        if (!S1JsonGetString(Root, Path, &Value, &ValueLength) ||
            !BsCopyNarrow(Shortcut->Target, sizeof(Shortcut->Target), Value, ValueLength, TRUE))
            goto fail;
        snprintf(Path, sizeof(Path), "shortcuts.%lu.arguments", (unsigned long)i);
        if (S1JsonGetString(Root, Path, &Value, &ValueLength) &&
            !BsCopyNarrow(Shortcut->Arguments, sizeof(Shortcut->Arguments), Value, ValueLength, TRUE))
            goto fail;
        Bundle->ShortcutCount++;
    }

    S1JsonFree(Root);
    return TRUE;

fail:
    S1JsonFree(Root);
    return FALSE;
}

DWORD
BsBundleLoad(
    _Out_ BS_BUNDLE *Bundle,
    _In_ PCWSTR BundlePath)
{
    WCHAR ManifestPath[BS_MAX_PATH_BUF], SignaturePath[BS_MAX_PATH_BUF];
    uint8_t *Manifest = NULL, *Signature = NULL;
    ULONG ManifestSize, SignatureSize;
    S1_SHA256_CTX Ctx;
    uint8_t Digest[S1_SHA256_SIZE];
    S1Bool Ok;
    ULONG i;

    memset(Bundle, 0, sizeof(*Bundle));
    wcscpy(Bundle->Path, BundlePath);

    /* Read the manifest once; verify the signature over, hash and parse that
     * same buffer. Reading it again for the signature would leave the bytes
     * actually used uncovered (Pam review B1). */
    if (swprintf(ManifestPath, RTL_NUMBER_OF(ManifestPath),
                 L"%s\\bootstrap-manifest.json", BundlePath) < 0 ||
        swprintf(SignaturePath, RTL_NUMBER_OF(SignaturePath),
                 L"%s\\bootstrap-manifest.sig", BundlePath) < 0)
        return ERROR_FILENAME_EXCED_RANGE;
    if (!BsReadWholeFile(ManifestPath, &Manifest, &ManifestSize))
        return GetLastError();
    if (!BsReadWholeFile(SignaturePath, &Signature, &SignatureSize))
    {
        free(Manifest);
        return GetLastError();
    }
    if (SignatureSize != S1_RSA2048_SIZE)
    {
        free(Manifest);
        free(Signature);
        return ERROR_BAD_LENGTH;
    }
    Ok = S1Rsa2048Sha256Verify(BsDevPublicKey, 65537, Manifest, ManifestSize, Signature);
    free(Signature);
    if (Ok != S1_TRUE)
    {
        free(Manifest);
        return ERROR_TRUST_FAILURE;
    }

    S1Sha256Init(&Ctx);
    S1Sha256Update(&Ctx, Manifest, ManifestSize);
    S1Sha256Final(&Ctx, Digest);
    S1HexEncode(Digest, S1_SHA256_SIZE, Bundle->ManifestDigest);

    if (!BsParseFiles(Bundle, (const char *)Manifest, ManifestSize))
    {
        free(Manifest);
        return ERROR_BAD_FORMAT;
    }
    free(Manifest);

    for (i = 0; i < Bundle->FileCount; i++)
    {
        WCHAR SourcePath[BS_MAX_PATH_BUF];
        WIN32_FILE_ATTRIBUTE_DATA Attr;

        if (swprintf(SourcePath, RTL_NUMBER_OF(SourcePath),
                     L"%s\\%s", BundlePath, Bundle->Files[i].Source) < 0)
            return ERROR_FILENAME_EXCED_RANGE;
        if (!GetFileAttributesExW(SourcePath, GetFileExInfoStandard, &Attr))
            return GetLastError();
        Bundle->TotalBytes += ((ULONG64)Attr.nFileSizeHigh << 32) | Attr.nFileSizeLow;
    }
    return ERROR_SUCCESS;
}
