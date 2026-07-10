#pragma once
/*
 * POSIX unistd.h shim for Windows simulator builds.
 * close() is already remapped to closesocket() via sys/socket.h.
 * Other unistd APIs used in this project can be added here as needed.
 */
