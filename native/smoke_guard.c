/* Loads only our own DLL and calls pure conversion functions. No disk API. */
#include <windows.h>
#include <stdio.h>
typedef unsigned long U32;
typedef unsigned char U8;
typedef U32 (__cdecl *Convert)(const U8*,U32,U32,U32,U32,U32,void*);
typedef U32 (__cdecl *Policy)(const void*);
int main(int argc,char **argv) {
    HMODULE library;
    Convert convert;
    Policy policy;
    U8 cdb[12]={0xa1,8,14,0,1,0,0,0,0xa0,0xec,0,0};
    U8 packet[40];
    if (argc!=2) return 1;
    library=LoadLibraryExA(argv[1],NULL,0x1100);
    if (!library) {printf("Load failed: %lu\n",GetLastError());return 2;}
    convert=(Convert)GetProcAddress(library,"BridgeConvert");
    policy=(Policy)GetProcAddress(library,"BridgeIdentifyOnly");
    if (!convert || !policy) return 3;
    if (convert(cdb,12,512,1,0x12340000,30,packet)) return 4;
    if (packet[0]!=40 || packet[2]!=3 || packet[33]!=1 || packet[38]!=0xec) return 5;
    if (policy(packet)!=1) return 6;
    packet[38]=0x31;
    if (policy(packet)!=0) return 7;
    if (!convert(cdb,12,1024,1,0x12340000,30,packet)) return 8;
    FreeLibrary(library);
    puts("PASS: native Windows x86 DLL load, conversion and rejection policy. No device access.");
    return 0;
}
