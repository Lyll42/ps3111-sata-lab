/* OFFLINE ONLY: synthetic device handle, no kernel disk open or ioctl. */
#define TARGET_RUNTIME
#define CAPTURE_FILE_TRACE
#include "sata_guard.c"
#include <stdio.h>
#include <string.h>
#include "capture_identify.h"
#include "capture_nand.h"
static void CaptureFilePathA(LPCSTR path) {FILE *f=fopen("offline-file-opens.log","a");if(f){fprintf(f,"%s\n",path);fclose(f);}}
static void CaptureFilePathW(LPCWSTR path) {FILE *f=fopen("offline-file-opens.log","a");if(f){fprintf(f,"%ls\n",path);fclose(f);}}
static U32 payload_number=0;
static U32 register_address[256],register_value[256];
static U32 register_count=0,parameter_address=0,registers_loaded=0;
static U32 burner_active=0;
static void load_register_fixture(void) {
    FILE *file;U32 count,i,row[2];
    if(registers_loaded)return;registers_loaded=1;
    file=fopen("offline-start-in-burner.flag","rb");if(file){burner_active=1;fclose(file);}
    file=fopen("offline-registers.bin","rb");if(!file)return;
    if(fread(&count,4,1,file)==1 && count<=256)for(i=0;i<count;i++) {
        if(fread(row,4,2,file)!=2)break;
        register_address[register_count]=row[0];register_value[register_count++]=row[1];
    }
    fclose(file);
}
static U32 parameter_size=0;
typedef struct {U32 address,size;} Written;
static Written written_ranges[256];static U32 written_count=0;
typedef struct {U32 lba,size;char filename[80];} DMAWrite;
static DMAWrite dma_writes[1024];static U32 dma_write_count=0;
static DMAWrite normal_writes[256];static U32 normal_write_count=0;
static U8 virtual_info_block[512];static U32 virtual_info_valid=0;
/* Compare captured encrypted RAM writes with the independently parsed firmware
 * file used in this offline case. Only exact matching ranges receive CRC OK. */
static U32 match_firmware_range(U32 address,const U8 *data,U32 size) {
    U32 count,i,row[3],j,part;U8 buffer[1024];FILE *layout=fopen("offline-iram-layout.bin","rb"),*fw;
    if(!layout)return 0;
    if(fread(&count,4,1,layout)!=1 || count>8){fclose(layout);return 0;}
    for(i=0;i<count;i++) {
        if(fread(row,4,3,layout)!=3)break;
        if(address<row[0] || address-row[0]>row[1] || size>row[1]-(address-row[0]))continue;
        fw=fopen("offline-firmware.bin","rb");if(!fw)break;
        fseek(fw,row[2]+address-row[0],SEEK_SET);
        for(j=0;j<size;j+=part) {
            part=size-j;if(part>sizeof(buffer))part=sizeof(buffer);
            if(fread(buffer,1,part,fw)!=part || memcmp(buffer,data+j,part)){fclose(fw);fclose(layout);return 0;}
        }
        fclose(fw);fclose(layout);return 1;
    }
    fclose(layout);return 0;
}
static U32 complete_written_range(U32 address,U32 size) {
    U32 position=0,i,found;
    while(position<size) {
        found=0;
        for(i=0;i<written_count;i++)if(written_ranges[i].address==address+position && written_ranges[i].size<=size-position) {
            position+=written_ranges[i].size;found=1;break;
        }
        if(!found)return 0;
    }
    return 1;
}
static HANDLE TargetOpenRequested(LPCWSTR path,DWORD a,DWORD s,LPSECURITY_ATTRIBUTES x,DWORD c,DWORD f,HANDLE t) {
    static const WCHAR wanted[]=L"\\\\.\\PhysicalDrive1";U32 i=0;
    while(path && wanted[i] && wanted[i]==path[i])i++;
    if(!path || wanted[i] || path[i]){SetLastError(5);return INVALID_HANDLE_VALUE;}
    return (HANDLE)0x51531111;
}
static BOOL TargetCloseHandle(HANDLE h) {if(h==(HANDLE)0x51531111)return TRUE;return CloseHandle(h);}
static BOOL TargetDeviceIoControl(HANDLE h,DWORD code,LPVOID in,DWORD is,LPVOID out,DWORD os,LPDWORD returned,LPOVERLAPPED ov) {
    U8 *p=in,*data;U32 i,n;FILE *log;
    if(returned)*returned=0;
    if(h!=(HANDLE)0x51531111 || code!=IOCTL_SPTD || !in || in!=out || is!=os || is<44 || ov)goto deny;
    load_register_fixture();
    n=read32(p+12);data=(U8*)read32(p+20);
    log=fopen("offline-command-capture.log","a");
    if(log){fprintf(log,"cdb=");for(i=0;i<p[6] && i<16;i++)fprintf(log,"%02x",p[28+i]);fprintf(log," direction=%u bytes=%lu\n",p[8],n);fclose(log);}
    if(p[6]>=12 && p[28]==0xa1 && p[29]==8 && p[30]==14 && p[31]==0 && p[32]==1 && p[37]==0xec && n==512 && p[8]==1 && data) {
        if(burner_active) {
            FILE *id=fopen(burner_active==2?"offline-normal-identify.bin":"offline-burner-identify.bin","rb");
            if(!id)goto deny;
            i=fread(data,1,512,id);fclose(id);if(i!=512)goto deny;
        } else for(i=0;i<512;i++)data[i]=capture_identify[i];
    } else if(p[28]==0x25 && n>=8 && data && p[8]==1) {
        zero_bytes(data,n);
        if(burner_active==2){U32 last=468862127;data[0]=last>>24;data[1]=last>>16;data[2]=last>>8;data[3]=last;}
        else data[2]=0xa0;
        data[6]=2;
    } else if(p[6]>=12 && p[28]==0xa1 && p[29]==6 && p[30]==0 && p[31]==0 && p[32]==0x6f && p[33]==0xfe && p[34]==0xef && p[35]==0xfa && p[36]==0xaf && p[37]==0xe0 && !n && p[8]==2) {
        /* A simulated AP key success, exclusively on this synthetic handle. */
    } else if(p[6]==12 && p[28]==0xa1 && p[29]==8 && p[30]==14 && p[31]==0x15 && p[32]==1 && p[33]==0 && p[34]==0 && p[35]==0 && p[36]==0xa0 && p[37]==0x21 && n==512 && p[8]==1 && data) {
        for(i=0;i<512;i++)data[i]=capture_nand[i];
    } else if(p[8]==1 && n==512 && data && p[28]==0xa1 && p[31]==0x61 && p[37]==0x21) {
        U32 value=0;
        for(i=0;i<register_count;i++)if(register_address[i]==parameter_address){value=register_value[i];break;}
        log=fopen("offline-command-capture.log","a");
        if(log){fprintf(log,"SIMULATED_REGISTER address=%08lx value=%08lx initialized=%u\n",parameter_address,value,i<register_count);fclose(log);}
        zero_bytes(data,n);write32(data,parameter_address);write32(data+4,value);
    } else if(p[8]==1 && n==512 && data && p[28]==0xa1 && p[31]==0x30 && p[37]==0x21
              && complete_written_range(parameter_address,parameter_size)) {
        zero_bytes(data,n);write32(data+8,0x55aa55aa);
    } else if(p[8]==1 && n==512 && data && p[28]==0xa1 && p[31]==0x13 && p[37]==0x21 && burner_active) {
        FILE *fixture=fopen(burner_active==2?"offline-normal-system-info.bin":"offline-system-info.bin","rb");
        if(!fixture)goto deny;
        i=fread(data,1,n,fixture);fclose(fixture);if(i!=n)goto deny;
    } else if(p[8]==1 && n==512 && data && p[28]==0xa1 && p[31]==0x14 && p[37]==0x21 && p[35]<64) {
        U32 ce=p[35];zero_bytes(data,n);for(i=0;i<8;i++)data[i]=capture_nand[ce*8+i];
    } else if(p[8]==1 && n==512 && data && p[28]==0xa1 && p[31]==0xa8 && p[37]==0x21 && !p[33] && !p[34] && !p[35] && !(p[36]&15)) {
        FILE *fixture=fopen("offline-nand-limits-lba0.bin","rb");if(!fixture)goto deny;
        i=fread(data,1,n,fixture);fclose(fixture);if(i!=n)goto deny;
    } else if(p[8]==1 && n==512 && data && p[28]==0xa1 && p[31]==0xa8 && p[37]==0x21 && !p[33] && p[34]==1 && !p[35] && !(p[36]&15)
              && parameter_address==468862128) {
        FILE *fixture=fopen("offline-nand-limits-240.bin","rb");if(!fixture)goto deny;
        i=fread(data,1,n,fixture);fclose(fixture);if(i!=n)goto deny;
    } else if(p[8]==1 && n==2048 && data && p[28]==0xa1 && (p[31]==0x74 || p[31]==0x75) && p[37]==0x21) {
        /* Hypothetical healthy-device branch only: not a measurement of NAND.
         * This response is never part of either physical diagnostic DLL. */
        zero_bytes(data,n);
        log=fopen("offline-command-capture.log","a");
        if(log){fprintf(log,"SYNTHETIC_BAD_COUNTS: zero, offline branch exploration only\n");fclose(log);}
    } else if(p[8]==1 && n==14 && data && p[28]==0xa1 && p[29]==0x1e) {
        /* Completed-command return descriptor of this virtual device only. */
        zero_bytes(data,n);data[0]=9;data[1]=12;data[12]=0xa0;data[13]=0x50;
    } else if(p[8]==1 && n==4096 && data && p[28]==0xa1 && p[31]==0xda && p[37]==0x21) {
        /* Erased virtual product history, not a read of the user's history. */
        for(i=0;i<n;i++)data[i]=0xff;
    } else if(p[8]==1 && n==512 && data && p[28]==0xa1 && p[31]==0x28 && p[37]==0x21 && virtual_info_valid) {
        for(i=0;i<512;i++)data[i]=virtual_info_block[i];
    } else if(p[8]==1 && data && p[28]==0xa1 && p[31]==0x31 && p[37]==0x21 && normal_write_count) {
        FILE *written;
        if(!p[35] && n==4096)written=fopen("offline-flash-header.bin","rb");
        else {
            U32 lba=p[33]|((U32)p[34]<<8)|((U32)p[35]<<16);
            for(i=0;i<normal_write_count;i++)if(normal_writes[i].lba==lba && normal_writes[i].size==n)break;
            if(i==normal_write_count)goto deny;
            written=fopen(normal_writes[i].filename,"rb");
        }
        if(!written)goto deny;i=fread(data,1,n,written);fclose(written);if(i!=n)goto deny;
    } else if(p[8]==1 && data && p[28]==0xa1 && p[29]==0x14 && p[37]==0xc8) {
        U32 lba=p[33]|((U32)p[34]<<8)|((U32)p[35]<<16)|((U32)(p[36]&15)<<24);FILE *written;
        for(i=0;i<dma_write_count;i++)if(dma_writes[i].lba==lba && dma_writes[i].size==n)break;
        if(i==dma_write_count)goto deny;
        written=fopen(dma_writes[i].filename,"rb");if(!written)goto deny;
        i=fread(data,1,n,written);fclose(written);if(i!=n)goto deny;
    } else if(p[8]==0 && data && n && n<=16*1024*1024) {
        /* Offline acknowledgement only. Preserve the requested payload for
         * review; no real device handle or kernel disk API exists here. */
        char filename[80];FILE *payload;
        sprintf(filename,"offline-payload-%05lu-feature-%02x.bin",++payload_number,p[31]);
        payload=fopen(filename,"wb");if(!payload)goto deny;
        fwrite(data,1,n,payload);fclose(payload);
        if(p[28]==0xa1 && p[31]==8 && n>=512 && !memcmp(data,"PhIsOn",6)) {
            for(i=0;i<512;i++)virtual_info_block[i]=data[i];virtual_info_valid=1;
        }
        if(p[28]==0xa1 && p[29]==0x16 && p[37]==0xca && dma_write_count<1024) {
            DMAWrite *entry=&dma_writes[dma_write_count++];entry->size=n;
            entry->lba=p[33]|((U32)p[34]<<8)|((U32)p[35]<<16)|((U32)(p[36]&15)<<24);
            strcpy(entry->filename,filename);
        }
        if(p[28]==0xa1 && p[31]==0x82 && p[37]==0x31 && normal_write_count<256) {
            DMAWrite *entry=&normal_writes[normal_write_count++];entry->size=n;
            entry->lba=p[33]|((U32)p[34]<<8)|((U32)p[35]<<16);strcpy(entry->filename,filename);
        }
        if(p[28]==0xa1 && p[31]==0x24 && n>=8){parameter_address=read32(data);parameter_size=read32(data+4);}
        if(p[28]==0xa1 && p[31]==0x40 && p[34]==1 && n==parameter_size
           && written_count<256 && match_firmware_range(parameter_address,data,n)) {
            written_ranges[written_count].address=parameter_address;written_ranges[written_count++].size=n;
        }
        if(p[28]==0xa1 && p[31]==0x60 && n>=8) {
            U32 address=read32(data);
            for(i=0;i<register_count;i++)if(register_address[i]==address)break;
            if(i<256){register_address[i]=address;register_value[i]=read32(data+4);if(i==register_count)register_count++;}
        }
        if(p[28]==0xa1 && p[31]==0x0f)burner_active=normal_write_count?2:1;
    } else if(p[8]==2 && !n) {
        /* Simulated no-data acknowledgement; never forwarded anywhere. */
        if(p[28]==0xa1 && p[31]==0x0f)burner_active=normal_write_count?2:1;
    } else goto deny;
    p[2]=0;if(returned)*returned=os;SetLastError(0);return TRUE;
deny:
    SetLastError(5);return FALSE;
}

API U32 __cdecl CaptureBuildId(void) {return 0x53494d31;}
