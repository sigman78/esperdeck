#include "ssh_txbuf.h"
#include <string.h>

void ssh_txbuf_reset(ssh_txbuf_t *t)
{
    t->len = 0;
}

size_t ssh_txbuf_pending(const ssh_txbuf_t *t)
{
    return t->len;
}

size_t ssh_txbuf_append(ssh_txbuf_t *t, const uint8_t *data, size_t len)
{
    size_t room = SSH_TXBUF_SIZE - t->len;
    if (len > room) len = room;
    if (len) {
        memcpy(t->buf + t->len, data, len);
        t->len += len;
    }
    return len;
}

int ssh_txbuf_flush(ssh_txbuf_t *t, ssh_txbuf_write_fn fn, void *ctx)
{
    while (t->len > 0) {
        ptrdiff_t rc = fn(ctx, t->buf, t->len);
        if (rc == SSH_TXBUF_AGAIN || rc == 0) return 0;
        if (rc < 0) { t->len = 0; return -1; }
        if ((size_t)rc >= t->len) { t->len = 0; break; }
        memmove(t->buf, t->buf + rc, t->len - (size_t)rc);
        t->len -= (size_t)rc;
    }
    return 0;
}
