/* asi_loader.h -- loads *.asi plugins sitting beside this DLL. */
#ifndef BO1VR_ASI_LOADER_H
#define BO1VR_ASI_LOADER_H

#include <windows.h>

/* Scans the directory containing this module for *.asi files and LoadLibrary's
 * each one. Returns the number successfully loaded. */
int asi_load_all(HMODULE self);

#endif /* BO1VR_ASI_LOADER_H */
