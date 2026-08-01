/* asihost.c -- stands in for BlackOps.exe.
 *
 * Loads the real dist/dinput8.dll ASI loader, which scans its own directory for
 * *.asi and LoadLibrary()s each one from DllMain. vrlive.asi spawns a worker
 * thread (it must not work under the loader lock) and signals "bo1vr_exp04_done"
 * when finished; we wait on that. */
#include <windows.h>
#include <stdio.h>

int main(void)
{
    HANDLE done;
    HMODULE m;
    DWORD w;

    done = CreateEventA(NULL, TRUE, FALSE, "bo1vr_exp04_done");
    fprintf(stderr, "asihost: loading ./dinput8.dll\n"); fflush(stderr);
    m = LoadLibraryA("./dinput8.dll");
    if (!m) { fprintf(stderr, "asihost: load failed %lu\n", GetLastError()); return 1; }
    fprintf(stderr, "asihost: dinput8.dll @ %p, waiting for the .asi worker\n", (void *)m);
    fflush(stderr);

    w = WaitForSingleObject(done, 90000);
    fprintf(stderr, "asihost: wait -> %lu (0 = signalled, 258 = timeout)\n", w);
    fflush(stderr);
    return w == WAIT_OBJECT_0 ? 0 : 2;
}
