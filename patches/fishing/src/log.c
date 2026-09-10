#include "osmw_fishing.h"

#include <stdarg.h>
#include <stdio.h>

void fg_log(const char *format, ...)
{
    const OSMW_CORE_API_V1 *api = fg_core_api();
    char message[768];
    va_list arguments;
    int written;

    if (api == NULL || api->log == NULL || format == NULL)
        return;
    va_start(arguments, format);
    written = _vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (written < 0 || written >= (int)sizeof(message))
        message[sizeof(message) - 1] = '\0';
    api->log(fg_core_owner(), OSMW_LOG_INFO, message);
}
