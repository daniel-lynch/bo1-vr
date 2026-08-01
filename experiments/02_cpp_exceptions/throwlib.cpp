/* experiments/02_cpp_exceptions/throwlib.cpp
 *
 * EXPERIMENT 2
 * How does a mingw/GNU-ABI 32-bit build behave when a C++ exception crosses a
 * DLL boundary under Wine/Proton -- with and without a debugger attached?
 *
 * Background: there is a report that with an MSVC-ABI build, any thrown
 * exception kills the game while a debugger is attached, but the same build is
 * fine undebugged. We need to know whether the GNU ABI shares that failure,
 * because if it does, every hook callback needs a hard no-exceptions rule
 * enforced at compile time rather than by convention.
 *
 * The i686 mingw toolchain here is configured --disable-sjlj-exceptions
 * --with-dwarf2, so this is the DWARF-2 unwinder, NOT SEH. DWARF-2 unwinding
 * walks .eh_frame; it has no knowledge of Windows SEH chains and cannot unwind
 * through a frame that lacks CFI -- which is every MSVC-built frame in
 * BlackOps.exe.
 */

#include <windows.h>
#include <cstdio>
#include <stdexcept>
#include <string>

#define P(...) do { std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); std::fflush(stderr); } while (0)

extern "C" {

/* 1. Throw a std::runtime_error out of the DLL, to be caught by the EXE. */
__declspec(dllexport) void throwlib_throw_std()
{
    P("  [dll] about to throw std::runtime_error");
    throw std::runtime_error("thrown from throwlib.dll");
}

/* 2. Throw a type defined only in this DLL. Catching this in the EXE by
 *    base-class reference exercises cross-module RTTI, which is where a
 *    statically linked libstdc++ typically breaks: each module gets its own
 *    copy of the type_info and the catch match fails. */
struct DllOnlyError : public std::runtime_error {
    explicit DllOnlyError(const char *m) : std::runtime_error(m) {}
};

__declspec(dllexport) void throwlib_throw_custom()
{
    P("  [dll] about to throw DllOnlyError");
    throw DllOnlyError("custom type from throwlib.dll");
}

/* 3. Throw and catch entirely inside the DLL. This is the ONLY pattern that is
 *    safe in a hook callback, and we want to confirm it works even if 1 and 2
 *    do not. */
__declspec(dllexport) int throwlib_throw_and_catch_internally()
{
    try {
        throw std::runtime_error("internal");
    } catch (const std::exception &e) {
        P("  [dll] caught internally: %s", e.what());
        return 1;
    } catch (...) {
        P("  [dll] caught internally via ...");
        return 2;
    }
}

/* 4. Throw through a C frame (compiled by gcc, no -fexceptions). This models a
 *    hook callback invoked from game code: if the intervening frame has no
 *    unwind info, DWARF-2 unwinding calls std::terminate. */
typedef void (*plain_cb)(void);
__declspec(dllexport) void throwlib_call_through_c(plain_cb cb);

/* 5. Report whether a debugger is attached, so the log is self-describing. */
__declspec(dllexport) int throwlib_debugger_present()
{
    return IsDebuggerPresent() ? 1 : 0;
}

} /* extern "C" */

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
