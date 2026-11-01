/*
 * PROJECT:     LiberNT Update
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Stage-1 boot applier core: pure logic, no OS dependencies
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Portable core of LiberNTUpdatePending.exe: SHA-256 (FIPS 180-4), RSA-2048
 * PKCS#1 v1.5 signature verification over SHA-256, a minimal JSON reader for
 * the S1 contracts (plan.json / attempt.json), the hash-chained journal, the
 * deterministic transaction id and the two-attempt rollback rule. This file
 * compiles for the native applier and for the host logic tests alike; all
 * system I/O goes through the S1Io callbacks supplied by the caller.
 */

#ifndef S1CORE_H
#define S1CORE_H

#include <stdint.h>
#include <stddef.h>

typedef int S1Bool;
#define S1_TRUE 1
#define S1_FALSE 0

/* ------------------------------------------------------------------ SHA-256 */

#define S1_SHA256_SIZE 32
#define S1_MAX_BOOT_ATTEMPTS 2

typedef struct _S1_SHA256_CTX
{
    uint32_t State[8];
    uint64_t Total;
    uint8_t Block[64];
    size_t BlockLen;
} S1_SHA256_CTX;

void S1Sha256Init(S1_SHA256_CTX *Ctx);
void S1Sha256Update(S1_SHA256_CTX *Ctx, const void *Data, size_t Length);
void S1Sha256Final(S1_SHA256_CTX *Ctx, uint8_t Digest[S1_SHA256_SIZE]);

/* Hex helpers: lowercase, no terminator (caller sizes 2*len+1). */
void S1HexEncode(const uint8_t *Bytes, size_t Length, char *Out);
S1Bool S1HexDecode(const char *Hex, uint8_t *Out, size_t OutLen);

/* ------------------------------------------------------- RSA-2048 verify  */

#define S1_RSA2048_SIZE 256 /* modulus bytes */

/* RSASSA-PKCS1-v1_5 verify with SHA-256: Sig = 256-byte big-endian block. */
S1Bool S1Rsa2048Sha256Verify(const uint8_t Modulus[S1_RSA2048_SIZE],
                             uint32_t Exponent,
                             const uint8_t *Message, size_t MessageLength,
                             const uint8_t Signature[S1_RSA2048_SIZE]);

/* ------------------------------------------------------------- JSON reader */
/* Minimal, allocation-free JSON for the S1 contracts: objects, arrays,
 * strings (with \uXXXX BMP escapes), numbers, true/false/null. Lookup by
 * dotted path; the reader never accepts trailing garbage. */

typedef struct _S1_JSON S1_JSON;

S1Bool S1JsonParse(const char *Text, size_t Length, S1_JSON **Out);
void S1JsonFree(S1_JSON *Json);

S1Bool S1JsonGetString(const S1_JSON *Json, const char *Path,
                       const char **Value, size_t *ValueLength);
S1Bool S1JsonGetUInt(const S1_JSON *Json, const char *Path, uint64_t *Value);
S1Bool S1JsonGetBool(const S1_JSON *Json, const char *Path, S1Bool *Value);
size_t S1JsonGetArrayCount(const S1_JSON *Json, const char *Path);
/* Item paths inside an array are addressed as "operations.0.sha256". */

/* --------------------------------------------------------------- plan model */

#define S1_MAX_OPERATIONS 64
#define S1_MAX_STRING 260

typedef enum _S1_OP_TYPE
{
    S1_OP_REPLACE_FILE,
    S1_OP_ADD_FILE,
    S1_OP_UNSUPPORTED
} S1_OP_TYPE;

typedef struct _S1_PLAN_OP
{
    S1_OP_TYPE Type;
    uint32_t Seq;
    char Component[S1_MAX_STRING];
    char Source[S1_MAX_STRING];
    char Target[S1_MAX_STRING];
    uint8_t ExpectedSha256[S1_SHA256_SIZE];
    /* SHA-256 of the file content the op replaces, captured when the plan
     * was written. REQUIRED for replace-file (owner decision, fail closed):
     * the applier re-hashes the backup against it before restoring, and a
     * plan that drops or mismatches it is refused. */
    uint8_t OriginalSha256[S1_SHA256_SIZE];
    S1Bool HasOriginal;
} S1_PLAN_OP;

typedef struct _S1_PLAN
{
    uint32_t Schema;
    char TransactionId[65];
    char MachineId[64];
    char Arch[16];
    char PackageId[S1_MAX_STRING];
    char Version[64];
    uint8_t ManifestDigest[S1_SHA256_SIZE];
    char Operation[16];
    size_t OperationCount;
    S1_PLAN_OP Operations[S1_MAX_OPERATIONS];
} S1_PLAN;

/* Parses and validates plan.json per S1-CONTRACTS.md section 1. Any unknown
 * schema, missing field or unsupported operation type fails the parse. */
S1Bool S1PlanParse(const char *Text, size_t Length, S1_PLAN *Plan);

/* Canonical transaction id per S1-CONTRACTS.md section 3. */
void S1TransactionId(const S1_PLAN *Plan, char Out[65]);

/* The plan is not signed (S1-CONTRACTS.md section 1b): the applier verifies
 * the package manifest signature with S1Rsa2048Sha256Verify and checks the
 * plan against the signed manifest. This helper only validates that the
 * plan's transactionId derives from its own fields. */
S1Bool S1PlanCheckTransactionId(const S1_PLAN *Plan);

/* ---------------------------------------------------------------- journal */

#define S1_JOURNAL_MAX_ENTRY 1024

/* Appends one hash-chained entry. PrevHash is the SHA-256 of the previous
 * entry's exact bytes (all-zeros for the first). On success the full entry
 * bytes are written through S1IoAppend and OutHash receives this entry's
 * hash. The entry is the caller's responsibility to flush before returning
 * to the caller's next step. */
S1Bool S1JournalAppend(void *IoCtx,
                       S1Bool (*Append)(void *IoCtx, const char *Bytes, size_t Length),
                       uint64_t Sequence,
                       const uint8_t PrevHash[S1_SHA256_SIZE],
                       const char *TransactionId,
                       const char *Phase,
                       int Operation,
                       const char *Result,
                       const char *Error,
                       uint8_t OutHash[S1_SHA256_SIZE]);

/* Verifies a journal byte range for hash-chain integrity; returns the
 * sequence of the last intact entry. */
uint64_t S1JournalVerifyChain(const char *Text, size_t Length,
                              S1Bool *Intact);

/* ------------------------------------------------------------ attempt.json */

typedef struct _S1_ATTEMPT
{
    uint32_t Schema;
    char TransactionId[65];
    uint32_t Attempt;
    S1Bool HealthConfirmed;
    S1Bool FailureRecorded;
    char LastResult[16];
} S1_ATTEMPT;

S1Bool S1AttemptParse(const char *Text, size_t Length, S1_ATTEMPT *Attempt);
/* Serializes schema 1. Buffer must hold 512 bytes. */
size_t S1AttemptSerialize(const S1_ATTEMPT *Attempt, char *Buffer, size_t BufferSize);

/* ------------------------------------------------------------- rollback rule */

/* Two boot attempts without a health confirmation revert. A recorded failure
 * makes it a verifiable failure; its absence means an unexpected reboot.
 * The caller journals which of the two paths fired. */
S1Bool S1ShouldRollback(uint32_t Attempt, S1Bool HealthConfirmed);
S1Bool S1IsVerifiableFailure(S1Bool FailureRecorded);

#endif /* S1CORE_H */
#ifdef S1_HOST_TEST
/* Host-only unit hooks for the logic tests; never compiled into the
 * native applier. */
void S1TestBnMulMod(uint32_t A, uint32_t B, uint32_t M, uint32_t *Out);
void S1TestBnMulModW(const uint32_t *Aw, const uint32_t *Bw, const uint32_t *Mw, uint32_t *Out);
void S1TestBnPowMod(uint32_t Base, uint32_t Exp, uint32_t M, uint32_t *Out);
#endif
