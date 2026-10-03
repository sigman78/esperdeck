#include "esp_check.h"
#include <stdlib.h>
#include <string.h>

static int calls;

static esp_err_t checked_result(int fail)
{
    calls++;
    fprintf(stderr, "evaluated\n");
    return fail ? ESP_ERR_INVALID_STATE : ESP_OK;
}

int main(int argc, char **argv)
{
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    int fail = argc > 1 && strcmp(argv[1], "fail") == 0;
    ESP_ERROR_CHECK(checked_result(fail));
    fprintf(stderr, "continued\n");
    return calls == 1 ? 0 : 1;
}
