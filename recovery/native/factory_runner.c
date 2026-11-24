/* Real SATA factory runner. No capture/emulation code is included.
 * No action by default; --preflight never arms NAND initialization.
 * Requires the separate pinned TargetFactoryGuard library and exact target.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
typedef unsigned char U8;typedef unsigned long U32;
#include "recovery_config.h"
#define EXPECTED_CAPACITY RECOVERY_SECTORS
typedef struct {unsigned short length,flags;U8 path,target,lun,reserved;
    U32 size,timeout,reserved_long,buffer;U8 previous[8],current[8];} Packet;
static HANDLE (WINAPI *guard_open)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
static BOOL (WINAPI *guard_ioctl)(HANDLE,DWORD,LPVOID,DWORD,LPVOID,DWORD,LPDWORD,LPOVERLAPPED);
static BOOL (WINAPI *guard_close)(HANDLE);
static U32 (__cdecl *guard_stop)(void);
static U32 stopped=0;
static int WINAPI log_message(HWND w,LPCSTR text,LPCSTR title,UINT flags) {
    printf("vendor-message flags=%lu title=%s text=%s\n",(U32)flags,title?title:"",text?text:"");fflush(stdout);
    stopped=1;if(guard_stop)guard_stop();
    if((flags&15)==MB_YESNO || (flags&15)==MB_YESNOCANCEL)return IDNO;
    if((flags&15)==MB_OK)return IDOK;
    if((flags&15)==MB_ABORTRETRYIGNORE)return IDABORT;
    return IDCANCEL;
}
static U32 guard_dialogs(HMODULE lib) {
    U8 *base=(U8*)lib;IMAGE_NT_HEADERS *nt=(void*)(base+((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_IMPORT_DESCRIPTOR *d=(void*)(base+nt->OptionalHeader.DataDirectory[1].VirtualAddress);U32 count=0;
    for(;d->Name;d++) {
        DWORD *lookup=(void*)(base+d->OriginalFirstThunk),*iat=(void*)(base+d->FirstThunk);DWORD old;
        if(!d->OriginalFirstThunk)return 0;
        for(;*lookup;lookup++,iat++)if(!(*lookup&0x80000000)) {
            char *name=(char*)(base+*lookup+2);
            if(!strcmp(name,"MessageBoxA")) {
                if(!VirtualProtect(iat,4,PAGE_READWRITE,&old))return 0;
                *iat=(DWORD)log_message;VirtualProtect(iat,4,old,&old);count++;
            }
        }
    }
    return count;
}
static U32 command(HANDLE h,U32 feature,U32 lba,void *data,U32 size,U32 output) {
    Packet p;DWORD returned=0;U32 ok;
    memset(&p,0,sizeof(p));p.length=40;p.flags=output?5:3;p.size=size;p.timeout=30;p.buffer=(U32)data;
    p.current[0]=feature;p.current[1]=size/512;p.current[2]=lba;p.current[3]=lba>>8;p.current[4]=lba>>16;
    p.current[5]=0xa0;p.current[6]=output?0x31:0x21;
    ok=guard_ioctl(h,0x4d030,&p,40,&p,40,&returned,NULL);
    printf("preflight feature=%02lx direction=%s result=%lu status=%02x error=%lu\n",feature,output?"out":"in",ok,p.current[6],ok?0:GetLastError());fflush(stdout);
    return ok && returned>=40 && p.size==size && !(p.current[6]&0xa9) && (p.current[6]&0x40);
}
static U32 ap_key(HANDLE h) {
    Packet p;DWORD returned=0;memset(&p,0,sizeof(p));p.length=40;p.flags=1;p.timeout=10;
    memcpy(p.current,"\x00\x6f\xfe\xef\xfa\xaf\xe0\x00",8);
    return guard_ioctl(h,0x4d030,&p,40,&p,40,&returned,NULL) && returned>=40 && !(p.current[6]&0xa9) && (p.current[6]&0x40);
}
static U32 latch(HANDLE h,U32 value) {
    U32 data[128]={0};data[0]=0x04002400;data[1]=value;
    return ap_key(h) && command(h,0x60,0x400,data,512,1);
}
static U32 preflight(HANDLE h) {
    U32 regs[4]={0x04002440,0x0400247c,0x040024bc,0x0400303e};U32 data[128],i,ok=0;
    if(!latch(h,3))goto cleanup;
    for(i=0;i<4;i++) {
        memset(data,0,sizeof(data));data[0]=regs[i];
        if(!ap_key(h) || !command(h,0x24,0,data,512,1))goto cleanup;
        memset(data,0,sizeof(data));
        if(!ap_key(h) || !command(h,0x61,i==3?0x100:0x400,data,512,0))goto cleanup;
        printf("controller-register %08lx=%08lx\n",data[0],data[1]);fflush(stdout);
    }
    ok=1;
cleanup:
    if(!latch(h,0))ok=0;
    if(!ok)return 0;
    memset(data,0,sizeof(data));if(!ap_key(h) || !command(h,0x13,0,data,512,0))return 0;
    memset(data,0,sizeof(data));if(!ap_key(h) || !command(h,0x15,0,data,512,0))return 0;
    return 1;
}
int main(int argc,char **argv) {
    HMODULE guard=NULL,lib=NULL;HANDLE h=INVALID_HANDLE_VALUE;U32 initialize=0,erase_first=0,result=2;
    U32 (__cdecl *build_id)(void);U32 (__cdecl *capacity)(void);U32 (__cdecl *arm)(void);U32 (__cdecl *cleanup_needed)(void)=NULL;
    U32 (__cdecl *find)(WCHAR*,U32,U32*)=NULL;WCHAR target[512];U32 devinst;
    U8 (__cdecl *identify)(U8,void*);U8 (__cdecl *flash_id)(U8,const char*);
    U8 (__cdecl *pretest)(U8,U32,const char*,const char*,const char*,void*,void*,void*);
    U8 identity[512],status[4096]={0},cancel[16]={0};char progress[1024]={0},directory[1024],ini[1100],path[1100];
    if(argc!=2 || (strcmp(argv[1],"--preflight") && strcmp(argv[1],"--initialize") && strcmp(argv[1],"--initialize-with-erase"))) {
        printf("No action. Use --preflight or explicitly authorized initialization mode.\n");return 2;
    }
    erase_first=!strcmp(argv[1],"--initialize-with-erase");initialize=erase_first || !strcmp(argv[1],"--initialize");
    if(!GetCurrentDirectoryA(1020,directory))return 2;strcat(directory,"\\");
    strcpy(path,directory);strcat(path,"TargetSataGuard.dll");guard=LoadLibraryExA(path,NULL,8);
    if(!guard){printf("guard-load-error=%lu\n",GetLastError());goto done;}
    build_id=(void*)GetProcAddress(guard,"FactoryBuildId");arm=(void*)GetProcAddress(guard,"FactoryArm");
    capacity=(void*)GetProcAddress(guard,"FactoryCapacity");
    guard_stop=(void*)GetProcAddress(guard,"FactoryStop");find=(void*)GetProcAddress(guard,"TargetFind");
    cleanup_needed=(void*)GetProcAddress(guard,"FactoryCleanupNeeded");
    guard_open=(void*)GetProcAddress(guard,"_GuardCreateFileW@28");guard_close=(void*)GetProcAddress(guard,"_GuardCloseHandle@4");
    guard_ioctl=(void*)GetProcAddress(guard,"_GuardDeviceIoControl@32");
    if(!build_id || build_id()!=0x53313146 || !arm || !guard_stop || !cleanup_needed || !find || !guard_open || !guard_close || !guard_ioctl)goto done;
    if(!capacity || capacity()!=EXPECTED_CAPACITY){printf("compiled-capacity-mismatch\n");goto done;}
    printf("compiled-capacity-sectors=%lu\n",capacity());fflush(stdout);
    if(!find(target,512,&devinst)){printf("exact-target-not-found\n");goto done;}
    h=guard_open(target,0xc0000000,3,NULL,3,0x80,NULL);
    if(h==INVALID_HANDLE_VALUE){printf("target-identity-rejected error=%lu\n",GetLastError());goto done;}
    if(!preflight(h)){printf("preflight-failed\n");goto done;}guard_close(h);h=INVALID_HANDLE_VALUE;
    printf("preflight-controller-and-nand-matched\n");fflush(stdout);
    strcpy(path,directory);strcat(path,"MP_API.dll");lib=LoadLibraryExA(path,NULL,8);
    if(!lib){printf("vendor-load-error=%lu\n",GetLastError());goto done;}
    if(!guard_dialogs(lib)){printf("message-handler-not-installed\n");goto done;}
    identify=(void*)GetProcAddress(lib,"GetIdentifyInfoBuf");flash_id=(void*)GetProcAddress(lib,"GetAllFlashID");pretest=(void*)GetProcAddress(lib,"DoPretestInit");
    if(!identify || !flash_id || !pretest)goto done;
    if(!identify(1,identity) || !flash_id(1,"preflight-flash-id.txt") || stopped){printf("vendor-preflight-failed\n");goto done;}
    printf("vendor-preflight-passed\n");fflush(stdout);
    if(!initialize){printf("outcome=preflight_only NAND initialization NOT armed\n");result=0;goto done;}
    if(!arm()){printf("factory-arm-rejected\n");goto done;}
    if(erase_first) {
        U32 erase_data[128]={0};FILE *snapshot;
        h=guard_open(target,0xc0000000,3,NULL,3,0x80,NULL);
        if(h==INVALID_HANDLE_VALUE)goto done;
        printf("begin-authorized-manufacturer-erase type05 configured-target\n");fflush(stdout);
        erase_data[0]=0xffffffff;
        if(!ap_key(h) || !command(h,0x16,0,erase_data,512,1))goto done;
        memset(erase_data,0,sizeof(erase_data));erase_data[0]=5;
        if(!ap_key(h) || !command(h,0x26,0x050000,erase_data,512,1)){printf("manufacturer-erase-failed no-programming\n");goto done;}
        memset(erase_data,0,sizeof(erase_data));
        if(!ap_key(h) || !command(h,0x13,1,erase_data,512,0)){printf("post-erase-status-rejected no-programming\n");goto done;}
        snapshot=fopen("erase-system-info.bin","wb");if(!snapshot)goto done;
        if(fwrite(erase_data,1,512,snapshot)!=512){fclose(snapshot);goto done;}fclose(snapshot);
        guard_close(h);h=INVALID_HANDLE_VALUE;
        printf("manufacturer-erase-command-completed continue-with-original-health-checks\n");fflush(stdout);
    }
    strcpy(ini,directory);strcat(ini,"param.ini");
    printf("begin-authorized-initialization configured-target SBFM61.3\n");fflush(stdout);
    result=pretest(1,1,directory,RECOVERY_SERIAL,ini,status,progress,cancel);
    printf("factory-return=%lu message-seen=%lu\n",result,stopped);fflush(stdout);
    result=(result==1 && !stopped)?0:3;
done:
    if(cleanup_needed && cleanup_needed()) {
        if(h==INVALID_HANDLE_VALUE && find && find(target,512,&devinst))h=guard_open(target,0xc0000000,3,NULL,3,0x80,NULL);
        if(h==INVALID_HANDLE_VALUE || !latch(h,0)){printf("access-latch-cleanup-failed\n");result=3;}
        else printf("access-latch-restored-zero\n");
    }
    if(h!=INVALID_HANDLE_VALUE && guard_close)guard_close(h);
    if(guard_stop)guard_stop();if(lib)FreeLibrary(lib);if(guard)FreeLibrary(guard);
    printf("runner-exit=%lu\n",result);return result;
}
