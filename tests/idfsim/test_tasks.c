#include "freertos/task.h"
#include <stdio.h>

static void increment(void *arg)
{
    InterlockedIncrement((volatile LONG *)arg);
}

static void increment_and_delete(void *arg)
{
    InterlockedIncrement((volatile LONG *)arg);
    vTaskDelete(NULL);
    InterlockedIncrement((volatile LONG *)arg);
}

static void signal_done(void *arg)
{
    SetEvent((HANDLE)arg);
}

static int join(TaskHandle_t task, volatile LONG *value)
{
    if (!task || WaitForSingleObject(task, 5000) != WAIT_OBJECT_0) return 1;
    DWORD status = 999;
    BOOL got_status = GetExitCodeThread(task, &status);
    CloseHandle(task);
    if (!got_status || status != 0 || *value != 1) {
        fprintf(stderr, "task return: status=%lu value=%ld\n", status, *value);
        return 1;
    }
    return 0;
}

int main(void)
{
    volatile LONG value = 0;
    TaskHandle_t task = NULL;
    if (xTaskCreatePinnedToCore(increment, "test", 1024, (void *)&value,
                                1, &task, 0) != pdPASS) return 1;
    if (join(task, &value)) return 1;
    value = 0;
    StackType_t stack[1024];
    StaticTask_t tcb;
    task = xTaskCreateStaticPinnedToCore(increment, "static", 1024,
                                       (void *)&value, 1, stack, &tcb, 0);
    if (join(task, &value)) return 1;
    value = 0;
    if (xTaskCreatePinnedToCore(increment_and_delete, "self-delete", 1024,
                                (void *)&value, 1, &task, 0) != pdPASS) return 1;
    if (join(task, &value)) return 1;

    HANDLE done = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!done) return 1;
    DWORD before, after;
    if (!GetProcessHandleCount(GetCurrentProcess(), &before)) return 1;
    for (int i = 0; i < 32; i++) {
        if (xTaskCreatePinnedToCore(signal_done, "detached", 1024, done,
                                    1, NULL, 0) != pdPASS) return 1;
        if (WaitForSingleObject(done, 5000) != WAIT_OBJECT_0) return 1;
    }
    if (!GetProcessHandleCount(GetCurrentProcess(), &after)) return 1;
    CloseHandle(done);
    if (after != before) {
        fprintf(stderr, "detached task handle leak: %lu -> %lu\n", before, after);
        return 1;
    }
    task = (HANDLE)1;
    if (xTaskCreatePinnedToCore(NULL, "invalid", 1024, NULL, 1, &task, 0)
            != pdFAIL || task != NULL) return 1;
    return 0;
}
