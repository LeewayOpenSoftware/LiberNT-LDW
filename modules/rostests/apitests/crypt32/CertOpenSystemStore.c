/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests the logical Root store AuthRoot aggregation
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#define WIN32_NO_STATUS
#define _INC_WINDOWS
#define COM_NO_WINDOWS_H

#include <apitest.h>
#include <windef.h>
#include <wincrypt.h>

static
BOOL
IsSelfSignedRoot(
    _In_ PCCERT_CONTEXT Cert)
{
    return Cert->pCertInfo->Subject.cbData == Cert->pCertInfo->Issuer.cbData &&
           !memcmp(Cert->pCertInfo->Subject.pbData,
                   Cert->pCertInfo->Issuer.pbData,
                   Cert->pCertInfo->Subject.cbData);
}

static
DWORD
CountStoreCerts(
    _In_ HCERTSTORE Store)
{
    DWORD count = 0;
    PCCERT_CONTEXT cert = NULL;

    while ((cert = CertEnumCertificatesInStore(Store, cert)) != NULL)
        count++;
    return count;
}

static
void
CheckRootChainsAsTrusted(
    _In_ HCERTSTORE AuthRoot)
{
    PCCERT_CONTEXT cert = NULL;
    BOOL tested = FALSE;

    while ((cert = CertEnumCertificatesInStore(AuthRoot, cert)) != NULL)
    {
        CERT_CHAIN_PARA chainPara;
        PCCERT_CHAIN_CONTEXT chain;
        CERT_CHAIN_POLICY_PARA policyPara;
        CERT_CHAIN_POLICY_STATUS policyStatus;

        if (!IsSelfSignedRoot(cert))
            continue;
        /* Some shipped roots are past their dates; that is not what this checks */
        if (CertVerifyTimeValidity(NULL, cert->pCertInfo) != 0)
            continue;

        memset(&chainPara, 0, sizeof(chainPara));
        chainPara.cbSize = sizeof(chainPara);
        chain = NULL;
        ok(CertGetCertificateChain(NULL, cert, NULL, NULL, &chainPara, 0, NULL,
                                   &chain),
           "CertGetCertificateChain failed for AuthRoot root: %lu\n",
           GetLastError());
        if (!chain)
            continue;

        ok((chain->TrustStatus.dwErrorStatus & CERT_TRUST_IS_UNTRUSTED_ROOT) == 0,
           "AuthRoot self-signed root reported UNTRUSTED_ROOT (status 0x%08lx)\n",
           chain->TrustStatus.dwErrorStatus);
        ok(chain->TrustStatus.dwErrorStatus == 0,
           "AuthRoot self-signed root chain status 0x%08lx\n",
           chain->TrustStatus.dwErrorStatus);

        memset(&policyPara, 0, sizeof(policyPara));
        policyPara.cbSize = sizeof(policyPara);
        memset(&policyStatus, 0, sizeof(policyStatus));
        policyStatus.cbSize = sizeof(policyStatus);
        ok(CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain,
                                            &policyPara, &policyStatus),
           "CertVerifyCertificateChainPolicy failed\n");
        ok(policyStatus.dwError == 0,
           "SSL policy on AuthRoot root failed: 0x%08lx\n",
           policyStatus.dwError);

        CertFreeCertificateChain(chain);
        tested = TRUE;
        break;
    }
    ok(tested, "no time-valid self-signed root found in AuthRoot store\n");
}

/*
 * Hermetic chains: a test root staged only in the CurrentUser AuthRoot store,
 * leaves issued from it, and a root that no store trusts. One ephemeral RSA
 * key signs everything; certificates are told apart by name.
 */
#define TEST_ROOT_NAME      L"CN=LiberNT AuthRoot Aggregation Test Root"
#define TEST_UNTRUSTED_NAME L"CN=LiberNT AuthRoot Aggregation Untrusted Root"
#define TEST_LEAF_NAME      L"CN=LiberNT AuthRoot Aggregation Test Leaf"

#define FILETIME_DAY (24ULL * 60 * 60 * 10000000)

static
FILETIME
FileTimeFromNow(
    _In_ LONGLONG Days)
{
    FILETIME now;
    ULARGE_INTEGER value;

    GetSystemTimeAsFileTime(&now);
    value.LowPart = now.dwLowDateTime;
    value.HighPart = now.dwHighDateTime;
    value.QuadPart += (ULONGLONG)Days * FILETIME_DAY;
    now.dwLowDateTime = value.LowPart;
    now.dwHighDateTime = value.HighPart;
    return now;
}

static
PCCERT_CONTEXT
MakeCert(
    _In_ HCRYPTPROV Prov,
    _In_ PCWSTR Subject,
    _In_ PCWSTR Issuer,
    _In_ BYTE Serial,
    _In_ LONGLONG NotBeforeDays,
    _In_ LONGLONG NotAfterDays,
    _In_ BOOL IsCA)
{
    BYTE subjectBuf[256], issuerBuf[256], bcBuf[16], pubBuf[1024], certBuf[2048];
    DWORD subjectSize = sizeof(subjectBuf), issuerSize = sizeof(issuerBuf);
    DWORD bcSize = sizeof(bcBuf), pubSize = sizeof(pubBuf), certSize = sizeof(certBuf);
    CERT_BASIC_CONSTRAINTS2_INFO bc = { IsCA, FALSE, 0 };
    CERT_EXTENSION ext;
    CERT_INFO info;
    BYTE serial = Serial;

    if (!CertStrToNameW(X509_ASN_ENCODING, Subject, CERT_X500_NAME_STR, NULL,
                        subjectBuf, &subjectSize, NULL) ||
        !CertStrToNameW(X509_ASN_ENCODING, Issuer, CERT_X500_NAME_STR, NULL,
                        issuerBuf, &issuerSize, NULL) ||
        !CryptEncodeObject(X509_ASN_ENCODING, X509_BASIC_CONSTRAINTS2, &bc,
                           bcBuf, &bcSize) ||
        !CryptExportPublicKeyInfo(Prov, AT_SIGNATURE, X509_ASN_ENCODING,
                                  (PCERT_PUBLIC_KEY_INFO)pubBuf, &pubSize))
    {
        trace("encoding %S failed: %lu\n", Subject, GetLastError());
        return NULL;
    }

    ext.pszObjId = (LPSTR)szOID_BASIC_CONSTRAINTS2;
    ext.fCritical = TRUE;
    ext.Value.cbData = bcSize;
    ext.Value.pbData = bcBuf;

    memset(&info, 0, sizeof(info));
    info.dwVersion = CERT_V3;
    info.SerialNumber.cbData = sizeof(serial);
    info.SerialNumber.pbData = &serial;
    info.SignatureAlgorithm.pszObjId = (LPSTR)szOID_RSA_SHA256RSA;
    info.Issuer.cbData = issuerSize;
    info.Issuer.pbData = issuerBuf;
    info.NotBefore = FileTimeFromNow(NotBeforeDays);
    info.NotAfter = FileTimeFromNow(NotAfterDays);
    info.Subject.cbData = subjectSize;
    info.Subject.pbData = subjectBuf;
    info.SubjectPublicKeyInfo = *(PCERT_PUBLIC_KEY_INFO)pubBuf;
    info.cExtension = 1;
    info.rgExtension = &ext;

    if (!CryptSignAndEncodeCertificate(Prov, AT_SIGNATURE, X509_ASN_ENCODING,
                                       X509_CERT_TO_BE_SIGNED, &info,
                                       &info.SignatureAlgorithm, NULL,
                                       certBuf, &certSize))
    {
        trace("signing %S failed: %lu\n", Subject, GetLastError());
        return NULL;
    }
    return CertCreateCertificateContext(X509_ASN_ENCODING, certBuf, certSize);
}

/* Builds the chain of Leaf with the default engine and checks both the trust
 * bits and the SSL policy verdict. */
static
void
CheckChain(
    _In_ PCSTR What,
    _In_ PCCERT_CONTEXT Leaf,
    _In_opt_ HCERTSTORE Extra,
    _In_ DWORD ExpectedBits,
    _In_ DWORD ForbiddenBits,
    _In_ DWORD ExpectedPolicyError)
{
    CERT_CHAIN_PARA chainPara;
    CERT_CHAIN_POLICY_PARA policyPara;
    CERT_CHAIN_POLICY_STATUS policyStatus;
    PCCERT_CHAIN_CONTEXT chain = NULL;

    memset(&chainPara, 0, sizeof(chainPara));
    chainPara.cbSize = sizeof(chainPara);
    ok(CertGetCertificateChain(NULL, Leaf, NULL, Extra, &chainPara, 0, NULL, &chain),
       "%s: CertGetCertificateChain failed: %lu\n", What, GetLastError());
    if (!chain)
        return;

    ok((chain->TrustStatus.dwErrorStatus & ExpectedBits) == ExpectedBits,
       "%s: status 0x%08lx lacks 0x%08lx\n", What,
       chain->TrustStatus.dwErrorStatus, ExpectedBits);
    ok((chain->TrustStatus.dwErrorStatus & ForbiddenBits) == 0,
       "%s: status 0x%08lx has 0x%08lx\n", What,
       chain->TrustStatus.dwErrorStatus, ForbiddenBits);

    memset(&policyPara, 0, sizeof(policyPara));
    policyPara.cbSize = sizeof(policyPara);
    memset(&policyStatus, 0, sizeof(policyStatus));
    policyStatus.cbSize = sizeof(policyStatus);
    ok(CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain,
                                        &policyPara, &policyStatus),
       "%s: CertVerifyCertificateChainPolicy failed\n", What);
    ok(policyStatus.dwError == ExpectedPolicyError,
       "%s: SSL policy 0x%08lx, expected 0x%08lx\n", What,
       policyStatus.dwError, ExpectedPolicyError);

    CertFreeCertificateChain(chain);
}

static
void
CheckStagedAuthRoot(void)
{
    HCRYPTPROV prov = 0;
    HCRYPTKEY key = 0;
    HCERTSTORE authRootCU = NULL, root = NULL, extra = NULL;
    PCCERT_CONTEXT testRoot = NULL, untrustedRoot = NULL, leaf = NULL;
    PCCERT_CONTEXT expiredLeaf = NULL, untrustedLeaf = NULL, staged = NULL, found;

    if (!CryptAcquireContextW(&prov, NULL, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES,
                              CRYPT_VERIFYCONTEXT) ||
        !CryptGenKey(prov, AT_SIGNATURE, 2048 << 16, &key))
    {
        skip("no RSA provider for the hermetic chains: %lu\n", GetLastError());
        goto Cleanup;
    }

    testRoot = MakeCert(prov, TEST_ROOT_NAME, TEST_ROOT_NAME, 1, -1, 30, TRUE);
    untrustedRoot = MakeCert(prov, TEST_UNTRUSTED_NAME, TEST_UNTRUSTED_NAME, 2, -1, 30, TRUE);
    leaf = MakeCert(prov, TEST_LEAF_NAME, TEST_ROOT_NAME, 3, -1, 30, FALSE);
    expiredLeaf = MakeCert(prov, TEST_LEAF_NAME, TEST_ROOT_NAME, 4, -30, -2, FALSE);
    untrustedLeaf = MakeCert(prov, TEST_LEAF_NAME, TEST_UNTRUSTED_NAME, 5, -1, 30, FALSE);
    ok(testRoot && untrustedRoot && leaf && expiredLeaf && untrustedLeaf,
       "could not build the test certificates\n");
    if (!testRoot || !untrustedRoot || !leaf || !expiredLeaf || !untrustedLeaf)
        goto Cleanup;

    /* Stage the test root in the CurrentUser AuthRoot store only */
    authRootCU = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                               CERT_SYSTEM_STORE_CURRENT_USER, L"AuthRoot");
    ok(authRootCU != NULL, "opening CurrentUser AuthRoot failed: %lu\n", GetLastError());
    if (!authRootCU)
        goto Cleanup;
    ok(CertAddCertificateContextToStore(authRootCU, testRoot, CERT_STORE_ADD_REPLACE_EXISTING,
                                        &staged),
       "staging the test root in AuthRoot failed: %lu\n", GetLastError());
    if (!staged)
        goto Cleanup;
    /* A registry store writes on commit or close; later opens read the registry */
    ok(CertControlStore(authRootCU, 0, CERT_STORE_CTRL_COMMIT, NULL),
       "committing AuthRoot failed: %lu\n", GetLastError());

    /* The CurrentUser logical Root store sees the CurrentUser AuthRoot store */
    root = CertOpenSystemStoreW(0, L"Root");
    ok(root != NULL, "CertOpenSystemStoreW(Root) failed: %lu\n", GetLastError());
    if (!root)
        goto Cleanup;
    found = CertFindCertificateInStore(root, X509_ASN_ENCODING, 0, CERT_FIND_EXISTING,
                                       testRoot, NULL);
    ok(found != NULL, "the CurrentUser AuthRoot test root is not in the logical Root store\n");

    /* A delete through the logical Root store must not reach AuthRoot */
    if (found)
    {
        SetLastError(0xdeadbeef);
        ok(!CertDeleteCertificateFromStore(found),
           "deleting an AuthRoot certificate through the logical Root store succeeded\n");
        found = CertFindCertificateInStore(authRootCU, X509_ASN_ENCODING, 0, CERT_FIND_EXISTING,
                                           testRoot, NULL);
        ok(found != NULL, "the test root left AuthRoot after a delete through Root\n");
        if (found)
            CertFreeCertificateContext(found);
    }

    /* Anchored by AuthRoot alone: trusted, and the SSL policy accepts it */
    CheckChain("leaf under AuthRoot root", leaf, NULL, 0,
               CERT_TRUST_IS_UNTRUSTED_ROOT | CERT_TRUST_IS_PARTIAL_CHAIN |
               CERT_TRUST_IS_NOT_TIME_VALID, 0);

    /* Same anchor, expired leaf: still rejected for its dates */
    CheckChain("expired leaf under AuthRoot root", expiredLeaf, NULL,
               CERT_TRUST_IS_NOT_TIME_VALID, CERT_TRUST_IS_UNTRUSTED_ROOT,
               CERT_E_EXPIRED);

    /* A root no store trusts: still untrusted */
    extra = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, NULL);
    if (extra && CertAddCertificateContextToStore(extra, untrustedRoot, CERT_STORE_ADD_ALWAYS, NULL))
        CheckChain("leaf under untrusted root", untrustedLeaf, extra,
                   CERT_TRUST_IS_UNTRUSTED_ROOT, 0, CERT_E_UNTRUSTEDROOT);
    else
        ok(0, "could not build the untrusted root store: %lu\n", GetLastError());

Cleanup:
    if (staged)
        ok(CertDeleteCertificateFromStore(staged),
           "removing the test root from AuthRoot failed: %lu\n", GetLastError());
    if (extra)
        CertCloseStore(extra, 0);
    if (root)
        CertCloseStore(root, 0);
    if (authRootCU)
        CertCloseStore(authRootCU, 0);
    if (untrustedLeaf)
        CertFreeCertificateContext(untrustedLeaf);
    if (expiredLeaf)
        CertFreeCertificateContext(expiredLeaf);
    if (leaf)
        CertFreeCertificateContext(leaf);
    if (untrustedRoot)
        CertFreeCertificateContext(untrustedRoot);
    if (testRoot)
        CertFreeCertificateContext(testRoot);
    if (key)
        CryptDestroyKey(key);
    if (prov)
        CryptReleaseContext(prov, 0);
}

START_TEST(CertOpenSystemStore)
{
    HCERTSTORE root, authRoot;
    PCCERT_CONTEXT cert = NULL;
    DWORD authCount = 0, before, after;

    CheckStagedAuthRoot();

    authRoot = CertOpenSystemStoreW(0, L"AuthRoot");
    root = CertOpenSystemStoreW(0, L"Root");
    ok(authRoot != NULL, "CertOpenSystemStoreW(AuthRoot) failed: %lu\n",
       GetLastError());
    ok(root != NULL, "CertOpenSystemStoreW(Root) failed: %lu\n", GetLastError());
    if (!authRoot || !root)
        goto Cleanup;

    while ((cert = CertEnumCertificatesInStore(authRoot, cert)) != NULL)
    {
        PCCERT_CONTEXT found;

        authCount++;
        found = CertFindCertificateInStore(root,
                                           X509_ASN_ENCODING |
                                           PKCS_7_ASN_ENCODING,
                                           0, CERT_FIND_EXISTING, cert, NULL);
        ok(found != NULL,
           "AuthRoot certificate %lu missing from the logical Root store\n",
           authCount);
        if (found)
            CertFreeCertificateContext(found);
    }
    ok(authCount > 0, "AuthRoot store is empty\n");

    CheckRootChainsAsTrusted(authRoot);

    cert = CertEnumCertificatesInStore(authRoot, NULL);
    if (cert)
    {
        PCCERT_CONTEXT added;

        before = CountStoreCerts(authRoot);
        added = NULL;
        ok(CertAddCertificateContextToStore(root, cert, CERT_STORE_ADD_ALWAYS,
                                            &added),
           "CertAddCertificateContextToStore(Root) failed: %lu\n",
           GetLastError());
        if (added)
        {
            after = CountStoreCerts(authRoot);
            ok(after == before,
               "logical Root add changed AuthRoot count %lu -> %lu\n",
               before, after);
            ok(CertDeleteCertificateFromStore(added),
               "removing test cert from Root failed: %lu\n", GetLastError());
        }
        CertFreeCertificateContext(cert);
    }

Cleanup:
    if (root)
        CertCloseStore(root, 0);
    if (authRoot)
        CertCloseStore(authRoot, 0);
}
