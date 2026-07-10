/*
 * Input HAL — private interface shared by backends.
 * NOT installed in include/; only included from within the input component.
 */

#pragma once

#include "input_hal.h"
#include "esp_err.h"

/**
 * Post an event to the shared queue.  Safe from any task context (not ISR).
 * Logs a warning and drops the event if the queue is full.
 */
void input_hal_post_event(const input_event_t *ev);

/*
 * Backend initialisers — always defined (real implementation or no-op),
 * so the linker is satisfied regardless of which CONFIG values are active.
 */
esp_err_t ble_keyboard_backend_init(void);
esp_err_t input_uart_backend_init(void);
esp_err_t touch_input_backend_init(void);
