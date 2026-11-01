/*
 * PROJECT:     LiberNT Update
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Stage-1 boot applier core implementation
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "s1core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ SHA-256 */

static uint32_t
S1Rotr32(uint32_t X, uint32_t N)
{
    return (X >> N) | (X << (32 - N));
}

void
S1Sha256Init(S1_SHA256_CTX *Ctx)
{
    static const uint32_t Init[8] =
        { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memset(Ctx, 0, sizeof(*Ctx));
    memcpy(Ctx->State, Init, sizeof(Init));
}

static void
S1Sha256Block(S1_SHA256_CTX *Ctx, const uint8_t Block[64])
{
    static const uint32_t K[64] =
    {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };
    uint32_t W[64], A, B, C, D, E, F, G, H;
    int i;

    for (i = 0; i < 16; i++)
        W[i] = ((uint32_t)Block[i * 4] << 24) | ((uint32_t)Block[i * 4 + 1] << 16) |
               ((uint32_t)Block[i * 4 + 2] << 8) | Block[i * 4 + 3];
    for (i = 16; i < 64; i++)
    {
        uint32_t S0 = S1Rotr32(W[i - 15], 7) ^ S1Rotr32(W[i - 15], 18) ^ (W[i - 15] >> 3);
        uint32_t S1 = S1Rotr32(W[i - 2], 17) ^ S1Rotr32(W[i - 2], 19) ^ (W[i - 2] >> 10);
        W[i] = W[i - 16] + S0 + W[i - 7] + S1;
    }

    A = Ctx->State[0]; B = Ctx->State[1]; C = Ctx->State[2]; D = Ctx->State[3];
    E = Ctx->State[4]; F = Ctx->State[5]; G = Ctx->State[6]; H = Ctx->State[7];

    for (i = 0; i < 64; i++)
    {
        uint32_t S1 = S1Rotr32(E, 6) ^ S1Rotr32(E, 11) ^ S1Rotr32(E, 25);
        uint32_t Ch = (E & F) ^ (~E & G);
        uint32_t T1 = H + S1 + Ch + K[i] + W[i];
        uint32_t S0 = S1Rotr32(A, 2) ^ S1Rotr32(A, 13) ^ S1Rotr32(A, 22);
        uint32_t Maj = (A & B) ^ (A & C) ^ (B & C);
        uint32_t T2 = S0 + Maj;
        H = G; G = F; F = E; E = D + T1;
        D = C; C = B; B = A; A = T1 + T2;
    }

    Ctx->State[0] += A; Ctx->State[1] += B; Ctx->State[2] += C; Ctx->State[3] += D;
    Ctx->State[4] += E; Ctx->State[5] += F; Ctx->State[6] += G; Ctx->State[7] += H;
}

void
S1Sha256Update(S1_SHA256_CTX *Ctx, const void *Data, size_t Length)
{
    const uint8_t *Bytes = (const uint8_t *)Data;
    Ctx->Total += Length;
    while (Length)
    {
        size_t Take = 64 - Ctx->BlockLen;
        if (Take > Length)
            Take = Length;
        memcpy(Ctx->Block + Ctx->BlockLen, Bytes, Take);
        Ctx->BlockLen += Take;
        Bytes += Take;
        Length -= Take;
        if (Ctx->BlockLen == 64)
        {
            S1Sha256Block(Ctx, Ctx->Block);
            Ctx->BlockLen = 0;
        }
    }
}

void
S1Sha256Final(S1_SHA256_CTX *Ctx, uint8_t Digest[S1_SHA256_SIZE])
{
    uint64_t BitTotal = Ctx->Total * 8;
    uint8_t Pad = 0x80;
    uint8_t Zero = 0;
    uint8_t LenBytes[8];
    int i;

    S1Sha256Update(Ctx, &Pad, 1);
    while (Ctx->BlockLen != 56)
        S1Sha256Update(Ctx, &Zero, 1);
    for (i = 0; i < 8; i++)
        LenBytes[i] = (uint8_t)(BitTotal >> (56 - i * 8));
    S1Sha256Update(Ctx, LenBytes, 8);
    for (i = 0; i < 8; i++)
    {
        Digest[i * 4] = (uint8_t)(Ctx->State[i] >> 24);
        Digest[i * 4 + 1] = (uint8_t)(Ctx->State[i] >> 16);
        Digest[i * 4 + 2] = (uint8_t)(Ctx->State[i] >> 8);
        Digest[i * 4 + 3] = (uint8_t)Ctx->State[i];
    }
}

void
S1HexEncode(const uint8_t *Bytes, size_t Length, char *Out)
{
    static const char Hex[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < Length; i++)
    {
        Out[i * 2] = Hex[Bytes[i] >> 4];
        Out[i * 2 + 1] = Hex[Bytes[i] & 15];
    }
    Out[Length * 2] = 0;
}

S1Bool
S1HexDecode(const char *Hex, uint8_t *Out, size_t OutLen)
{
    size_t i;
    for (i = 0; i < OutLen; i++)
    {
        char C1 = Hex[i * 2], C2 = Hex[i * 2 + 1];
        uint8_t V1, V2;
        if (C1 >= '0' && C1 <= '9') V1 = (uint8_t)(C1 - '0');
        else if (C1 >= 'a' && C1 <= 'f') V1 = (uint8_t)(C1 - 'a' + 10);
        else if (C1 >= 'A' && C1 <= 'F') V1 = (uint8_t)(C1 - 'A' + 10);
        else return S1_FALSE;
        if (C2 >= '0' && C2 <= '9') V2 = (uint8_t)(C2 - '0');
        else if (C2 >= 'a' && C2 <= 'f') V2 = (uint8_t)(C2 - 'a' + 10);
        else if (C2 >= 'A' && C2 <= 'F') V2 = (uint8_t)(C2 - 'A' + 10);
        else return S1_FALSE;
        Out[i] = (uint8_t)((V1 << 4) | V2);
    }
    return S1_TRUE;
}

/* ------------------------------------------------------ big number (32-bit
 * words, little-endian word order). Only what RSA-2048 verification needs. */

#define S1_BN_WORDS 64   /* 2048 bits */
#define S1_BN_DWORDS 128 /* intermediate product */

typedef struct _S1_BN { uint32_t W[S1_BN_WORDS]; } S1_BN;

static void
S1BnFromBytes(S1_BN *Bn, const uint8_t Bytes[S1_RSA2048_SIZE])
{
    int i;
    memset(Bn, 0, sizeof(*Bn));
    for (i = 0; i < S1_RSA2048_SIZE; i++)
        Bn->W[i / 4] |= (uint32_t)Bytes[S1_RSA2048_SIZE - 1 - i] << ((i % 4) * 8);
}

static int
S1BnCmp(const S1_BN *A, const S1_BN *B)
{
    int i;
    for (i = S1_BN_WORDS - 1; i >= 0; i--)
    {
        if (A->W[i] != B->W[i])
            return A->W[i] > B->W[i] ? 1 : -1;
    }
    return 0;
}

/* R = (A * B) mod M. Schoolbook multiply into 128 words, then word-aligned
 * long division: for each shift from the top, if the aligned 2048-bit window
 * of P is >= M and every word above the window is zero, subtract the shifted
 * modulus. The window is at most 64 words, so the borrow never escapes it
 * when the comparison held. */
/* Divide the 4096-bit P by the 2048-bit M (restoring division, one result
 * bit per step); the 2048-bit remainder lands in R. The quotient is not
 * needed by the callers and is discarded. */
static void
S1BnDivModRemainder(const uint32_t *P, const S1_BN *M, S1_BN *R)
{
    uint32_t Rem[65];
    int Bit, i;

    memset(Rem, 0, sizeof(Rem));
    for (Bit = S1_BN_DWORDS * 32 - 1; Bit >= 0; Bit--)
    {
        int Carry = (P[Bit / 32] >> (Bit % 32)) & 1;

        /* Rem = Rem*2 + bit (65 words, little-endian) */
        for (i = 64; i > 0; i--)
            Rem[i] = (Rem[i] << 1) | (Rem[i - 1] >> 31);
        Rem[0] = (Rem[0] << 1) | (uint32_t)Carry;

        /* Rem (up to 2049 bits) >= M? compare the extra word first */
        {
            int Ge = Rem[64] != 0 ? 1 : 1;
            if (Rem[64] == 0)
            {
                Ge = 1;
                for (i = S1_BN_WORDS - 1; i >= 0; i--)
                {
                    if (Rem[i] != M->W[i]) { Ge = Rem[i] > M->W[i] ? 1 : 0; break; }
                }
            }
            if (Ge)
            {
                uint64_t Borrow = 0;
                for (i = 0; i < S1_BN_WORDS; i++)
                {
                    uint64_t D = (uint64_t)Rem[i] - M->W[i] - Borrow;
                    Rem[i] = (uint32_t)D;
                    Borrow = (D >> 32) & 1;
                }
                if (Borrow)
                    Rem[64]--;
            }
        }
    }
    memcpy(R->W, Rem, sizeof(R->W));
}

static void
S1BnMulMod(S1_BN *R, const S1_BN *A, const S1_BN *B, const S1_BN *M)
{
    uint32_t P[S1_BN_DWORDS];
    uint64_t Carry;
    int i, j;

    memset(P, 0, sizeof(P));
    for (i = 0; i < S1_BN_WORDS; i++)
    {
        Carry = 0;
        for (j = 0; j < S1_BN_WORDS; j++)
        {
            uint64_t T = (uint64_t)A->W[i] * B->W[j] + P[i + j] + Carry;
            P[i + j] = (uint32_t)T;
            Carry = T >> 32;
        }
        P[i + S1_BN_WORDS] += (uint32_t)Carry;
    }

    S1BnDivModRemainder(P, M, R);
}

/* M = Base^Exp mod Mod, square-and-multiply over the exponent bits. */
static void
S1BnPowMod(S1_BN *Result, const S1_BN *Base, const S1_BN *Exp, const S1_BN *Mod)
{
    S1_BN R, B;
    int Bit, Started = 0;

    memset(&R, 0, sizeof(R));
    R.W[0] = 1;
    B = *Base;

    for (Bit = S1_BN_WORDS * 32 - 1; Bit >= 0; Bit--)
    {
        if (Started)
            S1BnMulMod(&R, &R, &R, Mod);
        if ((Exp->W[Bit / 32] >> (Bit % 32)) & 1)
        {
            if (!Started)
            {
                R = B;
                Started = 1;
            }
            else
            {
                S1BnMulMod(&R, &R, &B, Mod);
            }
        }
        if (!Started && Bit == 0)
        {
            /* exponent zero: result stays 1 */
        }
    }
    *Result = R;
}

S1Bool
S1Rsa2048Sha256Verify(const uint8_t Modulus[S1_RSA2048_SIZE],
                      uint32_t Exponent,
                      const uint8_t *Message, size_t MessageLength,
                      const uint8_t Signature[S1_RSA2048_SIZE])
{
    static const uint8_t Sha256Prefix[] =
    {
        0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65,
        0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20
    };
    S1_BN N, E, S, M;
    uint8_t Expected[S1_RSA2048_SIZE];
    uint8_t Digest[S1_SHA256_SIZE];
    S1_SHA256_CTX Ctx;
    size_t PadLen, i;

    S1Sha256Init(&Ctx);
    S1Sha256Update(&Ctx, Message, MessageLength);
    S1Sha256Final(&Ctx, Digest);

    /* PKCS#1 v1.5 expected block: 0x00 0x01 PS(0xFF...) 0x00 T, where T is
     * the DigestInfo prefix plus the 32-byte SHA-256. */
    memset(Expected, 0xFF, sizeof(Expected));
    Expected[0] = 0x00;
    Expected[1] = 0x01;
    memcpy(Expected + sizeof(Expected) - sizeof(Sha256Prefix) - S1_SHA256_SIZE,
           Sha256Prefix, sizeof(Sha256Prefix));
    memcpy(Expected + sizeof(Expected) - S1_SHA256_SIZE, Digest, S1_SHA256_SIZE);
    PadLen = sizeof(Expected) - 2 - sizeof(Sha256Prefix) - S1_SHA256_SIZE - 1;
    for (i = 2; i < 2 + PadLen; i++)
        Expected[i] = 0xFF;
    Expected[2 + PadLen] = 0x00; /* the 0x00 separator before the DigestInfo */

    /* The verifier is a trust root: pin the development-key shape. An
     * attacker-chosen exponent of 1 would accept a self-made block without
     * any private key, and a degenerate modulus breaks the math. */
    if (Exponent != 65537)
        return S1_FALSE;

    S1BnFromBytes(&N, Modulus);
    if (!(N.W[S1_BN_WORDS - 1] & 0x80000000) || !(N.W[0] & 1))
        return S1_FALSE; /* modulus must be exactly 2048-bit and odd */
    S1BnFromBytes(&S, Signature);
    if (S1BnCmp(&S, &N) >= 0)
        return S1_FALSE;
    memset(&E, 0, sizeof(E));
    E.W[0] = Exponent;

    S1BnPowMod(&M, &S, &E, &N);

    {
        uint8_t Actual[S1_RSA2048_SIZE];
        for (i = 0; i < S1_RSA2048_SIZE; i++)
            Actual[i] = (uint8_t)(M.W[(S1_RSA2048_SIZE - 1 - i) / 4] >> (((S1_RSA2048_SIZE - 1 - i) % 4) * 8));
        return memcmp(Actual, Expected, sizeof(Expected)) == 0 ? S1_TRUE : S1_FALSE;
    }
}

/* ------------------------------------------------------------- JSON reader */

typedef enum _S1_JSON_KIND
{
    S1_JSON_OBJECT, S1_JSON_ARRAY, S1_JSON_STRING, S1_JSON_NUMBER,
    S1_JSON_TRUE, S1_JSON_FALSE, S1_JSON_NULL
} S1_JSON_KIND;

struct _S1_JSON
{
    S1_JSON_KIND Kind;
    const char *Start;   /* string content for scalars */
    size_t Length;       /* content length for strings */
    uint64_t Number;
    struct _S1_JSON *FirstChild;
    struct _S1_JSON *NextSibling;
    char *Key;           /* object member key (heap) */
    size_t KeyLength;
    int ArrayIndex;
};

typedef struct _S1_JSON_POOL
{
    struct _S1_JSON *Nodes;
    size_t Count, Capacity;
    char *Strings;
    size_t StringsUsed, StringsCapacity;
} S1_JSON_POOL;

static void *
S1PoolAlloc(S1_JSON_POOL *Pool, size_t Size, int IsNode)
{
    if (IsNode)
    {
        if (Pool->Count == Pool->Capacity)
            return NULL;
        return &Pool->Nodes[Pool->Count++];
    }
    if (Pool->StringsUsed + Size > Pool->StringsCapacity)
        return NULL;
    {
        void *P = Pool->Strings + Pool->StringsUsed;
        Pool->StringsUsed += Size;
        return P;
    }
}

typedef struct _S1_JSON_PARSER
{
    const char *Text;
    size_t Length;
    size_t Pos;
    S1_JSON_POOL Pool;
} S1_JSON_PARSER;

static void
S1JsonSkipWs(S1_JSON_PARSER *P)
{
    while (P->Pos < P->Length)
    {
        char C = P->Text[P->Pos];
        if (C == ' ' || C == '\t' || C == '\r' || C == '\n')
            P->Pos++;
        else
            break;
    }
}

static S1_JSON *S1JsonParseValue(S1_JSON_PARSER *P);

static S1_JSON *
S1JsonNew(S1_JSON_PARSER *P, S1_JSON_KIND Kind)
{
    S1_JSON *N = (S1_JSON *)S1PoolAlloc(&P->Pool, 0, 1);
    if (!N) return NULL;
    memset(N, 0, sizeof(*N));
    N->Kind = Kind;
    N->ArrayIndex = -1;
    return N;
}

static S1Bool
S1JsonParseStringRaw(S1_JSON_PARSER *P, char **Out, size_t *OutLen)
{
    size_t Start, Cap, Used = 0;
    char *Buf;
    if (P->Pos >= P->Length || P->Text[P->Pos] != '"')
        return S1_FALSE;
    P->Pos++;
    Cap = P->Length - P->Pos + 1;
    Buf = (char *)S1PoolAlloc(&P->Pool, Cap, 0);
    if (!Buf)
    {
        /* strings pool exhausted path: use the raw pool for a fresh block */
        return S1_FALSE;
    }
    Start = P->Pos;
    while (P->Pos < P->Length && P->Text[P->Pos] != '"')
    {
        char C = P->Text[P->Pos];
        if (C == '\\')
        {
            char E;
            if (P->Pos + 1 >= P->Length) return S1_FALSE;
            E = P->Text[P->Pos + 1];
            P->Pos += 2;
            if (E == 'u')
            {
                uint32_t Code = 0;
                int i;
                if (P->Pos + 4 > P->Length) return S1_FALSE;
                for (i = 0; i < 4; i++)
                {
                    char H = P->Text[P->Pos + i];
                    Code <<= 4;
                    if (H >= '0' && H <= '9') Code |= (uint32_t)(H - '0');
                    else if (H >= 'a' && H <= 'f') Code |= (uint32_t)(H - 'a' + 10);
                    else if (H >= 'A' && H <= 'F') Code |= (uint32_t)(H - 'A' + 10);
                    else return S1_FALSE;
                }
                P->Pos += 4;
                /* BMP only; encode as UTF-8 (1..3 bytes) */
                if (Code < 0x80)
                {
                    if (Used + 1 > Cap) return S1_FALSE;
                    Buf[Used++] = (char)Code;
                }
                else if (Code < 0x800)
                {
                    if (Used + 2 > Cap) return S1_FALSE;
                    Buf[Used++] = (char)(0xC0 | (Code >> 6));
                    Buf[Used++] = (char)(0x80 | (Code & 0x3F));
                }
                else
                {
                    if (Used + 3 > Cap) return S1_FALSE;
                    Buf[Used++] = (char)(0xE0 | (Code >> 12));
                    Buf[Used++] = (char)(0x80 | ((Code >> 6) & 0x3F));
                    Buf[Used++] = (char)(0x80 | (Code & 0x3F));
                }
            }
            else
            {
                char OutC = 0;
                switch (E)
                {
                    case '"': OutC = '"'; break;
                    case '\\': OutC = '\\'; break;
                    case '/': OutC = '/'; break;
                    case 'b': OutC = '\b'; break;
                    case 'f': OutC = '\f'; break;
                    case 'n': OutC = '\n'; break;
                    case 'r': OutC = '\r'; break;
                    case 't': OutC = '\t'; break;
                    default: return S1_FALSE;
                }
                if (Used + 1 > Cap) return S1_FALSE;
                Buf[Used++] = OutC;
            }
        }
        else
        {
            if (Used + 1 > Cap) return S1_FALSE;
            Buf[Used++] = C;
            P->Pos++;
        }
    }
    if (P->Pos >= P->Length)
        return S1_FALSE;
    P->Pos++; /* closing quote */
    (void)Start;
    *Out = Buf;
    *OutLen = Used;
    return S1_TRUE;
}

static S1_JSON *
S1JsonParseObject(S1_JSON_PARSER *P)
{
    S1_JSON *Obj = S1JsonNew(P, S1_JSON_OBJECT);
    S1_JSON **Link = &Obj->FirstChild;
    if (P->Text[P->Pos] != '{') return NULL;
    P->Pos++;
    S1JsonSkipWs(P);
    if (P->Pos < P->Length && P->Text[P->Pos] == '}')
    {
        P->Pos++;
        return Obj;
    }
    for (;;)
    {
        S1_JSON *Member;
        char *Key;
        size_t KeyLen;
        S1JsonSkipWs(P);
        if (!S1JsonParseStringRaw(P, &Key, &KeyLen))
            return NULL;
        S1JsonSkipWs(P);
        if (P->Pos >= P->Length || P->Text[P->Pos] != ':')
            return NULL;
        P->Pos++;
        Member = S1JsonParseValue(P);
        if (!Member)
            return NULL;
        Member->Key = Key;
        Member->KeyLength = KeyLen;
        *Link = Member;
        Link = &Member->NextSibling;
        S1JsonSkipWs(P);
        if (P->Pos < P->Length && P->Text[P->Pos] == ',')
        {
            P->Pos++;
            continue;
        }
        if (P->Pos < P->Length && P->Text[P->Pos] == '}')
        {
            P->Pos++;
            return Obj;
        }
        return NULL;
    }
}

static S1_JSON *
S1JsonParseArray(S1_JSON_PARSER *P)
{
    S1_JSON *Arr = S1JsonNew(P, S1_JSON_ARRAY);
    S1_JSON **Link = &Arr->FirstChild;
    int Index = 0;
    if (P->Text[P->Pos] != '[') return NULL;
    P->Pos++;
    S1JsonSkipWs(P);
    if (P->Pos < P->Length && P->Text[P->Pos] == ']')
    {
        P->Pos++;
        return Arr;
    }
    for (;;)
    {
        S1_JSON *Item = S1JsonParseValue(P);
        if (!Item)
            return NULL;
        Item->ArrayIndex = Index++;
        *Link = Item;
        Link = &Item->NextSibling;
        S1JsonSkipWs(P);
        if (P->Pos < P->Length && P->Text[P->Pos] == ',')
        {
            P->Pos++;
            continue;
        }
        if (P->Pos < P->Length && P->Text[P->Pos] == ']')
        {
            P->Pos++;
            return Arr;
        }
        return NULL;
    }
}

static S1_JSON *
S1JsonParseValue(S1_JSON_PARSER *P)
{
    S1_JSON *V;
    S1JsonSkipWs(P);
    if (P->Pos >= P->Length)
        return NULL;
    switch (P->Text[P->Pos])
    {
        case '{': return S1JsonParseObject(P);
        case '[': return S1JsonParseArray(P);
        case '"':
            V = S1JsonNew(P, S1_JSON_STRING);
            if (!V) return NULL;
            if (!S1JsonParseStringRaw(P, (char **)&V->Start, &V->Length))
                return NULL;
            return V;
        case 't':
            if (P->Pos + 4 <= P->Length && memcmp(P->Text + P->Pos, "true", 4) == 0)
            {
                P->Pos += 4;
                return S1JsonNew(P, S1_JSON_TRUE);
            }
            return NULL;
        case 'f':
            if (P->Pos + 5 <= P->Length && memcmp(P->Text + P->Pos, "false", 5) == 0)
            {
                P->Pos += 5;
                return S1JsonNew(P, S1_JSON_FALSE);
            }
            return NULL;
        case 'n':
            if (P->Pos + 4 <= P->Length && memcmp(P->Text + P->Pos, "null", 4) == 0)
            {
                P->Pos += 4;
                return S1JsonNew(P, S1_JSON_NULL);
            }
            return NULL;
        default:
        {
            size_t Start = P->Pos;
            uint64_t N = 0;
            V = S1JsonNew(P, S1_JSON_NUMBER);
            if (!V) return NULL;
            while (P->Pos < P->Length && P->Text[P->Pos] >= '0' && P->Text[P->Pos] <= '9')
            {
                N = N * 10 + (uint64_t)(P->Text[P->Pos] - '0');
                P->Pos++;
            }
            if (P->Pos == Start)
                return NULL;
            V->Number = N;
            return V;
        }
    }
}

S1Bool
S1JsonParse(const char *Text, size_t Length, S1_JSON **Out)
{
    S1_JSON_PARSER P;
    S1_JSON *Root;
    static struct _S1_JSON StaticNodes[512];
    static char StaticStrings[65536];

    memset(&P, 0, sizeof(P));
    P.Text = Text;
    P.Length = Length;
    P.Pool.Nodes = StaticNodes;
    P.Pool.Capacity = 512;
    P.Pool.Strings = StaticStrings;
    P.Pool.StringsCapacity = sizeof(StaticStrings);

    Root = S1JsonParseValue(&P);
    if (!Root)
        return S1_FALSE;
    S1JsonSkipWs(&P);
    if (P.Pos != P.Length)
        return S1_FALSE; /* trailing garbage */
    *Out = Root;
    return S1_TRUE;
}

void
S1JsonFree(S1_JSON *Json)
{
    (void)Json; /* the host/nativetest builds use static pools */
}

static S1_JSON *
S1JsonFind(const S1_JSON *Json, const char *Path)
{
    char Seg[32];
    const char *Dot = strchr(Path, '.');
    size_t SegLen = Dot ? (size_t)(Dot - Path) : strlen(Path);
    S1_JSON *Cur = (S1_JSON *)Json;

    if (SegLen >= sizeof(Seg))
        return NULL;
    memcpy(Seg, Path, SegLen);
    Seg[SegLen] = 0;

    if (Cur->Kind == S1_JSON_ARRAY)
    {
        S1_JSON *Child;
        int Wanted = atoi(Seg);
        for (Child = Cur->FirstChild; Child; Child = Child->NextSibling)
        {
            if (Child->ArrayIndex == Wanted)
                break;
        }
        Cur = Child;
    }
    else
    {
        S1_JSON *Child;
        for (Child = Cur->FirstChild; Child; Child = Child->NextSibling)
        {
            if (Child->KeyLength == SegLen && memcmp(Child->Key, Seg, SegLen) == 0)
                break;
        }
        Cur = Child;
    }
    if (!Cur)
        return NULL;
    return Dot ? S1JsonFind(Cur, Dot + 1) : Cur;
}

S1Bool
S1JsonGetString(const S1_JSON *Json, const char *Path, const char **Value, size_t *ValueLength)
{
    S1_JSON *N = S1JsonFind(Json, Path);
    if (!N || N->Kind != S1_JSON_STRING)
        return S1_FALSE;
    *Value = N->Start;
    *ValueLength = N->Length;
    return S1_TRUE;
}

S1Bool
S1JsonGetUInt(const S1_JSON *Json, const char *Path, uint64_t *Value)
{
    S1_JSON *N = S1JsonFind(Json, Path);
    if (!N || N->Kind != S1_JSON_NUMBER)
        return S1_FALSE;
    *Value = N->Number;
    return S1_TRUE;
}

S1Bool
S1JsonGetBool(const S1_JSON *Json, const char *Path, S1Bool *Value)
{
    S1_JSON *N = S1JsonFind(Json, Path);
    if (!N)
        return S1_FALSE;
    if (N->Kind == S1_JSON_TRUE) { *Value = S1_TRUE; return S1_TRUE; }
    if (N->Kind == S1_JSON_FALSE) { *Value = S1_FALSE; return S1_TRUE; }
    return S1_FALSE;
}

size_t
S1JsonGetArrayCount(const S1_JSON *Json, const char *Path)
{
    S1_JSON *N = S1JsonFind(Json, Path);
    S1_JSON *Child;
    size_t Count = 0;
    if (!N || N->Kind != S1_JSON_ARRAY)
        return 0;
    for (Child = N->FirstChild; Child; Child = Child->NextSibling)
        Count++;
    return Count;
}

/* --------------------------------------------------------------- plan model */

static S1Bool
S1CopyString(char *Dst, size_t DstSize, const char *Src, size_t SrcLen)
{
    if (SrcLen == 0 || SrcLen >= DstSize)
        return S1_FALSE;
    memcpy(Dst, Src, SrcLen);
    Dst[SrcLen] = 0;
    return S1_TRUE;
}

S1Bool
S1PlanParse(const char *Text, size_t Length, S1_PLAN *Plan)
{
    S1_JSON *Root;
    const char *S;
    size_t SL;
    uint64_t N;
    size_t Count, i;

    memset(Plan, 0, sizeof(*Plan));
    if (!S1JsonParse(Text, Length, &Root))
        return S1_FALSE;
    if (!S1JsonGetUInt(Root, "schema", &N) || N != 1)
        return S1_FALSE;
    Plan->Schema = 1;
    if (!S1JsonGetString(Root, "transactionId", &S, &SL) || !S1CopyString(Plan->TransactionId, sizeof(Plan->TransactionId), S, SL))
        return S1_FALSE;
    if (!S1JsonGetString(Root, "target.machineId", &S, &SL) || !S1CopyString(Plan->MachineId, sizeof(Plan->MachineId), S, SL))
        return S1_FALSE;
    if (!S1JsonGetString(Root, "target.arch", &S, &SL) || !S1CopyString(Plan->Arch, sizeof(Plan->Arch), S, SL))
        return S1_FALSE;
    if (!S1JsonGetString(Root, "package.id", &S, &SL) || !S1CopyString(Plan->PackageId, sizeof(Plan->PackageId), S, SL))
        return S1_FALSE;
    if (!S1JsonGetString(Root, "package.version", &S, &SL) || !S1CopyString(Plan->Version, sizeof(Plan->Version), S, SL))
        return S1_FALSE;
    if (!S1JsonGetString(Root, "package.digest", &S, &SL) || SL != S1_SHA256_SIZE * 2 ||
        !S1HexDecode(S, Plan->ManifestDigest, S1_SHA256_SIZE))
        return S1_FALSE;
    if (!S1JsonGetString(Root, "operation", &S, &SL) || !S1CopyString(Plan->Operation, sizeof(Plan->Operation), S, SL))
        return S1_FALSE;

    Count = S1JsonGetArrayCount(Root, "operations");
    if (Count == 0 || Count > S1_MAX_OPERATIONS)
        return S1_FALSE;
    Plan->OperationCount = Count;

    for (i = 0; i < Count; i++)
    {
        char Path[64];
        S1_PLAN_OP *Op = &Plan->Operations[i];
        static const struct { const char *Name; S1_OP_TYPE Type; } Types[] =
        {
            { "replace-file", S1_OP_REPLACE_FILE },
            { "add-file", S1_OP_ADD_FILE },
        };
        size_t t;
        S1Bool TypeOk = S1_FALSE;

        Op->Seq = (uint32_t)i + 1;

        snprintf(Path, sizeof(Path), "operations.%lu.type", (unsigned long)i);
        if (!S1JsonGetString(Root, Path, &S, &SL))
            return S1_FALSE;
        for (t = 0; t < sizeof(Types) / sizeof(Types[0]); t++)
        {
            if (SL == strlen(Types[t].Name) && memcmp(S, Types[t].Name, SL) == 0)
            {
                Op->Type = Types[t].Type;
                TypeOk = S1_TRUE;
                break;
            }
        }
        if (!TypeOk)
            return S1_FALSE; /* unknown operation type: never applied */

        snprintf(Path, sizeof(Path), "operations.%lu.component", (unsigned long)i);
        if (!S1JsonGetString(Root, Path, &S, &SL) || !S1CopyString(Op->Component, sizeof(Op->Component), S, SL))
            return S1_FALSE;
        snprintf(Path, sizeof(Path), "operations.%lu.source", (unsigned long)i);
        if (!S1JsonGetString(Root, Path, &S, &SL) || !S1CopyString(Op->Source, sizeof(Op->Source), S, SL))
            return S1_FALSE;
        snprintf(Path, sizeof(Path), "operations.%lu.target", (unsigned long)i);
        if (!S1JsonGetString(Root, Path, &S, &SL) || !S1CopyString(Op->Target, sizeof(Op->Target), S, SL))
            return S1_FALSE;
        snprintf(Path, sizeof(Path), "operations.%lu.sha256", (unsigned long)i);
        if (!S1JsonGetString(Root, Path, &S, &SL) || SL != S1_SHA256_SIZE * 2 ||
            !S1HexDecode(S, Op->ExpectedSha256, S1_SHA256_SIZE))
            return S1_FALSE;
        /* originalSha256 is REQUIRED for replace-file (owner decision,
         * fail closed): the rollback re-hashes the backup against it, and
         * a plan that drops or mismatches it must be refused, never
         * silently restored. add-file has no backup and carries none. */
        if (Op->Type == S1_OP_REPLACE_FILE)
        {
            snprintf(Path, sizeof(Path), "operations.%lu.originalSha256", (unsigned long)i);
            if (!S1JsonGetString(Root, Path, &S, &SL) || SL != S1_SHA256_SIZE * 2 ||
                !S1HexDecode(S, Op->OriginalSha256, S1_SHA256_SIZE))
                return S1_FALSE;
            Op->HasOriginal = S1_TRUE;
        }
    }
    return S1_TRUE;
}

void
S1TransactionId(const S1_PLAN *Plan, char Out[65])
{
    S1_SHA256_CTX Ctx;
    uint8_t Digest[S1_SHA256_SIZE];
    char Manifest[65];

    S1HexEncode(Plan->ManifestDigest, S1_SHA256_SIZE, Manifest);
    S1Sha256Init(&Ctx);
    S1Sha256Update(&Ctx, Plan->MachineId, strlen(Plan->MachineId));
    S1Sha256Update(&Ctx, "\n", 1);
    S1Sha256Update(&Ctx, Plan->PackageId, strlen(Plan->PackageId));
    S1Sha256Update(&Ctx, "\n", 1);
    S1Sha256Update(&Ctx, Plan->Version, strlen(Plan->Version));
    S1Sha256Update(&Ctx, "\n", 1);
    S1Sha256Update(&Ctx, Manifest, 64);
    S1Sha256Update(&Ctx, "\n", 1);
    S1Sha256Update(&Ctx, Plan->Operation, strlen(Plan->Operation));
    S1Sha256Final(&Ctx, Digest);
    S1HexEncode(Digest, S1_SHA256_SIZE, Out);
}

S1Bool
S1PlanCheckTransactionId(const S1_PLAN *Plan)
{
    char TxId[65];

    S1TransactionId(Plan, TxId);
    return strcmp(TxId, Plan->TransactionId) == 0 ? S1_TRUE : S1_FALSE;
}

/* ---------------------------------------------------------------- journal */

S1Bool
S1JournalAppend(void *IoCtx,
                S1Bool (*Append)(void *IoCtx, const char *Bytes, size_t Length),
                uint64_t Sequence,
                const uint8_t PrevHash[S1_SHA256_SIZE],
                const char *TransactionId,
                const char *Phase,
                int Operation,
                const char *Result,
                const char *Error,
                uint8_t OutHash[S1_SHA256_SIZE])
{
    char Entry[S1_JOURNAL_MAX_ENTRY];
    char PrevHex[65];
    int Len;
    S1_SHA256_CTX Ctx;

    S1HexEncode(PrevHash, S1_SHA256_SIZE, PrevHex);
    if (Operation >= 0)
        Len = snprintf(Entry, sizeof(Entry),
                       "{\"seq\":%llu,\"prev\":\"%s\",\"ts\":\"boot\",\"tx\":\"%s\",\"phase\":\"%s\",\"op\":%d,\"result\":\"%s\",\"error\":%s%s%s}\n",
                       (unsigned long long)Sequence, PrevHex, TransactionId, Phase,
                       Operation, Result,
                       Error ? "\"" : "null", Error ? Error : "", Error ? "\"" : "");
    else
        Len = snprintf(Entry, sizeof(Entry),
                       "{\"seq\":%llu,\"prev\":\"%s\",\"ts\":\"boot\",\"tx\":\"%s\",\"phase\":\"%s\",\"op\":null,\"result\":\"%s\",\"error\":%s%s%s}\n",
                       (unsigned long long)Sequence, PrevHex, TransactionId, Phase,
                       Result,
                       Error ? "\"" : "null", Error ? Error : "", Error ? "\"" : "");
    if (Len <= 0 || (size_t)Len >= sizeof(Entry))
        return S1_FALSE;
    if (!Append(IoCtx, Entry, (size_t)Len))
        return S1_FALSE;
    S1Sha256Init(&Ctx);
    S1Sha256Update(&Ctx, Entry, (size_t)Len - 1); /* the hash covers the entry without its newline */
    S1Sha256Final(&Ctx, OutHash);
    return S1_TRUE;
}

uint64_t
S1JournalVerifyChain(const char *Text, size_t Length, S1Bool *Intact)
{
    uint8_t Prev[S1_SHA256_SIZE];
    uint64_t LastSeq = 0, Seq;
    const char *Cursor = Text, *End = Text + Length;
    S1Bool Ok = S1_TRUE;

    memset(Prev, 0, sizeof(Prev));
    while (Cursor < End && *Cursor)
    {
        const char *LineEnd = memchr(Cursor, '\n', (size_t)(End - Cursor));
        size_t LineLen = LineEnd ? (size_t)(LineEnd - Cursor) : (size_t)(End - Cursor);
        S1_JSON *Entry;
        const char *PrevHex;
        size_t PrevHexLen;
        uint8_t ExpectedPrev[S1_SHA256_SIZE];
        uint8_t Actual[S1_SHA256_SIZE];
        S1_SHA256_CTX Ctx;

        if (LineLen == 0)
            break;
        if (!S1JsonParse(Cursor, LineLen, &Entry))
        {
            Ok = S1_FALSE;
            break;
        }
        if (!S1JsonGetUInt(Entry, "seq", &Seq))
        {
            Ok = S1_FALSE;
            break;
        }
        if (!S1JsonGetString(Entry, "prev", &PrevHex, &PrevHexLen))
        {
            Ok = S1_FALSE;
            break;
        }
        if (PrevHexLen != S1_SHA256_SIZE * 2 || !S1HexDecode(PrevHex, ExpectedPrev, S1_SHA256_SIZE))
        {
            Ok = S1_FALSE;
            break;
        }
        if (memcmp(ExpectedPrev, Prev, sizeof(Prev)) != 0)
        {
            Ok = S1_FALSE;
            break;
        }
        S1Sha256Init(&Ctx);
        S1Sha256Update(&Ctx, Cursor, LineLen);
        S1Sha256Final(&Ctx, Actual);
        memcpy(Prev, Actual, sizeof(Prev));
        LastSeq = Seq;
        Cursor = LineEnd ? LineEnd + 1 : End;
    }
    *Intact = Ok;
    return LastSeq;
}

/* ------------------------------------------------------------ attempt.json */

S1Bool
S1AttemptParse(const char *Text, size_t Length, S1_ATTEMPT *Attempt)
{
    S1_JSON *Root;
    const char *S;
    size_t SL;
    uint64_t N;
    S1Bool B;

    memset(Attempt, 0, sizeof(*Attempt));
    if (!S1JsonParse(Text, Length, &Root))
        return S1_FALSE;
    if (!S1JsonGetUInt(Root, "schema", &N) || N != 1)
        return S1_FALSE;
    if (!S1JsonGetString(Root, "tx", &S, &SL) || !S1CopyString(Attempt->TransactionId, sizeof(Attempt->TransactionId), S, SL))
        return S1_FALSE;
    if (!S1JsonGetUInt(Root, "attempt", &N) || N > 1000000)
        return S1_FALSE;
    Attempt->Attempt = (uint32_t)N;
    if (!S1JsonGetBool(Root, "healthConfirmed", &B))
        return S1_FALSE;
    Attempt->HealthConfirmed = B;
    if (!S1JsonGetBool(Root, "failureRecorded", &B))
        return S1_FALSE;
    Attempt->FailureRecorded = B;
    if (S1JsonGetString(Root, "lastResult", &S, &SL))
        S1CopyString(Attempt->LastResult, sizeof(Attempt->LastResult), S, SL);
    return S1_TRUE;
}

size_t
S1AttemptSerialize(const S1_ATTEMPT *Attempt, char *Buffer, size_t BufferSize)
{
    int Len = snprintf(Buffer, BufferSize,
                       "{\"schema\":1,\"tx\":\"%s\",\"attempt\":%lu,\"healthConfirmed\":%s,\"failureRecorded\":%s,\"lastResult\":\"%s\"}",
                       Attempt->TransactionId,
                       (unsigned long)Attempt->Attempt,
                       Attempt->HealthConfirmed ? "true" : "false",
                       Attempt->FailureRecorded ? "true" : "false",
                       Attempt->LastResult);
    return Len > 0 ? (size_t)Len : 0;
}

/* ------------------------------------------------------------- rollback rule */

S1Bool
S1ShouldRollback(uint32_t Attempt, S1Bool HealthConfirmed)
{
    return (!HealthConfirmed && Attempt >= S1_MAX_BOOT_ATTEMPTS) ? S1_TRUE : S1_FALSE;
}

S1Bool
S1IsVerifiableFailure(S1Bool FailureRecorded)
{
    return FailureRecorded;
}

#ifdef S1_HOST_TEST
/* Host-only unit hooks for the logic tests; never compiled into the
 * native applier. */
void S1TestBnMulMod(uint32_t A, uint32_t B, uint32_t M, uint32_t *Out)
{
    S1_BN Ba, Bb, Bm, Br;
    memset(&Ba, 0, sizeof(Ba)); Ba.W[0] = A;
    memset(&Bb, 0, sizeof(Bb)); Bb.W[0] = B;
    memset(&Bm, 0, sizeof(Bm)); Bm.W[0] = M;
    S1BnMulMod(&Br, &Ba, &Bb, &Bm);
    *Out = Br.W[0];
}

void S1TestBnMulModW(const uint32_t *Aw, const uint32_t *Bw, const uint32_t *Mw, uint32_t *Out)
{
    S1_BN Ba, Bb, Bm, Br;
    memset(&Ba, 0, sizeof(Ba)); memcpy(Ba.W, Aw, 8);
    memset(&Bb, 0, sizeof(Bb)); memcpy(Bb.W, Bw, 8);
    memset(&Bm, 0, sizeof(Bm)); memcpy(Bm.W, Mw, 8);
    S1BnMulMod(&Br, &Ba, &Bb, &Bm);
    memcpy(Out, Br.W, 8);
}
void S1TestBnPowMod(uint32_t Base, uint32_t Exp, uint32_t M, uint32_t *Out)
{
    S1_BN Bb, Be, Bm, Br;
    memset(&Bb, 0, sizeof(Bb)); Bb.W[0] = Base;
    memset(&Be, 0, sizeof(Be)); Be.W[0] = Exp;
    memset(&Bm, 0, sizeof(Bm)); Bm.W[0] = M;
    S1BnPowMod(&Br, &Bb, &Be, &Bm);
    *Out = Br.W[0];
}
#endif /* S1_HOST_TEST */

