/* cframe.c -- a plain C frame with no exception tables, compiled by gcc (not
 * g++) and deliberately without -fexceptions. An exception thrown by the
 * callback has to unwind through this frame. This is the closest we can get,
 * with our own toolchain, to the situation where a hook callback throws and the
 * next frame up is MSVC-compiled game code with no GCC CFI. */

typedef void (*plain_cb)(void);

__declspec(dllexport) void throwlib_call_through_c(plain_cb cb)
{
    volatile int guard = 0xC0FFEE;   /* forces a real frame */
    cb();
    (void)guard;
}
