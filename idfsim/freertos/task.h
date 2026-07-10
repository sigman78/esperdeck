#pragma once
#include "FreeRTOS.h"

static inline void vTaskDelay(TickType_t ticks) { Sleep((DWORD)ticks); }
static inline TickType_t xTaskGetTickCount(void) { return (TickType_t)GetTickCount(); }

static inline void vTaskDelete(TaskHandle_t h) {
    if (h == NULL) ExitThread(0);
    else { TerminateThread(h, 0); CloseHandle(h); }
}

static inline BaseType_t xTaskCreatePinnedToCore(
    void (*fn)(void *), const char *name, uint32_t stack,
    void *arg, unsigned prio, TaskHandle_t *out, int core)
{
    (void)name; (void)stack; (void)prio; (void)core;
    HANDLE h = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)fn, arg, 0, NULL);
    if (!h) return pdFAIL;
    if (out) *out = h;
    return pdPASS;
}

/* Static variant — the host has no separate TCB/stack, so the caller's
 * buffers are ignored and a plain thread is spawned. Returns the handle
 * (NULL on failure), matching the real xTaskCreateStatic* signature. */
static inline TaskHandle_t xTaskCreateStaticPinnedToCore(
    void (*fn)(void *), const char *name, uint32_t stack,
    void *arg, unsigned prio, StackType_t *stackbuf, StaticTask_t *tcb, int core)
{
    (void)name; (void)stack; (void)prio; (void)stackbuf; (void)tcb; (void)core;
    return CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)fn, arg, 0, NULL);
}
