/*
 * PROJECT:     LiberNT Update
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Host logic tests for the S1 boot applier core
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Runs on the host (and as a LiberNT apitest): plan parse + signature verify,
 * hash-chained journal integrity, deterministic transactionId and the
 * two-attempt rollback rule. Fixtures were generated with the development
 * signing key (private key kept outside the repository) and an attacker key.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "s1core.h"

static int g_Failures = 0;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); g_Failures++; } \
        else { printf("ok:   %s\n", msg); } \
    } while (0)

static uint8_t g_DevPub[S1_RSA2048_SIZE] = {
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

static uint8_t *
ReadFile(const char *Path, size_t *Size)
{
    FILE *F = fopen(Path, "rb");
    uint8_t *Buf;
    long Len;
    if (!F) return NULL;
    fseek(F, 0, SEEK_END);
    Len = ftell(F);
    fseek(F, 0, SEEK_SET);
    Buf = (uint8_t *)malloc((size_t)Len);
    if (!Buf) { fclose(F); return NULL; }
    if (fread(Buf, 1, (size_t)Len, F) != (size_t)Len) { free(Buf); fclose(F); return NULL; }
    fclose(F);
    *Size = (size_t)Len;
    return Buf;
}

struct S1_TEST_CAPTURE
{
    char *Buf;
    size_t Used;
};

static S1Bool
S1TestAppender(void *IoCtx, const char *Bytes, size_t Length)
{
    struct S1_TEST_CAPTURE *C = (struct S1_TEST_CAPTURE *)IoCtx;
    memcpy(C->Buf + C->Used, Bytes, Length);
    C->Used += Length;
    return S1_TRUE;
}

static int
TestPlanAndSignature(const char *FixtureDir)
{
    char Path[512];
    size_t PlanSize, SigSize;
    uint8_t *PlanText, *Sig;
    S1_PLAN Plan;

    snprintf(Path, sizeof(Path), "%s/valid.plan.json", FixtureDir);
    PlanText = ReadFile(Path, &PlanSize);
    snprintf(Path, sizeof(Path), "%s/valid.sig.bin", FixtureDir);
    Sig = ReadFile(Path, &SigSize);
    if (!PlanText || !Sig) { printf("fixtures missing in %s\n", FixtureDir); return 1; }

    CHECK(S1PlanParse((const char *)PlanText, PlanSize, &Plan), "valid plan parses");
    CHECK(S1PlanCheckTransactionId(&Plan), "plan transactionId derives from its fields");
    CHECK(S1Rsa2048Sha256Verify(g_DevPub, 65537, PlanText, PlanSize, Sig),
          "manifest-style signature verifies with the development key");

    {
        char TxId[65];
        S1TransactionId(&Plan, TxId);
        CHECK(strcmp(TxId, Plan.TransactionId) == 0, "transactionId is deterministic and matches");
    }

    snprintf(Path, sizeof(Path), "%s/attacker.plan.json", FixtureDir);
    free(PlanText); free(Sig);
    PlanText = ReadFile(Path, &PlanSize);
    snprintf(Path, sizeof(Path), "%s/attacker.sig.bin", FixtureDir);
    Sig = ReadFile(Path, &SigSize);
    CHECK(S1PlanParse((const char *)PlanText, PlanSize, &Plan), "attacker-signed manifest parses");
    CHECK(!S1Rsa2048Sha256Verify(g_DevPub, 65537, PlanText, PlanSize, Sig),
          "manifest signature by a foreign key is rejected");
    CHECK(!S1Rsa2048Sha256Verify(g_DevPub, 1, PlanText, PlanSize, Sig),
          "exponent 1 cannot be injected into the trust root");
    CHECK(!S1Rsa2048Sha256Verify(g_DevPub, 3, PlanText, PlanSize, Sig),
          "exponent 3 cannot be injected into the trust root");

    snprintf(Path, sizeof(Path), "%s/wrongtx.plan.json", FixtureDir);
    free(PlanText);
    PlanText = ReadFile(Path, &PlanSize);
    CHECK(S1PlanParse((const char *)PlanText, PlanSize, &Plan), "tampered-transactionId plan parses");
    CHECK(!S1PlanCheckTransactionId(&Plan),
          "tampered transactionId is rejected before any signature check");

    /* structural rejections */
    {
        static const char BadSchema[] = "{\"schema\":2,\"transactionId\":\"x\"}";
        CHECK(!S1PlanParse(BadSchema, sizeof(BadSchema) - 1, &Plan), "unknown schema rejected");
    }
    {
        static const char BadSha[] =
            "{\"schema\":1,\"transactionId\":\"t\",\"target\":{\"machineId\":\"m\",\"arch\":\"a\"},"
            "\"package\":{\"id\":\"p\",\"version\":\"v\",\"digest\":\"0000000000000000000000000000000000000000000000000000000000000000\"},"
            "\"operation\":\"install\",\"operations\":[{\"seq\":1,\"type\":\"replace-file\","
            "\"component\":\"c\",\"source\":\"s\",\"target\":\"t\",\"sha256\":\"zz\"}]}";
        CHECK(!S1PlanParse(BadSha, sizeof(BadSha) - 1, &Plan), "malformed sha256 rejected");
    }
    {
        static const char BadType[] =
            "{\"schema\":1,\"transactionId\":\"t\",\"target\":{\"machineId\":\"m\",\"arch\":\"a\"},"
            "\"package\":{\"id\":\"p\",\"version\":\"v\",\"digest\":\"0000000000000000000000000000000000000000000000000000000000000000\"},"
            "\"operation\":\"install\",\"operations\":[{\"seq\":1,\"type\":\"replace-kernel\","
            "\"component\":\"c\",\"source\":\"s\",\"target\":\"t\",\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}]}";
        CHECK(!S1PlanParse(BadType, sizeof(BadType) - 1, &Plan), "unknown operation type rejected");
    }
    {
        static const char NoOriginal[] =
            "{\"schema\":1,\"transactionId\":\"t\",\"target\":{\"machineId\":\"m\",\"arch\":\"a\"},"
            "\"package\":{\"id\":\"p\",\"version\":\"v\",\"digest\":\"0000000000000000000000000000000000000000000000000000000000000000\"},"
            "\"operation\":\"install\",\"operations\":[{\"seq\":1,\"type\":\"replace-file\","
            "\"component\":\"c\",\"source\":\"s\",\"target\":\"t\",\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}]}";
        CHECK(!S1PlanParse(NoOriginal, sizeof(NoOriginal) - 1, &Plan),
              "replace-file without originalSha256 rejected (required, fail closed)");
    }
    {
        static const char AddNoOriginal[] =
            "{\"schema\":1,\"transactionId\":\"t\",\"target\":{\"machineId\":\"m\",\"arch\":\"a\"},"
            "\"package\":{\"id\":\"p\",\"version\":\"v\",\"digest\":\"0000000000000000000000000000000000000000000000000000000000000000\"},"
            "\"operation\":\"install\",\"operations\":[{\"seq\":1,\"type\":\"add-file\","
            "\"component\":\"c\",\"source\":\"s\",\"target\":\"t\",\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}]}";
        CHECK(S1PlanParse(AddNoOriginal, sizeof(AddNoOriginal) - 1, &Plan),
              "add-file without originalSha256 accepted (no backup)");
    }
    free(PlanText);
    free(Sig);
    return 0;
}

static int
TestJournal(const char *FixtureDir)
{
    char Path[512];
    size_t Size;
    uint8_t *Text;
    S1Bool Intact;
    uint64_t Last;

    snprintf(Path, sizeof(Path), "%s/journal-ok.jsonl", FixtureDir);
    Text = ReadFile(Path, &Size);
    Last = S1JournalVerifyChain((const char *)Text, Size, &Intact);
    CHECK(Intact && Last == 3, "intact 3-entry chain verifies");

    snprintf(Path, sizeof(Path), "%s/journal-truncated.jsonl", FixtureDir);
    free(Text);
    Text = ReadFile(Path, &Size);
    S1JournalVerifyChain((const char *)Text, Size, &Intact);
    CHECK(!Intact, "truncated journal breaks the chain");

    /* append produces a chain the verifier accepts */
    {
        uint8_t Hash1[S1_SHA256_SIZE], Hash2[S1_SHA256_SIZE], Zero[S1_SHA256_SIZE];
        static char Buf[1024];
        struct S1_TEST_CAPTURE Ctx = { Buf, 0 };
        memset(Zero, 0, sizeof(Zero));
        Buf[0] = 0;
        CHECK(S1JournalAppend(&Ctx, S1TestAppender, 1, Zero, "tx", "verified", -1, "ok", NULL, Hash1),
              "first journal entry appends");
        CHECK(S1JournalAppend(&Ctx, S1TestAppender, 2, Hash1, "tx", "applying", 1, "ok", NULL, Hash2),
              "second journal entry appends");
        Last = S1JournalVerifyChain(Buf, Ctx.Used, &Intact);
        CHECK(Intact && Last == 2, "appended two-entry chain verifies");
    }
    free(Text);
    return 0;
}

static int
TestRollbackRule(void)
{
    CHECK(!S1ShouldRollback(0, S1_FALSE), "fresh plan: no rollback");
    CHECK(!S1ShouldRollback(1, S1_FALSE), "first boot without health: no rollback yet");
    CHECK(S1ShouldRollback(2, S1_FALSE), "second boot without health: rollback");
    CHECK(!S1ShouldRollback(2, S1_TRUE), "health confirmed: no rollback");
    CHECK(S1IsVerifiableFailure(S1_TRUE), "failure marker makes it a verifiable failure");
    CHECK(!S1IsVerifiableFailure(S1_FALSE), "no marker: unexpected reboot");
    return 0;
}

static int
TestAttemptRoundTrip(void)
{
    S1_ATTEMPT A = { 1, "abc123", 2, S1_FALSE, S1_TRUE, "failed" };
    char Buf[512];
    S1_ATTEMPT B;

    CHECK(S1AttemptSerialize(&A, Buf, sizeof(Buf)) > 0, "attempt.json serializes");
    CHECK(S1AttemptParse(Buf, strlen(Buf), &B), "attempt.json parses back");
    CHECK(B.Attempt == 2 && B.FailureRecorded && !B.HealthConfirmed &&
          strcmp(B.TransactionId, "abc123") == 0,
          "attempt.json round-trips the rollback markers");
    return 0;
}

static int
TestBn(void)
{
    uint32_t R;
    S1TestBnMulMod(3, 3, 5, &R);
    CHECK(R == 4, "bn: 3*3 mod 5 == 4");
    S1TestBnMulMod(2, 10, 1000, &R);
    CHECK(R == 20, "bn: 2*10 mod 1000 == 20");
    S1TestBnMulMod(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFB, &R);
    CHECK(R == 16, "bn: (2^32-1)^2 mod (2^32-5) == 16");
    {
        /* 2^32 * 2^32 mod 7 == 2^64 mod 7 == 2 */
        static const uint32_t Two32[2] = { 0, 1 };
        static const uint32_t Seven[2] = { 7, 0 };
        uint32_t OutW[2];
        S1TestBnMulModW(Two32, Two32, Seven, OutW);
        CHECK(OutW[0] == 2 && OutW[1] == 0, "bn: 2^64 mod 7 == 2");
    }
    {
        uint32_t P;
        S1TestBnPowMod(3, 5, 7, &P);
        CHECK(P == 5, "bn: 3^5 mod 7 == 5");
        S1TestBnPowMod(2, 16, 65537, &P);
        CHECK(P == (uint32_t)((1ULL << 16) % 65537), "bn: 2^16 mod 65537");
    }
    return 0;
}

static int
TestPamCases(const char *FixtureDir)
{
    /* Portable negative-case fixture from Pam's review harness (run.py):
     * each record is name, RSA-2048 modulus, exponent, message, signature
     * and the expected result; all forged blocks are signed with the real
     * private key so only the parser can reject them. */
    char Path[512];
    size_t Size;
    uint8_t *Data = NULL;
    const uint8_t *Cursor;
    uint32_t Count, i;
    int Rejected = 0;

    snprintf(Path, sizeof(Path), "%s/rsacases.bin", FixtureDir);
    Data = ReadFile(Path, &Size);
    if (!Data)
    {
        printf("rsacases.bin fixture missing in %s (skipped)\n", FixtureDir);
        return 0;
    }
    Cursor = Data;
    memcpy(&Count, Cursor, 4); Cursor += 4;
    for (i = 0; i < Count; i++)
    {
        uint32_t NameLen, MsgLen, Exp;
        char Name[96];
        uint8_t N[S1_RSA2048_SIZE];
        uint8_t Sig[S1_RSA2048_SIZE];
        uint8_t Expected;
        S1Bool Got;

        memcpy(&NameLen, Cursor, 4); Cursor += 4;
        memcpy(&MsgLen, Cursor, 4); Cursor += 4;
        memcpy(Name, Cursor, NameLen < sizeof(Name) - 1 ? NameLen : sizeof(Name) - 1);
        Name[NameLen < sizeof(Name) - 1 ? NameLen : sizeof(Name) - 1] = 0;
        Cursor += NameLen;
        memcpy(N, Cursor, S1_RSA2048_SIZE); Cursor += S1_RSA2048_SIZE;
        memcpy(&Exp, Cursor, 4); Cursor += 4;
        {
            const uint8_t *Msg = Cursor;
            Cursor += MsgLen;
            memcpy(Sig, Cursor, S1_RSA2048_SIZE);
            Cursor += S1_RSA2048_SIZE;
            memcpy(&Expected, Cursor, 1);
            Cursor += 1;
            Got = S1Rsa2048Sha256Verify(N, Exp, Msg, MsgLen, Sig);
        }
        if ((Got == S1_TRUE) != (Expected == 1))
        {
            printf("FAIL: pam case %u (%s): expected %u got %d\n", i, Name, Expected, (int)Got);
            g_Failures++;
        }
        else if (Expected == 0)
            Rejected++;
    }
    free(Data);
    printf("pam cases: %u checked, %d negative cases rejected\n", Count, Rejected);
    return 0;
}

int
main(int argc, char **argv)
{
    const char *FixtureDir = (argc > 1) ? argv[1] : ".";
    TestPlanAndSignature(FixtureDir);
    TestJournal(FixtureDir);
    TestRollbackRule();
    TestAttemptRoundTrip();
    TestBn();
    TestPamCases(FixtureDir);
    if (g_Failures)
    {
        printf("%d FAILURES\n", g_Failures);
        return 1;
    }
    printf("ALL S1 CORE TESTS PASSED\n");
    return 0;
}
