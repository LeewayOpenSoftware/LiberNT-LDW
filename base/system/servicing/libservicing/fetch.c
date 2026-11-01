/*
 * PROJECT:     LiberNT Update - libservicing
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Fetch servicing packages over HTTP for add-package --from
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Minimal HTTP/1.0 client on raw winsock. WinHTTP is not usable on current
 * targets (request path works, the response path fails), so this speaks just
 * enough HTTP to download the manifest, its signature and the payloads the
 * manifest names. The manifest signature is verified against the pinned
 * certificate before anything the manifest says is acted on; the existing
 * verify and stage path remains the final authority over the fetched copy.
 */
#include "libservicing.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#define LN_FETCH_DEFAULT_CAP (64ULL * 1024ULL * 1024ULL)
#define LN_FETCH_DEFAULT_PACKAGE_CAP (256ULL * 1024ULL * 1024ULL)
#define LN_FETCH_DEFAULT_TIMEOUT_MS 30000
#define LN_FETCH_IO_CHUNK 32768
#define LN_FETCH_MAX_URL_CHARS 480      /* the request line buffer leaves headroom */
#define LN_FETCH_MAX_HOST_CHARS 255

static ULONGLONG
LnFetchEnvBytes(const WCHAR *name, ULONGLONG dflt)
{
    WCHAR buf[32];
    DWORD n = GetEnvironmentVariableW(name, buf, 32);

    if (n > 0 && n < 32)
        return _wcstoui64(buf, NULL, 10);
    return dflt;
}

static DWORD
LnFetchTimeoutMs(VOID)
{
    WCHAR buf[24];
    DWORD n = GetEnvironmentVariableW(L"LN_FETCH_TIMEOUT_MS", buf, 24);

    if (n > 0 && n < 24)
        return wcstoul(buf, NULL, 10);
    return LN_FETCH_DEFAULT_TIMEOUT_MS;
}

/* http://host[:port][/path...]; host is an IPv4 literal or a DNS name.
 * Everything else (https, userinfo, backslashes, empty path) is rejected,
 * and the host/path must be printable ASCII that fits the request line. */
static DWORD
LnFetchParseUrl(const WCHAR *url, WCHAR *host, DWORD hostCch, DWORD *port, const WCHAR **path)
{
    const WCHAR *p, *slash, *s;

    if (wcsncmp(url, L"http://", 7) != 0)
        return ERROR_INVALID_DATA;
    p = url + 7;
    if (wcschr(p, L'@') != NULL || wcschr(p, L'\\') != NULL)
        return ERROR_INVALID_DATA;
    for (s = p; *s; s++)
    {
        if (*s > 0x7E || *s <= 0x20 || *s == 0x7F)
            return ERROR_INVALID_DATA;
    }

    slash = wcschr(p, L'/');
    if (slash && (DWORD)(wcslen(slash)) > LN_FETCH_MAX_URL_CHARS)
        return ERROR_INVALID_DATA;
    {
        const WCHAR *colon = wcschr(p, L':');
        const WCHAR *hend = slash ? slash : p + wcslen(p);
        if (colon && colon < hend)
        {
            DWORD hp = wcstoul(colon + 1, NULL, 10);
            if (hp == 0 || hp > 65535)
                return ERROR_INVALID_DATA;
            *port = hp;
            if ((DWORD)(colon - p) >= hostCch || (DWORD)(colon - p) > LN_FETCH_MAX_HOST_CHARS)
                return ERROR_INVALID_DATA;
            wcsncpy(host, p, colon - p);
            host[colon - p] = 0;
        }
        else
        {
            if ((DWORD)(hend - p) == 0 || (DWORD)(hend - p) >= hostCch ||
                (DWORD)(hend - p) > LN_FETCH_MAX_HOST_CHARS)
                return ERROR_INVALID_DATA;
            wcsncpy(host, p, hend - p);
            host[hend - p] = 0;
            *port = 80;
        }
    }
    if (host[0] == 0)
        return ERROR_INVALID_DATA;
    *path = slash ? slash : L"/";
    return ERROR_SUCCESS;
}

/* One GET, status 200 only, body capped. Partial output is always removed. */
DWORD
LnFetchFile(const WCHAR *url, const WCHAR *destPath)
{
    WCHAR host[256];
    const WCHAR *path;
    DWORD port, err = ERROR_SUCCESS, wsaErr = 0, timeoutMs;
    ULONGLONG cap, total = 0;
    WSADATA wd;
    SOCKET s = INVALID_SOCKET;
    struct sockaddr_in sa;
    struct hostent *he;
    CHAR req[1200], *io = NULL;
    LONG status;
    HANDLE f = INVALID_HANDLE_VALUE;
    BOOL started = FALSE;
    DWORD got, sent = 0, reqLen = 0, bodyOff;
    CHAR *header = NULL, *hdrEnd;
    const CHAR *bodyStart;
    int n;

    err = LnFetchParseUrl(url, host, 256, &port, &path);
    if (err != ERROR_SUCCESS)
        return err;

    cap = LnFetchEnvBytes(L"LN_FETCH_MAX_BYTES", LN_FETCH_DEFAULT_CAP);
    timeoutMs = LnFetchTimeoutMs();

    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0)
        return ERROR_NOT_SUPPORTED;

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
    {
        wsaErr = WSAGetLastError();
        err = ERROR_NOT_SUPPORTED;
        goto done;
    }
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const CHAR *)&timeoutMs, sizeof(timeoutMs));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const CHAR *)&timeoutMs, sizeof(timeoutMs));

    {
        CHAR hostA[LN_FETCH_MAX_HOST_CHARS + 1], pathA[LN_FETCH_MAX_URL_CHARS + 1];

        if (!WideCharToMultiByte(CP_ACP, 0, host, -1, hostA, sizeof(hostA), NULL, NULL) ||
            !WideCharToMultiByte(CP_ACP, 0, path, -1, pathA, sizeof(pathA), NULL, NULL))
        {
            err = ERROR_INVALID_DATA;
            goto done;
        }
        sa.sin_family = AF_INET;
        sa.sin_port = htons((WORD)port);
        sa.sin_addr.s_addr = inet_addr(hostA);
        if (sa.sin_addr.s_addr == INADDR_NONE)
        {
            he = gethostbyname(hostA);
            if (he == NULL || he->h_addr_list[0] == NULL)
            {
                wsaErr = WSAGetLastError();
                err = ERROR_NOT_FOUND;
                goto done;
            }
            sa.sin_addr.s_addr = *(ULONG *)he->h_addr_list[0];
        }

        if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0)
        {
            wsaErr = WSAGetLastError();
            err = ERROR_NOT_CONNECTED;
            goto done;
        }

        n = _snprintf(req, sizeof(req),
                      "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: libernt-update\r\n"
                      "Connection: close\r\n\r\n",
                      pathA, hostA);
        if (n < 0 || (DWORD)n >= sizeof(req))
        {
            err = ERROR_INVALID_DATA;
            goto done;
        }
        reqLen = (DWORD)n;
    }
    while (sent < reqLen)
    {
        int w = send(s, req + sent, reqLen - sent, 0);
        if (w <= 0)
        {
            wsaErr = WSAGetLastError();
            err = ERROR_WRITE_FAULT;
            goto done;
        }
        sent += (DWORD)w;
    }

    io = malloc(LN_FETCH_IO_CHUNK);
    header = malloc(65536);
    if (io == NULL || header == NULL)
    {
        err = ERROR_NOT_ENOUGH_MEMORY;
        goto done;
    }
    {
        DWORD filled = 0;
        for (;;)
        {
            int r = recv(s, header + filled, 65536 - 1 - filled, 0);
            if (r <= 0)
                break;
            filled += (DWORD)r;
            header[filled] = 0;
            if (strstr(header, "\r\n\r\n"))
                break;
            if (filled == 65535)
            {
                err = ERROR_BAD_FORMAT;
                goto done;
            }
        }
        hdrEnd = strstr(header, "\r\n\r\n");
        if (filled == 0 || strncmp(header, "HTTP/1.", 7) != 0 || hdrEnd == NULL)
        {
            err = ERROR_BAD_FORMAT;
            goto done;
        }
        status = strtol(header + 8, NULL, 10);
        if (status != 200)
        {
            err = ERROR_INVALID_DATA;
            goto done;
        }
        bodyStart = hdrEnd + 4;
        bodyOff = filled - (DWORD)(bodyStart - header);
    }

    f = CreateFileW(destPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
    {
        err = GetLastError();
        goto done;
    }
    started = TRUE;

    if (bodyOff > 0)
    {
        if (!WriteFile(f, bodyStart, bodyOff, &got, NULL))
        {
            err = GetLastError();
            goto done;
        }
        total += bodyOff;
        if (total > cap)
        {
            err = ERROR_FILE_TOO_LARGE;
            goto done;
        }
    }
    free(header);
    header = NULL;
    for (;;)
    {
        int r = recv(s, io, LN_FETCH_IO_CHUNK, 0);
        if (r == 0)
            break;
        if (r < 0)
        {
            wsaErr = WSAGetLastError();
            err = ERROR_READ_FAULT;
            goto done;
        }
        if (!WriteFile(f, io, (DWORD)r, &got, NULL))
        {
            err = GetLastError();
            goto done;
        }
        total += (ULONGLONG)(ULONG)r;
        if (total > cap)
        {
            err = ERROR_FILE_TOO_LARGE;
            goto done;
        }
    }

done:
    if (f != INVALID_HANDLE_VALUE)
        CloseHandle(f);
    if (started && err != ERROR_SUCCESS)
        DeleteFileW(destPath);
    if (s != INVALID_SOCKET)
        closesocket(s);
    free(header);
    free(io);
    WSACleanup();
    if (err == ERROR_SUCCESS && wsaErr != 0)
        err = wsaErr;
    return err;
}

/* Download manifest.json + manifest.sig, verify the signature against the pinned
 * certificate, then fetch the payloads the now-trusted manifest names into destDir
 * (a fresh package directory). Per-payload hashes are checked as they land and the
 * total is capped; any failure removes the directory. The add path re-verifies
 * everything authoritatively over the fetched copy. */
DWORD
LnFetchPackage(const LN_TARGET *t, const WCHAR *baseUrl, const WCHAR *destDir)
{
    WCHAR *url = NULL, *file = NULL, *payloadDir = NULL, *cer = NULL;
    LN_MANIFEST m;
    DWORD err;
    size_t i;
    ULONGLONG total = 0, cap = LnFetchEnvBytes(L"LN_FETCH_MAX_PACKAGE_BYTES", LN_FETCH_DEFAULT_PACKAGE_CAP);

    memset(&m, 0, sizeof(m));

    err = LnEnsureDir(destDir);
    if (err != ERROR_SUCCESS)
        return err;

    url = malloc(sizeof(WCHAR) * (wcslen(baseUrl) + 32));
    file = malloc(sizeof(WCHAR) * (wcslen(destDir) + 32));
    cer = LnTargetPath(t, L"trust\\Leeway-LiberNT-Update-DEV.cer");
    if (!url || !file || !cer)
    {
        err = ERROR_NOT_ENOUGH_MEMORY;
        goto done;
    }

    wcscpy(url, baseUrl);
    wcscat(url, L"/manifest.json");
    wcscpy(file, destDir);
    wcscat(file, L"\\manifest.json");
    err = LnFetchFile(url, file);
    if (err != ERROR_SUCCESS)
        goto done;

    wcscpy(url, baseUrl);
    wcscat(url, L"/manifest.sig");
    wcscpy(file, destDir);
    wcscat(file, L"\\manifest.sig");
    err = LnFetchFile(url, file);
    if (err != ERROR_SUCCESS)
        goto done;

    /* the manifest is untrusted until its signature checks: read it once and
     * verify + parse those same bytes, then let its payload names drive the
     * downloads */
    {
        BYTE *man = NULL;
        size_t manLen = 0;

        wcscpy(file, destDir);
        wcscat(file, L"\\manifest.json");
        err = LnReadFileAll(file, &man, &manLen);
        if (err == ERROR_SUCCESS)
            err = LnVerifyManifestSignatureMem(man, manLen, destDir, cer);
        if (err == ERROR_SUCCESS)
            err = LnManifestParse((const char *)man, manLen, &m);
        free(man);
    }
    if (err != ERROR_SUCCESS)
        goto done;

    payloadDir = LnJoin(destDir, L"payload");
    if (!payloadDir)
    {
        err = ERROR_NOT_ENOUGH_MEMORY;
        goto done;
    }
    err = LnEnsureDir(payloadDir);
    if (err != ERROR_SUCCESS)
        goto done;

    for (i = 0; i < m.itemCount; i++)
    {
        char hex[65];
        WCHAR *dest, *bslash;
        LARGE_INTEGER sz;

        if (m.items[i].isDelete || !m.items[i].path)
            continue;

        free(url);
        url = malloc(sizeof(WCHAR) * (wcslen(baseUrl) + wcslen(m.items[i].path) + 16));
        if (!url)
        {
            err = ERROR_NOT_ENOUGH_MEMORY;
            goto done;
        }

        dest = LnJoin(payloadDir, m.items[i].path);
        if (!dest)
        {
            err = ERROR_NOT_ENOUGH_MEMORY;
            goto done;
        }

        bslash = wcsrchr(dest, L'\\');
        if (bslash)
        {
            *bslash = 0;
            err = LnEnsureDir(dest);
            *bslash = L'\\';
            if (err != ERROR_SUCCESS)
            {
                free(dest);
                goto done;
            }
        }

        wcscpy(url, baseUrl);
        wcscat(url, L"/payload/");
        wcscat(url, m.items[i].path);

        err = LnFetchFile(url, dest);
        if (err == ERROR_SUCCESS)
        {
            HANDLE f = CreateFileW(dest, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, NULL);
            if (f == INVALID_HANDLE_VALUE)
                err = GetLastError();
            else
            {
                if (GetFileSizeEx(f, &sz))
                {
                    total += (ULONGLONG)sz.QuadPart;
                    if (total > cap)
                        err = ERROR_FILE_TOO_LARGE;
                }
                else
                    err = GetLastError();
                CloseHandle(f);
            }
        }
        if (err == ERROR_SUCCESS)
            err = LnSha256FileHex(dest, hex);
        if (err == ERROR_SUCCESS && strcmp(hex, m.items[i].sha256) != 0)
            err = LN_E_HASH_MISMATCH;
        free(dest);
        if (err != ERROR_SUCCESS)
            goto done;
    }

done:
    LnManifestFree(&m);
    free(payloadDir);
    free(url);
    free(file);
    free(cer);
    if (err != ERROR_SUCCESS)
        LnRemoveTree(destDir);
    return err;
}
