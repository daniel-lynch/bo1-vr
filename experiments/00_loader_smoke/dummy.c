#include <windows.h>
#include <stdio.h>
BOOL WINAPI DllMain(HINSTANCE i, DWORD r, LPVOID v){
  if(r==DLL_PROCESS_ATTACH){ fprintf(stderr,"[dummy.asi] DllMain PROCESS_ATTACH, base=%p\n",(void*)i); fflush(stderr); }
  return TRUE; }
