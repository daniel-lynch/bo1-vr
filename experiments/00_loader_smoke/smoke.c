#include <windows.h>
#include <stdio.h>
#include <initguid.h>
DEFINE_GUID(IID_IDirectInput8A,0xBF798030,0x483A,0x4DA2,0xAA,0x99,0x5D,0x64,0xED,0x36,0x97,0x00);
int main(void){
  HMODULE m; HRESULT (WINAPI *create)(HINSTANCE,DWORD,REFIID,LPVOID*,LPUNKNOWN); void*di=0; HRESULT hr;
  fprintf(stderr,"smoke: loading ./dinput8.dll\n"); fflush(stderr);
  m=LoadLibraryA("./dinput8.dll");
  if(!m){fprintf(stderr,"smoke: load failed %lu\n",GetLastError());return 1;}
  create=(void*)GetProcAddress(m,"DirectInput8Create");
  fprintf(stderr,"smoke: DirectInput8Create export = %p\n",(void*)create); fflush(stderr);
  if(!create) return 2;
  hr=create(GetModuleHandleA(NULL),0x0800,&IID_IDirectInput8A,&di,NULL);
  fprintf(stderr,"smoke: DirectInput8Create -> hr=0x%08lx iface=%p\n",(unsigned long)hr,di); fflush(stderr);
  return 0; }
