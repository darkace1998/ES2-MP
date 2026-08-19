#include <windows.h>
#include <cstdio>
static void log(const char* s){ FILE* f=fopen("Z:\\tmp\\es2mod_hello.log","a"); if(f){fprintf(f,"%s\n",s);fclose(f);} }
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID){ if(reason==DLL_PROCESS_ATTACH){ log("hello from dll"); } return TRUE; }
extern "C" __declspec(dllexport) int es2_test(){ return 42; }
