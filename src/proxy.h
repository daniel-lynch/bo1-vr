/* proxy.h -- forwarding to the real dinput8.dll. */
#ifndef BO1VR_PROXY_H
#define BO1VR_PROXY_H

#include <windows.h>

/* Loads the genuine dinput8.dll out of the system directory and resolves the
 * five forwarded entry points. Returns TRUE on success. Safe to call more than
 * once; subsequent calls are no-ops. */
BOOL proxy_init(void);

void proxy_shutdown(void);

#endif /* BO1VR_PROXY_H */
