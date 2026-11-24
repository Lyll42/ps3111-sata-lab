/* Standalone identification test: loads only our diagnostic DLL.
 * No vendor executable/DLL, no Burner, no VUC or write command.
 */
#include <windows.h>
#include <stdio.h>
typedef unsigned char U8;
typedef unsigned long U32;
typedef U32 (__cdecl *Find)(WCHAR*,U32,U32*);
typedef HANDLE (WINAPI *Open)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
typedef BOOL (WINAPI *Ioctl)(HANDLE,DWORD,LPVOID,DWORD,LPVOID,DWORD,LPDWORD,LPOVERLAPPED);
typedef BOOL (WINAPI *Close)(HANDLE);
static void text_ata(const U8 *data,U32 offset,U32 size,char *out) {
    U32 i;for(i=0;i<size;i++)out[i]=data[offset+(i^1)];
    while(size && out[size-1]==' ')size--;out[size]=0;
}
static U32 same(const char *a,const char *b) {while(*a && *b && *a==*b){a++;b++;}return !*a && !*b;}
int main(void) {
    WCHAR module[1024],interface_path[512];const WCHAR *suffix=L"TargetSataGuard.dll";
    U32 n,i,devinst;HMODULE library;HANDLE handle;Find find;Open open;Ioctl ioctl;Close close;
    U8 packet[40]={0},data[512]={0},number[12]={0};DWORD returned=0;char model[41],firmware[9];
    n=GetModuleFileNameW(NULL,module,1024);
    if(!n || n>=1000)return 10;
    while(n && module[n-1]!='\\')n--;
    for(i=0;suffix[i];i++)module[n+i]=suffix[i];module[n+i]=0;
    library=LoadLibraryExW(module,NULL,0x1100);
    if(!library){printf("{\"status\":\"dll_load_failed\",\"error\":%lu}\n",GetLastError());return 11;}
    find=(Find)GetProcAddress(library,"TargetFind");
    open=(Open)GetProcAddress(library,"_GuardCreateFileW@28");
    ioctl=(Ioctl)GetProcAddress(library,"_GuardDeviceIoControl@32");
    close=(Close)GetProcAddress(library,"_GuardCloseHandle@4");
    if(!find || !open || !ioctl || !close){FreeLibrary(library);return 12;}
    if(!find(interface_path,512,&devinst)) {
        puts("{\"status\":\"target_not_found_or_ambiguous\",\"device_opened\":false}");
        FreeLibrary(library);return 2;
    }
    handle=open(interface_path,0xc0000000,3,NULL,3,0x80,NULL);
    if(handle==INVALID_HANDLE_VALUE){printf("{\"status\":\"target_validation_failed\",\"error\":%lu}\n",GetLastError());FreeLibrary(library);return 3;}
    *(unsigned short*)packet=40;*(unsigned short*)(packet+2)=3;
    *(U32*)(packet+8)=512;*(U32*)(packet+12)=10;*(U32*)(packet+20)=(U32)data;
    packet[33]=1;packet[37]=0xa0;packet[38]=0xec;
    if(!ioctl(handle,0x4d030,packet,40,packet,40,&returned,NULL) || returned<40 || *(U32*)(packet+8)!=512
        || (packet[38]&0xa9) || !(packet[38]&0x40)) {
        printf("{\"status\":\"identify_failed\",\"error\":%lu}\n",GetLastError());close(handle);FreeLibrary(library);return 4;
    }
    text_ata(data,54,40,model);text_ata(data,46,8,firmware);
    if(!same(model,"PS3111") || !same(firmware,"SBRM02.0")){close(handle);FreeLibrary(library);return 5;}
    if(!ioctl(handle,0x2d1080,NULL,0,number,12,&returned,NULL)){close(handle);FreeLibrary(library);return 6;}
    printf("{\"status\":\"native_sata_identify_ok\",\"model\":\"%s\",\"firmware\":\"%s\",\"disk_number\":%lu,\"lba28_sectors\":%lu,\"ata_command\":\"0xEC\",\"modifying_commands_sent\":0}\n",
           model,firmware,*(U32*)(number+4),*(U32*)(data+120));
    close(handle);FreeLibrary(library);return 0;
}
