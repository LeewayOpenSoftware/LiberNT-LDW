/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     select argument checks and WSAEventSelect level and edge behaviour for FD_READ, FD_WRITE and FD_CLOSE
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "ws2_32.h"

static BOOL MakePair(SOCKET *Client, SOCKET *Server)
{
    struct sockaddr_in address = { 0 };
    int length = sizeof(address);
    SOCKET listener;

    *Client = *Server = INVALID_SOCKET;
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET)
        return FALSE;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) ||
        getsockname(listener, (struct sockaddr *)&address, &length) || listen(listener, 1))
    {
        closesocket(listener);
        return FALSE;
    }
    *Client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (*Client != INVALID_SOCKET && !connect(*Client, (struct sockaddr *)&address, sizeof(address)))
        *Server = accept(listener, NULL, NULL);
    closesocket(listener);
    if (*Server == INVALID_SOCKET)
    {
        if (*Client != INVALID_SOCKET)
            closesocket(*Client);
        *Client = INVALID_SOCKET;
        return FALSE;
    }
    return TRUE;
}

static LONG WaitEvents(SOCKET Socket, WSAEVENT Event, DWORD TimeoutMs, WSANETWORKEVENTS *Events)
{
    memset(Events, 0, sizeof(*Events));
    if (WaitForSingleObject(Event, TimeoutMs) != WAIT_OBJECT_0)
        return 0;
    if (WSAEnumNetworkEvents(Socket, Event, Events))
        return -1;
    return Events->lNetworkEvents;
}

static void test_select_arguments(void)
{
    struct timeval tv = { 0, 1000 };
    int result;

    result = select(0, NULL, NULL, NULL, &tv);
    ok(result == SOCKET_ERROR && WSAGetLastError() == WSAEINVAL, "select with no sets: %d/%d, expected WSAEINVAL\n", result, WSAGetLastError());
}

static void test_event_select(void)
{
    SOCKET client, server;
    WSAEVENT event;
    WSANETWORKEVENTS events;
    char buffer[4];
    LONG mask;

    if (!MakePair(&client, &server))
    {
        skip("no tcp pair: %d\n", WSAGetLastError());
        return;
    }
    event = WSACreateEvent();
    ok(WSAEventSelect(client, event, FD_READ | FD_WRITE | FD_CLOSE) == 0, "WSAEventSelect: %d\n", WSAGetLastError());
    mask = WaitEvents(client, event, 1000, &events);
    ok(mask & FD_WRITE, "FD_WRITE not signalled after WSAEventSelect on a connected socket (mask 0x%lx)\n", mask);

    ok(send(server, "abcd", 4, 0) == 4, "send: %d\n", WSAGetLastError());
    mask = WaitEvents(client, event, 2000, &events);
    ok(mask & FD_READ, "FD_READ not signalled after the peer sent (mask 0x%lx)\n", mask);
    ok(events.iErrorCode[FD_READ_BIT] == 0, "FD_READ error %d\n", events.iErrorCode[FD_READ_BIT]);
    ok(recv(client, buffer, 2, 0) == 2, "partial recv: %d\n", WSAGetLastError());
    mask = WaitEvents(client, event, 1000, &events);
    ok(mask & FD_READ, "FD_READ not re-signalled while data remains after a partial recv (mask 0x%lx)\n", mask);
    ok(recv(client, buffer, 2, 0) == 2, "second recv: %d\n", WSAGetLastError());
    mask = WaitEvents(client, event, 300, &events);
    ok((mask & FD_READ) == 0, "FD_READ signalled with no data left (mask 0x%lx)\n", mask);

    closesocket(server);
    mask = WaitEvents(client, event, 3000, &events);
    ok(mask & FD_CLOSE, "FD_CLOSE not signalled after the peer closed (mask 0x%lx)\n", mask);
    ok(events.iErrorCode[FD_CLOSE_BIT] == 0, "FD_CLOSE error %d, expected 0 for a graceful close\n", events.iErrorCode[FD_CLOSE_BIT]);
    ok(recv(client, buffer, sizeof(buffer), 0) == 0, "recv after FD_CLOSE: %d\n", WSAGetLastError());
    WSACloseEvent(event);
    closesocket(client);
}

static void test_fd_write_rearm(void)
{
    SOCKET client, server;
    WSAEVENT event;
    WSANETWORKEVENTS events;
    char *chunk;
    int sent, total = 0, result, drained = 0, rounds;
    LONG mask;
    u_long nonblocking = 1;

    if (!MakePair(&client, &server))
    {
        skip("no tcp pair: %d\n", WSAGetLastError());
        return;
    }
    chunk = HeapAlloc(GetProcessHeap(), 0, 65536);
    if (chunk == NULL)
    {
        skip("no 64 KiB buffer\n");
        closesocket(client);
        closesocket(server);
        return;
    }
    memset(chunk, 'w', 65536);
    event = WSACreateEvent();
    ok(WSAEventSelect(client, event, FD_WRITE) == 0, "WSAEventSelect: %d\n", WSAGetLastError());
    WaitEvents(client, event, 1000, &events);
    for (rounds = 0; rounds < 256; rounds++)
    {
        sent = send(client, chunk, 65536, 0);
        if (sent == SOCKET_ERROR)
            break;
        total += sent;
    }
    ok(sent == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK, "send never blocked after %d bytes (%d/%d)\n", total, sent, WSAGetLastError());
    trace("send buffer absorbed %d bytes before WSAEWOULDBLOCK\n", total);
    mask = WaitEvents(client, event, 200, &events);
    ok((mask & FD_WRITE) == 0, "FD_WRITE signalled while the send buffer is full (mask 0x%lx)\n", mask);

    ioctlsocket(server, FIONBIO, &nonblocking);
    for (rounds = 0; rounds < 4096 && drained < total; rounds++)
    {
        result = recv(server, chunk, 65536, 0);
        if (result > 0)
            drained += result;
        else if (result == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
            Sleep(10);
        else
            break;
    }
    ok(drained == total, "drained %d of %d bytes\n", drained, total);
    mask = WaitEvents(client, event, 3000, &events);
    ok(mask & FD_WRITE, "FD_WRITE not re-armed after the peer drained the buffer (mask 0x%lx)\n", mask);
    HeapFree(GetProcessHeap(), 0, chunk);
    WSACloseEvent(event);
    closesocket(client);
    closesocket(server);
}

START_TEST(select_edge)
{
    WSADATA data;

    if (WSAStartup(MAKEWORD(2, 2), &data))
    {
        skip("WSAStartup failed\n");
        return;
    }
    test_select_arguments();
    test_event_select();
    test_fd_write_rearm();
    WSACleanup();
}
