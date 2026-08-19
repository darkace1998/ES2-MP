#include <windows.h>
#include <cstdio>
int main(int argc, char** argv){
  if(argc<2){ printf("usage: loader <dll>\n"); return 1; }
  HMODULE h = LoadLibraryA(argv[1]);
  if(!h){ printf("LoadLibrary failed: %lu\n", GetLastError()); return 2; }
  auto f = (int(*)())GetProcAddress(h, "es2_test");
  printf("loaded %p es2_test=%p -> %d\n", (void*)h, (void*)f, f?f():-1);
  return 0;
}
