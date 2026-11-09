/*
 * PROJECT:     LiberNT Update - libservicing
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Unit tests of the stage 1 core. They run on a Windows host against a temporary
 *              Image target; nothing touches the real system or the servicing tree of the host.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * usage: libservicing-tests <fixtures-dir> <dev-certificate.cer>
 * Fixtures are built by tools/libernt-update/Build-And-Test.ps1 (they need the signing tools).
 */

#include "../libservicing.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <aclapi.h>
#include <sddl.h>

static int g_checks, g_failed;
static const WCHAR *g_fx;
static const WCHAR *g_cer;

#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_ERR(expected, expr) do { DWORD _e = (expr); g_checks++; if (_e != (DWORD)(expected)) { g_failed++; \
    printf("  FAIL %s:%d: %s returned 0x%08lx, expected 0x%08lx\n", __FILE__, __LINE__, #expr, (unsigned long)_e, (unsigned long)(expected)); } } while (0)

static WCHAR g_work[MAX_PATH];

static void Section(const char *name)
{
    printf("[%s]\n", name);
}

static void WriteText(const WCHAR *path, const char *text)
{
    LnWriteFileAtomic(path, text, strlen(text));
}

static BOOL FileIs(const WCHAR *path, const char *text)
{
    BYTE *d;
    size_t n;
    BOOL ok;

    if (LnReadFileAll(path, &d, &n))
        return FALSE;
    ok = n == strlen(text) && !memcmp(d, text, n);
    free(d);
    return ok;
}

static WCHAR *Fx(const WCHAR *name)
{
    return LnJoin(g_fx, name);
}

static void NewImage(WCHAR *root, const WCHAR *name)
{
    WCHAR sys[MAX_PATH];

    wcscpy(root, g_work);
    wcscat(root, L"\\");
    wcscat(root, name);
    LnEnsureDir(root);
    wcscpy(sys, root);
    wcscat(sys, L"\\Windows\\System32");
    LnEnsureDir(sys);
}

static void InstallTrust(const LN_TARGET *t)
{
    WCHAR *dst = LnTargetPath(t, L"trust\\Leeway-LiberNT-Update-DEV.cer");

    LnCopyFileFlush(g_cer, dst);
    free(dst);
}

static void InstallApplierStub(const LN_TARGET *t)
{
    WCHAR *dst = LnTargetFile(t, L"Windows\\System32\\LiberNTUpdatePending.exe");

    WriteText(dst, "boot applier stub");
    free(dst);
}

static BOOL AttemptRecordExists(const LN_TARGET *t)
{
    WCHAR *a = LnTargetPath(t, L"pending\\attempt.json");
    BOOL ex = a && LnPathExists(a);

    free(a);
    return ex;
}

static BOOL OpenImage(LN_TARGET *t, const WCHAR *name)
{
    WCHAR root[MAX_PATH];

    NewImage(root, name);
    return LnTargetOpen(t, LnTargetImage, root, TRUE) == ERROR_SUCCESS;
}

/* ---- tests -------------------------------------------------------------- */

static void TestSha256(void)
{
    char h[65];
    char big[1000];

    Section("sha256");
    LnSha256Hex("", 0, h);
    CHECK(!strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    LnSha256Hex("abc", 3, h);
    CHECK(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    LnSha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, h);
    CHECK(!strcmp(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    memset(big, 'a', sizeof(big));
    LnSha256Hex(big, sizeof(big), h);
    CHECK(strlen(h) == 64 && LnIsSha256Hex(h));
    CHECK(!LnIsSha256Hex("ABCDEF") && !LnIsSha256Hex("zz") && !LnIsSha256Hex(NULL));
}

static DWORD JP(const char *t, LN_JSON **r)
{
    return LnJsonParse(t, strlen(t), r);
}

static void TestJson(void)
{
    LN_JSON *r;
    long long v;
    BOOL b;
    char deep[200];

    Section("json");
    CHECK_ERR(0, JP("{\"a\":1,\"b\":[true,null,\"x\\n\\u00e9\"],\"c\":{\"d\":\"e\\\\f\"}}", &r));
    CHECK(LnJsonGetInt(r, "a", &v) && v == 1);
    CHECK(LnJsonGet(r, "b")->type == LnJsonArray);
    CHECK(!strcmp(LnJsonGetString(LnJsonGet(r, "c"), "d"), "e\\f"));
    CHECK(!LnJsonGetBool(r, "a", &b));
    LnJsonFree(r);
    CHECK(JP("{\"a\":1,}", &r) != 0);
    CHECK(JP("{\"a\":", &r) != 0);
    CHECK(JP("[1] x", &r) != 0);
    CHECK(JP("{\"a\":\"\\u0000\"}", &r) != 0);          /* NUL would truncate a path */
    CHECK(JP("{\"a\":1.5}", &r) != 0);
    memset(deep, '[', 100);
    CHECK(LnJsonParse(deep, 100, &r) != 0);                          /* depth bomb */
}

static void TestPaths(void)
{
    Section("paths and the stage 1 white list");
    CHECK(LnIsSafeRelativePath(L"Windows\\System32\\a.dll"));
    CHECK(!LnIsSafeRelativePath(L"..\\x") && !LnIsSafeRelativePath(L"a\\..\\..\\x"));
    CHECK(!LnIsSafeRelativePath(L"\\Windows\\a") && !LnIsSafeRelativePath(L"C:\\a") && !LnIsSafeRelativePath(L"a/b"));
    CHECK(!LnIsSafeRelativePath(L"") && !LnIsSafeRelativePath(L"a\\\\b") && !LnIsSafeRelativePath(L"a\\b."));
    CHECK(!LnIsSafeRelativePath(L"a:stream") && !LnIsSafeRelativePath(L"a\\b*"));
    /* 8.3 aliases and device names (review B3) */
    CHECK(!LnIsSafeRelativePath(L"Windows\\System32\\NTOSKR~1.EXE") && !LnIsSafeRelativePath(L"Program Files\\PROGRA~2\\x.dll"));
    CHECK(!LnIsSafeRelativePath(L"Windows\\System32\\CON") && !LnIsSafeRelativePath(L"Windows\\System32\\nul.dll"));
    CHECK(!LnIsSafeRelativePath(L"Windows\\System32\\COM1.txt") && !LnIsSafeRelativePath(L"Windows\\System32\\lpt9") && !LnIsSafeRelativePath(L"Windows\\System32\\AUX .dll"));
    CHECK(LnIsSafeRelativePath(L"Windows\\System32\\console.dll") && LnIsSafeRelativePath(L"Windows\\System32\\COM10.dll"));
    CHECK(LnIsReservedName(L"CONOUT$", 7) && LnIsReservedName(L"prn.x.y", 7) && !LnIsReservedName(L"CONTROL", 7));

    /* user components: DLL and EXE */
    CHECK(LnTargetPathAllowed(L"Windows\\System32\\a.dll") && LnTargetPathAllowed(L"Program Files\\App\\app.exe"));
    CHECK(!LnTargetPathAllowed(L"Windows\\System32\\readme.txt") && !LnTargetPathAllowed(L"Windows\\System32\\a.dat"));
    /* kernel, HAL, boot path, critical processes */
    CHECK(!LnTargetPathAllowed(L"Windows\\System32\\ntoskrnl.exe"));
    CHECK(!LnTargetPathAllowed(L"windows\\system32\\NTDLL.DLL"));
    CHECK(!LnTargetPathAllowed(L"Windows\\System32\\hal.dll") && !LnTargetPathAllowed(L"Windows\\System32\\halmacpi.dll"));
    CHECK(!LnTargetPathAllowed(L"Windows\\System32\\config\\SYSTEM.dll"));
    CHECK(!LnTargetPathAllowed(L"Windows\\System32\\smss.exe") && !LnTargetPathAllowed(L"Windows\\System32\\csrss.exe"));
    CHECK(!LnTargetPathAllowed(L"Windows\\System32\\wininit.exe") && !LnTargetPathAllowed(L"Windows\\System32\\winlogon.exe"));
    CHECK(!LnTargetPathAllowed(L"Windows\\System32\\services.exe") && !LnTargetPathAllowed(L"Windows\\System32\\lsass.exe"));
    CHECK(!LnTargetPathAllowed(L"Windows\\System32\\ci.dll"));
    CHECK(!LnTargetPathAllowed(L"LiberNT\\Servicing\\journal\\x.dll"));
    CHECK(!LnTargetPathAllowed(L"Users\\x\\a.dll"));
    /* drivers: only System32\drivers and only Start >= 2 */
    CHECK(!LnTargetPathAllowed(L"Windows\\System32\\drivers\\lnfix.sys"));                       /* no declared Start */
    CHECK(LnTargetItemAllowed(L"Windows\\System32\\drivers\\lnfix.sys", 2));
    CHECK(LnTargetItemAllowed(L"Windows\\System32\\drivers\\lnfix.sys", 3));
    CHECK(!LnTargetItemAllowed(L"Windows\\System32\\drivers\\lnfix.sys", 0));                   /* boot-start */
    CHECK(!LnTargetItemAllowed(L"Windows\\System32\\drivers\\lnfix.sys", 1));                   /* system-start */
    CHECK(!LnTargetItemAllowed(L"Windows\\System32\\lnfix.sys", 3));                            /* not under drivers\ */
    /* boot class: the kernel, HAL, ntdll and early drivers are admitted only by the boot-class check */
    CHECK(LnTargetBootItemAllowed(L"Windows\\System32\\ntoskrnl.exe", -1));
    CHECK(LnTargetBootItemAllowed(L"Windows\\System32\\ntkrnlmp.exe", -1));
    CHECK(LnTargetBootItemAllowed(L"Windows\\System32\\hal.dll", -1));
    CHECK(LnTargetBootItemAllowed(L"Windows\\System32\\halmacpi.dll", -1));
    CHECK(LnTargetBootItemAllowed(L"Windows\\System32\\bootvid.dll", -1));
    CHECK(!LnTargetBootItemAllowed(L"Windows\\System32\\kdcom.dll", -1));                       /* no loader fallback (N3) */
    /* review M3: ntdll, win32k and boot-start drivers (ntfs.sys) have no loader fallback: not in the first delivery */
    CHECK(!LnTargetBootItemAllowed(L"Windows\\System32\\ntdll.dll", -1));
    CHECK(!LnTargetBootItemAllowed(L"Windows\\System32\\win32k.sys", -1));
    CHECK(!LnTargetBootItemAllowed(L"Windows\\System32\\drivers\\ntfs.sys", 0));
    CHECK(!LnTargetBootItemAllowed(L"Windows\\System32\\hal.dll", 0));                          /* Start only for a .sys */
    CHECK(!LnTargetBootItemAllowed(L"Windows\\System32\\config\\SYSTEM", -1));                  /* hives never */
    CHECK(!LnTargetBootItemAllowed(L"Windows\\System32\\freeldr.sys", -1));                     /* loader never */
    CHECK(!LnTargetBootItemAllowed(L"Windows\\System32\\notepad.exe", -1));                     /* user component: not boot class */
    CHECK(!LnTargetBootItemAllowed(L"Windows\\System32\\LiberNTUpdatePending.exe", -1));        /* the updater itself */
    CHECK(LnTargetBootGroup(L"Windows\\System32\\hal.dll") == 1);
    CHECK(LnTargetBootGroup(L"Windows\\System32\\drivers\\ntfs.sys") == 2);
    CHECK(LnTargetBootGroup(L"Windows\\System32\\ntdll.dll") == 3);
    CHECK(LnTargetBootGroup(L"Windows\\System32\\notepad.exe") == 0);
    CHECK(!LnTargetItemAllowed(L"Windows\\System32\\ntdll.dll", -1));                           /* the user route still refuses it */
    {
        /* PE identity read from the first bytes of a file (N4 kernel half) */
        BYTE pe[512];
        DWORD size = 0, sum = 0, ts = 0, v;

        memset(pe, 0, sizeof(pe));
        pe[0] = 'M';
        pe[1] = 'Z';
        v = 0x80;
        memcpy(pe + 0x3C, &v, 4);
        pe[0x80] = 'P';
        pe[0x81] = 'E';
        v = 0x4A5B6C7D;
        memcpy(pe + 0x80 + 8, &v, 4);                       /* TimeDateStamp */
        v = 0x00345000;
        memcpy(pe + 0x80 + 24 + 56, &v, 4);                 /* SizeOfImage */
        v = 0x00351234;
        memcpy(pe + 0x80 + 24 + 64, &v, 4);                 /* CheckSum */
        CHECK(LnPeIdentity(pe, sizeof(pe), &size, &sum, &ts));
        CHECK(size == 0x00345000 && sum == 0x00351234 && ts == 0x4A5B6C7D);
        pe[0] = 'X';
        CHECK(!LnPeIdentity(pe, sizeof(pe), &size, &sum, &ts));                 /* not MZ */
        pe[0] = 'M';
        pe[0x80] = 'N';
        CHECK(!LnPeIdentity(pe, sizeof(pe), &size, &sum, &ts));                 /* not PE */
        CHECK(!LnPeIdentity(pe, 16, &size, &sum, &ts));                         /* too short */
    }
    /* which kernel set is running (B2 review M1): the load options name *.old files on a fallback boot */
    CHECK(LnOptionsUseOldTrio(L"/KERNEL=ntkrnlmp.old /HAL=hal.old /BOOTVID=bootvid.old /FASTDETECT"));
    CHECK(LnOptionsUseOldTrio(L"/FASTDETECT /kernel=ntoskrnl.OLD"));
    CHECK(LnOptionsUseOldTrio(L"/HAL=hal.old"));
    CHECK(!LnOptionsUseOldTrio(L"/KERNEL=ntkrnlmp.exe /FASTDETECT /KDWATCHDOG=0"));
    CHECK(!LnOptionsUseOldTrio(L""));
    CHECK(!LnOptionsUseOldTrio(L"/DEBUG /DEBUGPORT=COM1 /SOMEKERNEL=x.old"));       /* only the trio keys count */
    {
        /* freeldr.ini fallback check (review N2) against the dx2400 layout */
        static const char ok[] =
            "[FREELOADER]\r\nDefaultOS=ReactOS\r\nTimeOut=5\r\n[Operating Systems]\r\nReactOS=\"LiberNT\"\r\n"
            "LiberNT_Fallback=\"LiberNT (old kernel)\"\r\n[ReactOS]\r\nBootType=Windows2003\r\n"
            "SystemPath=partition(2)\\Windows\r\nOptions=/KERNEL=ntkrnlmp.exe /FASTDETECT\r\n"
            "[LiberNT_Fallback]\r\nBootType=Windows2003\r\nSystemPath=partition(2)\\Windows\r\n"
            "Options=/KERNEL=ntkrnlmp.old /HAL=hal.old /BOOTVID=bootvid.old /FASTDETECT\r\n";
        static const char nofb[] =
            "[FREELOADER]\r\nDefaultOS=ReactOS\r\n[Operating Systems]\r\nReactOS=\"LiberNT\"\r\n"
            "[ReactOS]\r\nOptions=/KERNEL=ntkrnlmp.exe\r\n";
        static const char halacpi[] =
            "[FREELOADER]\r\nDefaultOS=A\r\n[Operating Systems]\r\nA=\"x\"\r\nB=\"y\"\r\n"
            "[A]\r\nOptions=/KERNEL=ntkrnlmp.exe /HAL=halmacpi.dll\r\n"
            "[B]\r\nOptions=/KERNEL=ntkrnlmp.old /HAL=hal.old /BOOTVID=bootvid.old\r\n";
        const WCHAR *good[3] = { L"Windows\\System32\\ntkrnlmp.exe", L"Windows\\System32\\hal.dll",
                                 L"Windows\\System32\\bootvid.dll" };
        const WCHAR *wrongKernel[3] = { L"Windows\\System32\\ntoskrnl.exe", L"Windows\\System32\\hal.dll",
                                        L"Windows\\System32\\bootvid.dll" };
        const WCHAR *halmac[3] = { L"Windows\\System32\\ntkrnlmp.exe", L"Windows\\System32\\halmacpi.dll",
                                   L"Windows\\System32\\bootvid.dll" };
        char why[160];

        static const char detecthal[] =
            "[FREELOADER]\r\nDefaultOS=A\r\n[Operating Systems]\r\nA=\"x\"\r\nB=\"y\"\r\n"
            "[A]\r\nSystemPath=partition(2)\\Windows\r\nOptions=/KERNEL=ntkrnlmp.exe /DETECTHAL\r\n"
            "[B]\r\nSystemPath=partition(2)\\Windows\r\nOptions=/KERNEL=ntkrnlmp.old /HAL=hal.old /BOOTVID=bootvid.old\r\n";
        static const char fbdetecthal[] =
            "[FREELOADER]\r\nDefaultOS=A\r\n[Operating Systems]\r\nA=\"x\"\r\nB=\"y\"\r\n"
            "[A]\r\nSystemPath=partition(2)\\Windows\r\nOptions=/KERNEL=ntkrnlmp.exe\r\n"
            "[B]\r\nSystemPath=partition(2)\\Windows\r\nOptions=/KERNEL=ntkrnlmp.old /HAL=hal.old /BOOTVID=bootvid.old /DETECTHAL\r\n";
        static const char otherpath[] =
            "[FREELOADER]\r\nDefaultOS=A\r\n[Operating Systems]\r\nA=\"x\"\r\nB=\"y\"\r\n"
            "[A]\r\nSystemPath=partition(2)\\Windows\r\nOptions=/KERNEL=ntkrnlmp.exe\r\n"
            "[B]\r\nSystemPath=partition(3)\\Windows\r\nOptions=/KERNEL=ntkrnlmp.old /HAL=hal.old /BOOTVID=bootvid.old\r\n";
        CHECK(LnFreeldrFallbackMatches(ok, sizeof(ok) - 1, good, 3, why, sizeof(why)));
        CHECK(!LnFreeldrFallbackMatches(detecthal, sizeof(detecthal) - 1, good, 3, why, sizeof(why)));    /* /DETECTHAL */
        CHECK(!LnFreeldrFallbackMatches(fbdetecthal, sizeof(fbdetecthal) - 1, good, 3, why, sizeof(why)));
        CHECK(!LnFreeldrFallbackMatches(otherpath, sizeof(otherpath) - 1, good, 3, why, sizeof(why)));    /* SystemPath */
        CHECK(!LnFreeldrFallbackMatches(ok, sizeof(ok) - 1, wrongKernel, 3, why, sizeof(why)));   /* default boots ntkrnlmp */
        CHECK(!LnFreeldrFallbackMatches(nofb, sizeof(nofb) - 1, good, 3, why, sizeof(why)));      /* no fallback entry */
        CHECK(!LnFreeldrFallbackMatches(halacpi, sizeof(halacpi) - 1, good, 3, why, sizeof(why))); /* HAL differs */
        CHECK(!LnFreeldrFallbackMatches(halacpi, sizeof(halacpi) - 1, halmac, 3, why, sizeof(why))); /* fallback names hal.old */
        CHECK(!LnFreeldrFallbackMatches(ok, sizeof(ok) - 1, good, 2, why, sizeof(why)));          /* not the whole trio */
    }
    CHECK(!LnTargetItemAllowed(L"Windows\\System32\\a.dll", 3));                                /* Start only for a .sys */
    CHECK(!LnTargetItemAllowed(L"Windows\\System32\\drivers\\win32k.sys", 3));
}

static void TestManifest(void)
{
    LN_MANIFEST m;
    static const char head[] = "{\"schemaVersion\":1,\"packageId\":\"p.id\",\"releaseVersion\":\"1.0\",\"type\":\"driver\","
        "\"targetFamily\":\"LiberNT\",\"architecture\":\"amd64\","
        "\"certificateSha256\":\"829459d8db6adc0a4fe7500e0572149e1d469b6a3707cf48e374b7660d754a34\",\"payloads\":[";
    static const char sha[] = "197c7c60ef8a8470a38d1a9212bdfde9cfe6fd4be910825fe6ac7880ac765d16";
    char text[1400];

#define MK(items) do { strcpy(text, head); strcat(text, items); strcat(text, "]}"); } while (0)
    Section("manifest");
    MK("{\"path\":\"a.bin\",\"target\":\"Windows\\\\System32\\\\a.dll\",\"sha256\":\"197c7c60ef8a8470a38d1a9212bdfde9cfe6fd4be910825fe6ac7880ac765d16\"}");
    CHECK_ERR(0, LnManifestParse(text, strlen(text), &m));
    CHECK(m.itemCount == 1 && !strcmp(m.packageId, "p.id") && !strcmp(m.items[0].sha256, sha));
    CHECK(strlen(m.manifestSha256) == 64);
    LnManifestFree(&m);

    MK("{\"action\":\"delete\",\"target\":\"Windows\\\\System32\\\\old.dll\"}");
    CHECK_ERR(0, LnManifestParse(text, strlen(text), &m));
    CHECK(m.items[0].isDelete);
    LnManifestFree(&m);

    MK("{\"path\":\"a.bin\",\"target\":\"Windows\\\\System32\\\\ntoskrnl.exe\",\"sha256\":\"197c7c60ef8a8470a38d1a9212bdfde9cfe6fd4be910825fe6ac7880ac765d16\"}");
    CHECK_ERR(LN_E_FORBIDDEN_TARGET, LnManifestParse(text, strlen(text), &m));
    MK("{\"path\":\"..\\\\a.bin\",\"target\":\"Windows\\\\System32\\\\a.dll\",\"sha256\":\"197c7c60ef8a8470a38d1a9212bdfde9cfe6fd4be910825fe6ac7880ac765d16\"}");
    CHECK_ERR(LN_E_MANIFEST_INVALID, LnManifestParse(text, strlen(text), &m));
    MK("{\"path\":\"a.bin\",\"target\":\"Windows\\\\System32\\\\..\\\\..\\\\x.dll\",\"sha256\":\"197c7c60ef8a8470a38d1a9212bdfde9cfe6fd4be910825fe6ac7880ac765d16\"}");
    CHECK_ERR(LN_E_FORBIDDEN_TARGET, LnManifestParse(text, strlen(text), &m));
    MK("{\"path\":\"a.bin\",\"target\":\"Windows\\\\System32\\\\a.dll\",\"sha256\":\"NOTAHASH\"}");
    CHECK_ERR(LN_E_MANIFEST_INVALID, LnManifestParse(text, strlen(text), &m));
    MK("{\"path\":\"a.bin\",\"target\":\"Windows\\\\System32\\\\a.dll\",\"sha256\":\"197c7c60ef8a8470a38d1a9212bdfde9cfe6fd4be910825fe6ac7880ac765d16\"},"
       "{\"path\":\"b.bin\",\"target\":\"windows\\\\system32\\\\A.DLL\",\"sha256\":\"197c7c60ef8a8470a38d1a9212bdfde9cfe6fd4be910825fe6ac7880ac765d16\"}");
    CHECK_ERR(LN_E_MANIFEST_INVALID, LnManifestParse(text, strlen(text), &m));   /* same target twice */
    {
        char drv[1400];

        /* drivers: Start declared by the signed manifest, 2..4 only */
        MK("{\"path\":\"d.bin\",\"target\":\"Windows\\\\System32\\\\drivers\\\\d.sys\",\"sha256\":\"197c7c60ef8a8470a38d1a9212bdfde9cfe6fd4be910825fe6ac7880ac765d16\",\"driverStart\":3}");
        CHECK_ERR(0, LnManifestParse(text, strlen(text), &m));
        CHECK(m.items[0].driverStart == 3);
        LnManifestFree(&m);
        MK("{\"path\":\"d.bin\",\"target\":\"Windows\\\\System32\\\\drivers\\\\d.sys\",\"sha256\":\"197c7c60ef8a8470a38d1a9212bdfde9cfe6fd4be910825fe6ac7880ac765d16\",\"driverStart\":0}");
        CHECK_ERR(LN_E_FORBIDDEN_TARGET, LnManifestParse(text, strlen(text), &m));
        MK("{\"path\":\"d.bin\",\"target\":\"Windows\\\\System32\\\\drivers\\\\d.sys\",\"sha256\":\"197c7c60ef8a8470a38d1a9212bdfde9cfe6fd4be910825fe6ac7880ac765d16\"}");
        CHECK_ERR(LN_E_FORBIDDEN_TARGET, LnManifestParse(text, strlen(text), &m));
        strcpy(drv, text);
        MK("{\"path\":\"a.bin\",\"target\":\"Windows\\\\System32\\\\NTOSKR~1.EXE\",\"sha256\":\"197c7c60ef8a8470a38d1a9212bdfde9cfe6fd4be910825fe6ac7880ac765d16\"}");
        CHECK_ERR(LN_E_FORBIDDEN_TARGET, LnManifestParse(text, strlen(text), &m));
        MK("{\"path\":\"a.bin\",\"target\":\"Windows\\\\System32\\\\winlogon.exe\",\"sha256\":\"197c7c60ef8a8470a38d1a9212bdfde9cfe6fd4be910825fe6ac7880ac765d16\"}");
        CHECK_ERR(LN_E_FORBIDDEN_TARGET, LnManifestParse(text, strlen(text), &m));
        MK("{\"path\":\"a.bin\",\"target\":\"Windows\\\\System32\\\\a.txt\",\"sha256\":\"197c7c60ef8a8470a38d1a9212bdfde9cfe6fd4be910825fe6ac7880ac765d16\"}");
        CHECK_ERR(LN_E_FORBIDDEN_TARGET, LnManifestParse(text, strlen(text), &m));
        /* a package id becomes a directory name in the store: no device names */
        {
            char bad[1400];

            MK("{\"path\":\"a.bin\",\"target\":\"Windows\\\\System32\\\\a.dll\",\"sha256\":\"197c7c60ef8a8470a38d1a9212bdfde9cfe6fd4be910825fe6ac7880ac765d16\"}");
            strcpy(bad, text);
            {
                char *id = strstr(bad, "p.id");

                if (id)
                    memcpy(id, "CON ", 4);                  /* "CON " is the device (trailing space) */
                CHECK(id != NULL);
            }
            memmove(strstr(bad, "CON ") + 3, strstr(bad, "CON ") + 4, strlen(strstr(bad, "CON ") + 4) + 1);
            CHECK_ERR(LN_E_MANIFEST_INVALID, LnManifestParse(bad, strlen(bad), &m));
        }
    }
    CHECK_ERR(LN_E_MANIFEST_INVALID, LnManifestParse("{\"schemaVersion\":2}", strlen("{\"schemaVersion\":2}"), &m));
    CHECK_ERR(LN_E_MANIFEST_INVALID, LnManifestParse("not json", 8, &m));
#undef MK
}

static void TestTransactionId(void)
{
    char a[65], b[65], c[65], d[65];

    Section("transaction id (S1-CONTRACTS.md section 3)");
    LnTransactionId("t", "p", "1", "m", "install", a);
    LnTransactionId("t", "p", "1", "m", "install", b);
    LnTransactionId("t", "p", "1", "m", "rollback", c);
    LnTransactionId("tp", "", "1", "m", "install", d);      /* the separator keeps fields apart */
    CHECK(!strcmp(a, b) && strcmp(a, c) && strcmp(a, d));
    {
        char expect[65];
        static const char raw[] = "t\np\n1\nm\ninstall";   /* machineId \n packageId \n version \n digest \n operation */

        LnSha256Hex(raw, sizeof(raw) - 1, expect);
        CHECK(!strcmp(a, expect));
    }
}

static void TestJournal(void)
{
    LN_TARGET t;
    LN_JENTRY e;
    LN_JOURNAL j;
    WCHAR *jp;
    BYTE *data;
    size_t len;
    int i;

    Section("journal");
    CHECK(OpenImage(&t, L"journal"));
    for (i = 0; i < 3; i++)
    {
        memset(&e, 0, sizeof(e));
        strcpy(e.transactionId, "aa");
        strcpy(e.phase, i == 0 ? "prepared" : i == 1 ? "applying" : "applied");
        strcpy(e.result, "ok");
        strcpy(e.error, "quote\" backslash\\ tab\t");
        CHECK_ERR(0, LnJournalAppend(&t, &e));
    }
    CHECK_ERR(0, LnJournalRead(&t, &j));
    CHECK(j.count == 3 && j.entries[2].sequence == 3 && !strcmp(j.entries[2].phase, "applied"));
    CHECK(!strcmp(j.entries[0].error, "quote\" backslash\\ tab\t"));
    CHECK(strcmp(j.entries[1].previousEntrySha256, j.entries[0].previousEntrySha256) != 0);
    CHECK(!strcmp(LnJournalLast(&j, "aa")->phase, "applied"));
    LnJournalFree(&j);

    /* a torn last line is ignored, the next append terminates it and the chain stays valid */
    jp = LnTargetPath(&t, L"journal\\transactions.jsonl");
    CHECK_ERR(0, LnReadFileAll(jp, &data, &len));
    {
        LN_BUF b;

        LnBufInit(&b);
        LnBufAppend(&b, (char *)data, len);
        LnBufAppendStr(&b, "{\"schemaVersion\":1,\"sequence\":4,\"transac");
        CHECK_ERR(0, LnWriteFileAtomic(jp, b.data, b.len));
        LnBufFree(&b);
    }
    free(data);
    CHECK_ERR(0, LnJournalRead(&t, &j));
    CHECK(j.count == 3 && j.tailTruncated);
    LnJournalFree(&j);
    memset(&e, 0, sizeof(e));
    strcpy(e.transactionId, "aa"); strcpy(e.phase, "confirmed"); strcpy(e.result, "ok");
    CHECK_ERR(0, LnJournalAppend(&t, &e));
    CHECK_ERR(0, LnJournalRead(&t, &j));
    CHECK(j.count == 4 && j.entries[3].sequence == 4);
    LnJournalFree(&j);

    /* altering or removing an earlier entry breaks the chain */
    CHECK_ERR(0, LnReadFileAll(jp, &data, &len));
    {
        char *p = strstr((char *)data, "\"phase\":\"applying\"");

        CHECK(p != NULL);
        if (p)
        {
            memcpy(p, "\"phase\":\"xpplying\"", 18);
            CHECK_ERR(0, LnWriteFileAtomic(jp, data, len));
            CHECK_ERR(LN_E_JOURNAL_CORRUPT, LnJournalRead(&t, &j));
        }
    }
    free(data);
    free(jp);
    CHECK(OpenImage(&t, L"journal2"));
    for (i = 0; i < 3; i++)
    {
        memset(&e, 0, sizeof(e));
        strcpy(e.transactionId, "bb"); strcpy(e.phase, "applying"); strcpy(e.result, "ok");
        LnJournalAppend(&t, &e);
    }
    jp = LnTargetPath(&t, L"journal\\transactions.jsonl");
    LnReadFileAll(jp, &data, &len);
    {
        char *nl = strchr((char *)data, '\n');              /* drop the first line */
        CHECK(nl != NULL);
        if (nl)
        {
            LnWriteFileAtomic(jp, nl + 1, len - (size_t)(nl + 1 - (char *)data));
            CHECK_ERR(LN_E_JOURNAL_CORRUPT, LnJournalRead(&t, &j));
        }
    }
    free(data);
    free(jp);
}

static void TestTarget(void)
{
    LN_TARGET a, b;
    WCHAR root[MAX_PATH];

    Section("target");
    NewImage(root, L"target");
    CHECK_ERR(ERROR_PATH_NOT_FOUND, LnTargetOpen(&a, LnTargetImage, root, FALSE));   /* no servicing tree yet */
    CHECK_ERR(0, LnTargetOpen(&a, LnTargetImage, root, TRUE));
    CHECK_ERR(0, LnTargetOpen(&b, LnTargetImage, root, FALSE));
    CHECK(strlen(a.identity) == 36 && a.identity[8] == '-' && !strcmp(a.identity, b.identity));   /* machineId GUID */
    CHECK_ERR(ERROR_PATH_NOT_FOUND, LnTargetOpen(&a, LnTargetImage, L"C:\\does\\not\\exist\\xyz", TRUE));
}

static void TestVerify(void)
{
    WCHAR *p, bogus[MAX_PATH];
    LN_MANIFEST m;

    Section("verify (enforce): manifest.sig is the root of trust");
    p = Fx(L"good1");
    CHECK_ERR(0, LnServicingVerifyPackage(p, g_cer, &m));
    CHECK(m.itemCount == 1);
    LnManifestFree(&m);
    CHECK_ERR(LN_E_NO_TRUST_ANCHOR, LnServicingVerifyPackage(p, L"C:\\no\\such\\certificate.cer", NULL));
    /* a pinned certificate that is not the one the manifest names */
    wcscpy(bogus, g_work);
    wcscat(bogus, L"\\other.cer");
    {
        BYTE fake[64];

        memset(fake, 0x30, sizeof(fake));
        CHECK_ERR(0, LnWriteFileAtomic(bogus, fake, sizeof(fake)));
    }
    CHECK_ERR(LN_E_CERT_MISMATCH, LnServicingVerifyPackage(p, bogus, NULL));
    free(p);

    p = Fx(L"tampered-payload");
    CHECK_ERR(LN_E_HASH_MISMATCH, LnServicingVerifyPackage(p, g_cer, NULL));
    free(p);
    p = Fx(L"tampered-manifest");
    CHECK_ERR(LN_E_BAD_SIGNATURE, LnServicingVerifyPackage(p, g_cer, NULL));          /* manifest changed after signing */
    free(p);
    p = Fx(L"tampered-sig");
    CHECK_ERR(LN_E_BAD_SIGNATURE, LnServicingVerifyPackage(p, g_cer, NULL));          /* manifest.sig damaged */
    free(p);
    p = Fx(L"no-manifest-sig");
    CHECK_ERR(LN_E_UNSIGNED, LnServicingVerifyPackage(p, g_cer, NULL));               /* manifest.sig missing */
    free(p);
    p = Fx(L"unsigned");
    CHECK_ERR(0, LnServicingVerifyPackage(p, g_cer, NULL));                           /* no catalog: it is optional */
    free(p);
    p = Fx(L"unsigned-catalog");
    CHECK_ERR(LN_E_UNSIGNED, LnServicingVerifyPackage(p, g_cer, NULL));               /* a catalog that is there must verify */
    free(p);
    p = Fx(L"wrongcert");
    CHECK_ERR(LN_E_CERT_MISMATCH, LnServicingVerifyPackage(p, g_cer, NULL));          /* valid signature, other signer */
    free(p);
    p = Fx(L"kernel");
    CHECK_ERR(LN_E_FORBIDDEN_TARGET, LnServicingVerifyPackage(p, g_cer, NULL));
    free(p);
    p = Fx(L"missing-payload");
    CHECK_ERR(LN_E_HASH_MISMATCH, LnServicingVerifyPackage(p, g_cer, NULL));
    free(p);
    /* negative cases of the signature itself: a copy of a good package whose manifest.sig is altered */
    {
        WCHAR *src = Fx(L"good1"), *dst = LnJoin(g_work, L"sigcopy"), *sigp = LnJoin(dst, L"manifest.sig");
        BYTE *sig = NULL, *cer = NULL, bad[300], mod[256];
        size_t slen = 0, clen = 0, k;
        uint32_t ex = 0;
        int flips[] = { 0, 1, 100, 255 };

        LnRemoveTree(dst);
        CHECK_ERR(0, LnCopyTreeFlush(src, dst));
        CHECK_ERR(0, LnReadFileAll(sigp, &sig, &slen));
        CHECK(slen == 256);
        for (k = 0; k < sizeof(flips) / sizeof(flips[0]); k++)
        {
            memcpy(bad, sig, 256);
            bad[flips[k]] ^= 0x01;                                                        /* one bit of the signature */
            CHECK_ERR(0, LnWriteFileAtomic(sigp, bad, 256));
            CHECK_ERR(LN_E_BAD_SIGNATURE, LnServicingVerifyPackage(dst, g_cer, NULL));
        }
        memcpy(bad, sig, 256);
        memset(bad + 256, 0, 2);
        CHECK_ERR(0, LnWriteFileAtomic(sigp, bad, 255));                                  /* too short */
        CHECK_ERR(LN_E_BAD_SIGNATURE, LnServicingVerifyPackage(dst, g_cer, NULL));
        CHECK_ERR(0, LnWriteFileAtomic(sigp, bad, 257));                                  /* too long */
        CHECK_ERR(LN_E_BAD_SIGNATURE, LnServicingVerifyPackage(dst, g_cer, NULL));
        CHECK_ERR(0, LnWriteFileAtomic(sigp, bad, 1));                                    /* nearly empty */
        CHECK_ERR(LN_E_BAD_SIGNATURE, LnServicingVerifyPackage(dst, g_cer, NULL));
        CHECK_ERR(0, LnWriteFileAtomic(sigp, sig, 256));                                  /* restored: valid again */
        CHECK_ERR(0, LnServicingVerifyPackage(dst, g_cer, NULL));

        /* the certificate's key: the real one is accepted, e != 65537 and a short modulus are not */
        CHECK_ERR(0, LnReadFileAll(g_cer, &cer, &clen));
        CHECK(LnTestCertRsa2048Key(cer, clen, mod, &ex) && ex == 65537 && (mod[0] & 0x80));
        for (k = 0; k + 5 < clen; k++)
            if (cer[k] == 0x02 && cer[k + 1] == 0x03 && cer[k + 2] == 0x01 && cer[k + 3] == 0x00 && cer[k + 4] == 0x01)
                break;
        CHECK(k + 5 < clen);
        if (k + 5 < clen)
        {
            cer[k + 2] = 0x00;                                                            /* e = 3 */
            cer[k + 3] = 0x00;
            cer[k + 4] = 0x03;
            CHECK(!LnTestCertRsa2048Key(cer, clen, mod, &ex));
            cer[k + 2] = 0x01;                                                            /* e = 65539 */
            cer[k + 3] = 0x00;
            cer[k + 4] = 0x03;
            CHECK(!LnTestCertRsa2048Key(cer, clen, mod, &ex));
            cer[k + 4] = 0x01;                                                            /* back to 65537 */
            CHECK(LnTestCertRsa2048Key(cer, clen, mod, &ex));
        }
        for (k = 0; k + 6 < clen; k++)
            if (cer[k] == 0x02 && cer[k + 1] == 0x82 && cer[k + 2] == 0x01 && cer[k + 3] == 0x01 && cer[k + 4] == 0x00)
                break;
        CHECK(k + 6 < clen);
        if (k + 6 < clen)
        {
            cer[k + 5] &= 0x7F;                                                           /* modulus is no longer 2048 bits */
            CHECK(!LnTestCertRsa2048Key(cer, clen, mod, &ex));
            cer[k + 5] |= 0x80;
            cer[k + 5 + 255] &= 0xFE;                                                     /* even modulus */
            CHECK(!LnTestCertRsa2048Key(cer, clen, mod, &ex));
        }
        CHECK(!LnTestCertRsa2048Key(cer, 10, mod, &ex));                                  /* truncated DER */
        free(sig);
        free(cer);
        LnRemoveTree(dst);
        free(sigp);
        free(dst);
        free(src);
    }
    /* drivers carry a signed catalog on top of manifest.sig (M6) */
    p = Fx(L"driver-cat");
    CHECK_ERR(0, LnServicingVerifyPackage(p, g_cer, &m));
    CHECK(m.items[0].driverStart == 3);
    LnManifestFree(&m);
    free(p);
    p = Fx(L"driver-nocat");
    CHECK_ERR(LN_E_UNSIGNED, LnServicingVerifyPackage(p, g_cer, NULL));
    free(p);
}

static void TestAddRejects(void)
{
    LN_TARGET t;
    WCHAR *p;
    char tx[65];
    LN_JOURNAL j;
    char (*ids)[130];
    size_t n;

    Section("add-package rejects before staging");
    CHECK(OpenImage(&t, L"rejects"));
    p = Fx(L"good1");
    CHECK_ERR(LN_E_NO_TRUST_ANCHOR, LnServicingAddPackage(&t, p, tx));                /* no certificate installed */
    free(p);
    InstallTrust(&t);
    p = Fx(L"tampered-payload");
    CHECK_ERR(LN_E_HASH_MISMATCH, LnServicingAddPackage(&t, p, tx));
    free(p);
    p = Fx(L"no-manifest-sig");
    CHECK_ERR(LN_E_UNSIGNED, LnServicingAddPackage(&t, p, tx));
    free(p);
    p = Fx(L"tampered-sig");
    CHECK_ERR(LN_E_BAD_SIGNATURE, LnServicingAddPackage(&t, p, tx));
    free(p);
    p = Fx(L"wrongcert");
    CHECK_ERR(LN_E_CERT_MISMATCH, LnServicingAddPackage(&t, p, tx));
    free(p);
    CHECK_ERR(0, LnStoreList(&t, &ids, &n));
    CHECK(n == 0);                                        /* nothing reached the store */
    free(ids);
    {
        WCHAR *objs = LnTargetPath(&t, L"store\\objects");

        CHECK(!LnPathExists(objs));
        free(objs);
    }
    CHECK_ERR(0, LnJournalRead(&t, &j));
    CHECK(j.count >= 3 && !strcmp(j.entries[j.count - 1].phase, "failed"));          /* rejections are journaled */
    LnJournalFree(&j);
}

static void TestLifecycle(void)
{
    LN_TARGET t;
    WCHAR *p, *target, *sys;
    char tx[65], tx2[65];
    BOOL reboot;
    LN_JOURNAL j;
    size_t before;

    Section("lifecycle: add, apply, idempotence, confirm, rollback");
    CHECK(OpenImage(&t, L"life"));
    InstallTrust(&t);
    target = LnTargetFile(&t, L"Windows\\System32\\lnfixture.dll");
    WriteText(target, "original");

    p = Fx(L"good1");
    CHECK_ERR(0, LnServicingAddPackage(&t, p, tx));
    CHECK_ERR(0, LnServicingAddPackage(&t, p, tx2));                                   /* idempotent */
    CHECK(!strcmp(tx, tx2));
    CHECK(FileIs(target, "original"));                                                 /* staging touches nothing */
    free(p);

    CHECK_ERR(0, LnServicingApplyPackage(&t, "ln.fixture", "1.0", &reboot, tx));
    CHECK(!reboot && FileIs(target, "version one"));
    CHECK_ERR(0, LnJournalRead(&t, &j));
    CHECK(!strcmp(LnJournalLast(&j, tx)->phase, "applied") && !LnJournalLast(&j, tx)->healthConfirmed);
    before = j.count;
    LnJournalFree(&j);
    CHECK_ERR(0, LnServicingApplyPackage(&t, "ln.fixture", "1.0", &reboot, tx2));      /* same request again */
    CHECK_ERR(0, LnJournalRead(&t, &j));
    CHECK(j.count == before);                                                          /* no duplicate work */
    LnJournalFree(&j);

    /* the backup was verified before the first change */
    {
        WCHAR bak[MAX_PATH];
        WCHAR *w = LnUtf8ToWide(tx), *bp;

        wcscpy(bak, L"backup\\");
        wcscat(bak, w);
        wcscat(bak, L"\\0001");
        bp = LnTargetPath(&t, bak);
        CHECK(FileIs(bp, "original"));
        free(bp);
        free(w);
    }

    /* not confirmed yet; corrupting the component makes confirmation refuse */
    WriteText(target, "damaged by someone");
    CHECK_ERR(LN_E_VERIFY_FINAL, LnServicingConfirmHealth(&t, "ln.fixture", "1.0"));
    CHECK_ERR(0, LnJournalRead(&t, &j));
    CHECK(!LnJournalLast(&j, tx)->healthConfirmed && !strcmp(LnJournalLast(&j, tx)->phase, "applied"));   /* history is not rewritten */
    LnJournalFree(&j);
    {
        LN_ATTEMPT att;

        CHECK_ERR(0, LnServicingReadAttempt(&t, &att));                                 /* but the failure is recorded */
        CHECK(att.failureRecorded && !att.healthConfirmed);
    }

    /* rollback works even though the updated component is damaged */
    CHECK_ERR(0, LnServicingRollback(&t, "ln.fixture", "1.0", tx));
    CHECK(FileIs(target, "original"));
    CHECK_ERR(0, LnJournalRead(&t, &j));
    CHECK(!strcmp(LnJournalLast(&j, tx)->phase, "rolled-back"));
    LnJournalFree(&j);
    CHECK_ERR(0, LnServicingRollback(&t, "ln.fixture", "1.0", tx));                    /* idempotent */

    /* apply again after a rollback, confirm, then remove-package restores the original */
    CHECK_ERR(0, LnServicingApplyPackage(&t, "ln.fixture", "1.0", &reboot, tx));
    CHECK(FileIs(target, "version one"));
    CHECK_ERR(0, LnServicingConfirmHealth(&t, "ln.fixture", "1.0"));
    CHECK_ERR(0, LnJournalRead(&t, &j));
    CHECK(!strcmp(LnJournalLast(&j, tx)->phase, "confirmed") && LnJournalLast(&j, tx)->healthConfirmed);
    LnJournalFree(&j);
    CHECK(!AttemptRecordExists(&t));                        /* a closed transaction leaves no attempt record */
    CHECK_ERR(0, LnServicingConfirmHealth(&t, "ln.fixture", "1.0"));                   /* idempotent */
    CHECK(!AttemptRecordExists(&t));
    CHECK_ERR(0, LnServicingRemovePackage(&t, "ln.fixture", "1.0"));
    CHECK(FileIs(target, "original"));
    CHECK_ERR(LN_E_NOT_FOUND, LnServicingApplyPackage(&t, "ln.fixture", "1.0", &reboot, tx));

    /* the state view is derived and reconstructible */
    {
        WCHAR *sp = LnTargetPath(&t, L"journal\\state.json");

        CHECK(LnPathExists(sp));
        DeleteFileW(sp);
        CHECK_ERR(0, LnJournalWriteState(&t));
        CHECK(LnPathExists(sp));
        free(sp);
    }
    sys = NULL;
    free(sys);
    free(target);
}

static void TestFailureRollsBack(void)
{
    LN_TARGET t;
    WCHAR *p, *t1, *t2, *dir;
    char tx[65];
    BOOL reboot;
    LN_JOURNAL j;
    HANDLE hold;

    Section("failed operation: immediate rollback, then recovery");
    CHECK(OpenImage(&t, L"fail"));
    InstallTrust(&t);
    t1 = LnTargetFile(&t, L"Windows\\System32\\first.dll");
    dir = LnTargetFile(&t, L"Windows\\System32\\blocked");
    t2 = LnTargetFile(&t, L"Windows\\System32\\blocked\\second.dll");
    WriteText(t1, "first original");
    WriteText(t2, "second original");

    p = Fx(L"twoitems");
    CHECK_ERR(0, LnServicingAddPackage(&t, p, tx));
    free(p);

    /* the second file is open without FILE_SHARE_DELETE: reading it works, replacing it does not */
    hold = CreateFileW(t2, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(hold != INVALID_HANDLE_VALUE);
    CHECK(LnServicingApplyPackage(&t, "ln.twoitems", "1.0", &reboot, tx) != 0);
    CHECK(FileIs(t1, "first original"));                                               /* the first one was reverted */
    CHECK(FileIs(t2, "second original"));
    CHECK_ERR(0, LnJournalRead(&t, &j));
    {
        const LN_JENTRY *l = LnJournalLast(&j, tx);

        CHECK(l && !strcmp(l->phase, "rolled-back"));
    }
    {
        size_t k;
        BOOL sawFailed = FALSE, sawPrepared = FALSE;

        for (k = 0; k < j.count; k++)
        {
            if (!strcmp(j.entries[k].phase, "failed")) sawFailed = TRUE;
            if (!strcmp(j.entries[k].phase, "rollback-prepared") && !strcmp(j.entries[k].error, "verifiable-failure")) sawPrepared = TRUE;
        }
        CHECK(sawFailed && sawPrepared);
    }
    LnJournalFree(&j);
    CloseHandle(hold);                                                                 /* cause removed: retry succeeds */
    CHECK_ERR(0, LnServicingApplyPackage(&t, "ln.twoitems", "1.0", &reboot, tx));
    CHECK(FileIs(t1, "first new") && FileIs(t2, "second"));
    free(t1);
    free(t2);
    free(dir);
}

static void TestCorruptStore(void)
{
    LN_TARGET t;
    WCHAR *p, *obj, *target;
    char tx[65];
    BOOL reboot;
    LN_MANIFEST m;

    Section("corrupt store object is refused before any change");
    CHECK(OpenImage(&t, L"corrupt"));
    InstallTrust(&t);
    p = Fx(L"good1");
    CHECK_ERR(0, LnServicingAddPackage(&t, p, tx));
    CHECK_ERR(0, LnServicingVerifyPackage(p, g_cer, &m));
    free(p);
    CHECK_ERR(0, LnStoreObjectPath(&t, m.items[0].sha256, &obj));
    WriteText(obj, "flipped bits");
    target = LnTargetFile(&t, L"Windows\\System32\\lnfixture.dll");
    CHECK_ERR(LN_E_HASH_MISMATCH, LnServicingApplyPackage(&t, "ln.fixture", "1.0", &reboot, tx));
    CHECK(!LnPathExists(target));                                                      /* nothing was written */
    LnManifestFree(&m);
    free(obj);
    free(target);
}

static void TestPendingPlan(void)
{
    LN_TARGET t;
    WCHAR *p, *target;
    char tx[65], *plan;
    BOOL reboot;
    HANDLE lock;
    LN_ATTEMPT a;
    const char *why;
    LN_JSON *root;

    Section("locked target: pending-first-boot plan and attempt rules (S1-CONTRACTS.md 1, 5)");
    CHECK(OpenImage(&t, L"pending"));
    InstallTrust(&t);
    InstallApplierStub(&t);
    target = LnTargetFile(&t, L"Windows\\System32\\lnfixture.dll");
    WriteText(target, "locked original");
    p = Fx(L"good1");
    CHECK_ERR(0, LnServicingAddPackage(&t, p, tx));
    free(p);

    lock = CreateFileW(target, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);   /* readers only: replace is denied */
    CHECK(lock != INVALID_HANDLE_VALUE);
    LnTestLockedAsPending = TRUE;
    CHECK_ERR(0, LnServicingApplyPackage(&t, "ln.fixture", "1.0", &reboot, tx));
    LnTestLockedAsPending = FALSE;
    CHECK(reboot);
    CloseHandle(lock);
    CHECK(FileIs(target, "locked original"));                                          /* not installed, not claimed */
    {
        LN_JOURNAL j;

        CHECK_ERR(0, LnJournalRead(&t, &j));
        CHECK(!strcmp(LnJournalLast(&j, tx)->phase, "pending-first-boot") && LnJournalLast(&j, tx)->rebootRequired);
        LnJournalFree(&j);
    }
    CHECK_ERR(0, LnServicingReadPendingPlan(&t, &plan));
    CHECK_ERR(0, LnJsonParse(plan, strlen(plan), &root));
    {
        long long schema;
        BOOL rr;
        const LN_JSON *ops = LnJsonGet(root, "operations"), *op0 = ops ? ops->child : NULL;
        const LN_JSON *tg = LnJsonGet(root, "target"), *pk = LnJsonGet(root, "package");
        char want[65], man[65];

        CHECK(LnJsonGetInt(root, "schema", &schema) && schema == 1);
        CHECK(!strcmp(LnJsonGetString(root, "transactionId"), tx));
        CHECK(!strcmp(LnJsonGetString(root, "operation"), "install"));
        CHECK(LnJsonGetBool(root, "requiresReboot", &rr) && rr);
        CHECK(!strcmp(LnJsonGetString(tg, "machineId"), t.identity) && !strcmp(LnJsonGetString(tg, "arch"), "amd64"));
        CHECK(!strcmp(LnJsonGetString(tg, "line"), "main-nt10") && LnJsonGetString(tg, "base") != NULL);
        CHECK(!strcmp(LnJsonGetString(pk, "id"), "ln.fixture") && !strcmp(LnJsonGetString(pk, "version"), "1.0"));
        {
            WCHAR *fx = Fx(L"good1\\manifest.json");
            BYTE *d;
            size_t n;

            CHECK_ERR(0, LnReadFileAll(fx, &d, &n));
            LnSha256Hex(d, n, man);
            free(d);
            free(fx);
        }
        CHECK(!strcmp(LnJsonGetString(pk, "digest"), man));
        CHECK(op0 && !op0->next);                                                       /* only the locked item is deferred */
        CHECK(!strcmp(LnJsonGetString(op0, "type"), "replace-file"));                  /* the file existed */
        CHECK(!strcmp(LnJsonGetString(op0, "component"), "ln.fixture"));
        CHECK(!strcmp(LnJsonGetString(op0, "target"), "\\Windows\\System32\\lnfixture.dll"));
        LnSha256Hex("version one", 11, want);
        CHECK(!strcmp(LnJsonGetString(op0, "sha256"), want));
        {
            char src[128], bk[128];

            strcpy(src, "store/objects/");
            strcat(src, want);
            CHECK(!strcmp(LnJsonGetString(op0, "source"), src));
            strcpy(bk, "backup/");
            strcat(bk, tx);
            strcat(bk, "/0001");
            CHECK(!strcmp(LnJsonGetString(op0, "backup"), bk));
        }
        CHECK(LnJsonGet(root, "signature") == NULL);                                   /* the plan is not signed: it is a local view */
    }
    LnJsonFree(root);
    free(plan);
    CHECK_ERR(0, LnServicingApplyPackage(&t, "ln.fixture", "1.0", &reboot, tx));       /* resumes: still pending */
    CHECK(reboot);

    /* confirming before the boot applier did its work is refused: the hash is not final */
    CHECK_ERR(LN_E_VERIFY_FINAL, LnServicingConfirmHealth(&t, "ln.fixture", "1.0"));

    /* boot attempt rules: ShouldRollback is evaluated BEFORE MarkBootStarted */
    CHECK_ERR(LN_E_NOT_FOUND, LnServicingReadAttempt(&t, &a));                         /* absent = new plan */
    memset(&a, 0, sizeof(a));
    CHECK(!LnServicingShouldRollback(&a, &why));
    CHECK_ERR(0, LnServicingMarkBootStarted(&t, &a));
    CHECK(a.attempt == 1 && !strcmp(a.lastResult, "started") && !a.failureRecorded);
    /* the machine lost power: no result was written; the next boot reads attempt=1 */
    CHECK_ERR(0, LnServicingReadAttempt(&t, &a));
    CHECK(!LnServicingShouldRollback(&a, &why));                                       /* within the limit: try again */
    CHECK_ERR(0, LnServicingMarkBootStarted(&t, &a));
    CHECK(a.attempt == 2 && !strcmp(a.previousResult, "started"));                     /* the last boot never finished */
    /* a third boot without health confirmation: roll back, as an unexpected reboot */
    CHECK_ERR(0, LnServicingReadAttempt(&t, &a));
    CHECK(LnServicingShouldRollback(&a, &why) && !strcmp(why, "unexpected-reboot"));

    {
        LN_ATTEMPT one;

        memset(&one, 0, sizeof(one));
        one.attempt = 1;
        CHECK(!LnServicingShouldRollback(&one, &why));
        one.attempt = 2;
        one.failureRecorded = TRUE;                                                    /* a verifiable failure names itself */
        CHECK(LnServicingShouldRollback(&one, &why) && !strcmp(why, "verifiable-failure"));
        one.healthConfirmed = TRUE;
        CHECK(!LnServicingShouldRollback(&one, &why));                                 /* health wins */
    }
    CHECK_ERR(0, LnServicingMarkBootOutcome(&t, TRUE, FALSE));
    CHECK_ERR(0, LnServicingReadAttempt(&t, &a));
    CHECK(a.failureRecorded && !strcmp(a.lastResult, "failed"));

    /* rolling back a transaction that never reached the target removes the plan */
    CHECK_ERR(0, LnServicingRollback(&t, "ln.fixture", "1.0", tx));
    CHECK(FileIs(target, "locked original"));
    CHECK_ERR(LN_E_NOT_FOUND, LnServicingReadPendingPlan(&t, &plan));
    free(target);
}

static void TestExportLog(void)
{
    LN_TARGET t;
    WCHAR dest[MAX_PATH], *f;

    Section("export-log");
    CHECK(OpenImage(&t, L"life"));
    wcscpy(dest, g_work);
    wcscat(dest, L"\\exported");
    CHECK_ERR(0, LnServicingExportLog(&t, dest));
    f = LnJoin(dest, L"transactions.jsonl");
    CHECK(LnPathExists(f));
    free(f);
    f = LnJoin(dest, L"state.json");
    CHECK(LnPathExists(f));
    free(f);
}


/* ---- review blockers B1..B4 ---------------------------------------------------------------------------- */

static PSID g_testSid;

static void SetDaclSddl(const WCHAR *path, const WCHAR *sddl, BOOL protect)
{
    PSECURITY_DESCRIPTOR sd = NULL;
    PACL dacl = NULL;
    BOOL present = FALSE, deflt = FALSE;

    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, NULL) &&
        GetSecurityDescriptorDacl(sd, &present, &dacl, &deflt))
    {
        SetNamedSecurityInfoW((LPWSTR)path, SE_FILE_OBJECT,
                              DACL_SECURITY_INFORMATION | (protect ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION),
                              NULL, NULL, dacl, NULL);
    }
    LocalFree(sd);
}

static void TestSecurityAcl(void)
{
    LN_TARGET t, t2;
    WCHAR root[MAX_PATH], *serv, sddl[256], *sidStr = NULL;

    Section("B1: protected servicing tree (owner and DACL checked before anything is read)");
    NewImage(root, L"acl");
    CHECK_ERR(0, LnTargetOpen(&t, LnTargetImage, root, TRUE));
    CHECK_ERR(0, LnTargetOpen(&t2, LnTargetImage, root, FALSE));                       /* what the library created is accepted */
    serv = _wcsdup(t.servicing);
    ConvertSidToStringSidW(g_testSid, &sidStr);

    /* Authenticated Users (or anyone else) in the DACL: refused */
    wcscpy(sddl, L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;AU)(A;OICI;FA;;;");
    wcscat(sddl, sidStr);
    wcscat(sddl, L")");
    SetDaclSddl(serv, sddl, TRUE);
    CHECK_ERR(LN_E_INSECURE_TARGET, LnTargetOpen(&t2, LnTargetImage, root, FALSE));
    /* back to the right list, then an unprotected DACL (inherits from the parent): refused */
    wcscpy(sddl, L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;");
    wcscat(sddl, sidStr);
    wcscat(sddl, L")");
    SetDaclSddl(serv, sddl, TRUE);
    CHECK_ERR(0, LnTargetOpen(&t2, LnTargetImage, root, FALSE));
    SetDaclSddl(serv, sddl, FALSE);
    CHECK_ERR(LN_E_INSECURE_TARGET, LnTargetOpen(&t2, LnTargetImage, root, FALSE));
    SetDaclSddl(serv, sddl, TRUE);
    /* the parent folder counts too */
    {
        WCHAR parent[MAX_PATH];

        wcscpy(parent, root);
        wcscat(parent, L"\\LiberNT");
        wcscpy(sddl, L"D:P(A;OICI;FA;;;WD)");
        SetDaclSddl(parent, sddl, TRUE);
        CHECK_ERR(LN_E_INSECURE_TARGET, LnTargetOpen(&t2, LnTargetImage, root, FALSE));
    }
    LocalFree(sidStr);
    free(serv);
}

static void TestReverifyAtUse(void)
{
    LN_TARGET t;
    WCHAR *p, *target, *cat, *man;
    char tx[65];
    BOOL reboot;
    LN_JOURNAL j;

    Section("B1: apply and confirm verify the stored package again");
    CHECK(OpenImage(&t, L"reverify"));
    InstallTrust(&t);
    target = LnTargetFile(&t, L"Windows\\System32\\lnfixture.dll");
    p = Fx(L"good1");
    CHECK_ERR(0, LnServicingAddPackage(&t, p, tx));
    CHECK_ERR(0, LnServicingVerifyStored(&t, "ln.fixture", "1.0", NULL));
    free(p);

    /* the catalog in the store is replaced by a copy without the signature: apply refuses, nothing is written */
    cat = LnTargetPath(&t, L"store\\packages\\ln.fixture\\1.0\\catalog.cat");
    p = Fx(L"unsigned-catalog\\catalog.cat");
    {
        BYTE *good;
        size_t glen;

        CHECK_ERR(0, LnReadFileAll(cat, &good, &glen));
        CHECK_ERR(0, LnCopyFileFlush(p, cat));
        CHECK_ERR(LN_E_UNSIGNED, LnServicingApplyPackage(&t, "ln.fixture", "1.0", &reboot, tx));
        CHECK(!LnPathExists(target));
        /* the manifest the engine reads differs from the verified one: refused */
        CHECK_ERR(0, LnWriteFileAtomic(cat, good, glen));
        man = LnTargetPath(&t, L"store\\manifests\\ln.fixture\\1.0\\manifest.json");
        {
            BYTE *m0;
            size_t ml;
            LN_BUF b;

            CHECK_ERR(0, LnReadFileAll(man, &m0, &ml));
            LnBufInit(&b);
            LnBufAppend(&b, (char *)m0, ml);
            LnBufAppendStr(&b, "  ");
            CHECK_ERR(0, LnWriteFileAtomic(man, b.data, b.len));
            CHECK(LnServicingApplyPackage(&t, "ln.fixture", "1.0", &reboot, tx) != 0);
            CHECK(!LnPathExists(target));
            CHECK_ERR(0, LnWriteFileAtomic(man, m0, ml));
            LnBufFree(&b);
            free(m0);
        }
        free(man);
        free(good);
    }
    free(p);

    /* untouched store: apply works; then the signature file is damaged and confirmation refuses */
    CHECK_ERR(0, LnServicingApplyPackage(&t, "ln.fixture", "1.0", &reboot, tx));
    CHECK(FileIs(target, "version one"));
    {
        WCHAR *sig = LnTargetPath(&t, L"store\\packages\\ln.fixture\\1.0\\manifest.sig");
        BYTE *good;
        size_t glen;

        CHECK_ERR(0, LnReadFileAll(sig, &good, &glen));
        good[5] ^= 0xff;
        CHECK_ERR(0, LnWriteFileAtomic(sig, good, glen));
        CHECK(LnServicingConfirmHealth(&t, "ln.fixture", "1.0") != 0);
        good[5] ^= 0xff;
        CHECK_ERR(0, LnWriteFileAtomic(sig, good, glen));
        CHECK_ERR(0, LnServicingConfirmHealth(&t, "ln.fixture", "1.0"));
        free(good);
        free(sig);
    }
    CHECK_ERR(0, LnJournalRead(&t, &j));
    CHECK(!strcmp(LnJournalLast(&j, tx)->phase, "confirmed"));
    LnJournalFree(&j);
    free(cat);
    free(target);
}

static WCHAR g_swapSource[MAX_PATH];

static void SwapSource(const WCHAR *sourceDir)
{
    WCHAR *man = LnJoin(sourceDir, L"manifest.json"), *cat = LnJoin(sourceDir, L"catalog.cat");

    WriteText(man, "{\"schemaVersion\":1,\"packageId\":\"evil\"}");
    WriteText(cat, "not a catalog");
    wcscpy(g_swapSource, sourceDir);
    free(man);
    free(cat);
}

static void TestAddToctou(void)
{
    LN_TARGET t;
    WCHAR src[MAX_PATH], *good, *storedMan;
    char tx[65], orig[65], stored[65];
    BYTE *d;
    size_t n;

    Section("B2: add-package verifies and imports a private copy");
    CHECK(OpenImage(&t, L"toctou"));
    InstallTrust(&t);
    wcscpy(src, g_work);
    wcscat(src, L"\\toctou-source");
    good = Fx(L"good1");
    LnRemoveTree(src);
    CHECK_ERR(0, LnCopyTreeFlush(good, src));
    {
        WCHAR *m = LnJoin(good, L"manifest.json");

        CHECK_ERR(0, LnReadFileAll(m, &d, &n));
        LnSha256Hex(d, n, orig);
        free(d);
        free(m);
    }
    LnTestAfterStageHook = SwapSource;                                                 /* someone rewrites the source meanwhile */
    CHECK_ERR(0, LnServicingAddPackage(&t, src, tx));
    LnTestAfterStageHook = NULL;
    storedMan = LnTargetPath(&t, L"store\\manifests\\ln.fixture\\1.0\\manifest.json");
    CHECK_ERR(0, LnReadFileAll(storedMan, &d, &n));
    LnSha256Hex(d, n, stored);
    free(d);
    CHECK(!strcmp(stored, orig));                                                      /* the store has what was verified */
    CHECK(g_swapSource[0] != 0);                                                       /* the swap did happen */
    {
        BOOL reboot;
        WCHAR *target = LnTargetFile(&t, L"Windows\\System32\\lnfixture.dll");

        CHECK_ERR(0, LnServicingApplyPackage(&t, "ln.fixture", "1.0", &reboot, tx));
        CHECK(FileIs(target, "version one"));
        free(target);
    }
    free(storedMan);
    free(good);
}

static void TestPathHardening(void)
{
    LN_TARGET t;
    WCHAR root[MAX_PATH], outside[MAX_PATH], cmd[1024], *p, *evil;
    char tx[65];
    BOOL reboot;

    Section("B3: junctions in the way are refused before anything is written");
    CHECK(OpenImage(&t, L"junction"));
    InstallTrust(&t);
    wcscpy(root, t.root);
    wcscpy(outside, g_work);
    wcscat(outside, L"\\outside-the-image");
    LnEnsureDir(outside);
    wsprintfW(cmd, L"cmd.exe /c mklink /J \"%sWindows\\System32\\jn\" \"%s\" >NUL", root, outside);
    {
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;

        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        CHECK(CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi));
        WaitForSingleObject(pi.hProcess, 15000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    p = Fx(L"junction");
    CHECK_ERR(0, LnServicingAddPackage(&t, p, tx));                                    /* the path is lexically fine */
    free(p);
    CHECK_ERR(LN_E_FORBIDDEN_TARGET, LnServicingApplyPackage(&t, "ln.junction", "1.0", &reboot, tx));
    evil = LnJoin(outside, L"evil.dll");
    CHECK(!LnPathExists(evil));                                                        /* nothing escaped the image */
    free(evil);
    {
        WCHAR rel[] = L"Windows\\System32\\jn\\evil.dll";

        CHECK_ERR(LN_E_FORBIDDEN_TARGET, LnTargetValidateReal(&t, rel));
        CHECK_ERR(0, LnTargetValidateReal(&t, L"Windows\\System32\\fine\\x.dll"));     /* a normal new path passes */
    }
}

static void TestJournalAnchor(void)
{
    LN_TARGET t;
    LN_JENTRY e;
    LN_JOURNAL j;
    WCHAR *jp, *ap;
    BYTE *data;
    size_t len;
    int i;
    char *nl;

    Section("B4: strict journal and anchor");
    CHECK(OpenImage(&t, L"anchor"));
    for (i = 0; i < 4; i++)
    {
        memset(&e, 0, sizeof(e));
        strcpy(e.transactionId, "aa");
        strcpy(e.phase, i == 0 ? "prepared" : i == 1 ? "applying" : i == 2 ? "failed" : "rolled-back");
        strcpy(e.result, "ok");
        e.op = -1;
        CHECK_ERR(0, LnJournalAppend(&t, &e));
    }
    jp = LnTargetPath(&t, L"journal\\transactions.jsonl");
    ap = LnTargetPath(&t, L"journal\\anchor.json");
    CHECK(LnPathExists(ap));
    CHECK_ERR(0, LnReadFileAll(jp, &data, &len));

    /* (a) the last entry rewritten with valid JSON ("rolled-back" -> "confirmed"): the chain alone cannot see it */
    {
        /* the LAST line: change "rolled-back" to "confirmed" keeping JSON valid and same length class */
        char *last = strstr((char *)data, "\"phase\":\"rolled-back\"");
        BYTE *edited = malloc(len + 16);
        size_t off;

        CHECK(last != NULL);
        off = (size_t)(last - (char *)data);
        memcpy(edited, data, off);
        memcpy(edited + off, "\"phase\":\"confirmed\"", 19);
        memcpy(edited + off + 19, data + off + 21, len - off - 21);
        CHECK_ERR(0, LnWriteFileAtomic(jp, edited, len - 2));
        CHECK_ERR(LN_E_JOURNAL_CORRUPT, LnJournalRead(&t, &j));
        CHECK_ERR(0, LnWriteFileAtomic(jp, data, len));                                 /* restored */
        free(edited);
    }
    CHECK_ERR(0, LnJournalRead(&t, &j));
    CHECK(j.count == 4);
    LnJournalFree(&j);

    /* (b) the whole last entry removed: the anchor is ahead of the journal */
    nl = (char *)data + len - 2;
    while (nl > (char *)data && *nl != '\n')
        nl--;
    CHECK_ERR(0, LnWriteFileAtomic(jp, data, (size_t)(nl - (char *)data) + 1));
    CHECK_ERR(LN_E_JOURNAL_CORRUPT, LnJournalRead(&t, &j));
    CHECK_ERR(0, LnWriteFileAtomic(jp, data, len));

    /* (c) a broken line in the middle (not the last, not torn) */
    {
        LN_BUF b;
        char *first = strchr((char *)data, '\n');

        LnBufInit(&b);
        LnBufAppend(&b, (char *)data, (size_t)(first - (char *)data) + 1);
        LnBufAppendStr(&b, "{this is not json\n");
        LnBufAppend(&b, first + 1, len - (size_t)(first + 1 - (char *)data));
        CHECK_ERR(0, LnWriteFileAtomic(jp, b.data, b.len));
        CHECK_ERR(LN_E_JOURNAL_CORRUPT, LnJournalRead(&t, &j));
        LnBufFree(&b);
    }
    CHECK_ERR(0, LnWriteFileAtomic(jp, data, len));

    /* (d) an unterminated last line is the torn write of a power cut: ignored, then cut away by the next append */
    {
        LN_BUF b;

        LnBufInit(&b);
        LnBufAppend(&b, (char *)data, len);
        LnBufAppendStr(&b, "{\"seq\":5,\"prev\":\"00");
        CHECK_ERR(0, LnWriteFileAtomic(jp, b.data, b.len));
        LnBufFree(&b);
    }
    CHECK_ERR(0, LnJournalRead(&t, &j));
    CHECK(j.count == 4 && j.tailTruncated);
    LnJournalFree(&j);
    memset(&e, 0, sizeof(e));
    strcpy(e.transactionId, "aa"); strcpy(e.phase, "prepared"); strcpy(e.result, "ok"); e.op = -1;
    CHECK_ERR(0, LnJournalAppend(&t, &e));
    CHECK_ERR(0, LnJournalRead(&t, &j));
    CHECK(j.count == 5 && !j.tailTruncated && j.entries[4].sequence == 5);
    LnJournalFree(&j);

    /* (e) crash between the append and the anchor write: one entry ahead is accepted */
    {
        BYTE *a0;
        size_t al;

        CHECK_ERR(0, LnReadFileAll(ap, &a0, &al));
        memset(&e, 0, sizeof(e));
        strcpy(e.transactionId, "aa"); strcpy(e.phase, "applying"); strcpy(e.result, "ok"); e.op = -1;
        CHECK_ERR(0, LnJournalAppend(&t, &e));
        CHECK_ERR(0, LnWriteFileAtomic(ap, a0, al));                                   /* anchor one behind */
        CHECK_ERR(0, LnJournalRead(&t, &j));
        CHECK(j.count == 6);
        LnJournalFree(&j);
        /* ... but two behind, or a deleted anchor, is not */
        DeleteFileW(ap);
        CHECK_ERR(LN_E_JOURNAL_CORRUPT, LnJournalRead(&t, &j));
        free(a0);
    }
    free(data);
    free(jp);
    free(ap);
}

static void TestPlanValidation(void)
{
    LN_TARGET t;
    WCHAR *p, *target;
    char tx[65], *plan, *bad;
    BOOL reboot;
    HANDLE lock;

    Section("N6: the plan must match the signed package");
    CHECK(OpenImage(&t, L"planval"));
    InstallTrust(&t);
    InstallApplierStub(&t);
    target = LnTargetFile(&t, L"Windows\\System32\\lnfixture.dll");
    WriteText(target, "locked original");
    p = Fx(L"good1");
    CHECK_ERR(0, LnServicingAddPackage(&t, p, tx));
    free(p);
    lock = CreateFileW(target, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    LnTestLockedAsPending = TRUE;
    CHECK_ERR(0, LnServicingApplyPackage(&t, "ln.fixture", "1.0", &reboot, tx));
    LnTestLockedAsPending = FALSE;
    CloseHandle(lock);
    CHECK_ERR(0, LnServicingReadPendingPlan(&t, &plan));
    CHECK_ERR(0, LnServicingValidatePlan(&t, plan));
    /* a plan whose operation points at another object, or another machine, or another digest is refused */
    bad = _strdup(plan);
    {
        char *at = strstr(bad, "store/objects/");

        CHECK(at != NULL);
        if (at)
            at[14] = (at[14] == '0') ? '1' : '0';
    }
    CHECK_ERR(LN_E_MANIFEST_INVALID, LnServicingValidatePlan(&t, bad));
    free(bad);
    bad = _strdup(plan);
    {
        char *at = strstr(bad, t.identity);

        CHECK(at != NULL);
        if (at)
            at[0] = (at[0] == 'a') ? 'b' : 'a';
    }
    CHECK(LnServicingValidatePlan(&t, bad) != 0);
    free(bad);
    free(plan);
    free(target);
}

static void TestPlanHashFailClosed(void)
{
    LN_TARGET t;
    WCHAR *target;
    WCHAR *p;
    BOOL reboot = FALSE;
    char tx[65];
    char *plan = NULL;
    PSECURITY_DESCRIPTOR Sd = NULL;

    Section("H1: an existing but unhashable target fails the plan closed (Pam review)");
    CHECK(OpenImage(&t, L"planhash"));
    InstallTrust(&t);
    InstallApplierStub(&t);
    target = LnTargetFile(&t, L"Windows\\System32\\lnfixture.dll");
    WriteText(target, "original content");
    /* deny read to everyone, including ourselves: neither the backup copy
     * nor the originalSha256 can be produced, so no plan may be written
     * (an add-file plan without backup would make the rollback DELETE a
     * system file) */
    CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorW(
              L"D:(D;;FR;;;WD)", SDDL_REVISION_1, &Sd, NULL));
    CHECK(SetFileSecurityW(target, DACL_SECURITY_INFORMATION, Sd));
    LocalFree(Sd);
    Sd = NULL;
    p = Fx(L"good1");
    CHECK_ERR(0, LnServicingAddPackage(&t, p, tx));
    free(p);
    CHECK(LnServicingApplyPackage(&t, "ln.fixture", "1.0", &reboot, tx) != 0);
    CHECK(LnServicingReadPendingPlan(&t, &plan) != 0);       /* no plan written */
    CHECK(plan == NULL);
    /* restore a readable DACL; the file must be untouched */
    CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorW(
              L"D:(A;;FR;;;WD)", SDDL_REVISION_1, &Sd, NULL));
    CHECK(SetFileSecurityW(target, DACL_SECURITY_INFORMATION, Sd));
    LocalFree(Sd);
    CHECK(FileIs(target, "original content"));
    free(target);
}

static void
TestApplierGuard(void)
{
    LN_TARGET t;
    WCHAR *p, *target, *plan;
    char tx[65];
    BOOL reboot;
    LN_JOURNAL j;

    Section("defer refuses when the boot applier is not installed (dx2400 stall)");
    SetEnvironmentVariableW(L"LNUPDATE_SYSTEM_DIRS", L"Windows");
    CHECK(OpenImage(&t, L"applierguard"));
    InstallTrust(&t);
    target = LnTargetFile(&t, L"Windows\\System32\\lnfixture.dll");
    WriteText(target, "old content");
    p = Fx(L"good1");
    CHECK_ERR(0, LnServicingAddPackage(&t, p, tx));
    free(p);
    LnTestLockedAsPending = TRUE;
    CHECK_ERR(LN_E_NO_BOOT_APPLIER, LnServicingApplyPackage(&t, "ln.fixture", "1.0", &reboot, tx));
    LnTestLockedAsPending = FALSE;
    CHECK(!reboot);
    CHECK(FileIs(target, "old content"));                       /* nothing may change */
    CHECK_ERR(0, LnJournalRead(&t, &j));
    CHECK(!strcmp(LnJournalLast(&j, tx)->phase, "failed"));        /* refused before anything changed */
    LnJournalFree(&j);
    plan = LnTargetPath(&t, L"pending\\plan.json");
    CHECK(!LnPathExists(plan));                                 /* no plan a boot could never consume */
    free(plan);

    /* with the applier present the same apply defers normally */
    InstallApplierStub(&t);
    LnTestLockedAsPending = TRUE;
    CHECK_ERR(0, LnServicingApplyPackage(&t, "ln.fixture", "1.0", &reboot, tx));
    LnTestLockedAsPending = FALSE;
    CHECK(reboot);
    free(target);
    SetEnvironmentVariableW(L"LNUPDATE_SYSTEM_DIRS", L"");
}

static void
TestBootExecuteListed(void)
{
    WCHAR buf[512];

    Section("BootExecute walk: bytes limit, tokens, case, paths (review F1/F2)");
    memset(buf, 0, sizeof(buf));
    wcscpy(buf, L"autocheck autochk *");
    wcscpy(buf + wcslen(buf) + 1, L"LiberNTUpdatePending");
    CHECK(LnBootExecuteListed(buf, (wcslen(buf) + 1 + 20) * sizeof(WCHAR)));      /* plain MULTI_SZ */
    CHECK(LnBootExecuteListed(buf, sizeof(buf)));                                  /* zeroed tail hides a missing double NUL */

    memset(buf, 0, sizeof(buf));
    wcscpy(buf, L"autocheck autochk * LiberNTUpdatePending");                      /* REG_SZ: the applier is not the first token of the entry */
    CHECK(!LnBootExecuteListed(buf, (wcslen(buf) + 1) * sizeof(WCHAR)));
    wcscpy(buf, L"LiberNTUpdatePending /log");                                     /* entry with arguments: first token matches */
    CHECK(LnBootExecuteListed(buf, sizeof(buf)));

    memset(buf, 0, sizeof(buf));
    wcscpy(buf, L"liberntupdatepending");                                          /* case-insensitive */
    CHECK(LnBootExecuteListed(buf, sizeof(buf)));
    wcscpy(buf, L"C:\\Windows\\System32\\LiberNTUpdatePending.exe");               /* path and .exe stripped */
    CHECK(LnBootExecuteListed(buf, sizeof(buf)));

    memset(buf, 0, sizeof(buf));
    wcscpy(buf, L"autocheck autochk *");
    CHECK(!LnBootExecuteListed(buf, sizeof(buf)));                                 /* absent */
    wcscpy(buf, L"C:\\Windows\\LiberNTUpdatePending.exe.bak");                     /* not the exe: .bak survives the strip */
    CHECK(!LnBootExecuteListed(buf, sizeof(buf)));
    wcscpy(buf, L"x\\NotLiberNTUpdatePending");                                    /* not a token */
    CHECK(!LnBootExecuteListed(buf, sizeof(buf)));
    buf[0] = 0;
    CHECK(!LnBootExecuteListed(buf, sizeof(buf)));                                 /* empty list */
    memset(buf, 0, sizeof(buf));
    memset(buf, 0x41, 400);                                                        /* no terminator within the bound: limit is in bytes */
    CHECK(!LnBootExecuteListed(buf, 400));
}

static void
TestAttemptOwnership(void)
{
    LN_TARGET t;
    WCHAR *p, *target;
    char txA[65], txB[65];
    BOOL reboot;
    LN_ATTEMPT rec;

    Section("attempt record ownership: closing A never touches B's counter (review F4)");
    CHECK(OpenImage(&t, L"attown"));
    InstallTrust(&t);
    InstallApplierStub(&t);

    /* A: staged, applied inline, confirmed and removed while B is pending */
    p = Fx(L"good1");
    CHECK_ERR(0, LnServicingAddPackage(&t, p, txA));
    free(p);
    CHECK_ERR(0, LnServicingApplyPackage(&t, "ln.fixture", "1.0", &reboot, txA));
    CHECK_ERR(0, LnServicingConfirmHealth(&t, "ln.fixture", "1.0"));

    /* B: staged and deferred to boot, then a boot marks it started */
    SetEnvironmentVariableW(L"LNUPDATE_SYSTEM_DIRS", L"Windows");
    p = Fx(L"twoitems");
    CHECK_ERR(0, LnServicingAddPackage(&t, p, txB));
    free(p);
    target = LnTargetFile(&t, L"Windows\\System32\\first.dll");
    WriteText(target, "old first");
    LnTestLockedAsPending = TRUE;
    CHECK_ERR(0, LnServicingApplyPackage(&t, "ln.twoitems", "1.0", &reboot, txB));
    LnTestLockedAsPending = FALSE;
    CHECK(reboot);
    free(target);
    CHECK_ERR(0, LnServicingMarkBootStarted(&t, &rec));
    CHECK_ERR(0, LnServicingReadAttempt(&t, &rec));
    CHECK(!strcmp(rec.transactionId, txB));

    /* removing the closed A (rollback path) must keep B's boot counter */
    CHECK_ERR(0, LnServicingRemovePackage(&t, "ln.fixture", "1.0"));
    CHECK_ERR(0, LnServicingReadAttempt(&t, &rec));
    CHECK(!strcmp(rec.transactionId, txB));

    /* removing B itself clears the record */
    CHECK_ERR(0, LnServicingRemovePackage(&t, "ln.twoitems", "1.0"));
    CHECK_ERR(LN_E_NOT_FOUND, LnServicingReadAttempt(&t, &rec));
    SetEnvironmentVariableW(L"LNUPDATE_SYSTEM_DIRS", L"");
}

static void
TestSystemDefer(void)
{
    /* Route rule (god kv-optA): a replace under a system root always plans at boot, without
     * trusting the lock probe. Each scenario runs in a fresh image and flips
     * LNUPDATE_SYSTEM_DIRS itself; the rest of the suite runs with the rule disabled. */
    LN_TARGET t;
    WCHAR *p, *target;
    char tx[65];
    BOOL reboot;
    LN_JOURNAL j;

    Section("route rule: replace under a system dir defers without a lock");

    /* default list (env unset): Windows is a system root -> deferred, file untouched */
    SetEnvironmentVariableW(L"LNUPDATE_SYSTEM_DIRS", NULL);
    CHECK(OpenImage(&t, L"sysdefer1"));
    InstallTrust(&t);
    InstallApplierStub(&t);
    target = LnTargetFile(&t, L"Windows\\System32\\lnfixture.dll");
    WriteText(target, "old content");
    p = Fx(L"good1");
    CHECK_ERR(0, LnServicingAddPackage(&t, p, tx));
    free(p);
    LnTestLockedAsPending = TRUE;
    CHECK_ERR(0, LnServicingApplyPackage(&t, "ln.fixture", "1.0", &reboot, tx));
    LnTestLockedAsPending = FALSE;
    CHECK(reboot);
    CHECK(FileIs(target, "old content"));                       /* no inline replace was attempted */
    CHECK_ERR(0, LnJournalRead(&t, &j));
    CHECK(!strcmp(LnJournalLast(&j, tx)->phase, "pending-first-boot") && LnJournalLast(&j, tx)->rebootRequired);
    LnJournalFree(&j);
    free(target);

    /* empty list: the rule is off -> the same replace applies inline under \Windows */
    SetEnvironmentVariableW(L"LNUPDATE_SYSTEM_DIRS", L"");
    CHECK(OpenImage(&t, L"sysdefer2"));
    InstallTrust(&t);
    target = LnTargetFile(&t, L"Windows\\System32\\lnfixture.dll");
    WriteText(target, "old content");
    p = Fx(L"good1");
    CHECK_ERR(0, LnServicingAddPackage(&t, p, tx));
    free(p);
    LnTestLockedAsPending = TRUE;
    CHECK_ERR(0, LnServicingApplyPackage(&t, "ln.fixture", "1.0", &reboot, tx));
    LnTestLockedAsPending = FALSE;
    CHECK(!reboot);
    CHECK(FileIs(target, "version one"));
    free(target);

    /* a list without Windows: no match -> inline */
    SetEnvironmentVariableW(L"LNUPDATE_SYSTEM_DIRS", L"Program Files");
    CHECK(OpenImage(&t, L"sysdefer3"));
    InstallTrust(&t);
    target = LnTargetFile(&t, L"Windows\\System32\\lnfixture.dll");
    WriteText(target, "old content");
    p = Fx(L"good1");
    CHECK_ERR(0, LnServicingAddPackage(&t, p, tx));
    free(p);
    LnTestLockedAsPending = TRUE;
    CHECK_ERR(0, LnServicingApplyPackage(&t, "ln.fixture", "1.0", &reboot, tx));
    LnTestLockedAsPending = FALSE;
    CHECK(!reboot);
    CHECK(FileIs(target, "version one"));
    free(target);

    /* a multi-root list mentioning Windows: deferred again */
    SetEnvironmentVariableW(L"LNUPDATE_SYSTEM_DIRS", L"Program Files;Windows");
    CHECK(OpenImage(&t, L"sysdefer4"));
    InstallTrust(&t);
    InstallApplierStub(&t);
    target = LnTargetFile(&t, L"Windows\\System32\\lnfixture.dll");
    WriteText(target, "old content");
    p = Fx(L"good1");
    CHECK_ERR(0, LnServicingAddPackage(&t, p, tx));
    free(p);
    LnTestLockedAsPending = TRUE;
    CHECK_ERR(0, LnServicingApplyPackage(&t, "ln.fixture", "1.0", &reboot, tx));
    LnTestLockedAsPending = FALSE;
    CHECK(reboot);
    CHECK(FileIs(target, "old content"));
    free(target);

    /* a brand-new file under \Windows is an add-file: nothing can hold it, so it applies inline */
    SetEnvironmentVariableW(L"LNUPDATE_SYSTEM_DIRS", NULL);
    CHECK(OpenImage(&t, L"sysdefer5"));
    InstallTrust(&t);
    target = LnTargetFile(&t, L"Windows\\System32\\lnfixture.dll");
    CHECK(!LnPathExists(target));
    p = Fx(L"good1");
    CHECK_ERR(0, LnServicingAddPackage(&t, p, tx));
    free(p);
    LnTestLockedAsPending = TRUE;
    CHECK_ERR(0, LnServicingApplyPackage(&t, "ln.fixture", "1.0", &reboot, tx));
    LnTestLockedAsPending = FALSE;
    CHECK(!reboot);
    CHECK(FileIs(target, "version one"));                       /* created inline */
    free(target);

    SetEnvironmentVariableW(L"LNUPDATE_SYSTEM_DIRS", L"");   /* restore the suite default */
}
static LONG WINAPI CrashReport(EXCEPTION_POINTERS *ep)
{
    printf("\nCRASH code 0x%08lx at %p (image base %p)", (unsigned long)ep->ExceptionRecord->ExceptionCode,
           ep->ExceptionRecord->ExceptionAddress, (void *)GetModuleHandleW(NULL));
    if (ep->ExceptionRecord->NumberParameters >= 2)
        printf(", access type %Iu address %p", ep->ExceptionRecord->ExceptionInformation[0],
               (void *)ep->ExceptionRecord->ExceptionInformation[1]);
    {
        HMODULE mod = NULL;
        WCHAR name[MAX_PATH] = L"?";

        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)ep->ExceptionRecord->ExceptionAddress, &mod))
            GetModuleFileNameW(mod, name, MAX_PATH);
        printf(", in %ls +0x%lx", name, (unsigned long)((char *)ep->ExceptionRecord->ExceptionAddress - (char *)mod));
    }
    {
        /* frame-pointer walk (the debug build keeps rbp): offsets from the image base for llvm-symbolizer */
#if defined(_M_AMD64)
        ULONG_PTR *fp = (ULONG_PTR *)ep->ContextRecord->Rbp;
#elif defined(_M_IX86)
        ULONG_PTR *fp = (ULONG_PTR *)ep->ContextRecord->Ebp;
#elif defined(_M_ARM64)
        ULONG_PTR *fp = (ULONG_PTR *)ep->ContextRecord->Fp;
#else
        ULONG_PTR *fp = NULL;
#endif
        char *base = (char *)GetModuleHandleW(NULL);
        int depth;

        printf("\n  frames:");
        for (depth = 0; fp && depth < 14; depth++)
        {
            ULONG_PTR ret = fp[1];

            printf(" %p(+0x%lx)", (void *)ret, (unsigned long)((char *)ret - base));
            if (fp[0] <= (ULONG_PTR)fp)
                break;
            fp = (ULONG_PTR *)fp[0];
        }
    }
    printf("\n");
    return EXCEPTION_EXECUTE_HANDLER;
}

static void TestFetch(void)
{
    WCHAR base[512], url[1024], dest[MAX_PATH];
    DWORD n, attr;
    LN_TARGET t;
    char tx[65];

    Section("add-package --from (HTTP fetch)");
    n = GetEnvironmentVariableW(L"LN_FETCH_TEST_URL", base, 512);
    if (n == 0 || n >= 512)
    {
        printf("  skipped (LN_FETCH_TEST_URL not set; the serving harness starts it)\n");
        return;
    }

    /* a plain file lands byte-exact */
    wsprintfW(url, L"%s/blob.bin", base);
    wsprintfW(dest, L"%s\\fetch-blob.bin", g_work);
    CHECK_ERR(0, LnFetchFile(url, dest));
    {
        HANDLE f = CreateFileW(dest, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD sz = GetFileSize(f, NULL), rd = 0;
        CHAR *buf = malloc(sz);
        BOOL allA = TRUE;
        DWORD i;

        CHECK(f != INVALID_HANDLE_VALUE);
        CHECK(ReadFile(f, buf, sz, &rd, NULL) && rd == sz);
        for (i = 0; i < sz; i++)
            if (buf[i] != 'A')
                allA = FALSE;
        CHECK(sz == 1000 && allA);
        free(buf);
        CloseHandle(f);
    }
    DeleteFileW(dest);

    /* a missing file fails and leaves nothing behind */
    wsprintfW(url, L"%s/missing.bin", base);
    wsprintfW(dest, L"%s\\fetch-missing.bin", g_work);
    CHECK(LnFetchFile(url, dest) != 0);
    attr = GetFileAttributesW(dest);
    CHECK(attr == INVALID_FILE_ATTRIBUTES);

    /* a larger file crosses the IO chunk with the default cap */
    wsprintfW(url, L"%s/big.bin", base);
    wsprintfW(dest, L"%s\\fetch-big.bin", g_work);
    CHECK_ERR(0, LnFetchFile(url, dest));
    {
        HANDLE f = CreateFileW(dest, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        CHECK(GetFileSize(f, NULL) == 10000);
        CloseHandle(f);
    }
    DeleteFileW(dest);

    /* the cap is enforced mid-stream and the partial file is removed */
    SetEnvironmentVariableW(L"LN_FETCH_MAX_BYTES", L"500");
    wsprintfW(dest, L"%s\\fetch-cap.bin", g_work);
    CHECK_ERR(ERROR_FILE_TOO_LARGE, LnFetchFile(url, dest));
    SetEnvironmentVariableW(L"LN_FETCH_MAX_BYTES", NULL);
    attr = GetFileAttributesW(dest);
    CHECK(attr == INVALID_FILE_ATTRIBUTES);

    /* a refused connection fails cleanly */
    CHECK(LnFetchFile(L"http://127.0.0.1:9/x.bin", dest) != 0);

    /* URL shapes the fetch refuses to even attempt (no network touched) */
    CHECK_ERR(ERROR_INVALID_DATA, LnFetchFile(L"https://127.0.0.1/x", dest));
    CHECK_ERR(ERROR_INVALID_DATA, LnFetchFile(L"ftp://127.0.0.1/x", dest));
    CHECK_ERR(ERROR_INVALID_DATA, LnFetchFile(L"http://user@127.0.0.1/x", dest));
    CHECK_ERR(ERROR_INVALID_DATA, LnFetchFile(L"http://\\\\share/x", dest));

    /* a signed package fetches end to end and stages like a local one */
    CHECK(OpenImage(&t, L"fetch"));
    InstallTrust(&t);
    wsprintfW(url, L"%s/good1", base);
    wsprintfW(dest, L"%s\\fetch-pkg", g_work);
    CHECK_ERR(0, LnFetchPackage(&t, url, dest));
    CHECK_ERR(0, LnServicingAddPackage(&t, dest, tx));
    CHECK_ERR(0, LnServicingRemovePackage(&t, "ln.fixture", "1.0"));
    LnRemoveTree(dest);

    /* a payload that does not match the manifest hash is rejected at fetch time */
    wsprintfW(url, L"%s/tampered-payload", base);
    wsprintfW(dest, L"%s\\fetch-tampered", g_work);
    CHECK_ERR(LN_E_HASH_MISMATCH, LnFetchPackage(&t, url, dest));
    CHECK(!LnPathExists(dest));                                 /* a failed fetch leaves no directory */

    /* the manifest signature is verified before any payload name is acted on */
    wsprintfW(url, L"%s/tampered-manifest", base);
    wsprintfW(dest, L"%s\\fetch-tampman", g_work);
    CHECK_ERR(LN_E_BAD_SIGNATURE, LnFetchPackage(&t, url, dest));
    CHECK(!LnPathExists(dest));
    wsprintfW(url, L"%s/no-manifest-sig", base);
    CHECK_ERR(ERROR_INVALID_DATA, LnFetchPackage(&t, url, dest));   /* the sig fetch itself 404s */
    wsprintfW(url, L"%s/wrongcert", base);
    CHECK_ERR(LN_E_BAD_SIGNATURE, LnFetchPackage(&t, url, dest));   /* not the pinned key */

    /* a manifest naming an overlong payload path fails cleanly instead of
     * overflowing the URL buffer (review F1) */
    wsprintfW(url, L"%s/hugepath", base);
    wsprintfW(dest, L"%s\\fetch-huge", g_work);
    CHECK_ERR(LN_E_MANIFEST_INVALID, LnFetchPackage(&t, url, dest));
    CHECK(!LnPathExists(dest));

    /* the total package size is capped across payloads (review F2) */
    SetEnvironmentVariableW(L"LN_FETCH_MAX_PACKAGE_BYTES", L"5");
    wsprintfW(url, L"%s/good1", base);
    wsprintfW(dest, L"%s\\fetch-cap2", g_work);
    CHECK_ERR(ERROR_FILE_TOO_LARGE, LnFetchPackage(&t, url, dest));
    SetEnvironmentVariableW(L"LN_FETCH_MAX_PACKAGE_BYTES", NULL);
    CHECK(!LnPathExists(dest));

    /* URLs with control or non-ASCII characters are refused outright (review F3) */
    CHECK_ERR(ERROR_INVALID_DATA, LnFetchFile(L"http://127.0.0.1/x y", dest));
    CHECK_ERR(ERROR_INVALID_DATA, LnFetchFile(L"http://127.0.0.1/x\xE9y", dest));
}

int wmain(int argc, WCHAR **argv)
{
    WCHAR tmp[MAX_PATH];

    setvbuf(stdout, NULL, _IONBF, 0);
    SetUnhandledExceptionFilter(CrashReport);                    /* a crash must not swallow the section names */
    if (argc < 3)
    {
        fputs("usage: libservicing-tests <fixtures-dir> <dev-certificate.cer>\n", stderr);
        return 2;
    }
    g_fx = argv[1];
    g_cer = argv[2];
    {
        HANDLE tok;
        DWORD need = 0;
        TOKEN_USER *tu;

        /* the account running the tests is accepted next to SYSTEM/Administrators (they run unelevated) */
        OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok);
        GetTokenInformation(tok, TokenUser, NULL, 0, &need);
        tu = malloc(need);
        GetTokenInformation(tok, TokenUser, tu, need, &need);
        g_testSid = tu->User.Sid;
        LnTestExtraSid = g_testSid;
        CloseHandle(tok);
    }
    GetTempPathW(MAX_PATH, tmp);
    wsprintfW(g_work, L"%slnsvc-tests-%lu-%lu", tmp, GetCurrentProcessId(), GetTickCount());   /* never reuse a stale tree */
    LnEnsureDir(g_work);
    /* the route rule is exercised by TestSystemDefer alone; every other test keeps the pre-rule
     * semantics (an unlocked replace under \Windows applies inline) */
    SetEnvironmentVariableW(L"LNUPDATE_SYSTEM_DIRS", L"");

    TestSha256();
    TestJson();
    TestPaths();
    TestManifest();
    TestTransactionId();
    TestJournal();
    TestTarget();
    TestVerify();
    TestAddRejects();
    TestLifecycle();
    TestFailureRollsBack();
    TestCorruptStore();
    TestPendingPlan();
    TestExportLog();
    TestSecurityAcl();
    TestReverifyAtUse();
    TestAddToctou();
    TestPathHardening();
    TestJournalAnchor();
    TestPlanValidation();
    TestPlanHashFailClosed();
    TestApplierGuard();
    TestBootExecuteListed();
    TestAttemptOwnership();
    TestSystemDefer();
    TestFetch();

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    LnRemoveTree(g_work);
    return g_failed ? 1 : 0;
}
