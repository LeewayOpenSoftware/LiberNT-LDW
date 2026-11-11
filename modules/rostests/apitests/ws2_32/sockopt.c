/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Transport socket options round trip through the Winsock helper for TCP, UDP, IPv4 and IPv6
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "ws2_32.h"
#include <mstcpip.h>

static void RoundTripInt(SOCKET Socket, int Level, int Option, const char *Name, int Value)
{
    int stored = -1, length = sizeof(stored), result;

    result = setsockopt(Socket, Level, Option, (const char *)&Value, sizeof(Value));
    ok(result == 0, "%s: setsockopt(%d) failed with %d\n", Name, Value, WSAGetLastError());
    result = getsockopt(Socket, Level, Option, (char *)&stored, &length);
    ok(result == 0, "%s: getsockopt failed with %d\n", Name, WSAGetLastError());
    ok(stored == Value, "%s: got %d after setting %d\n", Name, stored, Value);
}

static void RoundTripBool(SOCKET Socket, int Level, int Option, const char *Name)
{
    BOOL stored = 2;
    int length = sizeof(stored), result;
    BOOL value = TRUE;

    result = setsockopt(Socket, Level, Option, (const char *)&value, sizeof(value));
    ok(result == 0, "%s: setsockopt failed with %d\n", Name, WSAGetLastError());
    result = getsockopt(Socket, Level, Option, (char *)&stored, &length);
    ok(result == 0, "%s: getsockopt failed with %d\n", Name, WSAGetLastError());
    ok(stored != 0, "%s: reads back as 0 after setting it\n", Name);
}

static void test_tcp_options(void)
{
    SOCKET tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct tcp_keepalive keepalive = { 1, 10000, 1000 };
    DWORD returned = 0;
    int type = 0, length = sizeof(type);

    ok(tcp != INVALID_SOCKET, "socket: %d\n", WSAGetLastError());
    if (tcp == INVALID_SOCKET)
        return;
    ok(getsockopt(tcp, SOL_SOCKET, SO_TYPE, (char *)&type, &length) == 0 && type == SOCK_STREAM, "SO_TYPE %d (%d)\n", type, WSAGetLastError());
    RoundTripBool(tcp, IPPROTO_TCP, TCP_NODELAY, "TCP_NODELAY");
    RoundTripBool(tcp, SOL_SOCKET, SO_KEEPALIVE, "SO_KEEPALIVE");
    RoundTripBool(tcp, SOL_SOCKET, SO_REUSEADDR, "SO_REUSEADDR");
    RoundTripBool(tcp, SOL_SOCKET, SO_OOBINLINE, "SO_OOBINLINE");
    RoundTripInt(tcp, SOL_SOCKET, SO_RCVBUF, "SO_RCVBUF", 65536);
    RoundTripInt(tcp, SOL_SOCKET, SO_SNDBUF, "SO_SNDBUF", 32768);
    RoundTripInt(tcp, SOL_SOCKET, SO_RCVTIMEO, "SO_RCVTIMEO", 1234);
    RoundTripInt(tcp, IPPROTO_IP, IP_TTL, "IP_TTL", 77);
    RoundTripInt(tcp, IPPROTO_IP, IP_TOS, "IP_TOS", 0);
    ok(WSAIoctl(tcp, SIO_KEEPALIVE_VALS, &keepalive, sizeof(keepalive), NULL, 0, &returned, NULL, NULL) == 0,
       "SIO_KEEPALIVE_VALS: %d\n", WSAGetLastError());
    closesocket(tcp);
}

static void test_udp_options(void)
{
    SOCKET udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct in_addr iface, stored;
    int length = sizeof(stored);

    ok(udp != INVALID_SOCKET, "socket: %d\n", WSAGetLastError());
    if (udp == INVALID_SOCKET)
        return;
    RoundTripBool(udp, SOL_SOCKET, SO_BROADCAST, "SO_BROADCAST");
    RoundTripInt(udp, IPPROTO_IP, IP_TTL, "IP_TTL (udp)", 33);
    RoundTripInt(udp, IPPROTO_IP, IP_MULTICAST_TTL, "IP_MULTICAST_TTL", 3);
    RoundTripBool(udp, IPPROTO_IP, IP_DONTFRAGMENT, "IP_DONTFRAGMENT");
    RoundTripBool(udp, IPPROTO_IP, IP_PKTINFO, "IP_PKTINFO");
    RoundTripInt(udp, IPPROTO_IP, IP_MULTICAST_LOOP, "IP_MULTICAST_LOOP", 0);
    iface.s_addr = htonl(INADDR_LOOPBACK);
    ok(setsockopt(udp, IPPROTO_IP, IP_MULTICAST_IF, (const char *)&iface, sizeof(iface)) == 0, "IP_MULTICAST_IF set: %d\n", WSAGetLastError());
    stored.s_addr = 0;
    ok(getsockopt(udp, IPPROTO_IP, IP_MULTICAST_IF, (char *)&stored, &length) == 0, "IP_MULTICAST_IF get: %d\n", WSAGetLastError());
    ok(stored.s_addr == iface.s_addr, "IP_MULTICAST_IF reads back 0x%08lx, expected 0x%08lx\n", (unsigned long)ntohl(stored.s_addr), (unsigned long)ntohl(iface.s_addr));
    closesocket(udp);
}

static void test_ipv6_options(void)
{
    SOCKET tcp6 = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    SOCKET udp6 = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    DWORD v6only = 2;
    int length = sizeof(v6only);

    ok(tcp6 != INVALID_SOCKET && udp6 != INVALID_SOCKET, "ipv6 sockets: %d\n", WSAGetLastError());
    if (tcp6 == INVALID_SOCKET || udp6 == INVALID_SOCKET)
    {
        if (tcp6 != INVALID_SOCKET)
            closesocket(tcp6);
        if (udp6 != INVALID_SOCKET)
            closesocket(udp6);
        return;
    }
    ok(getsockopt(tcp6, IPPROTO_IPV6, IPV6_V6ONLY, (char *)&v6only, &length) == 0, "IPV6_V6ONLY get: %d\n", WSAGetLastError());
    ok(v6only == 1, "IPV6_V6ONLY default %lu, expected 1\n", v6only);
    RoundTripInt(tcp6, IPPROTO_IPV6, IPV6_V6ONLY, "IPV6_V6ONLY", 0);
    RoundTripInt(udp6, IPPROTO_IPV6, IPV6_UNICAST_HOPS, "IPV6_UNICAST_HOPS", 33);
    RoundTripInt(udp6, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, "IPV6_MULTICAST_HOPS", 4);
    RoundTripBool(udp6, IPPROTO_IPV6, IPV6_PKTINFO, "IPV6_PKTINFO");
    closesocket(tcp6);
    closesocket(udp6);
}

START_TEST(sockopt)
{
    WSADATA data;

    if (WSAStartup(MAKEWORD(2, 2), &data))
    {
        skip("WSAStartup failed\n");
        return;
    }
    test_tcp_options();
    test_udp_options();
    test_ipv6_options();
    WSACleanup();
}
