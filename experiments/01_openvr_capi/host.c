/* host.c -- minimal 32-bit EXE that loads vrprobe.dll and calls into it.
 * The point is that the OpenVR calls happen inside a DLL, exactly as they will
 * in the real ASI plugin, rather than in an EXE's main(). */
#include <windows.h>
#include <stdio.h>

int main(void)
{
    HMODULE m;
    int (*run)(void);

    fprintf(stderr, "host: pid=%lu, pointer size=%d\n",
            GetCurrentProcessId(), (int)sizeof(void *));
    fflush(stderr);

    m = LoadLibraryA("vrprobe.dll");
    if (!m) {
        fprintf(stderr, "host: LoadLibraryA(vrprobe.dll) failed, err=%lu\n", GetLastError());
        return 1;
    }
    run = (int (*)(void))GetProcAddress(m, "vrprobe_run");
    if (!run) {
        fprintf(stderr, "host: GetProcAddress(vrprobe_run) failed, err=%lu\n", GetLastError());
        return 2;
    }
    return run();
}
