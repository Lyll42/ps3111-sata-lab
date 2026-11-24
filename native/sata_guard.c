/* Laboratory build: all raw device opens and IOCTLs are blocked.
 * Conversion is pure memory-to-memory; no function enables hardware access.
 * Do not remove the device gate before target validation and integration review.
 */
#include <windows.h>

#define API __declspec(dllexport)
#define IOCTL_SPTD 0x4d014
#define IOCTL_APTD 0x4d030
#ifdef TARGET_RUNTIME
static HANDLE TargetOpenRequested(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
static BOOL TargetDeviceIoControl(HANDLE,DWORD,LPVOID,DWORD,LPVOID,DWORD,LPDWORD,LPOVERLAPPED);
static BOOL TargetCloseHandle(HANDLE);
#endif
typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned long U32;

typedef struct {
    U16 length, flags;
    U8 path, target, lun, reserved_byte;
    U32 transfer_length, timeout, reserved_long, buffer;
    U8 previous[8], current[8];
} NativePacket;

static void zero_bytes(void *ptr, U32 count) {
    U8 *p = ptr;
    while (count--) *p++ = 0;
}

/* Returns 0 on success; otherwise a stable validation error code.
 * data_direction: 2 none, 1 input, 0 output (Windows SPTD convention).
 */
API U32 __cdecl BridgeConvert(const U8 *cdb, U32 length, U32 data_length,
    U32 data_direction, U32 buffer, U32 timeout, NativePacket *out) {
    U32 protocol, extend, count, expected, i;
    if (!cdb || !out || (length != 12 && length != 16)) return 1;
    zero_bytes(out, sizeof(*out));
    if (length == 12 && cdb[0] == 0xa1 && !(cdb[1] & 1)) {
        for (i=0; i<7; i++) out->current[i] = cdb[i+3];
        extend = 0;
    } else if (length == 16 && cdb[0] == 0x85) {
        extend = cdb[1] & 1;
        for (i=0; i<5; i++) {
            out->current[i] = cdb[4+i*2];
            if (extend) out->previous[i] = cdb[3+i*2];
            else if (cdb[3+i*2]) return 2;
        }
        out->current[5] = cdb[13]; out->current[6] = cdb[14];
    } else return 3;
    if ((cdb[1] & 0xe0) || (cdb[2] & 0xd0) || cdb[length-1]) return 4;
    protocol = (cdb[1] >> 1) & 15;
    if (protocol == 3) {
        if (data_length || data_direction != 2) return 5;
    } else if (protocol == 4 || protocol == 5 || protocol == 10 || protocol == 11) {
        if ((cdb[2] & 7) != 6 || !!(cdb[2] & 8) != (protocol == 4 || protocol == 10)) return 6;
        if (data_direction != (protocol == 4 || protocol == 10 ? 1 : 0)) return 7;
        count = out->current[1] | (extend ? ((U32)out->previous[1] << 8) : 0);
        expected = (count ? count : (extend ? 65536 : 256)) * 512;
        if (data_length != expected || !buffer) return 8;
    } else return 9;
    if (!timeout || timeout > 600) return 10;
    out->length = sizeof(*out);
    /* ntddscsi.h / Microsoft's storahci sample: 48BIT=8, USE_DMA=16. */
    out->flags = 1 | (extend ? 8 : 0) | (protocol == 10 || protocol == 11 ? 16 : 0)
        | (protocol == 4 || protocol == 10 ? 2 : protocol == 5 || protocol == 11 ? 4 : 0);
    out->transfer_length = data_length; out->timeout = timeout;
    out->buffer = data_length ? buffer : 0;
    return 0;
}

/* Proposed read-only hardware policy: only exact IDENTIFY, never VUC/erase.
 * Laboratory hooks below still block even this command.
 */
API U32 __cdecl BridgeIdentifyOnly(const NativePacket *p) {
    U32 i;
    if (!p || p->length != 40 || p->flags != 3 || p->transfer_length != 512 || !p->buffer) return 0;
    for (i=0; i<8; i++) if (p->previous[i]) return 0;
    if (p->current[0] || p->current[1] != 1 || p->current[2] || p->current[3]
        || p->current[4] || (p->current[5] != 0 && p->current[5] != 0xa0 && p->current[5] != 0xe0)
        || p->current[6] != 0xec || p->current[7]) return 0;
    return 1;
}

static U32 read32(const U8 *p) { return p[0] | ((U32)p[1]<<8) | ((U32)p[2]<<16) | ((U32)p[3]<<24); }
static void write32(U8 *p, U32 value) {
    p[0]=value; p[1]=value>>8; p[2]=value>>16; p[3]=value>>24;
}

/* SCSI_PASS_THROUGH_DIRECT (32-bit ABI) -> ATA_PASS_THROUGH_DIRECT.
 * These two functions only transform caller memory; they do not perform I/O.
 */
API U32 __cdecl BridgePrepareSPTD(const U8 *sptd, U32 size, NativePacket *out) {
    U32 cdb_length,i;
    if (!sptd || size < 44 || sptd[0]!=44 || sptd[1]) return 11;
    cdb_length=sptd[6];
    /* GetIdentifyInfoBuf in this vendor DLL declares a zero-padded SAT12
     * request as 16 bytes. Accept that representation only with zero padding;
     * BridgeConvert still validates the complete 12-byte command and policy
     * remains independently restricted to IDENTIFY on real hardware. */
    if(cdb_length==16 && sptd[28]==0xa1) {
        for(i=40;i<44;i++)if(sptd[i])return 12;
        cdb_length=12;
    }
    if (cdb_length!=12 && cdb_length!=16) return 12;
    return BridgeConvert(sptd+28,cdb_length,read32(sptd+12),sptd[8],read32(sptd+20),read32(sptd+16),out);
}

API U32 __cdecl BridgeCompleteSPTD(U8 *sptd, U32 size, const NativePacket *native,
    U32 windows_success, U32 returned) {
    U32 expected, failed, ck_cond, offset, i;
    U8 *sense;
    if (!sptd || !native || size<44) return 0;
    expected=read32(sptd+12);
    sptd[2]=2; /* Remain failed until all checks pass. */
    if (!windows_success || returned<40 || native->length!=40 || native->transfer_length!=expected) return 0;
    failed=(native->current[6]&0xa9) || !(native->current[6]&0x40);
    ck_cond=sptd[30]&0x20;
    if (failed || ck_cond) {
        offset=read32(sptd+24);
        if (sptd[7]<22 || offset<44 || offset>size || size-offset<22) return 0;
        sense=sptd+offset;
        zero_bytes(sense,22);
        sense[0]=0x72; sense[1]=failed ? 4 : 1; sense[3]=0x1d;
        sense[7]=14; sense[8]=9; sense[9]=12; sense[10]=!!(native->flags&8);
        sense[11]=native->current[0];
        for (i=1;i<5;i++) { sense[10+i*2]=native->previous[i]; sense[11+i*2]=native->current[i]; }
        sense[20]=native->current[5]; sense[21]=native->current[6];
        sptd[7]=22;
    } else sptd[2]=0;
    write32(sptd+12,native->transfer_length);
    return 1;
}

typedef BOOL (WINAPI *VirtualBackend)(HANDLE,DWORD,LPVOID,DWORD,LPVOID,DWORD,LPDWORD,LPOVERLAPPED);

/* Vendor protocol15 reads a 14-byte ATA Return Descriptor. Windows supplies
 * these task-file fields at synchronous command completion. This function
 * exposes only that completed result snapshot; never manufactures READY and
 * rejects unfinished BSY/DRQ states rather than emulating live polling. */
API U32 __cdecl BridgeReturnResponse(U8 *sptd,U32 size,const NativePacket *completed,U32 valid) {
    U32 i;U8 *data;
    if(!valid || !sptd || !completed || size<44 || completed->length!=40
       || (completed->current[6]&0x88) || sptd[0]!=44 || sptd[1] || sptd[8]!=1
       || (sptd[6]!=12 && sptd[6]!=16) || read32(sptd+12)!=14
       || sptd[28]!=0xa1 || sptd[29]!=0x1e || !read32(sptd+20))return 0;
    for(i=30;i<28+sptd[6];i++)if(sptd[i])return 0;
    data=(U8*)read32(sptd+20);zero_bytes(data,14);data[0]=9;data[1]=12;
    data[2]=!!(completed->flags&8);data[3]=completed->current[0];
    for(i=1;i<5;i++){data[i*2+2]=completed->previous[i];data[i*2+3]=completed->current[i];}
    data[12]=completed->current[5];data[13]=completed->current[6];sptd[2]=0;return 1;
}

/* Integration entry for an explicitly injected virtual backend.
 * GuardDeviceIoControl does NOT call this function: hardware remains blocked.
 * The laboratory emulator supplies the callback; there is no native I/O backend.
 */
API BOOL WINAPI BridgeDispatchVirtual(HANDLE handle,DWORD code,LPVOID input,DWORD input_size,
    LPVOID output,DWORD output_size,LPDWORD returned,LPOVERLAPPED overlapped,VirtualBackend backend) {
    NativePacket native;
    DWORD native_returned=0;
    BOOL ok;
    if (returned) *returned=0;
    if (!backend || code!=IOCTL_SPTD || !input || input!=output || input_size!=output_size || overlapped
        || BridgePrepareSPTD(input,input_size,&native)) {
        SetLastError(ERROR_INVALID_PARAMETER);return FALSE;
    }
    ok=backend(handle,IOCTL_APTD,&native,sizeof(native),&native,sizeof(native),&native_returned,NULL);
    if (!ok) return FALSE; /* Preserve backend's last error. */
    if (!BridgeCompleteSPTD(output,output_size,&native,1,native_returned)) {
        SetLastError(ERROR_INVALID_DATA);return FALSE;
    }
    if (returned) *returned=output_size;
    SetLastError(0);return TRUE;
}

/* Detect device namespaces anywhere, including extended and NT namespace forms.
 * Rejecting rather than normalising prevents aliases from bypassing the gate.
 */
API U32 __cdecl BridgeIsDevicePathW(const WCHAR *path) {
    if (!path) return 1;
    if (path[0] == '\\') return 1;
    if (path[0] && path[1] == ':' && !path[2]) return 1;
    return 0;
}

API HANDLE WINAPI GuardCreateFileW(LPCWSTR path, DWORD access, DWORD sharing,
    LPSECURITY_ATTRIBUTES security, DWORD creation, DWORD flags, HANDLE template_file) {
    if (BridgeIsDevicePathW(path)) {
#ifdef TARGET_RUNTIME
        return TargetOpenRequested(path,access,sharing,security,creation,flags,template_file);
#else
        SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE;
#endif
    }
    return CreateFileW(path,access,sharing,security,creation,flags,template_file);
}

API HANDLE WINAPI GuardCreateFileA(LPCSTR path, DWORD access, DWORD sharing,
    LPSECURITY_ATTRIBUTES security, DWORD creation, DWORD flags, HANDLE template_file) {
    if (!path || path[0] == '\\' || (path[0] && path[1] == ':' && !path[2])) {
#ifdef TARGET_RUNTIME
        WCHAR wide[512]; U32 i=0;
        if (path) {
            while (path[i] && i<511 && (U8)path[i]<128) {wide[i]=(U8)path[i];i++;}
            if (!path[i]) {wide[i]=0;return TargetOpenRequested(wide,access,sharing,security,creation,flags,template_file);}
        }
#endif
        SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE;
    }
    return CreateFileA(path,access,sharing,security,creation,flags,template_file);
}

API BOOL WINAPI GuardDeviceIoControl(HANDLE handle, DWORD code, LPVOID in, DWORD in_size,
    LPVOID out, DWORD out_size, LPDWORD returned, LPOVERLAPPED overlapped) {
#ifdef TARGET_RUNTIME
    return TargetDeviceIoControl(handle,code,in,in_size,out,out_size,returned,overlapped);
#else
    if (returned) *returned = 0;
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
#endif
}

API BOOL WINAPI GuardCloseHandle(HANDLE handle) {
#ifdef TARGET_RUNTIME
    return TargetCloseHandle(handle);
#else
    return CloseHandle(handle);
#endif
}

API BOOL WINAPI GuardEnableWindow(HWND window,BOOL enable) {
#ifdef TARGET_RUNTIME
    if(GetDlgCtrlID(window)==0x430)enable=FALSE;
#endif
    return EnableWindow(window,enable);
}

API FARPROC WINAPI GuardGetProcAddress(HMODULE module, LPCSTR name) {
    /* Catch dynamically resolved raw-device entry points as well. */
    const char *s; U32 i;
    static const char *names[] = {"CreateFileA","CreateFileW","DeviceIoControl","GetProcAddress","CloseHandle","EnableWindow"};
    FARPROC replacements[6];
    replacements[0]=(FARPROC)GuardCreateFileA; replacements[1]=(FARPROC)GuardCreateFileW;
    replacements[2]=(FARPROC)GuardDeviceIoControl; replacements[3]=(FARPROC)GuardGetProcAddress;
    replacements[4]=(FARPROC)GuardCloseHandle;
    replacements[5]=(FARPROC)GuardEnableWindow;
    if ((U32)name > 65535) {
        for (i=0;i<6;i++) {
            s=names[i];
            { const char *n=name; while (*s && *n && *s==*n) {s++;n++;} if (!*s && !*n) return replacements[i]; }
        }
    }
    return GetProcAddress(module,name);
}

BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID reserved) { return TRUE; }
