/*
 * ssh_txbuf.h - internal: ordered TX FIFO ahead of libssh2_channel_write().
 * Append at the tail; consume from the head only by the reported count.
 * Not thread-safe; the owner serializes access.
 */
#ifndef SSH_TXBUF_H
#define SSH_TXBUF_H

#include <stddef.h>
#include <stdint.h>

#define SSH_TXBUF_SIZE   1024
#define SSH_TXBUF_AGAIN  (-0x7fff)   /* write fn: would block, bytes untouched */

typedef struct {
    uint8_t buf[SSH_TXBUF_SIZE];
    size_t  len;
} ssh_txbuf_t;

/* Returns bytes accepted (>0), SSH_TXBUF_AGAIN, or another negative = fatal. */
typedef ptrdiff_t (*ssh_txbuf_write_fn)(void *ctx, const uint8_t *data, size_t len);

void   ssh_txbuf_reset(ssh_txbuf_t *t);
size_t ssh_txbuf_pending(const ssh_txbuf_t *t);

/* Appends what fits; returns the byte count taken. */
size_t ssh_txbuf_append(ssh_txbuf_t *t, const uint8_t *data, size_t len);

/* Writes from the head until empty or blocked. Returns 0, or -1 when the
 * write fn failed (buffer dropped). Bytes left after 0 mean "blocked". */
int    ssh_txbuf_flush(ssh_txbuf_t *t, ssh_txbuf_write_fn fn, void *ctx);

#endif
