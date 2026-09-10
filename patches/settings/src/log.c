/* Format locally, then send one completed line through the core logger.
 *
 * Varargs never cross the DLL boundary.  Core adds the timestamp, level and
 * stable module id and owns the single opt-in per-process log file.
 */
#include "osmw_settings.h"

#include <stdarg.h>
#include <stdio.h>

void st_log(const char *format, ...)
{
    const OSMW_CORE_API_V1 *api = st_core_api();
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

    api->log(st_core_owner(), OSMW_LOG_INFO, message);
}
