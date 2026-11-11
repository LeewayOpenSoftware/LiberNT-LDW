/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Listen backlog, data queued before accept, FD_ACCEPT readiness and refused connects
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "ws2_32.h"

static SOCKET MakeListener(struct sockaddr_in *Address, int Backlog)
{
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int length = sizeof(*Address);

    memset(Address, 0, sizeof(*Address));
    Address->sin_family = AF_INET;
    Address->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (listener == INVALID_SOCKET)
        return INVALID_SOCKET;
    if (bind(listener, (struct sockaddr *)Address, sizeof(*Address)) ||
        getsockname(listener, (struct sockaddr *)Address, &length) ||
        listen(listener, Backlog))
    {
        closesocket(listener);
        return INVALID_SOCKET;
    }
    return listener;
}

static BOOL Readable(SOCKET Socket, DWORD TimeoutMs)
{
    fd_set set;
    struct timeval tv;

    FD_ZERO(&set);
    FD_SET(Socket, &set);
    tv.tv_sec = TimeoutMs / 1000;
    tv.tv_usec = (TimeoutMs % 1000) * 1000;
    return select(0, &set, NULL, NULL, &tv) == 1;
}

static void test_data_before_accept(void)
{
    struct sockaddr_in address;
    SOCKET listener, client, server;
    char buffer[16];
    DWORD timeout = 3000;
    int received;

    listener = MakeListener(&address, 1);
    ok(listener != INVALID_SOCKET, "listener: %d\n", WSAGetLastError());
    if (listener == INVALID_SOCKET)
        return;
    client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(connect(client, (struct sockaddr *)&address, sizeof(address)) == 0, "connect: %d\n", WSAGetLastError());
    ok(send(client, "hello", 5, 0) == 5, "send before accept: %d\n", WSAGetLastError());
    ok(Readable(listener, 2000), "listener not readable with a pending connection\n");
    server = accept(listener, NULL, NULL);
    ok(server != INVALID_SOCKET, "accept: %d\n", WSAGetLastError());
    ok(!Readable(listener, 100), "listener readable after the only pending connection was accepted\n");
    setsockopt(server, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    received = recv(server, buffer, sizeof(buffer), 0);
    ok(received == 5, "recv after accept returned %d (%d)\n", received, WSAGetLastError());
    ok(received == 5 && !memcmp(buffer, "hello", 5), "data sent before accept was not delivered intact\n");
    closesocket(server);
    closesocket(client);
    closesocket(listener);
}

static void test_refused(void)
{
    struct sockaddr_in address;
    SOCKET probe, client;
    int length = sizeof(address);

    probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ok(bind(probe, (struct sockaddr *)&address, sizeof(address)) == 0, "bind: %d\n", WSAGetLastError());
    ok(getsockname(probe, (struct sockaddr *)&address, &length) == 0, "getsockname: %d\n", WSAGetLastError());
    closesocket(probe);
    client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(connect(client, (struct sockaddr *)&address, sizeof(address)) == SOCKET_ERROR, "connect to a closed port succeeded\n");
    ok(WSAGetLastError() == WSAECONNREFUSED, "connect to a closed port: %d, expected WSAECONNREFUSED\n", WSAGetLastError());
    closesocket(client);
}

static void test_backlog(void)
{
    struct sockaddr_in address;
    SOCKET listener, clients[4], accepted;
    fd_set writable;
    struct timeval tv = { 1, 0 };
    u_long nonblocking = 1;
    int i, connected = 0, acceptedCount = 0, error;

    listener = MakeListener(&address, 1);
    ok(listener != INVALID_SOCKET, "listener: %d\n", WSAGetLastError());
    if (listener == INVALID_SOCKET)
        return;
    FD_ZERO(&writable);
    for (i = 0; i < 4; i++)
    {
        clients[i] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        ioctlsocket(clients[i], FIONBIO, &nonblocking);
        error = connect(clients[i], (struct sockaddr *)&address, sizeof(address));
        ok(error == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK, "nonblocking connect %d: %d/%d\n", i, error, WSAGetLastError());
        FD_SET(clients[i], &writable);
    }
    Sleep(500);
    connected = select(0, NULL, &writable, NULL, &tv);
    trace("%d of 4 connects completed against backlog 1\n", connected);
    ok(connected >= 1, "no connect completed against a listening socket\n");
    nonblocking = 1;
    ioctlsocket(listener, FIONBIO, &nonblocking);
    for (i = 0; i < 4; i++)
    {
        accepted = accept(listener, NULL, NULL);
        if (accepted == INVALID_SOCKET)
            break;
        acceptedCount++;
        closesocket(accepted);
    }
    trace("%d connections accepted\n", acceptedCount);
    ok(acceptedCount >= 1, "accept found nothing after completed connects\n");
    ok(acceptedCount <= connected, "accepted %d but only %d connects completed\n", acceptedCount, connected);
    for (i = 0; i < 4; i++)
        closesocket(clients[i]);
    closesocket(listener);
}

START_TEST(accept_backlog)
{
    WSADATA data;

    if (WSAStartup(MAKEWORD(2, 2), &data))
    {
        skip("WSAStartup failed\n");
        return;
    }
    test_data_before_accept();
    test_refused();
    test_backlog();
    WSACleanup();
}
