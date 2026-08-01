/* host.c -- minimal 32-bit EXE that loads vrsubmit.dll and calls into it, so
 * every OpenVR and D3D9 call happens inside a DLL exactly as it will in the
 * real ASI plugin. Same shape as experiments/04_live_fntable/host.c. */
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

    m = LoadLibraryA("vrsubmit.dll");
    if (!m) { fprintf(stderr, "host: LoadLibraryA(vrsubmit.dll) failed err=%lu\n", GetLastError()); return 1; }
    run = (int (*)(void))GetProcAddress(m, "vrsubmit_run");
    if (!run) { fprintf(stderr, "host: GetProcAddress failed err=%lu\n", GetLastError()); return 2; }
    rc = run();
    fprintf(stderr, "host: vrsubmit_run returned %d\n", rc);
    fflush(stderr);
    return rc;
}
