/* host.c -- minimal 32-bit EXE that loads vrlive.dll and calls into it, so the
 * OpenVR calls happen inside a DLL exactly as they will in the real ASI plugin. */
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

    m = LoadLibraryA("vrlive.dll");
    if (!m) { fprintf(stderr, "host: LoadLibraryA(vrlive.dll) failed err=%lu\n", GetLastError()); return 1; }
    run = (int (*)(void))GetProcAddress(m, "vrlive_run");
    if (!run) { fprintf(stderr, "host: GetProcAddress failed err=%lu\n", GetLastError()); return 2; }
    rc = run();
    fprintf(stderr, "host: vrlive_run returned %d\n", rc);
    fflush(stderr);
    return rc;
}
