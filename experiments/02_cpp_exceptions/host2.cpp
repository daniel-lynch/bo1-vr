/* host2.cpp -- EXE side of experiment 2.
 *
 * Each case is run in sequence and every outcome is logged before and after, so
 * that if the process dies we can tell from the log exactly which case killed
 * it. That matters: a crash produces no return value to inspect.
 */

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <stdexcept>

/* Log to BOTH stderr and a file. winedbg (native and --gdb) detaches the
 * inferior's stdio, so under a debugger stderr is not visible to the shell that
 * launched winedbg -- the file is the only way to read the result. */
static std::FILE *g_log;
static void logline(const char *fmt, ...)
{
    va_list ap;
    if (!g_log) {
        const char *p = std::getenv("EXP2_LOG");
        g_log = std::fopen(p ? p : "exp2.log", "a");
    }
    va_start(ap, fmt); std::vfprintf(stderr, fmt, ap); va_end(ap);
    std::fputc('\n', stderr); std::fflush(stderr);
    if (g_log) {
        va_start(ap, fmt); std::vfprintf(g_log, fmt, ap); va_end(ap);
        std::fputc('\n', g_log); std::fflush(g_log);
    }
}
#define P(...) logline(__VA_ARGS__)

typedef void (*fn_void)(void);
typedef int  (*fn_int)(void);
typedef void (*fn_cb)(void (*)(void));

static fn_void  dll_throw_std;
static fn_void  dll_throw_custom;
static fn_int   dll_throw_internal;
static fn_cb    dll_call_through_c;
static fn_int   dll_dbg_present;

/* A callback that throws, to be invoked from the DLL's plain C frame. */
static void throwing_callback()
{
    P("  [exe] callback throwing std::runtime_error");
    throw std::runtime_error("from exe callback");
}

int main()
{
    HMODULE m;

    P("");
    P("=== EXPERIMENT 2: C++ exceptions across a DLL boundary (GNU ABI, i686) ===");
    P("compiler: GCC %s   pointer size: %d", __VERSION__, (int)sizeof(void *));
#ifdef __SEH__
    P("exception model: SEH");
#elif defined(__USING_SJLJ_EXCEPTIONS__)
    P("exception model: SJLJ");
#else
    P("exception model: DWARF-2 (no SEH, no SJLJ)");
#endif

    m = LoadLibraryA("throwlib.dll");
    if (!m) { P("LoadLibrary(throwlib.dll) failed err=%lu", GetLastError()); return 1; }

    dll_throw_std      = (fn_void)GetProcAddress(m, "throwlib_throw_std");
    dll_throw_custom   = (fn_void)GetProcAddress(m, "throwlib_throw_custom");
    dll_throw_internal = (fn_int) GetProcAddress(m, "throwlib_throw_and_catch_internally");
    dll_call_through_c = (fn_cb)  GetProcAddress(m, "throwlib_call_through_c");
    dll_dbg_present    = (fn_int) GetProcAddress(m, "throwlib_debugger_present");

    P("IsDebuggerPresent(): exe=%d dll=%d",
      IsDebuggerPresent() ? 1 : 0, dll_dbg_present ? dll_dbg_present() : -1);
    P("");

    /* --- case A: throw inside DLL, caught inside DLL ---------------------- */
    P("[A] throw and catch entirely inside the DLL");
    P("    (this is the only pattern we will allow in hook callbacks)");
    if (dll_throw_internal) P("    returned %d -> SURVIVED", dll_throw_internal());
    P("");

    /* --- case B: throw in DLL, catch in EXE, std type --------------------- */
    P("[B] throw std::runtime_error in DLL, catch in EXE");
    try {
        dll_throw_std();
        P("    *** no exception arrived ***");
    } catch (const std::runtime_error &e) {
        P("    caught std::runtime_error: %s -> CROSS-MODULE CATCH WORKS", e.what());
    } catch (const std::exception &e) {
        P("    caught std::exception: %s", e.what());
    } catch (...) {
        P("    caught via ... (type match FAILED but unwinding worked)");
    }
    P("");

    /* --- case C: DLL-private type, caught by base reference in EXE -------- */
    P("[C] throw DLL-private type in DLL, catch as std::exception& in EXE");
    try {
        dll_throw_custom();
        P("    *** no exception arrived ***");
    } catch (const std::exception &e) {
        P("    caught as std::exception&: %s -> CROSS-MODULE RTTI WORKS", e.what());
    } catch (...) {
        P("    caught via ... -> RTTI match failed, unwinding still worked");
    }
    P("");

    /* --- case D: throw through a plain C frame in the DLL ----------------- */
    P("[D] EXE callback throws; unwinds through a plain C frame inside the DLL");
    P("    if the process dies here, that is the 'no unwind info' failure mode");
    try {
        dll_call_through_c(throwing_callback);
        P("    *** no exception arrived ***");
    } catch (const std::exception &e) {
        P("    caught: %s -> UNWOUND THROUGH THE C FRAME", e.what());
    } catch (...) {
        P("    caught via ...");
    }
    P("");

    P("=== EXPERIMENT 2 END (process survived all cases) ===");
    return 0;
}
