#pragma once
/*
 * POSIX unistd.h shim for Windows simulator builds.
 * close() is already remapped to closesocket() via sys/socket.h.
 * Add other unistd APIs this project needs here.
 */
