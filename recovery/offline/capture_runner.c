/* Offline export runner. Use only with capture_guard.c, never target_guard.c. */
#include <windows.h>
#include "recovery_config.h"
#include <stdio.h>
#include <string.h>
static int WINAPI silent_message(HWND w,LPCSTR text,LPCSTR title,UINT flags) {
    printf("offline-message=%s\n",text);fflush(stdout);return 1;
}
static void suppress_own_lab_dialogs(HMODULE lib) {
    unsigned char *base=(unsigned char*)lib;IMAGE_NT_HEADERS *nt=(void*)(base+((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_IMPORT_DESCRIPTOR *d=(void*)(base+nt->OptionalHeader.DataDirectory[1].VirtualAddress);
    for(;d->Name;d++) {
        DWORD *lookup=(void*)(base+d->OriginalFirstThunk),*iat=(void*)(base+d->FirstThunk);DWORD old;
        for(;*lookup;lookup++,iat++)if(!(*lookup&0x80000000)) {
            char *name=(char*)(base+*lookup+2);
            if(!strcmp(name,"MessageBoxA") && VirtualProtect(iat,4,PAGE_READWRITE,&old)) {
                *iat=(DWORD)silent_message;VirtualProtect(iat,4,old,&old);
            }
        }
    }
}
int main(int argc,char **argv) {
    HMODULE lib,guard=LoadLibraryExA("TargetSataGuard.dll",NULL,8);unsigned char id[512];
    unsigned long (__cdecl *marker)(void);
    if(!guard)return 9;
    marker=(void*)GetProcAddress(guard,"CaptureBuildId");
    if(!marker || marker()!=0x53494d31){FreeLibrary(guard);return 9;}
    lib=LoadLibraryExA("MP_API.dll",NULL,8);
    unsigned char (__cdecl *identify)(unsigned char,void*);
    unsigned char (__cdecl *flash_id)(unsigned char,const char*);
    unsigned char (__cdecl *pretest)(unsigned char,unsigned long,const char*,const char*,const char*,void*,void*,void*);
    if(!lib){printf("load-error=%lu\n",GetLastError());return 2;}
    suppress_own_lab_dialogs(lib);
    identify=(void*)GetProcAddress(lib,"GetIdentifyInfoBuf");
    flash_id=(void*)GetProcAddress(lib,"GetAllFlashID");
    if(!identify || !flash_id)return 3;
    printf("offline-identify=%u\n",identify(1,id));fflush(stdout);
    printf("offline-get-flash-id=%u\n",flash_id(1,"offline-flash-id.txt"));fflush(stdout);
    if(argc==2 && (!strcmp(argv[1],"--pretest-simulation") || !strcmp(argv[1],"--erase-simulation"))) {
        char directory[1024],ini[1100],progress[1024]={0};unsigned char status[4096]={0},cancel[16]={0};
        GetCurrentDirectoryA(1020,directory);strcat(directory,"\\");strcpy(ini,directory);strcat(ini,"param.ini");
        pretest=(void*)GetProcAddress(lib,!strcmp(argv[1],"--erase-simulation")?"DoEraseAll":"DoPretestInit");
        if(!pretest)return 4;
        printf("OFFLINE PRETEST SIMULATION: virtual handle only, no disk API\n");fflush(stdout);
        printf("offline-pretest-result=%u\n",pretest(1,1,directory,RECOVERY_SERIAL,ini,status,progress,cancel));fflush(stdout);
    }
    FreeLibrary(lib);return 0;
}
