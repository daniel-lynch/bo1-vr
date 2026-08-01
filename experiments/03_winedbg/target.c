/* target.c -- the EXE half of the winedbg target.
 *
 * Calls into the DLL through several frames so a backtrace has something to
 * show, then optionally faults. Pass "crash" on the command line to make it
 * fault; with no argument it exits cleanly.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef int (*fn_middle)(int);
typedef void (*fn_crash)(void);

static int app_level_two(fn_middle f, int v)
{
    int local_two = v * 2;    /* inspect `local_two` from the debugger */
    return f(local_two);
}

static int app_level_one(fn_middle f)
{
    int local_one = 21;
    return app_level_two(f, local_one);
}

int main(int argc, char **argv)
{
    HMODULE m;
    fn_middle mid;
    fn_crash  crash;
    int r;

    fprintf(stderr, "target: pid=%lu ptr=%d IsDebuggerPresent=%d\n",
            GetCurrentProcessId(), (int)sizeof(void *), IsDebuggerPresent() ? 1 : 0);
    fflush(stderr);

    m = LoadLibraryA("tgtlib.dll");
    if (!m) { fprintf(stderr, "target: LoadLibrary failed %lu\n", GetLastError()); return 1; }

    mid   = (fn_middle)GetProcAddress(m, "tgtlib_middle");
    crash = (fn_crash) GetProcAddress(m, "tgtlib_crash_now");

    r = app_level_one(mid);
    fprintf(stderr, "target: app_level_one returned %d\n", r);
    fflush(stderr);

    if (argc > 1 && strcmp(argv[1], "crash") == 0) {
        fprintf(stderr, "target: about to fault deliberately\n");
        fflush(stderr);
        crash();
    }

    fprintf(stderr, "target: exiting cleanly\n");
    fflush(stderr);
    return 0;
}
