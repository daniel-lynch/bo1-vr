/* asi_loader.c
 *
 * An ".asi" is just a DLL with a different extension. Loading one means calling
 * LoadLibrary on it and letting its DllMain do the work; there is no ASI-specific
 * entry point or ABI. We scan the directory our own module lives in, not the
 * process working directory -- Steam does not reliably set cwd to the game
 * directory under Proton, and getting this wrong yields a loader that silently
 * finds nothing.
 */

#include "asi_loader.h"
#include "log.h"

#include <stdio.h>

int asi_load_all(HMODULE self)
{
    char dir[MAX_PATH];
    char pattern[MAX_PATH];
    char full[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    DWORD n;
    char *slash;
    int loaded = 0;

    n = GetModuleFileNameA(self, dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        LOGE("asi: GetModuleFileNameA failed, err=%lu", GetLastError());
        return 0;
    }

    slash = strrchr(dir, '\\');
    if (!slash) {
        LOGE("asi: no backslash in module path \"%s\"", dir);
        return 0;
    }
    *slash = '\0';

    _snprintf(pattern, MAX_PATH, "%s\\*.asi", dir);
    pattern[MAX_PATH - 1] = '\0';

    LOGI("asi: scanning %s", pattern);

    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        LOGI("asi: no .asi plugins found (err=%lu)", GetLastError());
        return 0;
    }

    do {
        HMODULE m;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;

        _snprintf(full, MAX_PATH, "%s\\%s", dir, fd.cFileName);
        full[MAX_PATH - 1] = '\0';

        /* SetLastError(0) first: LoadLibrary can succeed while leaving a stale
         * error, and we log the error only on failure. */
        SetLastError(0);
        m = LoadLibraryA(full);
        if (m) {
            LOGI("asi: loaded %s at %p", fd.cFileName, (void *)m);
            loaded++;
        } else {
            LOGE("asi: FAILED to load %s, err=%lu", fd.cFileName, GetLastError());
        }
    } while (FindNextFileA(h, &fd));

    FindClose(h);
    LOGI("asi: %d plugin(s) loaded", loaded);
    return loaded;
}
