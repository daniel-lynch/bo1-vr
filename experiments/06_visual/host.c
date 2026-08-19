/* host.c -- minimal 32-bit EXE that loads visual.dll and calls into it, so every
 * OpenVR and D3D9 call happens inside a DLL exactly as it will in the real ASI
 * plugin. Same shape as experiments/05_submit/host.c. */
#include <windows.h>
#include <stdio.h>

int main(void)
{
    HMODULE m;
    int (*run)(void);
    int rc;

    fprintf(stderr, "host: pid=%lu pointer size=%d\n",
            GetCurrentProcessId(), (int)sizeof(void *));
    fflush(stderr);

    m = LoadLibraryA("visual.dll");
    if (!m) { fprintf(stderr, "host: LoadLibraryA(visual.dll) failed err=%lu\n", GetLastError()); return 1; }
    run = (int (*)(void))GetProcAddress(m, "visual_run");
    if (!run) { fprintf(stderr, "host: GetProcAddress failed err=%lu\n", GetLastError()); return 2; }
    rc = run();
    fprintf(stderr, "host: visual_run returned %d\n", rc);
    fflush(stderr);
    return rc;
}
