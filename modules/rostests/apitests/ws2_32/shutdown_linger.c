/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Half close, shutdown error codes, graceful and abortive close as seen by the peer
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "ws2_32.h"

static BOOL MakePair(SOCKET *Client, SOCKET *Server)
{
    struct sockaddr_in address = { 0 };
    int length = sizeof(address);
    SOCKET listener;
    DWORD timeout = 3000;

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
    setsockopt(*Client, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    setsockopt(*Server, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    return TRUE;
}

static void test_half_close(void)
{
    SOCKET client, server;
    char buffer[8];
    int result;

    if (!MakePair(&client, &server))
    {
        skip("no tcp pair: %d\n", WSAGetLastError());
        return;
    }
    ok(shutdown(client, SD_SEND) == 0, "shutdown SD_SEND: %d\n", WSAGetLastError());
    result = recv(server, buffer, sizeof(buffer), 0);
    ok(result == 0, "peer recv after SD_SEND returned %d (%d), expected 0\n", result, WSAGetLastError());
    ok(send(server, "back", 4, 0) == 4, "server send after client half close: %d\n", WSAGetLastError());
    result = recv(client, buffer, sizeof(buffer), 0);
    ok(result == 4, "client recv after its own SD_SEND returned %d (%d), expected 4\n", result, WSAGetLastError());
    result = send(client, "x", 1, 0);
    ok(result == SOCKET_ERROR && WSAGetLastError() == WSAESHUTDOWN, "send after SD_SEND: %d/%d, expected WSAESHUTDOWN\n", result, WSAGetLastError());
    ok(shutdown(client, SD_RECEIVE) == 0, "shutdown SD_RECEIVE: %d\n", WSAGetLastError());
    result = recv(client, buffer, sizeof(buffer), 0);
    ok(result == SOCKET_ERROR && WSAGetLastError() == WSAESHUTDOWN, "recv after SD_RECEIVE: %d/%d, expected WSAESHUTDOWN\n", result, WSAGetLastError());
    closesocket(client);
    closesocket(server);
}

static void test_graceful_close(void)
{
    SOCKET client, server;
    char buffer[8];
    int result;

    if (!MakePair(&client, &server))
    {
        skip("no tcp pair: %d\n", WSAGetLastError());
        return;
    }
    ok(send(client, "bye", 3, 0) == 3, "send: %d\n", WSAGetLastError());
    closesocket(client);
    result = recv(server, buffer, sizeof(buffer), 0);
    ok(result == 3, "recv of data sent before close returned %d (%d)\n", result, WSAGetLastError());
    result = recv(server, buffer, sizeof(buffer), 0);
    ok(result == 0, "recv after graceful close returned %d (%d), expected 0\n", result, WSAGetLastError());
    closesocket(server);
}

static void test_abortive_close(void)
{
    SOCKET client, server;
    struct linger hard = { 1, 0 };
    char buffer[8];
    int result;

    if (!MakePair(&client, &server))
    {
        skip("no tcp pair: %d\n", WSAGetLastError());
        return;
    }
    ok(setsockopt(client, SOL_SOCKET, SO_LINGER, (const char *)&hard, sizeof(hard)) == 0, "SO_LINGER: %d\n", WSAGetLastError());
    closesocket(client);
    result = recv(server, buffer, sizeof(buffer), 0);
    ok(result == SOCKET_ERROR, "recv after abortive close returned %d, expected an error\n", result);
    ok(WSAGetLastError() == WSAECONNRESET, "recv after abortive close: %d, expected WSAECONNRESET\n", WSAGetLastError());
    closesocket(server);
}

START_TEST(shutdown_linger)
{
    WSADATA data;

    if (WSAStartup(MAKEWORD(2, 2), &data))
    {
        skip("WSAStartup failed\n");
        return;
    }
    test_half_close();
    test_graceful_close();
    test_abortive_close();
    WSACleanup();
}
