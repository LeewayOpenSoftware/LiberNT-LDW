/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Cancellation of pending socket I/O by CancelIo, by closesocket, and from another thread
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
    *Client = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
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

static void test_cancel_io(void)
{
    SOCKET client, server;
    WSAOVERLAPPED overlapped = { 0 };
    WSABUF buffer;
    char storage[16];
    DWORD received = 0, flags = 0, error;
    int result;

    if (!MakePair(&client, &server))
    {
        skip("no tcp pair: %d\n", WSAGetLastError());
        return;
    }
    overlapped.hEvent = WSACreateEvent();
    buffer.buf = storage;
    buffer.len = sizeof(storage);
    result = WSARecv(client, &buffer, 1, &received, &flags, &overlapped, NULL);
    ok(result == SOCKET_ERROR && WSAGetLastError() == WSA_IO_PENDING, "WSARecv: %d/%d, expected WSA_IO_PENDING\n", result, WSAGetLastError());
    ok(CancelIo((HANDLE)client), "CancelIo: %lu\n", GetLastError());
    ok(WaitForSingleObject(overlapped.hEvent, 3000) == WAIT_OBJECT_0, "cancelled receive did not complete within 3 s\n");
    ok(!WSAGetOverlappedResult(client, &overlapped, &received, FALSE, &flags), "cancelled receive reported success\n");
    error = WSAGetLastError();
    ok(error == WSA_OPERATION_ABORTED, "cancelled receive: %lu, expected WSA_OPERATION_ABORTED\n", error);
    WSACloseEvent(overlapped.hEvent);
    closesocket(client);
    closesocket(server);
}

static void test_close_cancels(void)
{
    SOCKET client, server;
    WSAOVERLAPPED overlapped = { 0 };
    WSABUF buffer;
    char storage[16];
    DWORD received = 0, flags = 0, wait;
    int result;

    if (!MakePair(&client, &server))
    {
        skip("no tcp pair: %d\n", WSAGetLastError());
        return;
    }
    overlapped.hEvent = WSACreateEvent();
    buffer.buf = storage;
    buffer.len = sizeof(storage);
    result = WSARecv(client, &buffer, 1, &received, &flags, &overlapped, NULL);
    ok(result == SOCKET_ERROR && WSAGetLastError() == WSA_IO_PENDING, "WSARecv: %d/%d, expected WSA_IO_PENDING\n", result, WSAGetLastError());
    ok(closesocket(client) == 0, "closesocket with a pending receive: %d\n", WSAGetLastError());
    wait = WaitForSingleObject(overlapped.hEvent, 3000);
    ok(wait == WAIT_OBJECT_0, "pending receive did not complete within 3 s after closesocket\n");
    ok(overlapped.Internal != (ULONG_PTR)STATUS_PENDING, "overlapped still pending after closesocket\n");
    trace("completion status after closesocket: 0x%Ix\n", overlapped.Internal);
    ok(overlapped.Internal == (ULONG_PTR)STATUS_CANCELLED, "completion status 0x%Ix, expected STATUS_CANCELLED\n", overlapped.Internal);
    WSACloseEvent(overlapped.hEvent);
    closesocket(server);
}

typedef struct _BLOCKING_RECV
{
    SOCKET Socket;
    int Result;
    int Error;
} BLOCKING_RECV;

static DWORD WINAPI BlockingRecv(LPVOID Parameter)
{
    BLOCKING_RECV *ctx = Parameter;
    char storage[16];

    ctx->Result = recv(ctx->Socket, storage, sizeof(storage), 0);
    ctx->Error = WSAGetLastError();
    return 0;
}

static void test_close_other_thread(void)
{
    SOCKET client, server;
    BLOCKING_RECV ctx;
    HANDLE thread;
    DWORD wait;

    if (!MakePair(&client, &server))
    {
        skip("no tcp pair: %d\n", WSAGetLastError());
        return;
    }
    ctx.Socket = client;
    ctx.Result = 12345;
    ctx.Error = 0;
    thread = CreateThread(NULL, 0, BlockingRecv, &ctx, 0, NULL);
    Sleep(300);
    ok(closesocket(client) == 0, "closesocket while another thread receives: %d\n", WSAGetLastError());
    wait = WaitForSingleObject(thread, 3000);
    ok(wait == WAIT_OBJECT_0, "blocking recv did not return within 3 s after closesocket\n");
    if (wait == WAIT_OBJECT_0)
    {
        trace("blocking recv returned %d, error %d\n", ctx.Result, ctx.Error);
        ok(ctx.Result == SOCKET_ERROR, "blocking recv returned %d after closesocket\n", ctx.Result);
        ok(ctx.Error == WSAEINTR || ctx.Error == WSAENOTSOCK || ctx.Error == WSAECONNABORTED,
           "blocking recv error %d after closesocket\n", ctx.Error);
    }
    else
        TerminateThread(thread, 0);
    CloseHandle(thread);
    closesocket(server);
}

START_TEST(cancel)
{
    WSADATA data;

    if (WSAStartup(MAKEWORD(2, 2), &data))
    {
        skip("WSAStartup failed\n");
        return;
    }
    test_cancel_io();
    test_close_cancels();
    test_close_other_thread();
    WSACleanup();
}
