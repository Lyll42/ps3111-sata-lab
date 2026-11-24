/* Separate experimental factory build. No simulated responses or counters.
 * Allow only the pinned SBFM61.3 code and the reviewed manufacturer's flow. */
#include "factory_data.h"
#ifndef FACTORY_SECTORS
#define FACTORY_SECTORS 468862128UL
#endif
enum { F_READ=10,F_PARAM,F_REG,F_MASK,F_SCAN,F_PREFORMAT,F_SEED,F_CODE,
       F_HEADER,F_JUMP,F_INFO,F_HISTORY,F_AUTH,F_REG_READ,F_HEADER_READ,F_CODE_READ,F_ERASE };
static U32 factory_failed=0,factory_controller=0,factory_nand=0,factory_unlocked=0;
static U32 factory_parameter=0,factory_preformatted=0,factory_seeded=0;
static U32 factory_armed=0;
static U32 factory_access_latch=0;
static U32 factory_erased=0;
static U32 factory_code_mask=0,factory_verified_mask=0,factory_header=0,factory_header_verified=0,factory_jumped=0;
#define FACTORY_CHUNKS ((sizeof(factory_code)+65535)/65536)
#define FACTORY_ALL_CHUNKS ((1UL<<FACTORY_CHUNKS)-1)
static U32 same_bytes(const U8 *a,const U8 *b,U32 count) {while(count--)if(*a++!=*b++)return 0;return 1;}
static U32 all_zero(const U8 *data,U32 count) {while(count--)if(*data++)return 0;return 1;}
static U32 factory_lba(const NativePacket *p) {return p->current[2]|((U32)p->current[3]<<8)|((U32)p->current[4]<<16);}
static U32 factory_chunk_size(U32 index) {
    U32 offset=index*65536;if(offset>=sizeof(factory_code))return 0;
    return sizeof(factory_code)-offset>65536?65536:sizeof(factory_code)-offset;
}
static U32 factory_header_crc(const U8 *data,U32 length) {
    U32 state=0x31113111,i,j;
    if(length!=4096 || data[0]!='I' || data[1]!='D')return 0;
    for(i=0;i<length-8;i+=4) {
        state^=read32(data+i);
        for(j=0;j<32;j++)state=(state<<1)^((state&0x80000000)?0x04c11db7:0);
    }
    return state==read32(data+length-4);
}
static U32 factory_command(const NativePacket *p) {
    const U8 *data;U32 i,feature,lba,index,size,address,value;
    if(!p || p->length!=40 || p->current[7] || p->path || p->target || p->lun
       || p->reserved_byte || p->reserved_long || !p->timeout || p->timeout>600)return 0;
    if(BridgeIdentifyOnly(p))return 1;
    for(i=0;i<8;i++)if(p->previous[i])return 0;
    if(p->flags==1 && !p->transfer_length && !p->buffer
       && same_bytes(p->current,(const U8*)"\x00\x6f\xfe\xef\xfa\xaf\xe0\x00",8))return 2;
    feature=p->current[0];size=p->transfer_length;lba=factory_lba(p);data=(const U8*)p->buffer;
    if(!data || !size || size>65536 || size%512 || p->current[1]!=size/512
       || p->current[5]!=0xa0 || (p->flags!=3 && p->flags!=5))return 0;
    if(p->current[6]==0x21 && p->flags==3) {
        if(factory_failed && feature!=0x13)return 0;
        if(feature==0x61 && size==512 && (lba==0x100 || lba==0x400)) {
            if(factory_parameter==0x04002440 || factory_parameter==0x0400247c
               || factory_parameter==0x040024bc || factory_parameter==0x0400303e)return F_REG_READ;
            return 0;
        }
        if(feature==0x31 && factory_header) {
            if(lba==0 && size==4096)return F_HEADER_READ;
            index=p->current[3];
            if(p->current[2]==0 && p->current[4]==1 && size==factory_chunk_size(index))return F_CODE_READ;
            return 0;
        }
        if((feature==0x13 && size==512 && (lba==0 || lba==1))
           || (feature==0x15 && size==512 && lba==0)
           || (feature==0x14 && size==512 && !(lba&0xffff) && p->current[4]<64)
           || (feature==0xa8 && size==512 && (lba==0 || lba==0x100))
           || ((feature==0x74 || feature==0x75) && size==2048 && lba==0)
           || (feature==0xda && size==4096 && lba==0)
           || (feature==0x28 && size==512 && lba==0)
           || (feature==0xc5 && size==512 && lba==0))return F_READ;
        return 0;
    }
    if(p->current[6]!=0x31 || p->flags!=5)return 0;
    if(feature==0x60 && size==512 && lba==0x400) {
        address=read32(data);value=read32(data+4);
        /* The only MMIO write is the documented eFuse read-access latch.
         * No eFuse cell, clock, reset or other register may be written. */
        if(address==0x04002400 && (value==0 || value==3) && all_zero(data+8,504)
           && (!factory_failed || value==0))return F_REG;
        return 0;
    }
    if(factory_failed)return 0;
    if(feature==0x24 && size==512 && (lba==0 || lba==0x330000)) {
        address=read32(data);value=read32(data+4);
        if(all_zero(data+8,504) && ((address==FACTORY_SECTORS && value==0)
          || ((address==0x04002440 || address==0x0400247c || address==0x040024bc
               || address==0x0400303e) && value==0)))return F_PARAM;
        return 0;
    }
    if((feature==0xc4 || feature==0xc6) && size==512 && lba==0
       && (feature==0xc6 || all_zero(data,512)))return F_AUTH;
    if(!factory_unlocked)return 0;
    /* Exact manufacturer ERASE_ALL type05 from the reviewed original flow.
     * Never available in preflight; one command per armed factory session. */
    if(feature==0x26 && size==512 && lba==0x050000 && factory_armed
       && factory_controller==7 && factory_nand && !factory_erased && !factory_preformatted
       && read32(data)==5 && all_zero(data+4,508))return F_ERASE;
    if(feature==0x16 && size==512 && lba==0 && read32(data)==0xffffffff && all_zero(data+4,508))return F_MASK;
    if(feature==0x70 && size==512 && lba==0 && all_zero(data+4,508)
       && (read32(data)==0xff007400 || read32(data)==0xff007500))return F_SCAN;
    if(feature==8 && size==1024 && lba==0x021000 && factory_armed && factory_controller==7 && factory_nand
       && !factory_preformatted && same_bytes(data,factory_preformat_data,1024))return F_PREFORMAT;
    if(feature==0x80 && size==sizeof(factory_seed) && lba==0 && factory_preformatted
       && same_bytes(data,factory_seed,size))return F_SEED;
    if(feature==0x82 && factory_seeded && p->current[2]==0 && p->current[4]==1) {
        index=p->current[3];
        if(size==factory_chunk_size(index) && same_bytes(data,factory_code+index*65536,size))return F_CODE;
        return 0;
    }
    if(feature==0x41 && size==512 && lba==0x2000 && factory_code_mask==FACTORY_ALL_CHUNKS && all_zero(data,512))return F_HEADER;
    if(feature==0x0f && size==512 && lba==0 && factory_header_verified
       && factory_verified_mask==FACTORY_ALL_CHUNKS && all_zero(data,512))return F_JUMP;
    if(feature==0x25 && size==512 && lba==0 && factory_jumped
       && same_bytes(data,factory_info,511) && (data[511]==0 || data[511]==1))return F_INFO;
    if(feature==0xdb && size==4096 && lba==0 && factory_jumped)return F_HISTORY;
    return 0;
}
static U32 factory_after(U32 kind,const NativePacket *request,const U8 *data) {
    U32 address,value,index=request->current[3],feature=request->current[0];
    if(kind==F_PARAM)factory_parameter=read32(data);
    if(kind==F_REG)factory_access_latch=read32(data+4);
    if(kind==F_REG_READ) {
        address=factory_parameter;if(read32(data)!=address)return 0;value=read32(data+4);
        if(address==0x0400247c){if(value!=RECOVERY_CONTROLLER_ID0)return 0;factory_controller|=1;}
        if(address==0x040024bc){if(value!=RECOVERY_CONTROLLER_ID1)return 0;factory_controller|=2;}
        if(address==0x0400303e){if(data[4]!=RECOVERY_CONTROLLER_REVISION)return 0;factory_controller|=4;}
    }
    if(kind==F_READ && feature==0x15) {
        if(!same_bytes(data,factory_nand_ids,512))return 0;factory_nand=1;
    }
    if(kind==F_READ && feature==0x13) {
        const U8 *version=data+FACTORY_SYSTEM_VERSION_OFFSET;
        if(!same_bytes(version,(const U8*)"BSMB163.",8) && !same_bytes(version,(const U8*)"BSMF163.",8))return 0;
        factory_unlocked=data[FACTORY_VUC_MODE_OFFSET]==3;
    }
    if(kind==F_PREFORMAT)factory_preformatted=1;
    if(kind==F_ERASE)factory_erased=1;
    if(kind==F_SEED){factory_seeded=1;factory_code_mask=0;factory_verified_mask=0;factory_header_verified=0;}
    if(kind==F_CODE)factory_code_mask|=1UL<<index;
    if(kind==F_HEADER)factory_header=1;
    if(kind==F_HEADER_READ){if(!factory_header_crc(data,request->transfer_length))return 0;factory_header_verified=1;}
    if(kind==F_CODE_READ) {
        if(!same_bytes(data,factory_code+index*65536,request->transfer_length))return 0;
        factory_verified_mask|=1UL<<index;
    }
    if(kind==F_JUMP){factory_jumped=1;factory_unlocked=0;}
    return 1;
}
API U32 __cdecl FactoryBuildId(void) {return 0x53313146;}
API U32 __cdecl FactoryCapacity(void) {return FACTORY_SECTORS;}
API U32 __cdecl FactoryStop(void) {factory_failed=1;factory_armed=0;return 1;}
API U32 __cdecl FactoryCleanupNeeded(void) {return factory_access_latch!=0;}
API U32 __cdecl FactoryArm(void) {
    if(factory_failed || factory_controller!=7 || !factory_nand || !factory_unlocked)return 0;
    factory_armed=1;return 1;
}
