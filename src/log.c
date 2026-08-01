#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <windows.h>

void bo1vr_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);

    /* Also emit to the debugger channel. Under Proton this shows up with
     * WINEDEBUG=+debugstr, which is useful when stderr is being swallowed by a
     * launcher script sitting between us and Steam. */
    {
        char buf[1024];
        va_list ap2;
        va_start(ap2, fmt);
        _vsnprintf(buf, sizeof(buf) - 2, fmt, ap2);
        va_end(ap2);
        buf[sizeof(buf) - 2] = '\0';
        strcat(buf, "\n");
        OutputDebugStringA(buf);
    }
}
