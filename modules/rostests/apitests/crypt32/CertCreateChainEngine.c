/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests chain-engine trust configuration through hExclusiveRoot
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#define WIN32_NO_STATUS
#define _INC_WINDOWS
#define COM_NO_WINDOWS_H

#include <apitest.h>
#include <windef.h>
#include <wincrypt.h>

#include "tlspoc_selfsigned_pem.h"

static
PCCERT_CONTEXT
LoadFixture(VOID)
{
    DWORD cb = 0;
    BYTE *buf;
    PCCERT_CONTEXT cert;

    CryptStringToBinaryA(g_tlspocSelfSignedPem,
                         (DWORD)strlen(g_tlspocSelfSignedPem),
                         CRYPT_STRING_BASE64HEADER, NULL, &cb, NULL, NULL);
    buf = HeapAlloc(GetProcessHeap(), 0, cb);
    CryptStringToBinaryA(g_tlspocSelfSignedPem,
                         (DWORD)strlen(g_tlspocSelfSignedPem),
                         CRYPT_STRING_BASE64HEADER, buf, &cb, NULL, NULL);
    cert = CertCreateCertificateContext(X509_ASN_ENCODING, buf, cb);
    HeapFree(GetProcessHeap(), 0, buf);
    return cert;
}

static
HCERTSTORE
StoreWithFixture(
    _In_ PCCERT_CONTEXT Cert)
{
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0,
                                     CERT_STORE_CREATE_NEW_FLAG, NULL);

    if (store)
        ok(CertAddCertificateContextToStore(store, Cert, CERT_STORE_ADD_ALWAYS,
                                            NULL),
           "CertAddCertificateContextToStore failed: %lu\n", GetLastError());
    return store;
}

static
PCCERT_CHAIN_CONTEXT
BuildChain(
    _In_opt_ HCERTCHAINENGINE Engine,
    _In_ PCCERT_CONTEXT Cert,
    _In_opt_ HCERTSTORE AdditionalStore)
{
    CERT_CHAIN_PARA para;
    PCCERT_CHAIN_CONTEXT chain = NULL;

    memset(&para, 0, sizeof(para));
    para.cbSize = sizeof(para);
    ok(CertGetCertificateChain(Engine, Cert, NULL, AdditionalStore, &para, 0,
                               NULL, &chain),
       "CertGetCertificateChain failed: %lu\n", GetLastError());
    return chain;
}

static
VOID
CheckSslPolicy(
    _In_ PCCERT_CHAIN_CONTEXT Chain,
    _In_ PCWSTR ServerName,
    _In_ DWORD ExpectedError)
{
    SSL_EXTRA_CERT_CHAIN_POLICY_PARA extra;
    CERT_CHAIN_POLICY_PARA para;
    CERT_CHAIN_POLICY_STATUS status;

    memset(&extra, 0, sizeof(extra));
    extra.cbSize = sizeof(extra);
    extra.dwAuthType = AUTHTYPE_SERVER;
    extra.pwszServerName = (PWSTR)ServerName;
    memset(&para, 0, sizeof(para));
    para.cbSize = sizeof(para);
    para.pvExtraPolicyPara = &extra;
    memset(&status, 0, sizeof(status));
    status.cbSize = sizeof(status);
    ok(CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, Chain, &para,
                                        &status),
       "CertVerifyCertificateChainPolicy failed\n");
    ok(status.dwError == ExpectedError,
       "SSL policy error 0x%08lx, expected 0x%08lx\n", status.dwError,
       ExpectedError);
}

START_TEST(CertCreateChainEngine)
{
    PCCERT_CONTEXT cert;
    HCERTSTORE store;
    HCERTSTORE emptyStore;
    HCERTCHAINENGINE engine;
    CERT_CHAIN_ENGINE_CONFIG config;
    PCCERT_CHAIN_CONTEXT chain;

    cert = LoadFixture();
    ok(cert != NULL, "failed to decode fixture certificate: %lu\n",
       GetLastError());
    if (!cert)
        return;
    store = StoreWithFixture(cert);
    ok(store != NULL, "CertOpenStore failed: %lu\n", GetLastError());
    if (!store)
    {
        CertFreeCertificateContext(cert);
        return;
    }

    chain = BuildChain(NULL, cert, NULL);
    ok(chain != NULL, "default-engine chain build failed\n");
    if (chain)
    {
        ok((chain->TrustStatus.dwErrorStatus & CERT_TRUST_IS_UNTRUSTED_ROOT) != 0,
           "fixture unexpectedly trusted by the default engine (status 0x%08lx)\n",
           chain->TrustStatus.dwErrorStatus);
        CertFreeCertificateChain(chain);
    }

    chain = BuildChain(NULL, cert, store);
    ok(chain != NULL, "additional-store chain build failed\n");
    if (chain)
    {
        ok((chain->TrustStatus.dwErrorStatus & CERT_TRUST_IS_UNTRUSTED_ROOT) != 0,
           "hAdditionalStore conferred trust (status 0x%08lx)\n",
           chain->TrustStatus.dwErrorStatus);
        CertFreeCertificateChain(chain);
    }

    memset(&config, 0, sizeof(config));
    config.cbSize = sizeof(config);
    config.hExclusiveRoot = store;
    engine = NULL;
    ok(CertCreateCertificateChainEngine(&config, &engine),
       "CertCreateCertificateChainEngine failed: %lu\n", GetLastError());
    ok(engine != NULL, "no chain engine returned\n");
    if (engine)
    {
        chain = BuildChain(engine, cert, NULL);
        ok(chain != NULL, "exclusive-engine chain build failed\n");
        if (chain)
        {
            ok(chain->TrustStatus.dwErrorStatus == 0,
               "hExclusiveRoot chain status 0x%08lx\n",
               chain->TrustStatus.dwErrorStatus);
            CheckSslPolicy(chain, L"tlspoc.local", 0);
            CertFreeCertificateChain(chain);
        }
        CertFreeCertificateChainEngine(engine);
    }

    emptyStore = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0,
                               CERT_STORE_CREATE_NEW_FLAG, NULL);
    ok(emptyStore != NULL, "CertOpenStore(empty) failed: %lu\n",
       GetLastError());
    if (emptyStore)
    {
        memset(&config, 0, sizeof(config));
        config.cbSize = sizeof(config);
        config.hExclusiveRoot = emptyStore;
        engine = NULL;
        if (CertCreateCertificateChainEngine(&config, &engine) && engine)
        {
            chain = BuildChain(engine, cert, NULL);
            ok(chain != NULL, "empty-exclusive chain build failed\n");
            if (chain)
            {
                ok((chain->TrustStatus.dwErrorStatus &
                    CERT_TRUST_IS_UNTRUSTED_ROOT) != 0,
                   "certificate trusted outside its exclusive root set "
                   "(status 0x%08lx)\n", chain->TrustStatus.dwErrorStatus);
                CertFreeCertificateChain(chain);
            }
            CertFreeCertificateChainEngine(engine);
        }
        CertCloseStore(emptyStore, 0);
    }

    CertCloseStore(store, 0);
    CertFreeCertificateContext(cert);
}
