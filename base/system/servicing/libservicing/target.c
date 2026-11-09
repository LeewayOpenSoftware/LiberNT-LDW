/*
 * PROJECT:     LiberNT Update - libservicing
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Servicing target: Online (running system) or Image (offline root), the protected
 *              servicing tree, and the stage 1 white list of what may be replaced
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * The Image mode only reads and writes files below the given root. It never loads or runs
 * anything found there (DESIGN-v0, section 3).
 *
 * Trust boundary: everything under <root>\LiberNT\Servicing (store, trust anchor, journal, plan,
 * backups) decides what the engine installs as an administrator, so the tree is created with a
 * PROTECTED DACL that grants only SYSTEM and Administrators, and a target whose servicing tree has
 * another owner or any other ACE is refused (LN_E_INSECURE_TARGET) before anything is read from it.
 */

#include "libservicing.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <aclapi.h>
#include <sddl.h>

#define SERVICING_REL L"LiberNT\\Servicing"
#define LIBERNT_REL   L"LiberNT"

#ifdef LN_TESTING
PSID LnTestExtraSid = NULL;
#endif

/* ---- the white list ------------------------------------------------------------------------
 * Stage 1 policy, stated plainly: this is a list of what MAY be replaced, not a list of what is dangerous.
 * It admits user-mode DLL/EXE under the four roots and auto/demand drivers, and refuses the kernel, the HAL,
 * the boot path and the critical processes by name. It does NOT by itself exclude other security-critical
 * user components (lsasrv, samsrv, msv1_0, sechost, svchost, kernel32, kernelbase...): whoever signs a
 * package with the development key is trusted to ship them, and the owner decides when that list grows. */

static BOOL StartsWithNoCase(const WCHAR *s, const WCHAR *prefix)
{
    return _wcsnicmp(s, prefix, wcslen(prefix)) == 0;
}

BOOL LnTargetItemAllowed(const WCHAR *rel, int driverStart)
{
    static const WCHAR *roots[] =
    {
        L"Windows\\System32\\", L"Windows\\SysWOW64\\", L"Program Files\\", L"Program Files (x86)\\"
    };
    static const WCHAR *deniedPrefix[] =
    {
        L"Windows\\System32\\config\\", L"Windows\\System32\\boot\\", L"LiberNT\\"
    };
    /* kernel, HAL, boot path and the critical processes: never replaced by the engine in stage 1 */
    static const WCHAR *deniedName[] =
    {
        L"ntoskrnl.exe", L"ntkrnlmp.exe", L"ntkrnlpa.exe", L"ntkrpamp.exe", L"ntdll.dll", L"smss.exe",
        L"csrss.exe", L"wininit.exe", L"winlogon.exe", L"services.exe", L"lsass.exe", L"bootvid.dll",
        L"kdcom.dll", L"ci.dll", L"win32k.sys", L"win32kbase.sys", L"win32kfull.sys", L"autochk.exe",
        L"ntldr", L"bootmgr", L"freeldr.sys", L"freeldr.ini"
    };
    const WCHAR *base, *ext;
    size_t i, bl;
    BOOL inRoot = FALSE, isDriver;

    if (!LnIsSafeRelativePath(rel))
        return FALSE;
    for (i = 0; i < sizeof(roots) / sizeof(roots[0]); i++)
    {
        if (StartsWithNoCase(rel, roots[i]))
            inRoot = TRUE;
    }
    if (!inRoot)
        return FALSE;
    for (i = 0; i < sizeof(deniedPrefix) / sizeof(deniedPrefix[0]); i++)
    {
        if (StartsWithNoCase(rel, deniedPrefix[i]))
            return FALSE;
    }
    base = wcsrchr(rel, L'\\');
    base = base ? base + 1 : rel;
    for (i = 0; i < sizeof(deniedName) / sizeof(deniedName[0]); i++)
    {
        if (!_wcsicmp(base, deniedName[i]))
            return FALSE;
    }
    bl = wcslen(base);
    if (!_wcsnicmp(base, L"hal", 3) && bl > 4 && !_wcsicmp(base + bl - 4, L".dll"))
        return FALSE;                                   /* hal*.dll */

    ext = bl > 4 ? base + bl - 4 : L"";
    isDriver = !_wcsicmp(ext, L".sys");
    if (isDriver)
    {
        /* drivers: only under System32\drivers, and only auto (2) or demand (3) / disabled (4) start;
         * boot-start (0) and system-start (1) drivers are part of the boot path */
        return StartsWithNoCase(rel, L"Windows\\System32\\drivers\\") && driverStart >= 2 && driverStart <= 4;
    }
    if (driverStart != -1)
        return FALSE;                                   /* driverStart only makes sense for a .sys */
    return !_wcsicmp(ext, L".dll") || !_wcsicmp(ext, L".exe");
}

/* ---- boot class ------------------------------------------------------------------------------ */

static BOOL BootBaseIs(const WCHAR *base, const WCHAR *const *names, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
    {
        if (!_wcsicmp(base, names[i]))
            return TRUE;
    }
    return FALSE;
}

#define LN_BOOT_GROUPS_ENABLED (1 << 1)         /* bit n = group n; 1 = kernel trio */

int LnTargetBootGroup(const WCHAR *rel)
{
    /* kdcom.dll is NOT here (review N3): the Fallback entry has no switch that redirects it, and our labs boot
     * with /DEBUG, so a bad kdcom would fail under Fallback too. It is refused until a loader switch exists. */
    static const WCHAR *kernel[] = { L"ntoskrnl.exe", L"ntkrnlmp.exe", L"ntkrnlpa.exe", L"ntkrpamp.exe",
                                     L"bootvid.dll" };
    static const WCHAR *critical[] = { L"ntdll.dll", L"smss.exe", L"csrss.exe", L"wininit.exe", L"winlogon.exe",
                                       L"services.exe", L"lsass.exe", L"ci.dll", L"win32k.sys", L"win32kbase.sys",
                                       L"win32kfull.sys" };
    const WCHAR *base = wcsrchr(rel, L'\\');
    size_t bl;

    base = base ? base + 1 : rel;
    bl = wcslen(base);
    if (!StartsWithNoCase(rel, L"Windows\\System32\\"))
        return 0;
    if (BootBaseIs(base, kernel, RTL_NUMBER_OF(kernel)) ||
        (!_wcsnicmp(base, L"hal", 3) && bl > 4 && !_wcsicmp(base + bl - 4, L".dll")))
        return 1;
    if (BootBaseIs(base, critical, RTL_NUMBER_OF(critical)))
        return 3;
    if (bl > 4 && !_wcsicmp(base + bl - 4, L".sys") && StartsWithNoCase(rel, L"Windows\\System32\\drivers\\"))
        return 2;
    return 0;
}

/* ---- freeldr.ini fallback check ------------------------------------------------------------- */

#define LNF_MAXSEC 24

typedef struct _LNF_SEC
{
    char name[48];
    char opts[400];
    char kernel[64], hal[64];
    char path[160];                 /* SystemPath */
    int truncated;                  /* Options or SystemPath did not fit: never judge a cut-off value */
} LNF_SEC;

static void LnfCopy(char *dst, size_t cch, const char *src, size_t len)
{
    if (len >= cch)
        len = cch - 1;
    memcpy(dst, src, len);
    dst[len] = 0;
}

/* value of "/KEY=" inside an Options string ("" when absent) */
static void LnfOpt(const char *opts, const char *key, char *out, size_t cch)
{
    size_t kl = strlen(key);
    const char *p = opts;

    out[0] = 0;
    while (*p)
    {
        if ((p == opts || p[-1] == ' ' || p[-1] == '\t') && !_strnicmp(p, key, kl))
        {
            size_t l = 0;

            p += kl;
            while (p[l] && p[l] != ' ' && p[l] != '\t')
                l++;
            LnfCopy(out, cch, p, l);
            return;
        }
        p++;
    }
}

/* is the bare switch (e.g. "/DETECTHAL") present as a whole word? */
static BOOL LnfHasOpt(const char *opts, const char *sw)
{
    size_t l = strlen(sw);
    const char *p = opts;

    for (; *p; p++)
    {
        if ((p == opts || p[-1] == ' ' || p[-1] == '\t') && !_strnicmp(p, sw, l) &&
            (p[l] == 0 || p[l] == ' ' || p[l] == '\t'))
            return TRUE;
    }
    return FALSE;
}

static void LnfEffective(const LNF_SEC *s, const char *key, const char *fromKey, const char *def,
                         char *out, size_t cch)
{
    LnfOpt(s->opts, key, out, cch);
    if (!out[0] && fromKey && fromKey[0])
        LnfCopy(out, cch, fromKey, strlen(fromKey));
    if (!out[0])
        LnfCopy(out, cch, def, strlen(def));
}

static char *LnfTrim(char *s)
{
    char *e;

    while (*s == ' ' || *s == '\t')
        s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
        *--e = 0;
    return s;
}

static void LnfStem(const char *file, char *out, size_t cch)
{
    const char *dot = strrchr(file, '.');

    LnfCopy(out, cch, file, dot ? (size_t)(dot - file) : strlen(file));
}

BOOL LnFreeldrFallbackMatches(const char *ini, size_t len, const WCHAR *const *targets, size_t n,
                              char *why, size_t whyCch)
{
    LNF_SEC secs[LNF_MAXSEC];
    char entries[LNF_MAXSEC][48];
    char defaultOs[48] = "";
    size_t nsec = 0, nent = 0, i, j;
    int cur = -1;                       /* index in secs, -2 = [FREELOADER], -3 = [Operating Systems] */
    char *buf, *line, *next;
    const LNF_SEC *def = NULL;
    char dk[64], dh[64], db[64], ok[64], oh[64], ob[64], sk[64], sh[64], sb[64];
    BOOL found = FALSE;
    const char *msg;

    msg = "";
    if (!ini || !targets || n != 3)
    {
        msg = "the package must carry exactly the kernel, HAL and bootvid files";
        goto out_msg;
    }
    buf = malloc(len + 1);
    if (!buf)
    {
        msg = "out of memory";
        goto out_msg;
    }
    memcpy(buf, ini, len);
    buf[len] = 0;
    memset(secs, 0, sizeof(secs));

    for (line = buf; line && *line; line = next)
    {
        char *eq, *key, *val;

        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        line = LnfTrim(line);
        if (!*line || *line == ';' || *line == '#')
            continue;
        if (*line == '[')
        {
            char *close = strchr(line, ']');
            char name[48];

            if (!close)
                continue;
            LnfCopy(name, sizeof(name), line + 1, (size_t)(close - line - 1));
            if (!_stricmp(name, "FREELOADER"))
                cur = -2;
            else if (!_stricmp(name, "Operating Systems"))
                cur = -3;
            else if (nsec < LNF_MAXSEC)
            {
                LnfCopy(secs[nsec].name, sizeof(secs[nsec].name), name, strlen(name));
                cur = (int)nsec++;
            }
            else
                cur = -1;
            continue;
        }
        eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        key = LnfTrim(line);
        val = LnfTrim(eq + 1);
        if (*val == '"')
        {
            char *q = strchr(val + 1, '"');

            val++;
            if (q)
                *q = 0;
        }
        if (cur == -2)
        {
            if (!_stricmp(key, "DefaultOS"))
                LnfCopy(defaultOs, sizeof(defaultOs), val, strlen(val));
        }
        else if (cur == -3)
        {
            if (nent < LNF_MAXSEC)
                LnfCopy(entries[nent++], sizeof(entries[0]), key, strlen(key));
        }
        else if (cur >= 0)
        {
            if (!_stricmp(key, "Options"))
            {
                if (strlen(val) >= sizeof(secs[cur].opts))
                    secs[cur].truncated = 1;
                LnfCopy(secs[cur].opts, sizeof(secs[cur].opts), val, strlen(val));
            }
            else if (!_stricmp(key, "SystemPath"))
            {
                if (strlen(val) >= sizeof(secs[cur].path))
                    secs[cur].truncated = 1;
                LnfCopy(secs[cur].path, sizeof(secs[cur].path), val, strlen(val));
            }
            else if (!_stricmp(key, "Kernel"))
                LnfCopy(secs[cur].kernel, sizeof(secs[cur].kernel), val, strlen(val));
            else if (!_stricmp(key, "Hal"))
                LnfCopy(secs[cur].hal, sizeof(secs[cur].hal), val, strlen(val));
        }
    }
    free(buf);

    for (i = 0; i < nsec; i++)
    {
        if (!_stricmp(secs[i].name, defaultOs))
            def = &secs[i];
    }
    if (!def)
    {
        msg = "freeldr.ini has no default entry section";
        goto out_msg;
    }
    if (def->truncated)
    {
        msg = "the default entry's Options or SystemPath is too long to be checked";
        goto out_msg;
    }
    if (LnfHasOpt(def->opts, "/DETECTHAL"))
    {
        msg = "the default entry uses /DETECTHAL, which overrides /HAL= and breaks the fallback";
        goto out_msg;
    }
    LnfEffective(def, "/KERNEL=", def->kernel, "ntoskrnl.exe", dk, sizeof(dk));
    LnfEffective(def, "/HAL=", def->hal, "hal.dll", dh, sizeof(dh));
    LnfEffective(def, "/BOOTVID=", NULL, "bootvid.dll", db, sizeof(db));

    /* the package must replace exactly these three files, nothing else */
    {
        const char *want[3];
        BOOL used[3] = { FALSE, FALSE, FALSE };

        want[0] = dk;
        want[1] = dh;
        want[2] = db;
        for (i = 0; i < 3; i++)
        {
            const WCHAR *base = wcsrchr(targets[i], L'\\');
            char nb[64];
            size_t k = 0;
            BOOL ok3 = FALSE;

            base = base ? base + 1 : targets[i];
            for (; base[k] && k < sizeof(nb) - 1; k++)
                nb[k] = (char)base[k];
            nb[k] = 0;
            for (j = 0; j < 3; j++)
            {
                if (!used[j] && !_stricmp(nb, want[j]))
                {
                    used[j] = TRUE;
                    ok3 = TRUE;
                    break;
                }
            }
            if (!ok3)
            {
                msg = "a package file is not the kernel, HAL or bootvid the default boot entry loads";
                goto out_msg;
            }
        }
    }

    LnfStem(dk, sk, sizeof(sk));
    LnfStem(dh, sh, sizeof(sh));
    LnfStem(db, sb, sizeof(sb));
    for (i = 0; i < nent && !found; i++)
    {
        for (j = 0; j < nsec; j++)
        {
            const LNF_SEC *s = &secs[j];
            char e1[80], e2[80], e3[80];

            if (_stricmp(s->name, entries[i]) || s == def)
                continue;
            if (s->truncated || LnfHasOpt(s->opts, "/DETECTHAL") || _stricmp(s->path, def->path))
                continue;                   /* same SystemPath, no /DETECTHAL, nothing cut off */
            LnfEffective(s, "/KERNEL=", s->kernel, "ntoskrnl.exe", ok, sizeof(ok));
            LnfEffective(s, "/HAL=", s->hal, "hal.dll", oh, sizeof(oh));
            LnfEffective(s, "/BOOTVID=", NULL, "bootvid.dll", ob, sizeof(ob));
            LnfCopy(e1, sizeof(e1), sk, strlen(sk));
            strcat(e1, ".old");
            LnfCopy(e2, sizeof(e2), sh, strlen(sh));
            strcat(e2, ".old");
            LnfCopy(e3, sizeof(e3), sb, strlen(sb));
            strcat(e3, ".old");
            if (!_stricmp(ok, e1) && !_stricmp(oh, e2) && !_stricmp(ob, e3))
            {
                found = TRUE;
                break;
            }
        }
    }
    if (!found)
    {
        msg = "freeldr.ini has no fallback entry loading the <name>.old kernel, HAL and bootvid";
        goto out_msg;
    }
    if (why && whyCch)
        why[0] = 0;
    return TRUE;

out_msg:
    if (why && whyCch)
        LnfCopy(why, whyCch, msg, strlen(msg));
    return FALSE;
}

BOOL LnOptionsUseOldTrio(const WCHAR *options)
{
    static const WCHAR *keys[] = { L"/KERNEL=", L"/HAL=", L"/BOOTVID=" };
    size_t k;

    for (k = 0; k < RTL_NUMBER_OF(keys); k++)
    {
        size_t kl = wcslen(keys[k]);
        const WCHAR *p = options;

        for (; *p; p++)
        {
            if ((p == options || p[-1] == L' ' || p[-1] == L'\t') && !_wcsnicmp(p, keys[k], kl))
            {
                const WCHAR *v = p + kl;
                size_t l = 0;

                while (v[l] && v[l] != L' ' && v[l] != L'\t')
                    l++;
                if (l >= 4 && !_wcsnicmp(v + l - 4, L".old", 4))
                    return TRUE;
            }
        }
    }
    return FALSE;
}

BOOL LnTargetBootItemAllowed(const WCHAR *rel, int driverStart)
{
    static const WCHAR *deniedPrefix[] =
    {
        L"Windows\\System32\\config\\", L"Windows\\System32\\boot\\", L"LiberNT\\"
    };
    /* loader files and the updater's own binaries are never replaced by this route */
    static const WCHAR *deniedName[] =
    {
        L"ntldr", L"bootmgr", L"freeldr.sys", L"freeldr.ini", L"rosload.exe", L"autochk.exe",
        L"LiberNTUpdatePending.exe", L"libernt-update.exe"
    };
    size_t i;
    int group;
    const WCHAR *base;

    if (!LnIsSafeRelativePath(rel))
        return FALSE;
    for (i = 0; i < RTL_NUMBER_OF(deniedPrefix); i++)
    {
        if (StartsWithNoCase(rel, deniedPrefix[i]))
            return FALSE;
    }
    base = wcsrchr(rel, L'\\');
    base = base ? base + 1 : rel;
    for (i = 0; i < RTL_NUMBER_OF(deniedName); i++)
    {
        if (!_wcsicmp(base, deniedName[i]))
            return FALSE;
    }
    group = LnTargetBootGroup(rel);
    if (!group)
        return FALSE;
    /* First delivery: the kernel/HAL/bootvid trio only (review M3). Boot-start drivers (ntfs.sys, disk class)
     * and ntdll/win32k/the critical processes have no loader fallback, so they wait for the control-set
     * fallback design. Enable more groups by widening this mask, with that design in place. */
    if (!((1 << group) & LN_BOOT_GROUPS_ENABLED))
        return FALSE;
    if (group == 2)
        return driverStart >= 0 && driverStart <= 4;    /* a driver must declare its Start in the signed manifest */
    return driverStart == -1;                           /* win32k*.sys is a kernel module, not a service */
}

BOOL LnTargetPathAllowed(const WCHAR *rel)
{
    return LnTargetItemAllowed(rel, -1);
}

/* ---- real path ----------------------------------------------------------------------------- */

static DWORD FinalPath(const WCHAR *path, WCHAR *out, DWORD cap)
{
    HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    DWORD n;

    if (h == INVALID_HANDLE_VALUE)
        return GetLastError();
    n = GetFinalPathNameByHandleW(h, out, cap, FILE_NAME_NORMALIZED);
    CloseHandle(h);
    return (n == 0 || n >= cap) ? ERROR_BAD_PATHNAME : ERROR_SUCCESS;
}

DWORD LnTargetValidateReal(const LN_TARGET *t, const WCHAR *rel)
{
    WCHAR cur[MAX_PATH], deepest[MAX_PATH], finalRoot[MAX_PATH], finalDeep[MAX_PATH], expect[MAX_PATH];
    const WCHAR *p = rel;
    DWORD err;
    size_t rootLen;
    WCHAR *rootNoSlash;

    if (!LnIsSafeRelativePath(rel))
        return LN_E_FORBIDDEN_TARGET;
    wcscpy(cur, t->root);
    wcscpy(deepest, t->root);
    while (*p)
    {
        const WCHAR *e = wcschr(p, L'\\');
        size_t n = e ? (size_t)(e - p) : wcslen(p);
        DWORD attrs;

        if (wcslen(cur) + n + 2 >= MAX_PATH)
            return ERROR_BAD_PATHNAME;
        wcsncat(cur, p, n);
        attrs = GetFileAttributesW(cur);
        if (attrs == INVALID_FILE_ATTRIBUTES)
            break;                                      /* the rest does not exist yet: we create it */
        if (attrs & FILE_ATTRIBUTE_REPARSE_POINT)
            return LN_E_FORBIDDEN_TARGET;               /* junction or symbolic link in the way */
        if (e)
        {
            if (!(attrs & FILE_ATTRIBUTE_DIRECTORY))
                return LN_E_PRECONDITION;
            wcscpy(deepest, cur);
            wcscat(cur, L"\\");
        }
        if (!e)
            break;
        p = e + 1;
    }

    /* the existing part of the parent chain must resolve to exactly what was written (no 8.3, no alias) */
    rootNoSlash = _wcsdup(t->root);
    if (!rootNoSlash)
        return ERROR_NOT_ENOUGH_MEMORY;
    rootLen = wcslen(rootNoSlash);
    if (rootLen > 3 && rootNoSlash[rootLen - 1] == L'\\')
        rootNoSlash[rootLen - 1] = 0;
    err = FinalPath(rootNoSlash, finalRoot, MAX_PATH);
    free(rootNoSlash);
    if (err)
        return err;
    err = FinalPath(deepest, finalDeep, MAX_PATH);
    if (err)
        return err;
    {
        size_t rl = wcslen(finalRoot);
        const WCHAR *tail = deepest + wcslen(t->root);

        wcscpy(expect, finalRoot);
        if (rl && expect[rl - 1] != L'\\' && *tail)
            wcscat(expect, L"\\");
        wcscat(expect, tail);
        rl = wcslen(expect);
        if (rl > 3 && expect[rl - 1] == L'\\')
            expect[rl - 1] = 0;
        rl = wcslen(finalDeep);
        if (rl > 3 && finalDeep[rl - 1] == L'\\')
            finalDeep[rl - 1] = 0;
    }
    if (_wcsicmp(expect, finalDeep))
        return LN_E_FORBIDDEN_TARGET;
    return ERROR_SUCCESS;
}

/* ---- the protected servicing tree --------------------------------------------------------------- */

static BOOL SidTrusted(PSID sid)
{
    BYTE buf[SECURITY_MAX_SID_SIZE];
    DWORD n = sizeof(buf);
    static const WELL_KNOWN_SID_TYPE ok[] = { WinLocalSystemSid, WinBuiltinAdministratorsSid };
    size_t i;

    for (i = 0; i < sizeof(ok) / sizeof(ok[0]); i++)
    {
        n = sizeof(buf);
        if (CreateWellKnownSid(ok[i], NULL, buf, &n) && EqualSid(sid, buf))
            return TRUE;
    }
    {
        /* NT SERVICE\TrustedInstaller owns the files of a stock Windows tree */
        SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
        PSID ti = NULL;
        BOOL same = FALSE;

        if (AllocateAndInitializeSid(&nt, 6, 80, 956008885, 3418522649UL, 1831038044, 1853292631, 2271478464UL, 0, 0, &ti))
        {
            same = EqualSid(sid, ti);
            FreeSid(ti);
        }
        if (same)
            return TRUE;
    }
#ifdef LN_TESTING
    if (LnTestExtraSid && EqualSid(sid, LnTestExtraSid))
        return TRUE;
#endif
    return FALSE;
}

/* Owner SYSTEM/Administrators and a PROTECTED DACL made only of allow ACEs for SYSTEM/Administrators. */
static DWORD CheckSecure(const WCHAR *path)
{
    PSECURITY_DESCRIPTOR sd = NULL;
    PSID owner = NULL;
    PACL dacl = NULL;
    SECURITY_DESCRIPTOR_CONTROL ctl = 0;
    DWORD rev, i, result = LN_E_INSECURE_TARGET;

    if (GetNamedSecurityInfoW((LPWSTR)path, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                              &owner, NULL, &dacl, NULL, &sd) != ERROR_SUCCESS)
        return LN_E_INSECURE_TARGET;                    /* no ACLs (FAT) or unreadable: fail closed */
    if (!owner || !dacl || !GetSecurityDescriptorControl(sd, &ctl, &rev) || !(ctl & SE_DACL_PROTECTED))
        goto out;
    if (!SidTrusted(owner) || dacl->AceCount == 0)
        goto out;
    for (i = 0; i < dacl->AceCount; i++)
    {
        ACE_HEADER *ace;

        if (!GetAce(dacl, i, (void **)&ace))
            goto out;
        if (ace->AceType != ACCESS_ALLOWED_ACE_TYPE)
            goto out;
        if (!SidTrusted((PSID)&((ACCESS_ALLOWED_ACE *)ace)->SidStart))
            goto out;
    }
    result = ERROR_SUCCESS;
out:
    LocalFree(sd);
    return result;
}

static DWORD CreateSecureDir(const WCHAR *path)
{
    WCHAR sddl[512];
    PSECURITY_DESCRIPTOR sd = NULL;
    SECURITY_ATTRIBUTES sa;
    DWORD err = ERROR_SUCCESS;

    wcscpy(sddl, L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)");
#ifdef LN_TESTING
    if (LnTestExtraSid)
    {
        WCHAR *str = NULL;

        if (ConvertSidToStringSidW(LnTestExtraSid, &str))
        {
            wcscat(sddl, L"(A;OICI;FA;;;");
            wcscat(sddl, str);
            wcscat(sddl, L")");
            LocalFree(str);
        }
    }
#endif
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, NULL))
        return GetLastError();
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;
    if (!CreateDirectoryW(path, &sa) && GetLastError() != ERROR_ALREADY_EXISTS)
        err = GetLastError();
    LocalFree(sd);
    return err;
}

/* ---- target -------------------------------------------------------------------------------- */

static BOOL IsGuid(const char *s)
{
    int i;

    if (strlen(s) != 36)
        return FALSE;
    for (i = 0; i < 36; i++)
    {
        char c = s[i];

        if (i == 8 || i == 13 || i == 18 || i == 23)
        {
            if (c != '-')
                return FALSE;
        }
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return FALSE;
    }
    return TRUE;
}

static DWORD MakeIdentity(const LN_TARGET *t, char out[65])
{
    unsigned char seed[64];
    LARGE_INTEGER pc;
    FILETIME ft;
    DWORD pid = GetCurrentProcessId(), tick = GetTickCount();
    LN_SHA256_CTX c;
    unsigned char h[32];
    HMODULE k = GetModuleHandleW(L"kernel32.dll");

    QueryPerformanceCounter(&pc);
    GetSystemTimeAsFileTime(&ft);
    memset(seed, 0, sizeof(seed));
    memcpy(seed, &pc, sizeof(pc));
    memcpy(seed + 8, &ft, sizeof(ft));
    memcpy(seed + 16, &pid, sizeof(pid));
    memcpy(seed + 20, &tick, sizeof(tick));
    memcpy(seed + 24, &k, sizeof(k));
    LnSha256Init(&c);
    LnSha256Update(&c, seed, sizeof(seed));
    LnSha256Update(&c, t->root, wcslen(t->root) * sizeof(WCHAR));
    LnSha256Final(&c, h);
    {
        char hex[65];

        LnHexEncode(h, 32, hex);                         /* GUID text from the first 16 bytes */
        memcpy(out, hex, 8);           out[8] = '-';
        memcpy(out + 9, hex + 8, 4);   out[13] = '-';
        memcpy(out + 14, hex + 12, 4); out[18] = '-';
        memcpy(out + 19, hex + 16, 4); out[23] = '-';
        memcpy(out + 24, hex + 20, 12);
        out[36] = 0;
    }
    return ERROR_SUCCESS;
}

DWORD LnTargetOpen(LN_TARGET *t, LN_TARGET_MODE mode, const WCHAR *imageRoot, BOOL create)
{
    WCHAR win[MAX_PATH], *idPath, libernt[MAX_PATH];
    DWORD err, attrs;
    size_t n;
    BYTE *data;
    size_t len;

    memset(t, 0, sizeof(*t));
    t->mode = mode;
    if (mode == LnTargetOnline)
    {
        if (!GetSystemWindowsDirectoryW(win, MAX_PATH) || win[1] != L':')
            return ERROR_PATH_NOT_FOUND;
        t->root[0] = win[0];
        t->root[1] = L':';
        t->root[2] = L'\\';
        t->root[3] = 0;
    }
    else if (mode == LnTargetImage)
    {
        if (!imageRoot || !*imageRoot || wcslen(imageRoot) > MAX_PATH - 100)
            return ERROR_BAD_PATHNAME;
        wcscpy(t->root, imageRoot);
        n = wcslen(t->root);
        if (t->root[n - 1] != L'\\')
            wcscat(t->root, L"\\");
        attrs = GetFileAttributesW(t->root);
        if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
            return ERROR_PATH_NOT_FOUND;
    }
    else
        return ERROR_INVALID_PARAMETER;

    wcscpy(libernt, t->root);
    wcscat(libernt, LIBERNT_REL);
    wcscpy(t->servicing, t->root);
    wcscat(t->servicing, SERVICING_REL);

    if (GetFileAttributesW(t->servicing) == INVALID_FILE_ATTRIBUTES)
    {
        if (!create)
            return ERROR_PATH_NOT_FOUND;
        err = CreateSecureDir(libernt);
        if (!err)
            err = CreateSecureDir(t->servicing);
        if (err)
            return err;
    }
    /* never read the store, the trust anchor or the journal from a tree anyone else could write */
    err = CheckSecure(libernt);
    if (!err)
        err = CheckSecure(t->servicing);
    if (!err)
    {
        /* a link in place of either folder would redirect the whole tree */
        DWORD a1 = GetFileAttributesW(libernt), a2 = GetFileAttributesW(t->servicing);

        if (a1 == INVALID_FILE_ATTRIBUTES || a2 == INVALID_FILE_ATTRIBUTES ||
            (a1 & FILE_ATTRIBUTE_REPARSE_POINT) || (a2 & FILE_ATTRIBUTE_REPARSE_POINT))
            err = LN_E_INSECURE_TARGET;
    }
    if (err)
        return err;

    idPath = LnJoin(t->servicing, L"target-id");
    if (!idPath)
        return ERROR_NOT_ENOUGH_MEMORY;
    err = LnReadFileAll(idPath, &data, &len);
    if (!err)
    {
        if (len >= 36)
        {
            char id[65];

            memcpy(id, data, 36);
            id[36] = 0;
            if (IsGuid(id))
                strcpy(t->identity, id);
            else
                err = LN_E_MANIFEST_INVALID;
        }
        else
            err = LN_E_MANIFEST_INVALID;
        free(data);
    }
    else if (create && (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND))
    {
        MakeIdentity(t, t->identity);
        err = LnWriteFileAtomic(idPath, t->identity, 36);
    }
    free(idPath);
    return err;
}

WCHAR *LnTargetPath(const LN_TARGET *t, const WCHAR *rel)
{
    return LnJoin(t->servicing, rel);
}

WCHAR *LnTargetFile(const LN_TARGET *t, const WCHAR *rel)
{
    return LnJoin(t->root, rel);
}
