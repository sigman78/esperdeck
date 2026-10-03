#pragma once
#include "FreeRTOS.h"
#include <stdlib.h>

typedef struct {
    void (*fn)(void *);
    void *arg;
} idfsim_task_start_t;

static DWORD WINAPI idfsim_task_entry(LPVOID opaque)
{
    idfsim_task_start_t start = *(idfsim_task_start_t *)opaque;
    free(opaque);  /* The task may delete itself instead of returning. */
    start.fn(start.arg);
    return 0;
}

static inline HANDLE idfsim_task_create(void (*fn)(void *), void *arg)
{
    if (!fn) return NULL;
    idfsim_task_start_t *start = malloc(sizeof(*start));
    if (!start) return NULL;
    start->fn = fn;
    start->arg = arg;
    HANDLE h = CreateThread(NULL, 0, idfsim_task_entry, start, 0, NULL);
    if (!h) free(start);
    return h;
}

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
    if (out) *out = NULL;
    HANDLE h = idfsim_task_create(fn, arg);
    if (!h) return pdFAIL;
    if (out) *out = h;
    else CloseHandle(h);
    return pdPASS;
}

/* Static variant — the host has no separate TCB/stack, so this ignores
 * the caller's buffers and spawns a plain thread instead. It returns the
 * handle (NULL on failure), matching the real xTaskCreateStatic* signature. */
static inline TaskHandle_t xTaskCreateStaticPinnedToCore(
    void (*fn)(void *), const char *name, uint32_t stack,
    void *arg, unsigned prio, StackType_t *stackbuf, StaticTask_t *tcb, int core)
{
    (void)name; (void)stack; (void)prio; (void)stackbuf; (void)tcb; (void)core;
    return idfsim_task_create(fn, arg);
}
