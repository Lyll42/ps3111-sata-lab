/* Target-specific bridge. Build flags select IDENTIFY, NAND diagnostics or
 * factory operations. recovery_config.h pins the device and its identity. */
#include "recovery_config.h"
#define TARGET_RUNTIME
#include "sata_guard.c"

typedef struct { DWORD cbSize; GUID ClassGuid; DWORD DevInst; ULONG_PTR Reserved; } DevInfo;
typedef struct { DWORD cbSize; GUID InterfaceClassGuid; DWORD Flags; ULONG_PTR Reserved; } IfInfo;
typedef HANDLE (WINAPI *GetClass)(const GUID*,LPCWSTR,HWND,DWORD);
typedef BOOL (WINAPI *EnumInfo)(HANDLE,DWORD,DevInfo*);
typedef BOOL (WINAPI *InstanceId)(HANDLE,DevInfo*,LPWSTR,DWORD,PDWORD);
typedef BOOL (WINAPI *EnumInterface)(HANDLE,DevInfo*,const GUID*,DWORD,IfInfo*);
typedef BOOL (WINAPI *InterfaceDetail)(HANDLE,IfInfo*,LPVOID,DWORD,PDWORD,DevInfo*);
typedef BOOL (WINAPI *DestroyInfo)(HANDLE);
static const GUID DiskGuid={0x53f56307,0xb6bf,0x11d0,{0x94,0xf2,0x00,0xa0,0xc9,0x1e,0xfb,0x8b}};
static const WCHAR ExpectedPNP[]=RECOVERY_PNP;
#ifdef FACTORY_RUNTIME
static const WCHAR ExpectedNormalPNP[]=RECOVERY_NORMAL_PNP;
#endif
typedef struct { HANDLE handle; U32 number; U32 sectors; } Slot;
static Slot slots[32];
static LONG slots_lock=0;
static NativePacket completed_result;static U32 completed_result_valid=0;
#ifdef FACTORY_RUNTIME
#include "factory_policy.h"
/* Before DoPretestInit installs its output directory, the original vendor
 * helper uses a root-relative report filename. Redirect only this exact
 * ordinary log into the runner's own package directory. Device namespaces
 * and all other rooted paths retain their existing rejection policy. */
static U32 FactoryReportPathW(LPCWSTR source,WCHAR *result,U32 capacity) {
    static const WCHAR expected[]=L"\\Report__0.txt";
    static const WCHAR tail[]=L"Report_preflight_target_only.txt";
    U32 i,n;
    if(!source)return 0;
    for(i=0;expected[i];i++)if(source[i]!=expected[i])return 0;
    if(source[i])return 0;
    n=GetModuleFileNameW(NULL,result,capacity);
    if(!n || n>=capacity-40)return 0;
    while(n && result[n-1]!='\\')n--;
    if(!n)return 0;
    for(i=0;tail[i];i++)result[n+i]=tail[i];result[n+i]=0;return 1;
}
#endif
#ifdef NAND_DIAGNOSTICS
static LONG diagnostic_ap_key=0;
#endif
/* Stable finite policy: EC, plus (only in the separate NAND build) the exact
 * previously reviewed AP key and one 512-byte NAND-ID read. No RAM or flash. */
static U32 diagnostic_command(const NativePacket *p) {
#ifdef FACTORY_RUNTIME
    return factory_command(p);
#else
    U32 i;
    if(BridgeIdentifyOnly(p))return 1;
#ifdef NAND_DIAGNOSTICS
    if(!p || p->length!=40)return 0;
    for(i=0;i<8;i++)if(p->previous[i])return 0;
    if(p->current[7])return 0;
    if(p->flags==1 && !p->transfer_length && !p->buffer
       && !p->current[0] && p->current[1]==0x6f && p->current[2]==0xfe
       && p->current[3]==0xef && p->current[4]==0xfa && p->current[5]==0xaf
       && p->current[6]==0xe0)return 2;
    if(p->flags==3 && p->transfer_length==512 && p->buffer
       && p->current[0]==0x15 && p->current[1]==1 && !p->current[2]
       && !p->current[3] && !p->current[4] && p->current[5]==0xa0
       && p->current[6]==0x21 && diagnostic_ap_key)return 3;
#endif
    return 0;
#endif
}
/* Bounded local trace. Preserves the error code observed by the caller and
 * uses ordinary file I/O only; never logs payloads or opens a disk. */
static void trace_event(const char *event,U32 a,U32 b) {
    WCHAR path[1024];char line[128];U32 n,i,j,k;DWORD error,written;HANDLE file;
    error=GetLastError();
    n=GetModuleFileNameW(NULL,path,1024);
    if(!n || n>=980)goto done;
    while(n && path[n-1]!='\\')n--;
    {static const WCHAR tail[]=L"sata-discovery-trace.log";
     for(i=0;tail[i];i++)path[n+i]=tail[i];path[n+i]=0;}
    file=CreateFileW(path,0x40000000,3,NULL,4,0x80,NULL);
    if(file==INVALID_HANDLE_VALUE)goto done;
    if(GetFileSize(file,NULL)>65536) {CloseHandle(file);goto done;}
    SetFilePointer(file,0,NULL,2);
    for(n=0;event[n] && n<80;n++)line[n]=event[n];
    for(j=0;j<2;j++) {
        line[n++]=' ';k=j?b:a;
        for(i=0;i<8;i++)line[n++]="0123456789abcdef"[(k>>(28-4*i))&15];
    }
    line[n++]='\r';line[n++]='\n';WriteFile(file,line,n,&written,NULL);CloseHandle(file);
done:
    SetLastError(error);
}
#ifdef NAND_DIAGNOSTICS
static void save_nand_id(const U8 *data) {
    WCHAR path[1024];U32 n,i;DWORD error=GetLastError(),written;HANDLE file;
    n=GetModuleFileNameW(NULL,path,1024);
    if(!n || n>=980)goto done;
    while(n && path[n-1]!='\\')n--;
    {static const WCHAR tail[]=L"nand-id-current.bin";
     for(i=0;tail[i];i++)path[n+i]=tail[i];path[n+i]=0;}
    file=CreateFileW(path,0x40000000,1,NULL,2,0x80,NULL);
    if(file!=INVALID_HANDLE_VALUE){WriteFile(file,data,512,&written,NULL);CloseHandle(file);}
done:SetLastError(error);
}
#endif
static void lock_slots(void) { while(InterlockedCompareExchange(&slots_lock,1,0)) Sleep(0); }
static void unlock_slots(void) { InterlockedExchange(&slots_lock,0); }
static U32 lower_w(U32 c) { return c>='A' && c<='Z' ? c+32 : c; }
static U32 same_w(const WCHAR *a,const WCHAR *b) {
    U32 i=0;
    while (a[i] && b[i] && i<511) {if(lower_w(a[i])!=lower_w(b[i])) return 0;i++;}
    return !a[i] && !b[i];
}
static U32 requested_number(const WCHAR *path,U32 *number) {
    static const WCHAR prefix[]=L"\\\\.\\PhysicalDrive";
    U32 i=0,n=0,digits=0;
    if (!path) return 0;
    while (prefix[i]) {if(lower_w(path[i])!=lower_w(prefix[i]))return 0;i++;}
    while (path[i]>='0' && path[i]<='9' && digits<3) {n=n*10+(path[i++]-'0');digits++;}
    if (!digits || path[i])return 0;
    *number=n;return 1;
}

/* Metadata enumeration only. No disk handle is opened by this function. */
API U32 __cdecl TargetFind(WCHAR *result,U32 capacity,U32 *devinst) {
    HMODULE library;HANDLE set;GetClass get_class;EnumInfo enum_info;InstanceId instance_id;
    EnumInterface enum_interface;InterfaceDetail detail;DestroyInfo destroy;
    DevInfo info;IfInfo iface;WCHAR id[512];U8 buffer[2052];DWORD required=0;
    U32 index,found=0,i;WCHAR *path;
    if (!result || capacity<512 || !devinst)return 0;
    result[0]=0;*devinst=0;
    library=LoadLibraryExW(L"setupapi.dll",NULL,0x800);
    if (!library)return 0;
    get_class=(GetClass)GetProcAddress(library,"SetupDiGetClassDevsW");
    enum_info=(EnumInfo)GetProcAddress(library,"SetupDiEnumDeviceInfo");
    instance_id=(InstanceId)GetProcAddress(library,"SetupDiGetDeviceInstanceIdW");
    enum_interface=(EnumInterface)GetProcAddress(library,"SetupDiEnumDeviceInterfaces");
    detail=(InterfaceDetail)GetProcAddress(library,"SetupDiGetDeviceInterfaceDetailW");
    destroy=(DestroyInfo)GetProcAddress(library,"SetupDiDestroyDeviceInfoList");
    if (!get_class || !enum_info || !instance_id || !enum_interface || !detail || !destroy) {FreeLibrary(library);return 0;}
    set=get_class(&DiskGuid,NULL,NULL,18);
    if (set==INVALID_HANDLE_VALUE) {FreeLibrary(library);return 0;}
    for(index=0;index<512;index++) {
        zero_bytes(&info,sizeof(info));info.cbSize=sizeof(info);
        if (!enum_info(set,index,&info)) {
            if (GetLastError()!=259)found=0;
            break;
        }
        zero_bytes(id,sizeof(id));
        if (!instance_id(set,&info,id,512,&required)) {found=0;break;}
        if (!same_w(id,ExpectedPNP)
#ifdef FACTORY_RUNTIME
            && !same_w(id,ExpectedNormalPNP)
#endif
            )continue;
        if (found) {found=0;break;}
        zero_bytes(&iface,sizeof(iface));iface.cbSize=sizeof(iface);
        if (!enum_interface(set,&info,&DiskGuid,0,&iface)) {found=0;break;}
        zero_bytes(buffer,sizeof(buffer));buffer[0]=6; /* x86 WCHAR detail ABI */
        if (!detail(set,&iface,buffer,sizeof(buffer),&required,NULL) || required>sizeof(buffer)) {found=0;break;}
        path=(WCHAR*)(buffer+4);
        for(i=0;i<capacity-1 && i<1023 && path[i];i++)result[i]=path[i];
        if (i==capacity-1 || i==1023) {found=0;break;}
        result[i]=0;*devinst=info.DevInst;found=1;
    }
    if (index==512)found=0;
    destroy(set);FreeLibrary(library);
    if (!found)result[0]=0;
    return found;
}

static U32 ascii_field(const U8 *raw,U32 size,U32 offset,const char *wanted) {
    U32 i=0;
    if (!offset || offset>=size)return 0;
    while(offset<size && raw[offset]==' ')offset++;
    while(wanted[i]) {if(offset+i>=size || raw[offset+i]!=(U8)wanted[i])return 0;i++;}
    offset+=i;
    while(offset<size && raw[offset]==' ')offset++;
    return offset<size && !raw[offset];
}
static U32 identify_field(const U8 *raw,U32 offset,U32 length,const char *wanted) {
    U32 i=0,n=0;
    while(wanted[n])n++;
    if(n>length)return 0;
    for(i=0;i<length;i++)if(raw[offset+(i^1)]!=(i<n?(U8)wanted[i]:' '))return 0;
    return 1;
}
static U32 validate_handle(HANDLE handle,U32 requested,U32 *number_out,U32 *sectors) {
    DWORD returned;U8 query[12],descriptor[1024],number[12];NativePacket packet;U8 *data;
    zero_bytes(query,sizeof(query));zero_bytes(descriptor,sizeof(descriptor));
    if (!DeviceIoControl(handle,0x2d1400,query,sizeof(query),descriptor,sizeof(descriptor),&returned,NULL)
        || returned<36 || returned>sizeof(descriptor) || read32(descriptor+28)!=11
#ifndef FACTORY_RUNTIME
        || !ascii_field(descriptor,returned,read32(descriptor+16),"PS3111")
        || !ascii_field(descriptor,returned,read32(descriptor+20),"SBRM02.0")
#endif
        )return 0;
    if (!DeviceIoControl(handle,0x2d1080,NULL,0,number,sizeof(number),&returned,NULL)
        || returned<12 || read32(number)!=7 || read32(number+4)>255
#ifdef FACTORY_RUNTIME
        || read32(number+4)!=RECOVERY_DISK_INDEX
#endif
        || (requested!=0xffffffff && read32(number+4)!=requested))return 0;
    *number_out=read32(number+4);
    data=VirtualAlloc(NULL,4096,0x3000,4);
    if (!data)return 0;
    zero_bytes(&packet,sizeof(packet));packet.length=40;packet.flags=3;packet.transfer_length=512;
    packet.timeout=10;packet.buffer=(U32)data;packet.current[1]=1;packet.current[5]=0xa0;packet.current[6]=0xec;
    if (!DeviceIoControl(handle,IOCTL_APTD,&packet,40,&packet,40,&returned,NULL)
        || returned<40 || packet.transfer_length!=512 || packet.current[6]&0xa9 || !(packet.current[6]&0x40)
#ifndef FACTORY_RUNTIME
        || !identify_field(data,54,40,"PS3111") || !identify_field(data,46,8,"SBRM02.0")
#endif
        ) {
        VirtualFree(data,0,0x8000);return 0;
    }
    *sectors=read32(data+120);
#ifdef FACTORY_RUNTIME
    {
        U32 burner=identify_field(data,54,40,"SATABURN   SB") && identify_field(data,46,8,"SBBM61.3")
            /* SBBM61.3 actually reports the saturated LBA28 placeholder;
             * this is not a measurement of the original 240GB capacity. */
            && ((*sectors>0 && *sectors<=65536) || *sectors==0x0fffffff || *sectors==FACTORY_SECTORS);
        U32 normal=factory_jumped && identify_field(data,54,40,RECOVERY_MODEL)
            && identify_field(data,46,8,"SBFM61.3") && identify_field(data,20,20,RECOVERY_SERIAL)
            && read32(data+200)==FACTORY_SECTORS && !read32(data+204);
        if(!burner && !normal){VirtualFree(data,0,0x8000);return 0;}
        if(normal)*sectors=FACTORY_SECTORS;
    }
#endif
    VirtualFree(data,0,0x8000);
#ifdef FACTORY_RUNTIME
    return *sectors>0 && (*sectors<=65536 || *sectors==0x0fffffff || *sectors==FACTORY_SECTORS);
#else
    return *sectors>0 && *sectors<=65536;
#endif
}

static HANDLE TargetOpenRequested(LPCWSTR path,DWORD access,DWORD sharing,
    LPSECURITY_ATTRIBUTES security,DWORD creation,DWORD flags,HANDLE template_file) {
    WCHAR interface_path[512];U32 number,devinst,sectors,i;HANDLE handle;
    if (creation!=3 || template_file || security || (flags&0x40000000)
        || !TargetFind(interface_path,512,&devinst))goto denied;
    if (!requested_number(path,&number)) {
        if (!same_w(path,interface_path))goto denied;
        number=0xffffffff;
    }
#ifdef FACTORY_RUNTIME
    /* IDENTIFY between SET_PARAMETER and READ_REGISTER replaces the pending
     * response on real SBBM61.3 (reproduced with a bounded read-only test).
     * Pin one initially ATA-verified handle for this process. Subsequent opens
     * still require the same unique PNP interface and requested disk number;
     * no additional ATA command is injected into the vendor's transaction. */
    lock_slots();
    if(slots[0].handle) {
        handle=slots[0].handle;
        if(number!=0xffffffff && number!=slots[0].number){unlock_slots();goto denied;}
        number=slots[0].number;unlock_slots();
        trace_event("reuse-verified-session",number,0);return handle;
    }
    unlock_slots();
#endif
    /* Only this exact PNP interface is opened; other disks are never probed. */
    handle=CreateFileW(interface_path,0xc0000000,3,NULL,3,0x80,NULL);
    if (handle==INVALID_HANDLE_VALUE)goto denied;
    if (!validate_handle(handle,number,&number,&sectors)) {CloseHandle(handle);goto denied;}
    lock_slots();
    for(i=0;i<32;i++)if(!slots[i].handle)break;
    if(i<32) {slots[i].handle=handle;slots[i].number=number;slots[i].sectors=sectors;}
    unlock_slots();
    if(i==32) {CloseHandle(handle);goto denied;}
    trace_event("open-verified",number,sectors);
    return handle;
denied:
    lock_slots();completed_result_valid=0;unlock_slots();
    trace_event("open-denied",GetLastError(),0);
    SetLastError(ERROR_ACCESS_DENIED);return INVALID_HANDLE_VALUE;
}
static U32 known_handle(HANDLE handle,Slot *result) {
    U32 i,found=0;
    lock_slots();
    for(i=0;i<32;i++)if(slots[i].handle==handle && handle) {*result=slots[i];found=1;break;}
    unlock_slots();return found;
}
static BOOL WINAPI identify_backend(HANDLE handle,DWORD code,LPVOID input,DWORD input_size,
    LPVOID output,DWORD output_size,LPDWORD returned,LPOVERLAPPED overlapped) {
    Slot slot;NativePacket aligned_packet,original_packet;U8 *aligned_data=NULL,*destination;U32 i,original_buffer,kind;DWORD error;BOOL ok;
    if(!known_handle(handle,&slot) || code!=IOCTL_APTD || input_size!=40 || output_size!=40 || input!=output
        || overlapped) {lock_slots();completed_result_valid=0;unlock_slots();SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
    kind=diagnostic_command(input);
    if(!kind){lock_slots();completed_result_valid=0;unlock_slots();SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
    if(((NativePacket*)input)->transfer_length) {
        aligned_data=VirtualAlloc(NULL,((NativePacket*)input)->transfer_length,0x3000,4);
        if(!aligned_data){SetLastError(ERROR_NOT_ENOUGH_MEMORY);return FALSE;}
    }
    original_packet=aligned_packet=*(NativePacket*)input;original_buffer=aligned_packet.buffer;
    if(aligned_packet.flags&4)
        for(i=0;i<aligned_packet.transfer_length;i++)aligned_data[i]=((U8*)original_buffer)[i];
    /* The vendor's IDENTIFY request sets E0 in device/head. Convert only
     * this already validated EC request to the fixed A0 form verified on SATA.
     * No caller-supplied addressing mode reaches the real device. */
    if(kind==1)aligned_packet.current[5]=0xa0;
    aligned_packet.buffer=(U32)aligned_data;
#ifdef FACTORY_RUNTIME
    if(kind==F_PREFORMAT || kind==F_ERASE)aligned_packet.timeout=600;
#endif
    ok=DeviceIoControl(handle,code,&aligned_packet,40,&aligned_packet,40,returned,NULL);
    error=GetLastError();
    lock_slots();
    completed_result_valid=ok && returned && *returned>=40
        && aligned_packet.transfer_length==((NativePacket*)input)->transfer_length
        && !(aligned_packet.current[6]&0x88);
    if(completed_result_valid){completed_result=aligned_packet;completed_result.buffer=0;}
    unlock_slots();
    if(ok && (!returned || *returned<40 || aligned_packet.transfer_length!=((NativePacket*)input)->transfer_length
       || (aligned_packet.current[6]&0xa9) || !(aligned_packet.current[6]&0x40))) {ok=FALSE;error=ERROR_INVALID_DATA;}
#ifdef FACTORY_RUNTIME
    if(ok && !factory_after(kind,&original_packet,aligned_data)){ok=FALSE;error=ERROR_INVALID_DATA;}
    if(ok && kind==1) {
        U32 valid;
        if(factory_jumped)valid=identify_field(aligned_data,54,40,RECOVERY_MODEL)
          && identify_field(aligned_data,46,8,"SBFM61.3") && identify_field(aligned_data,20,20,RECOVERY_SERIAL)
          && read32(aligned_data+200)==FACTORY_SECTORS && !read32(aligned_data+204);
        else valid=identify_field(aligned_data,54,40,"SATABURN   SB") && identify_field(aligned_data,46,8,"SBBM61.3");
        if(!valid){ok=FALSE;error=ERROR_INVALID_DATA;}
        else if(factory_jumped){lock_slots();slots[0].sectors=FACTORY_SECTORS;unlock_slots();}
    }
    if(!ok){factory_failed=1;factory_armed=0;lock_slots();completed_result_valid=0;unlock_slots();}
#endif
    if(ok) {
        destination=(U8*)original_buffer;
        if(aligned_packet.flags&2)
            for(i=0;i<aligned_packet.transfer_length;i++)destination[i]=aligned_data[i];
#ifdef NAND_DIAGNOSTICS
        if(kind==3)save_nand_id(aligned_data);
#endif
        aligned_packet.buffer=original_buffer;*(NativePacket*)output=aligned_packet;
    }
    if(aligned_data)VirtualFree(aligned_data,0,0x8000);
#ifdef NAND_DIAGNOSTICS
    if(kind==2)InterlockedExchange(&diagnostic_ap_key,ok?1:0);
#endif
    trace_event("diagnostic-result",kind,ok?0:error);SetLastError(error);return ok;
}
static BOOL TargetDeviceIoControl(HANDLE handle,DWORD code,LPVOID input,DWORD input_size,
    LPVOID output,DWORD output_size,LPDWORD returned,LPOVERLAPPED overlapped) {
    Slot slot;NativePacket packet,snapshot;U32 i,length,buffer,valid;U8 *sptd,*data;
    if(returned)*returned=0;
    trace_event("ioctl-request",code,input_size);
    if(code==IOCTL_SPTD && input && input_size>=44)
    {   trace_event("cdb-prefix",read32((U8*)input+28),read32((U8*)input+32));
        trace_event("cdb-tail-and-header",read32((U8*)input+36),read32((U8*)input+4)); }
    if(!known_handle(handle,&slot) || overlapped)goto denied;
    if(code==0x2d1080 && output && output_size>=12) {
        write32(output,7);write32((U8*)output+4,slot.number);write32((U8*)output+8,0xffffffff);
        if(returned)*returned=12;SetLastError(0);return TRUE;
    }
    if(code==IOCTL_APTD)return identify_backend(handle,code,input,input_size,output,output_size,returned,NULL);
    if(code==IOCTL_SPTD && input && input==output && input_size==output_size && input_size>=44) {
        sptd=input;length=read32(sptd+12);buffer=read32(sptd+20);data=(U8*)buffer;
        if(sptd[28]==0xa1 && sptd[29]==0x1e) {
            lock_slots();snapshot=completed_result;valid=completed_result_valid;unlock_slots();
            if(!BridgeReturnResponse(sptd,input_size,&snapshot,valid))goto denied;
            if(returned)*returned=output_size;SetLastError(0);return TRUE;
        }
        if(sptd[0]==44 && !sptd[1] && sptd[8]==1 && data && length<=4096) {
            if(sptd[6]==6 && sptd[28]==0x12 && !sptd[29] && !sptd[30] && !sptd[31]
                && sptd[32]<=length && !sptd[33] && length>=36) {
                zero_bytes(data,length);data[2]=5;data[3]=2;data[4]=31;
                for(i=8;i<36;i++)data[i]=' ';
                data[8]='A';data[9]='T';data[10]='A';
                data[16]='P';data[17]='S';data[18]='3';data[19]='1';data[20]='1';data[21]='1';
                data[32]='0';data[33]='2';data[34]='.';data[35]='0';
                sptd[2]=0;if(returned)*returned=output_size;SetLastError(0);return TRUE;
            }
            if((sptd[6]==10 || sptd[6]==12 || sptd[6]==16) && sptd[28]==0x25 && length>=8) {
                for(i=29;i<28+sptd[6];i++)if(sptd[i])goto denied;
                zero_bytes(data,length);length=slot.sectors-1;
                data[0]=length>>24;data[1]=length>>16;data[2]=length>>8;data[3]=length;
                data[6]=2;sptd[2]=0;if(returned)*returned=output_size;SetLastError(0);return TRUE;
            }
        }
    }
    if(code==IOCTL_SPTD && input && input==output && input_size==output_size
        && !BridgePrepareSPTD(input,input_size,&packet) && diagnostic_command(&packet))
        return BridgeDispatchVirtual(handle,code,input,input_size,output,output_size,returned,NULL,identify_backend);
denied:
    lock_slots();completed_result_valid=0;unlock_slots();
    trace_event("ioctl-denied",code,0);
    SetLastError(ERROR_ACCESS_DENIED);return FALSE;
}
static BOOL TargetCloseHandle(HANDLE handle) {
    U32 i;
#ifdef FACTORY_RUNTIME
    /* Only the process owns the pinned disk handle. Windows releases it at
     * process exit; per-command vendor closes must not break VUC sequencing. */
    Slot current;if(known_handle(handle,&current))return TRUE;
#endif
    lock_slots();for(i=0;i<32;i++)if(slots[i].handle==handle){zero_bytes(&slots[i],sizeof(Slot));break;}unlock_slots();
    return CloseHandle(handle);
}

/* Replacement for the original no-argument scanner (this pointer supplied by
 * an x86 adapter stub). Writes only the arrays cleared by its original reset.
 */
API U32 WINAPI TargetPopulateDiscovery(U8 *object) {
    WCHAR path[512];U32 devinst,number,i;HANDLE handle;Slot slot;
    trace_event("scanner-enter",0,0);
    if(!object)return 0;
    zero_bytes(object+0x38,96);zero_bytes(object+0xb0,24);write32(object+0xc4,0);
    zero_bytes(object+0xc8,0x3000);zero_bytes(object+0x60c8,96);
    if(!TargetFind(path,512,&devinst)){trace_event("scanner-not-found",0,0);return 0;}
    handle=TargetOpenRequested(path,0xc0000000,3,NULL,3,0x80,NULL);
    if(handle==INVALID_HANDLE_VALUE)return 0;
    if(!known_handle(handle,&slot) || !slot.number) {TargetCloseHandle(handle);return 0;}
    for(i=0;path[i] && i<511;i++) {
        if(path[i]>127) {TargetCloseHandle(handle);return 0;}
        object[0xc8+i]=(U8)path[i];
    }
    object[0xc8+i]=0;write32(object+0x60c8,slot.number);write32(object+0x38,devinst);
    object[0xb0]=0;trace_event("scanner-populated",slot.number,devinst);TargetCloseHandle(handle);return 1;
}
