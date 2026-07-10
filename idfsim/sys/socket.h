#pragma once
/*
 * POSIX sys/socket.h shim for Windows simulator builds.
 *
 * ssh_client.c uses socket/connect/close with POSIX int-socket semantics.
 * We wrap the Winsock2 calls so that:
 *   - socket()  returns int  (-1 on failure, handle otherwise)
 *   - connect() matches the POSIX signature
 *   - close(s)  maps to closesocket()
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

/* Provide POSIX socklen_t if not already defined */
#ifndef _SOCKLEN_T
#define _SOCKLEN_T
typedef int socklen_t;
#endif

/* Wrap socket() to return int; normalize INVALID_SOCKET → -1 */
static inline int _sim_socket(int af, int type, int proto)
{
    SOCKET s = socket(af, type, proto);
    return (s == INVALID_SOCKET) ? -1 : (int)s;
}

/* Wrap connect() to take int socket */
static inline int _sim_connect(int s, const struct sockaddr *addr, socklen_t len)
{
    return connect((SOCKET)s, addr, len);
}

static inline int _sim_setsockopt(int s, int level, int oname, const void* oval, socklen_t olen)
{
    return setsockopt((SOCKET)s, level, oname, (const char*)oval, (int)olen);
}

#define socket(af, type, proto)       _sim_socket(af, type, proto)
#define connect(s, addr, len)         _sim_connect(s, addr, (int)(len))
#define close(s)                      closesocket((SOCKET)(s))
#define setsockopt(s, level, oname, oval, olen)	_sim_setsockopt(s, level, oname, oval, olen)
