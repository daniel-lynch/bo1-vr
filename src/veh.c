#include "veh.h"
#include "log.h"

#include <windows.h>

static PVOID g_veh;

/* MSVC C++ exceptions arrive as this code; GNU C++ exceptions use 0x20474e55
 * ("GNU\0" style tag). We log and decline both -- declining is essential,
 * because a first-chance C++ exception is normal control flow and swallowing it
 * would break the thrower. */
#define MSVC_CPP_EXCEPTION  0xE06D7363u  /* 'msc' */
#define GNU_CPP_EXCEPTION   0x20474E55u  /* 'UN GNU' */

/* Raised by OutputDebugStringA/W. These MUST be ignored before we log anything:
 * our own logger calls OutputDebugStringA, so logging one of these re-enters the
 * handler and spins forever. This is not hypothetical -- it was the first thing
 * the loader did when it ran under Wine, and it flooded stderr until the process
 * was killed. */
#define DBG_PRINTEXCEPTION_C_       0x40010006u
#define DBG_PRINTEXCEPTION_WIDE_C_  0x4001000Au

/* Belt and braces: even for codes we do log, never let the handler re-enter
 * itself. A fault raised from inside our own logging path would otherwise be
 * unbounded recursion. Per-thread, because VEH runs on whatever thread faulted. */
static __thread int g_in_handler;

static LONG CALLBACK on_exception(PEXCEPTION_POINTERS ep)
{
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    LONG ret;

    /* Debug-print notifications: bail out before touching the logger. */
    if (code == DBG_PRINTEXCEPTION_C_ || code == DBG_PRINTEXCEPTION_WIDE_C_)
        return EXCEPTION_CONTINUE_SEARCH;

    /* Anything with the "informational" severity (top two bits 01) is a
     * notification, not a fault. Not worth a log line each. */
    if ((code & 0xC0000000u) == 0x40000000u)
        return EXCEPTION_CONTINUE_SEARCH;

    if (g_in_handler)
        return EXCEPTION_CONTINUE_SEARCH;
    g_in_handler = 1;

    switch (code) {
    case MSVC_CPP_EXCEPTION:
    case GNU_CPP_EXCEPTION:
        /* Language-level exception in flight. Not our business. A first-chance
         * C++ exception is normal control flow; swallowing it would break the
         * thrower. */
        g_in_handler = 0;
        return EXCEPTION_CONTINUE_SEARCH;

    case EXCEPTION_ACCESS_VIOLATION:
        LOGE("VEH: access violation at %p (%s address %p)",
             ep->ExceptionRecord->ExceptionAddress,
             ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
             (void *)ep->ExceptionRecord->ExceptionInformation[1]);
        break;

    case EXCEPTION_ILLEGAL_INSTRUCTION:
        LOGE("VEH: illegal instruction at %p -- suspect a bad hook trampoline",
             ep->ExceptionRecord->ExceptionAddress);
        break;

    case EXCEPTION_BREAKPOINT:
    case EXCEPTION_SINGLE_STEP:
        /* Leave debugger traps entirely alone. */
        g_in_handler = 0;
        return EXCEPTION_CONTINUE_SEARCH;

    default:
        LOGE("VEH: exception 0x%08lx at %p", (unsigned long)code,
             ep->ExceptionRecord->ExceptionAddress);
        break;
    }

    /* Always continue the search. We are a tracer, not a handler: pretending to
     * handle a fault here would mask real game crashes and make every bug look
     * like a hang. */
    ret = EXCEPTION_CONTINUE_SEARCH;
    g_in_handler = 0;
    return ret;
}

void veh_install(void)
{
    if (g_veh)
        return;
    /* First=1: run before the game's own handlers, so we see the fault even if
     * BlackOps.exe installs a swallowing top-level filter (it does). */
    g_veh = AddVectoredExceptionHandler(1, on_exception);
    LOGI("VEH: handler %s", g_veh ? "installed" : "FAILED to install");
}

void veh_remove(void)
{
    if (g_veh) {
        RemoveVectoredExceptionHandler(g_veh);
        g_veh = NULL;
    }
}
