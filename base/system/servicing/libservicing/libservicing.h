/*
 * PROJECT:     LiberNT Update - libservicing
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Internal API of the stage 1 servicing core (contract: S1-CORE.md)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Every function returns a Win32 error code (ERROR_SUCCESS on success) or one
 * of the LN_E_* codes below. Nothing here is a public ABI.
 */

#ifndef LIBSERVICING_H
#define LIBSERVICING_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- error codes (customer bit set, so they never collide with Win32) --- */
#define LN_E_BASE               0xE0000000UL
#define LN_E_MANIFEST_INVALID   (LN_E_BASE + 1)   /* syntax, schema or path rule violated */
#define LN_E_HASH_MISMATCH      (LN_E_BASE + 2)   /* payload or object differs from its declared hash */
#define LN_E_UNSIGNED           (LN_E_BASE + 3)   /* no catalog or no signature */
#define LN_E_CATALOG_INVALID    (LN_E_BASE + 4)   /* signature invalid, or members do not cover the package */
#define LN_E_CERT_MISMATCH      (LN_E_BASE + 5)   /* signer is not the pinned development certificate */
#define LN_E_JOURNAL_CORRUPT    (LN_E_BASE + 6)   /* hash chain broken before the last entry */
#define LN_E_PRECONDITION       (LN_E_BASE + 7)   /* target state does not match what the package expects */
#define LN_E_NOT_FOUND          (LN_E_BASE + 8)
#define LN_E_FORBIDDEN_TARGET   (LN_E_BASE + 9)   /* kernel / boot path / outside the allowed roots */
#define LN_E_BAD_STATE          (LN_E_BASE + 10)  /* transaction is not in a state that allows the request */
#define LN_E_VERIFY_FINAL       (LN_E_BASE + 11)  /* final hash check after an operation failed */
#define LN_E_NO_TRUST_ANCHOR    (LN_E_BASE + 12)  /* target has no trusted certificate installed */

#define LN_E_BAD_SIGNATURE      (LN_E_BASE + 14)  /* manifest.sig does not verify with the pinned key */
#define LN_E_NO_BOOT_APPLIER    (LN_E_BASE + 15)  /* a reboot is required but the boot updater is not installed */
#define LN_E_INSECURE_TARGET    (LN_E_BASE + 13)  /* servicing tree owner/DACL is not SYSTEM + Administrators only */
#define LN_SHA256_HEX_LEN   64
#define LN_MAX_ITEMS        4096

/* ---- sha256.c ---------------------------------------------------------- */
typedef struct _LN_SHA256_CTX
{
    unsigned long state[8];
    unsigned long long bitlen;
    unsigned char data[64];
    unsigned long datalen;
} LN_SHA256_CTX;

void LnSha256Init(LN_SHA256_CTX *ctx);
void LnSha256Update(LN_SHA256_CTX *ctx, const void *data, size_t len);
void LnSha256Final(LN_SHA256_CTX *ctx, unsigned char out[32]);
void LnSha256(const void *data, size_t len, unsigned char out[32]);
void LnHexEncode(const unsigned char *in, size_t len, char *out);          /* out: 2*len+1 bytes */
void LnSha256Hex(const void *data, size_t len, char out[65]);
DWORD LnSha256FileHex(const WCHAR *path, char out[65]);
BOOL LnIsSha256Hex(const char *s);                                          /* 64 lowercase hex */

/* ---- util.c ------------------------------------------------------------ */
typedef struct _LN_BUF
{
    char *data;
    size_t len;
    size_t cap;
} LN_BUF;

void LnBufInit(LN_BUF *b);
void LnBufFree(LN_BUF *b);
DWORD LnBufAppend(LN_BUF *b, const char *s, size_t n);
DWORD LnBufAppendStr(LN_BUF *b, const char *s);
DWORD LnBufAppendJsonString(LN_BUF *b, const char *utf8);                   /* with quotes */
DWORD LnBufAppendJsonWide(LN_BUF *b, const WCHAR *w);                       /* with quotes */

char *LnWideToUtf8(const WCHAR *w);                                         /* free() */
WCHAR *LnUtf8ToWide(const char *s);                                         /* free() */
DWORD LnReadFileAll(const WCHAR *path, BYTE **data, size_t *len);           /* free(*data) */
HANDLE LnOpenRetry(const WCHAR *path, DWORD access, DWORD share, DWORD disposition, DWORD flags);   /* transient errors retried */
DWORD LnWriteFileAtomic(const WCHAR *path, const void *data, size_t len);   /* tmp + flush + replace */
DWORD LnEnsureDir(const WCHAR *path);                                       /* recursive */
DWORD LnCopyFileFlush(const WCHAR *src, const WCHAR *dst);                  /* copy + flush, parent created */
BOOL LnPathExists(const WCHAR *path);
BOOL LnIsSafeRelativePath(const WCHAR *path);                               /* no drive, root, .., 8.3 aliases, device names */
BOOL LnIsReservedName(const WCHAR *component, size_t len);                  /* CON, NUL, COM1, LPT1... with or without extension */
void LnRemoveTree(const WCHAR *dir);
DWORD LnCopyTreeFlush(const WCHAR *srcDir, const WCHAR *dstDir);           /* files and subdirectories, flushed */
void LnNowUtc(char out[32]);
void LnI64ToStr(long long v, char out[24]);
WCHAR *LnJoin(const WCHAR *a, const WCHAR *b);                              /* a\b, free() */

/* ---- json.c ------------------------------------------------------------ */
typedef enum _LN_JSON_TYPE
{
    LnJsonNull, LnJsonBool, LnJsonNumber, LnJsonString, LnJsonArray, LnJsonObject
} LN_JSON_TYPE;

typedef struct _LN_JSON
{
    LN_JSON_TYPE type;
    char *str;                  /* string value (UTF-8) */
    char *key;                  /* member name when the parent is an object */
    long long num;
    int boolean;
    struct _LN_JSON *child;     /* first child (array element or object member) */
    struct _LN_JSON *next;      /* next sibling */
} LN_JSON;

DWORD LnJsonParse(const char *text, size_t len, LN_JSON **root);
void LnJsonFree(LN_JSON *n);
const LN_JSON *LnJsonGet(const LN_JSON *obj, const char *key);              /* NULL if absent */
const char *LnJsonGetString(const LN_JSON *obj, const char *key);          /* NULL unless a string */
BOOL LnJsonGetInt(const LN_JSON *obj, const char *key, long long *out);
BOOL LnJsonGetBool(const LN_JSON *obj, const char *key, BOOL *out);

/* ---- target.c ---------------------------------------------------------- */
typedef enum _LN_TARGET_MODE { LnTargetOnline = 1, LnTargetImage = 2 } LN_TARGET_MODE;

typedef struct _LN_TARGET
{
    LN_TARGET_MODE mode;
    WCHAR root[MAX_PATH];         /* ends with a backslash */
    WCHAR servicing[MAX_PATH];    /* <root>LiberNT\Servicing */
    char identity[65];            /* machineId: a GUID kept in target-id, stable per target */
} LN_TARGET;

/* Online: root = the drive of the running system. Image: an unmounted or offline root. */
DWORD LnTargetOpen(LN_TARGET *t, LN_TARGET_MODE mode, const WCHAR *imageRoot, BOOL create);
WCHAR *LnTargetPath(const LN_TARGET *t, const WCHAR *relInServicing);       /* free() */
WCHAR *LnTargetFile(const LN_TARGET *t, const WCHAR *relInRoot);            /* free() */
/* Stage 1 white list: DLL/EXE of user components outside the critical processes and the boot path,
 * and .sys drivers under System32\drivers with a declared Start of 2 (auto) to 4. driverStart is -1
 * for anything that is not a driver. The kernel, the HAL and boot-start/system-start drivers are refused. */
BOOL LnTargetItemAllowed(const WCHAR *relInRoot, int driverStart);
BOOL LnTargetPathAllowed(const WCHAR *relInRoot);                          /* LnTargetItemAllowed(rel, -1) */
/* Boot-class route (owner directive 2026-11-08, stage-1 condition 5 lifted): the kernel, HAL, bootvid, kdcom,
 * ntdll, win32k*, the critical processes and drivers (driverStart 0..4, REQUIRED for a .sys) may be replaced,
 * but only by items that declare "class":"boot" in the signed manifest. The hives, System32\boot, LiberNT\
 * and the loader files stay refused. Group: 1 = kernel trio + kdcom, 2 = drivers, 3 = ntdll, win32k* and the
 * critical processes; 0 = not a boot-class target. */
BOOL LnTargetBootItemAllowed(const WCHAR *relInRoot, int driverStart);
int LnTargetBootGroup(const WCHAR *relInRoot);
/* Parents must not be reparse points, and the real path must equal the lexical one (8.3, junctions). */
DWORD LnTargetValidateReal(const LN_TARGET *t, const WCHAR *relInRoot);

/* ---- manifest.c -------------------------------------------------------- */
typedef struct _LN_ITEM
{
    WCHAR *path;                  /* inside payload\  (NULL for delete) */
    WCHAR *target;                /* inside the target root */
    int driverStart;              /* Start value declared for a .sys (2..4; 0..4 for a boot-class item), -1 otherwise */
    BOOL bootClass;               /* "class":"boot": swapped in by the boot applier, never applied inline */
    char sha256[65];              /* of the new content ("" for delete) */
    BOOL isDelete;
} LN_ITEM;

typedef struct _LN_MANIFEST
{
    char packageId[65];
    char releaseVersion[65];
    char type[24];                /* system-component | driver | tool */
    char architecture[16];
    char baseVersion[32];         /* optional, copied into plan.target.base (default "0") */
    char certificateSha256[65];
    LN_ITEM *items;
    size_t itemCount;
    char manifestSha256[65];      /* of manifest.json bytes */
} LN_MANIFEST;

DWORD LnManifestParse(const char *text, size_t len, LN_MANIFEST *m);
DWORD LnManifestLoad(const WCHAR *path, LN_MANIFEST *m);
void LnManifestFree(LN_MANIFEST *m);

/* sha256 of the certificate installed in the target's trust\ directory (the pinned signer) */
DWORD LnPinnedCertSha256(const LN_TARGET *t, char hash[65]);

/* ---- verify.c ---------------------------------------------------------- */
/* Root of trust: manifest.sig (RSA-2048 PKCS#1 v1.5 over SHA-256, verified with the s1core of the boot
 * applier) against the pinned certificate; catalog.cat is optional, additional, required for drivers.
 * pinnedCerPath: the trusted development certificate (the target's trust\ directory). */
DWORD LnServicingVerifyPackage(const WCHAR *pkgDir, const WCHAR *pinnedCerPath, LN_MANIFEST *out);
/* Same checks over the copy held in the store (manifest, catalog, signature, objects). */
/* Same checks over the copy held in the store (manifest, catalog, signature, objects). */
DWORD LnServicingVerifyStored(const LN_TARGET *t, const char *packageId, const char *version, LN_MANIFEST *out);

/* The root-of-trust check alone (manifest.sig over the given manifest bytes against
 * the pinned certificate), without the payload pass: a downloader can refuse an
 * untrusted manifest before acting on anything the manifest says. The manifest
 * bytes are the caller's, so verification and parsing run over one read. */
DWORD LnVerifyManifestSignatureMem(const BYTE *man, size_t mlen, const WCHAR *sigDir,
                                   const WCHAR *pinnedCerPath);
DWORD LnCertFileSha256(const WCHAR *cerPath, char out[65]);

/* ---- journal.c --------------------------------------------------------- */
typedef struct _LN_JENTRY
{
    long long sequence;           /* "seq" */
    char transactionId[65];       /* "tx" */
    char phase[24];
    long long op;                 /* sequence number of the operation, -1 = null */
    char result[12];              /* ok | error | pending */
    char error[96];               /* "" = null; hex code, or unexpected-reboot | verifiable-failure */
    char previousEntrySha256[65]; /* "prev" */
    char timestampUtc[32];        /* "ts" */
    BOOL rebootRequired;          /* derived: phase == pending-first-boot */
    BOOL healthConfirmed;         /* derived: phase == confirmed */
} LN_JENTRY;

typedef struct _LN_JOURNAL
{
    LN_JENTRY *entries;
    size_t count;
    BOOL tailTruncated;           /* last line was partial and was ignored */
} LN_JOURNAL;

/* S1-CONTRACTS.md section 3: SHA-256 of machineId \n packageId \n version \n manifestDigest \n operation */
void LnTransactionId(const char *machineId, const char *packageId, const char *version,
                     const char *manifestDigest, const char *operation, char out[65]);
DWORD LnJournalAppend(const LN_TARGET *t, LN_JENTRY *e);                    /* fills sequence/prev/time */
DWORD LnJournalRead(const LN_TARGET *t, LN_JOURNAL *j);
void LnJournalFree(LN_JOURNAL *j);
const LN_JENTRY *LnJournalLast(const LN_JOURNAL *j, const char *transactionId);
DWORD LnJournalWriteState(const LN_TARGET *t);                              /* derived view */

/* ---- store.c ----------------------------------------------------------- */
/* the object IS the file store\objects\<sha256> (S1-CONTRACTS.md plan.source) */
DWORD LnStoreObjectPath(const LN_TARGET *t, const char *sha256, WCHAR **path);
DWORD LnStoreImport(const LN_TARGET *t, const WCHAR *pkgDir, const LN_MANIFEST *m);   /* idempotent */
DWORD LnStoreLoadManifest(const LN_TARGET *t, const char *packageId, const char *version, LN_MANIFEST *m);
DWORD LnStoreRemovePackage(const LN_TARGET *t, const char *packageId, const char *version);
DWORD LnStoreList(const LN_TARGET *t, char (**ids)[130], size_t *count);   /* "id\\version", free(*ids) */

/* ---- servicing.c: add / apply / rollback ------------------------------- */
/* TRUE when a raw BootExecute value (REG_SZ or REG_MULTI_SZ) lists the LiberNT boot
 * applier. `bytes` is the buffer size in bytes. Each entry is matched by its first
 * token (up to a space), with any path and a trailing ".exe" stripped,
 * case-insensitively. The buffer must contain a L'\0' within `bytes` (or be zeroed
 * past the data): the walk advances with wcslen over each entry. */
BOOL LnBootExecuteListed(const WCHAR *multiSz, size_t bytes);
DWORD LnServicingAddPackage(const LN_TARGET *t, const WCHAR *pkgDir, char txId[65]);
/* Applies a package that is already in the store. Idempotent: the same request resumes or returns. */
DWORD LnServicingApplyPackage(const LN_TARGET *t, const char *packageId, const char *version,
                              BOOL *rebootRequired, char txId[65]);
DWORD LnServicingRollback(const LN_TARGET *t, const char *packageId, const char *version, char txId[65]);
DWORD LnServicingRemovePackage(const LN_TARGET *t, const char *packageId, const char *version);
/* Re-checks every final hash and, only then, writes confirmed / healthConfirmed. */
DWORD LnServicingConfirmHealth(const LN_TARGET *t, const char *packageId, const char *version);
DWORD LnServicingExportLog(const LN_TARGET *t, const WCHAR *destDir);

/* ---- fetch.c: HTTP download for add-package --from ---------------------- */
/* Minimal HTTP/1.0 GET (raw winsock; WinHTTP's response path is broken on current
 * targets). http:// only, status 200 only, no redirects, body capped
 * (LN_FETCH_MAX_BYTES overrides the 64 MiB default, LN_FETCH_TIMEOUT_MS the 30 s one).
 * Any failure removes a partially written destPath. */
DWORD LnFetchFile(const WCHAR *url, const WCHAR *destPath);
/* Downloads manifest.json and manifest.sig, verifies the signature against the pinned
 * certificate of the target, then downloads the payloads the manifest names into
 * destDir (a fresh package directory); checks each payload hash and caps the total
 * (LN_FETCH_MAX_PACKAGE_BYTES overrides the 256 MiB default). Any failure removes
 * destDir. The add path re-verifies everything authoritatively. */
DWORD LnFetchPackage(const LN_TARGET *t, const WCHAR *baseUrl, const WCHAR *destDir);

#ifdef LN_TESTING
/* Hooks that exist only in the test build of the library (compile with -DLN_TESTING). */
extern BOOL LnTestLockedAsPending;      /* treat a locked target of an Image as in use, as Online would */
extern PSID LnTestExtraSid;             /* an extra principal accepted in the servicing DACL and as owner */
extern void (*LnTestAfterStageHook)(const WCHAR *sourcePackageDir);   /* runs after add-package copied the package */
BOOL LnTestCertRsa2048Key(const BYTE *der, size_t len, BYTE modulus[256], uint32_t *exponent);
#endif

/* ---- pending.c --------------------------------------------------------- */
/* pending\attempt.json, S1-CONTRACTS.md section 5 */
typedef struct _LN_ATTEMPT
{
    char transactionId[65];       /* "tx" */
    long long attempt;
    BOOL healthConfirmed;
    BOOL failureRecorded;
    char lastResult[16];          /* started | applied | failed */
    char previousResult[16];      /* lastResult found before this boot marked it (not stored) */
} LN_ATTEMPT;

DWORD LnServicingReadPendingPlan(const LN_TARGET *t, char **planJson);      /* free() */
/* The plan is not trusted: it must match the signed package in the store (digest, targets, hashes). */
DWORD LnServicingValidatePlan(const LN_TARGET *t, const char *planJson);
DWORD LnServicingReadAttempt(const LN_TARGET *t, LN_ATTEMPT *a);
/* attempt+1 and lastResult=started, flushed BEFORE the applier operates (a power cut still counts) */
DWORD LnServicingMarkBootStarted(const LN_TARGET *t, LN_ATTEMPT *a);
DWORD LnServicingMarkBootOutcome(const LN_TARGET *t, BOOL verifiedFailure, BOOL completed);
/* internal: written only after the final hashes were re-checked */
DWORD LnAttemptSetHealth(const LN_TARGET *t, const char *transactionId);
DWORD LnAttemptRecordFailure(const LN_TARGET *t, const char *transactionId);
/* Evaluate BEFORE MarkBootStarted. healthConfirmed false and attempt >= 2 -> roll back;
 * reason: verifiable-failure (failureRecorded) or unexpected-reboot. */
BOOL LnServicingShouldRollback(const LN_ATTEMPT *a, const char **reason);

#ifdef __cplusplus
}
#endif

#endif /* LIBSERVICING_H */
