#pragma once
/*
 * FreeRTOS semaphore stub for the host simulator.
 * Only plain mutexes are provided — enough for ssh_client's session lock.
 */
#include "FreeRTOS.h"
#include <stdlib.h>

typedef struct idfsim_mutex {
    CRITICAL_SECTION cs;
} *SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    SemaphoreHandle_t m = (SemaphoreHandle_t)malloc(sizeof(*m));
    if (m) InitializeCriticalSection(&m->cs);
    return m;
}

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t timeout)
{
    (void)timeout;                      /* host lock is uncontended-fast */
    EnterCriticalSection(&m->cs);
    return pdPASS;
}

static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t m)
{
    LeaveCriticalSection(&m->cs);
    return pdPASS;
}

static inline void vSemaphoreDelete(SemaphoreHandle_t m)
{
    if (m) { DeleteCriticalSection(&m->cs); free(m); }
}
