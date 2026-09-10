/* Format locally, then submit one completed line through core's logger. */
#include "osmw_login.h"

#include <stdarg.h>
#include <stdio.h>

void lg_log_open(const char *path) { (void)path; }
void lg_log_close(void) { }
void lg_log_prune(const char *dir, int keep) { (void)dir; (void)keep; }

void lg_log(const char *format, ...)
{
    const OSMW_CORE_API_V1 *api = lg_core_api();
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
    api->log(lg_core_owner(), OSMW_LOG_INFO, message);
}
