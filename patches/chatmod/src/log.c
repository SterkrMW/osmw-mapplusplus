/* Chat-local formatting adapter over the core-owned session log. */
#include "osmw_chat.h"

#include <stdarg.h>
#include <stdio.h>

void mw_log(const char *fmt, ...)
{
    const OSMW_CORE_API_V1 *api = mw_core_api();
    char message[1024];
    va_list args;

    if (api == NULL || api->log == NULL || fmt == NULL)
        return;
    va_start(args, fmt);
    _vsnprintf(message, sizeof(message) - 1, fmt, args);
    va_end(args);
    message[sizeof(message) - 1] = '\0';
    api->log(mw_core_owner(), OSMW_LOG_INFO, message);
}
