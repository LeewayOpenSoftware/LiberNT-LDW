/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Out-of-band data on TCP (separate and inline) and its rejection on UDP
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

static void test_tcp_oob(void)
{
    SOCKET client, server;
    fd_set except;
    struct timeval tv = { 2, 0 };
    char buffer[8];
    u_long atmark = 2;
    int result;

    if (!MakePair(&client, &server))
    {
        skip("no tcp pair: %d\n", WSAGetLastError());
        return;
    }
    result = send(client, "U", 1, MSG_OOB);
    ok(result == 1, "send MSG_OOB returned %d (%d), expected 1\n", result, WSAGetLastError());
    FD_ZERO(&except);
    FD_SET(server, &except);
    result = select(0, NULL, NULL, &except, &tv);
    ok(result == 1, "select exceptfds returned %d (%d) after an OOB byte, expected 1\n", result, WSAGetLastError());
    ok(ioctlsocket(server, SIOCATMARK, &atmark) == 0, "SIOCATMARK: %d\n", WSAGetLastError());
    trace("SIOCATMARK before reading: %lu\n", atmark);
    result = recv(server, buffer, 1, MSG_OOB);
    ok(result == 1, "recv MSG_OOB returned %d (%d), expected 1\n", result, WSAGetLastError());
    ok(result == 1 && buffer[0] == 'U', "OOB byte was %c\n", result == 1 ? buffer[0] : '?');
    ok(send(client, "abc", 3, 0) == 3, "normal send after OOB: %d\n", WSAGetLastError());
    result = recv(server, buffer, 3, 0);
    ok(result == 3 && !memcmp(buffer, "abc", 3), "normal data after OOB returned %d (%d)\n", result, WSAGetLastError());
    closesocket(client);
    closesocket(server);
}

static void test_tcp_oob_inline(void)
{
    SOCKET client, server;
    BOOL inlineOob = TRUE;
    char buffer[8];
    int result;

    if (!MakePair(&client, &server))
    {
        skip("no tcp pair: %d\n", WSAGetLastError());
        return;
    }
    ok(setsockopt(server, SOL_SOCKET, SO_OOBINLINE, (const char *)&inlineOob, sizeof(inlineOob)) == 0, "SO_OOBINLINE: %d\n", WSAGetLastError());
    ok(send(client, "V", 1, MSG_OOB) == 1, "send MSG_OOB: %d\n", WSAGetLastError());
    result = recv(server, buffer, sizeof(buffer), 0);
    ok(result == 1 && buffer[0] == 'V', "inline recv of an OOB byte returned %d (%d)\n", result, WSAGetLastError());
    ok(send(client, "W", 1, MSG_OOB) == 1, "second send MSG_OOB: %d\n", WSAGetLastError());
    result = recv(server, buffer, 1, MSG_OOB);
    ok(result == SOCKET_ERROR && WSAGetLastError() == WSAEINVAL, "recv MSG_OOB with SO_OOBINLINE: %d/%d, expected WSAEINVAL\n", result, WSAGetLastError());
    closesocket(client);
    closesocket(server);
}

static void test_udp_oob(void)
{
    struct sockaddr_in address = { 0 };
    SOCKET udp;
    int result;

    udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(9);
    result = sendto(udp, "x", 1, MSG_OOB, (struct sockaddr *)&address, sizeof(address));
    ok(result == SOCKET_ERROR && WSAGetLastError() == WSAEOPNOTSUPP, "sendto MSG_OOB on UDP: %d/%d, expected WSAEOPNOTSUPP\n", result, WSAGetLastError());
    closesocket(udp);
}

START_TEST(oob)
{
    WSADATA data;

    if (WSAStartup(MAKEWORD(2, 2), &data))
    {
        skip("WSAStartup failed\n");
        return;
    }
    test_tcp_oob();
    test_tcp_oob_inline();
    test_udp_oob();
    WSACleanup();
}
