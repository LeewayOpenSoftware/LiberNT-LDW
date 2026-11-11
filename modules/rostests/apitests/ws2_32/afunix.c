/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     AF_UNIX stream sockets: bind to a path, listen, connect, echo, names and address reuse
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "ws2_32.h"
#include <afunix.h>

static void MakePath(char *Path, size_t Length)
{
    char temp[MAX_PATH];

    GetTempPathA(sizeof(temp), temp);
    _snprintf(Path, Length, "%slnt-afunix-%lu.sock", temp, GetCurrentProcessId());
    Path[Length - 1] = 0;
    DeleteFileA(Path);
}

START_TEST(afunix)
{
    WSADATA data;
    SOCKET listener, client, server, again;
    SOCKADDR_UN address, name;
    int length, result;
    char path[UNIX_PATH_MAX], buffer[8];
    DWORD timeout = 3000;

    if (WSAStartup(MAKEWORD(2, 2), &data))
    {
        skip("WSAStartup failed\n");
        return;
    }
    listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener == INVALID_SOCKET)
    {
        ok(WSAGetLastError() == WSAEAFNOSUPPORT, "AF_UNIX socket: %d\n", WSAGetLastError());
        skip("AF_UNIX not available: %d\n", WSAGetLastError());
        WSACleanup();
        return;
    }
    MakePath(path, sizeof(path));
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, path);
    ok(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0, "bind %s: %d\n", path, WSAGetLastError());
    ok(listen(listener, 1) == 0, "listen: %d\n", WSAGetLastError());

    again = socket(AF_UNIX, SOCK_STREAM, 0);
    ok(again != INVALID_SOCKET, "second AF_UNIX socket: %d\n", WSAGetLastError());
    if (again != INVALID_SOCKET)
    {
        result = bind(again, (struct sockaddr *)&address, sizeof(address));
        ok(result == SOCKET_ERROR && WSAGetLastError() == WSAEADDRINUSE, "second bind to the same path: %d/%d, expected WSAEADDRINUSE\n", result, WSAGetLastError());
        closesocket(again);
    }

    client = socket(AF_UNIX, SOCK_STREAM, 0);
    ok(connect(client, (struct sockaddr *)&address, sizeof(address)) == 0, "connect: %d\n", WSAGetLastError());
    server = accept(listener, NULL, NULL);
    ok(server != INVALID_SOCKET, "accept: %d\n", WSAGetLastError());
    if (server != INVALID_SOCKET)
    {
        setsockopt(server, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
        ok(send(client, "ping", 4, 0) == 4, "client send: %d\n", WSAGetLastError());
        result = recv(server, buffer, sizeof(buffer), 0);
        ok(result == 4 && !memcmp(buffer, "ping", 4), "server recv returned %d (%d)\n", result, WSAGetLastError());
        ok(send(server, "pong", 4, 0) == 4, "server send: %d\n", WSAGetLastError());
        result = recv(client, buffer, sizeof(buffer), 0);
        ok(result == 4 && !memcmp(buffer, "pong", 4), "client recv returned %d (%d)\n", result, WSAGetLastError());
        length = sizeof(name);
        memset(&name, 0, sizeof(name));
        ok(getsockname(server, (struct sockaddr *)&name, &length) == 0, "getsockname: %d\n", WSAGetLastError());
        ok(name.sun_family == AF_UNIX, "getsockname family %u\n", name.sun_family);
        ok(!_stricmp(name.sun_path, path), "getsockname path %s, expected %s\n", name.sun_path, path);
        ok(shutdown(client, SD_SEND) == 0, "shutdown: %d\n", WSAGetLastError());
        result = recv(server, buffer, sizeof(buffer), 0);
        ok(result == 0, "recv after peer shutdown returned %d (%d)\n", result, WSAGetLastError());
        closesocket(server);
    }
    closesocket(client);
    closesocket(listener);
    ok(DeleteFileA(path) || GetLastError() == ERROR_FILE_NOT_FOUND, "socket file cleanup: %lu\n", GetLastError());
    WSACleanup();
}
