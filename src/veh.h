/* veh.h -- vectored exception handling.
 *
 * 32-bit mingw has NO SEH. __try/__except do not compile at all, and the
 * DWARF-2 unwinder cannot propagate an exception through MSVC-built frames --
 * which is every frame in BlackOps.exe. Consequently no exception may ever be
 * allowed to escape a hook callback: doing so does not raise into the game's
 * handler, it terminates the process.
 *
 * AddVectoredExceptionHandler is the supported substitute. It is a flat,
 * ABI-neutral callback registered with the OS/Wine rather than a compiler
 * construct, so it works identically for mingw and MSVC frames.
 */
#ifndef BO1VR_VEH_H
#define BO1VR_VEH_H

void veh_install(void);
void veh_remove(void);

#endif /* BO1VR_VEH_H */
