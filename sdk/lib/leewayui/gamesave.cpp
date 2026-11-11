/*
 * PROJECT:     LeewayUI
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     G0 save/resume: versioned CRC-checked blob + registry slot store
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Packed layout (little endian): "LWGS" | version | game tag | payload size | crc32(payload) |
 * reserved (0) | payload. A blob that fails any check is never handed to the game.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <leewayui/game.h>

#define LWG_MAGIC LWG_TAG('L', 'W', 'G', 'S')   /* the bytes "LWGS" at offset 0 */

static UINT32 Crc32(const void *data, UINT32 size)
{
    const BYTE *p = (const BYTE *)data;
    UINT32 crc = 0xFFFFFFFFu, i, j;

    for (i = 0; i < size; ++i)
    {
        crc ^= p[i];
        for (j = 0; j < 8; ++j)
            crc = (crc >> 1) ^ (0xEDB88320u & (UINT32)-(INT32)(crc & 1));
    }
    return ~crc;
}

static void Put32(BYTE *p, UINT32 v)
{
    p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); p[2] = (BYTE)(v >> 16); p[3] = (BYTE)(v >> 24);
}

static UINT32 Get32(const BYTE *p)
{
    return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24);
}

UINT32 LwgSavePack(UINT32 game_tag, UINT32 version, const void *data, UINT32 size, void *out, UINT32 out_cap)
{
    BYTE *o = (BYTE *)out;

    if (!out || (!data && size) || size > 0x7FFFFFFFu - LWG_SAVE_HEADER_SIZE || out_cap < size + LWG_SAVE_HEADER_SIZE)
        return 0;
    Put32(o, LWG_MAGIC);
    Put32(o + 4, version);
    Put32(o + 8, game_tag);
    Put32(o + 12, size);
    Put32(o + 16, Crc32(data, size));
    Put32(o + 20, 0);
    if (size)
        memcpy(o + LWG_SAVE_HEADER_SIZE, data, size);
    return size + LWG_SAVE_HEADER_SIZE;
}

LwgSaveStatus LwgSaveUnpack(UINT32 game_tag, UINT32 max_version, const void *packed, UINT32 packed_size,
                            const void **payload, UINT32 *payload_size, UINT32 *version)
{
    const BYTE *p = (const BYTE *)packed;
    UINT32 size;

    if (!p || packed_size < LWG_SAVE_HEADER_SIZE)
        return LWG_SAVE_TRUNCATED;
    if (Get32(p) != LWG_MAGIC)
        return LWG_SAVE_BAD_MAGIC;
    if (Get32(p + 8) != game_tag)
        return LWG_SAVE_WRONG_GAME;
    if (Get32(p + 4) > max_version)
        return LWG_SAVE_NEWER;
    size = Get32(p + 12);
    if (size != packed_size - LWG_SAVE_HEADER_SIZE)
        return LWG_SAVE_TRUNCATED;
    if (Crc32(p + LWG_SAVE_HEADER_SIZE, size) != Get32(p + 16))
        return LWG_SAVE_BAD_CRC;
    if (payload)
        *payload = p + LWG_SAVE_HEADER_SIZE;
    if (payload_size)
        *payload_size = size;
    if (version)
        *version = Get32(p + 4);
    return LWG_SAVE_OK;
}

static BOOL SavePath(const WCHAR *game, WCHAR *out, size_t cch)
{
    if (!game || !*game)
        return FALSE;
    return _snwprintf(out, cch, L"Software\\Leeway\\Games\\%ls\\Save", game) > 0;
}

void LwgFree(void *p)
{
    if (p)
        HeapFree(GetProcessHeap(), 0, p);
}

BOOL LwgSaveStore(const WCHAR *game, const WCHAR *slot, UINT32 game_tag, UINT32 version, const void *data, UINT32 size)
{
    WCHAR path[260];
    HKEY k;
    BYTE *buf;
    UINT32 n;
    BOOL ok = FALSE;

    if (!slot || !*slot || !SavePath(game, path, 260) || size > 4u * 1024 * 1024)
        return FALSE;
    buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, size + LWG_SAVE_HEADER_SIZE);
    if (!buf)
        return FALSE;
    n = LwgSavePack(game_tag, version, data, size, buf, size + LWG_SAVE_HEADER_SIZE);
    if (n && RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS)
    {
        ok = RegSetValueExW(k, slot, 0, REG_BINARY, buf, n) == ERROR_SUCCESS;
        RegCloseKey(k);
    }
    HeapFree(GetProcessHeap(), 0, buf);
    return ok;
}

LwgSaveStatus LwgSaveRestore(const WCHAR *game, const WCHAR *slot, UINT32 game_tag, UINT32 max_version,
                             void **out, UINT32 *out_size, UINT32 *version)
{
    WCHAR path[260];
    HKEY k;
    DWORD type = 0, size = 0;
    BYTE *buf;
    const void *payload = NULL;
    UINT32 psize = 0;
    LwgSaveStatus st;

    if (out)
        *out = NULL;
    if (out_size)
        *out_size = 0;
    if (!out || !slot || !*slot || !SavePath(game, path, 260))
        return LWG_SAVE_TRUNCATED;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return LWG_SAVE_TRUNCATED;
    if (RegQueryValueExW(k, slot, NULL, &type, NULL, &size) != ERROR_SUCCESS || type != REG_BINARY ||
        size > 4u * 1024 * 1024 + LWG_SAVE_HEADER_SIZE)
    {
        RegCloseKey(k);
        return LWG_SAVE_TRUNCATED;
    }
    buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, size ? size : 1);
    if (!buf || RegQueryValueExW(k, slot, NULL, &type, buf, &size) != ERROR_SUCCESS)
    {
        LwgFree(buf);
        RegCloseKey(k);
        return LWG_SAVE_TRUNCATED;
    }
    RegCloseKey(k);
    st = LwgSaveUnpack(game_tag, max_version, buf, size, &payload, &psize, version);
    if (st == LWG_SAVE_OK)
    {
        BYTE *copy = (BYTE *)HeapAlloc(GetProcessHeap(), 0, psize ? psize : 1);
        if (!copy)
            st = LWG_SAVE_TRUNCATED;
        else
        {
            if (psize)
                memcpy(copy, payload, psize);
            *out = copy;
            if (out_size)
                *out_size = psize;
        }
    }
    HeapFree(GetProcessHeap(), 0, buf);
    return st;
}

BOOL LwgSaveDiscard(const WCHAR *game, const WCHAR *slot)
{
    WCHAR path[260];
    HKEY k;

    if (!slot || !*slot || !SavePath(game, path, 260))
        return FALSE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_WRITE, &k) != ERROR_SUCCESS)
        return TRUE;                            /* nothing stored */
    RegDeleteValueW(k, slot);
    RegCloseKey(k);
    return TRUE;
}
