#pragma once
/*
 * POSIX netdb.h shim for Windows simulator builds.
 * ws2tcpip.h provides getaddrinfo / freeaddrinfo / struct addrinfo;
 * sys/socket.h already pulls it in.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
