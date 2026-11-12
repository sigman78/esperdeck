/*
 * test_ssh_txbuf.c -- Unity tests for the SSH TX FIFO.
 *
 * Covers: append bounds, partial consume, EAGAIN keeping the head bytes,
 * fatal-error drop, and write order across several appenders.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <string.h>
#include "unity.h"
#include "ssh_txbuf.h"

static ssh_txbuf_t t;

/* Fake link: accepts up to `cap` bytes per call, or blocks/fails on demand. */
static struct {
    uint8_t out[4096];
    size_t  out_len;
    size_t  cap;          /* max bytes per call */
    int     block;        /* calls to refuse with AGAIN */
    int     fail;         /* return -5 */
    int     calls;
    const uint8_t *last_head;
} lnk;

static ptrdiff_t fake_write(void *ctx, const uint8_t *d, size_t n)
{
    (void)ctx;
    lnk.calls++;
    lnk.last_head = d;
    if (lnk.fail) return -5;
    if (lnk.block > 0) { lnk.block--; return SSH_TXBUF_AGAIN; }
    size_t k = n < lnk.cap ? n : lnk.cap;
    memcpy(lnk.out + lnk.out_len, d, k);
    lnk.out_len += k;
    return (ptrdiff_t)k;
}

void setUp(void)
{
    ssh_txbuf_reset(&t);
    memset(&lnk, 0, sizeof(lnk));
    lnk.cap = SSH_TXBUF_SIZE;
}
void tearDown(void) {}

void test_append_then_flush_all(void)
{
    TEST_ASSERT_EQUAL_size_t(3, ssh_txbuf_append(&t, (const uint8_t *)"abc", 3));
    TEST_ASSERT_EQUAL_INT(0, ssh_txbuf_flush(&t, fake_write, NULL));
    TEST_ASSERT_EQUAL_size_t(0, ssh_txbuf_pending(&t));
    TEST_ASSERT_EQUAL_size_t(3, lnk.out_len);
    TEST_ASSERT_EQUAL_MEMORY("abc", lnk.out, 3);
}

void test_partial_consume_keeps_tail_in_order(void)
{
    ssh_txbuf_append(&t, (const uint8_t *)"abcdefg", 7);
    lnk.cap = 3;
    TEST_ASSERT_EQUAL_INT(0, ssh_txbuf_flush(&t, fake_write, NULL));
    TEST_ASSERT_EQUAL_size_t(0, ssh_txbuf_pending(&t));
    TEST_ASSERT_EQUAL_INT(3, lnk.calls);
    TEST_ASSERT_EQUAL_MEMORY("abcdefg", lnk.out, 7);
}

void test_eagain_leaves_bytes_and_resumes(void)
{
    ssh_txbuf_append(&t, (const uint8_t *)"xyz", 3);
    lnk.block = 1;
    TEST_ASSERT_EQUAL_INT(0, ssh_txbuf_flush(&t, fake_write, NULL));
    TEST_ASSERT_EQUAL_size_t(3, ssh_txbuf_pending(&t));
    TEST_ASSERT_EQUAL_size_t(0, lnk.out_len);
    TEST_ASSERT_EQUAL_INT(0, ssh_txbuf_flush(&t, fake_write, NULL));
    TEST_ASSERT_EQUAL_size_t(0, ssh_txbuf_pending(&t));
    TEST_ASSERT_EQUAL_MEMORY("xyz", lnk.out, 3);
}

void test_append_while_blocked_goes_to_tail(void)
{
    ssh_txbuf_append(&t, (const uint8_t *)"key", 3);
    lnk.block = 1;
    ssh_txbuf_flush(&t, fake_write, NULL);
    ssh_txbuf_append(&t, (const uint8_t *)"reply", 5);
    ssh_txbuf_flush(&t, fake_write, NULL);
    TEST_ASSERT_EQUAL_size_t(8, lnk.out_len);
    TEST_ASSERT_EQUAL_MEMORY("keyreply", lnk.out, 8);
}

void test_head_pointer_is_stable_across_retries(void)
{
    const uint8_t *first;
    ssh_txbuf_append(&t, (const uint8_t *)"ab", 2);
    lnk.block = 1;
    ssh_txbuf_flush(&t, fake_write, NULL);
    first = lnk.last_head;
    ssh_txbuf_append(&t, (const uint8_t *)"cd", 2);
    lnk.block = 1;
    ssh_txbuf_flush(&t, fake_write, NULL);
    TEST_ASSERT_TRUE(first == lnk.last_head);
}

void test_overflow_takes_what_fits(void)
{
    uint8_t big[SSH_TXBUF_SIZE + 10];
    memset(big, 'q', sizeof(big));
    TEST_ASSERT_EQUAL_size_t(SSH_TXBUF_SIZE, ssh_txbuf_append(&t, big, sizeof(big)));
    TEST_ASSERT_EQUAL_size_t(0, ssh_txbuf_append(&t, big, 1));
    TEST_ASSERT_EQUAL_size_t(SSH_TXBUF_SIZE, ssh_txbuf_pending(&t));
}

void test_fatal_error_drops_buffer(void)
{
    ssh_txbuf_append(&t, (const uint8_t *)"abc", 3);
    lnk.fail = 1;
    TEST_ASSERT_EQUAL_INT(-1, ssh_txbuf_flush(&t, fake_write, NULL));
    TEST_ASSERT_EQUAL_size_t(0, ssh_txbuf_pending(&t));
}

void test_zero_return_is_treated_as_blocked(void)
{
    ssh_txbuf_append(&t, (const uint8_t *)"abc", 3);
    lnk.cap = 0;
    TEST_ASSERT_EQUAL_INT(0, ssh_txbuf_flush(&t, fake_write, NULL));
    TEST_ASSERT_EQUAL_size_t(3, ssh_txbuf_pending(&t));
    TEST_ASSERT_EQUAL_INT(1, lnk.calls);
}

void test_flush_empty_does_not_call(void)
{
    TEST_ASSERT_EQUAL_INT(0, ssh_txbuf_flush(&t, fake_write, NULL));
    TEST_ASSERT_EQUAL_INT(0, lnk.calls);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_append_then_flush_all);
    RUN_TEST(test_partial_consume_keeps_tail_in_order);
    RUN_TEST(test_eagain_leaves_bytes_and_resumes);
    RUN_TEST(test_append_while_blocked_goes_to_tail);
    RUN_TEST(test_head_pointer_is_stable_across_retries);
    RUN_TEST(test_overflow_takes_what_fits);
    RUN_TEST(test_fatal_error_drops_buffer);
    RUN_TEST(test_zero_return_is_treated_as_blocked);
    RUN_TEST(test_flush_empty_does_not_call);
    return UNITY_END();
}
