/*
 * PROJECT:     LiberNT Update - libservicing
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     SHA-256 (FIPS 180-4), self-contained so that verification does not depend
 *              on a component that an update may replace
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "libservicing.h"

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define M32(x) ((x) & 0xffffffffUL)

static const unsigned long K[64] =
{
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL, 0x3956c25bUL, 0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL,
    0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL, 0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL,
    0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL, 0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
    0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL, 0xc6e00bf3UL, 0xd5a79147UL, 0x06ca6351UL, 0x14292967UL,
    0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL, 0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
    0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL, 0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
    0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL, 0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL, 0x682e6ff3UL,
    0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL, 0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL
};

static void Transform(LN_SHA256_CTX *ctx, const unsigned char *d)
{
    unsigned long a, b, c, dd, e, f, g, h, t1, t2, m[64];
    int i;

    for (i = 0; i < 16; i++)
    {
        m[i] = ((unsigned long)d[i * 4] << 24) | ((unsigned long)d[i * 4 + 1] << 16) |
               ((unsigned long)d[i * 4 + 2] << 8) | (unsigned long)d[i * 4 + 3];
    }
    for (; i < 64; i++)
    {
        unsigned long s0 = ROR(m[i - 15], 7) ^ ROR(m[i - 15], 18) ^ (m[i - 15] >> 3);
        unsigned long s1 = ROR(m[i - 2], 17) ^ ROR(m[i - 2], 19) ^ (m[i - 2] >> 10);
        m[i] = M32(m[i - 16] + s0 + m[i - 7] + s1);
    }

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; dd = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (i = 0; i < 64; i++)
    {
        unsigned long S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
        unsigned long ch = (e & f) ^ ((~e) & g);
        unsigned long S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
        unsigned long maj = (a & b) ^ (a & c) ^ (b & c);

        t1 = M32(h + S1 + ch + K[i] + m[i]);
        t2 = M32(S0 + maj);
        h = g; g = f; f = e; e = M32(dd + t1);
        dd = c; c = b; b = a; a = M32(t1 + t2);
    }

    ctx->state[0] = M32(ctx->state[0] + a); ctx->state[1] = M32(ctx->state[1] + b);
    ctx->state[2] = M32(ctx->state[2] + c); ctx->state[3] = M32(ctx->state[3] + dd);
    ctx->state[4] = M32(ctx->state[4] + e); ctx->state[5] = M32(ctx->state[5] + f);
    ctx->state[6] = M32(ctx->state[6] + g); ctx->state[7] = M32(ctx->state[7] + h);
}

void LnSha256Init(LN_SHA256_CTX *ctx)
{
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667UL; ctx->state[1] = 0xbb67ae85UL;
    ctx->state[2] = 0x3c6ef372UL; ctx->state[3] = 0xa54ff53aUL;
    ctx->state[4] = 0x510e527fUL; ctx->state[5] = 0x9b05688cUL;
    ctx->state[6] = 0x1f83d9abUL; ctx->state[7] = 0x5be0cd19UL;
}

void LnSha256Update(LN_SHA256_CTX *ctx, const void *data, size_t len)
{
    const unsigned char *p = data;
    size_t i;

    for (i = 0; i < len; i++)
    {
        ctx->data[ctx->datalen++] = p[i];
        if (ctx->datalen == 64)
        {
            Transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

void LnSha256Final(LN_SHA256_CTX *ctx, unsigned char out[32])
{
    unsigned long i = ctx->datalen;
    int k;

    ctx->data[i++] = 0x80;
    if (ctx->datalen < 56)
    {
        while (i < 56)
            ctx->data[i++] = 0;
    }
    else
    {
        while (i < 64)
            ctx->data[i++] = 0;
        Transform(ctx, ctx->data);
        for (i = 0; i < 56; i++)
            ctx->data[i] = 0;
    }

    ctx->bitlen += (unsigned long long)ctx->datalen * 8;
    for (k = 0; k < 8; k++)
        ctx->data[63 - k] = (unsigned char)(ctx->bitlen >> (8 * k));
    Transform(ctx, ctx->data);

    for (k = 0; k < 8; k++)
    {
        out[k * 4]     = (unsigned char)(ctx->state[k] >> 24);
        out[k * 4 + 1] = (unsigned char)(ctx->state[k] >> 16);
        out[k * 4 + 2] = (unsigned char)(ctx->state[k] >> 8);
        out[k * 4 + 3] = (unsigned char)(ctx->state[k]);
    }
}

void LnSha256(const void *data, size_t len, unsigned char out[32])
{
    LN_SHA256_CTX c;

    LnSha256Init(&c);
    LnSha256Update(&c, data, len);
    LnSha256Final(&c, out);
}

void LnHexEncode(const unsigned char *in, size_t len, char *out)
{
    static const char hex[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < len; i++)
    {
        out[i * 2] = hex[in[i] >> 4];
        out[i * 2 + 1] = hex[in[i] & 15];
    }
    out[len * 2] = 0;
}

void LnSha256Hex(const void *data, size_t len, char out[65])
{
    unsigned char h[32];

    LnSha256(data, len, h);
    LnHexEncode(h, 32, out);
}

DWORD LnSha256FileHex(const WCHAR *path, char out[65])
{
    HANDLE h;
    LN_SHA256_CTX c;
    unsigned char buf[65536], d[32];
    DWORD got;

    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return GetLastError();

    LnSha256Init(&c);
    for (;;)
    {
        if (!ReadFile(h, buf, sizeof(buf), &got, NULL))
        {
            DWORD err = GetLastError();
            CloseHandle(h);
            return err;
        }
        if (got == 0)
            break;
        LnSha256Update(&c, buf, got);
    }
    CloseHandle(h);
    LnSha256Final(&c, d);
    LnHexEncode(d, 32, out);
    return ERROR_SUCCESS;
}

BOOL LnIsSha256Hex(const char *s)
{
    int i;

    if (!s)
        return FALSE;
    for (i = 0; i < 64; i++)
    {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return FALSE;
    }
    return s[64] == 0;
}
