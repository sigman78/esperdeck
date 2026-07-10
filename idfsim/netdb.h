#pragma once
/*
 * POSIX netdb.h shim for Windows simulator builds.
 * getaddrinfo / freeaddrinfo / struct addrinfo are provided by ws2tcpip.h
 * which is already pulled in via sys/socket.h.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
