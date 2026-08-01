/* tgtlib.c -- the DLL half of the winedbg target.
 *
 * Deliberately mirrors the shape of the real loader: a 32-bit DLL, built with
 * -gdwarf-4, not stripped, with distinctly named functions and locals that a
 * debugger should be able to name and read.
 */

#include <windows.h>
#include <stdio.h>

__declspec(dllexport) int tgtlib_leaf(int a, int b)
{
    int product = a * b;      /* breakpoint target; inspect `product` */
    volatile int sink = product;
    return sink;
}

__declspec(dllexport) int tgtlib_middle(int n)
{
    int scaled = n + 100;     /* second frame in the backtrace */
    return tgtlib_leaf(scaled, 3);
}

__declspec(dllexport) void tgtlib_crash_now(void)
{
    /* A deliberate access violation, to check whether the debugger catches the
     * fault and can produce a usable backtrace across the DLL boundary. */
    volatile int *p = (volatile int *)0x00000010;
    *p = 0x41414141;
}

__declspec(dllexport) const char *tgtlib_name(void)
{
    return "tgtlib";
}

BOOL WINAPI DllMain(HINSTANCE i, DWORD r, LPVOID v) { (void)i;(void)r;(void)v; return TRUE; }
