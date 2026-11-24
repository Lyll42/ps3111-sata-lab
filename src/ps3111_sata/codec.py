"""Offline SAT12/SAT16 -> native ATA codec and exact-plan adapter.

This module has no OS/device API. An explicit injected backend is required.
Neither a DLL nor a physical disk is ever opened here.
"""
from dataclasses import dataclass
import struct

@dataclass(frozen=True)
class Request:
    current: bytes
    previous: bytes
    flags: int
    size: int
    direction: str
    ck_cond: bool

def decode(cdb, data):
    cdb=bytes(cdb)
    if len(cdb)==12 and cdb[0]==0xa1:
        if cdb[1]&1: raise ValueError('SAT12 EXTEND unsupported')
        current=bytes([cdb[3],cdb[4],cdb[5],cdb[6],cdb[7],cdb[8],cdb[9],0])
        previous=bytes(8); extend=False
    elif len(cdb)==16 and cdb[0]==0x85:
        extend=bool(cdb[1]&1)
        current=bytes([cdb[4],cdb[6],cdb[8],cdb[10],cdb[12],cdb[13],cdb[14],0])
        previous=bytes([cdb[3],cdb[5],cdb[7],cdb[9],cdb[11],0,0,0]) if extend else bytes(8)
        if not extend and any(cdb[i] for i in (3,5,7,9,11)):
            raise ValueError('Upper registers without EXTEND')
    else: raise ValueError('Only SAT12/SAT16 ATA commands are supported')
    if cdb[1]&0xe0 or cdb[2]&0xd0 or cdb[-1]:
        raise ValueError('Unsupported SAT flags/control')
    protocol=(cdb[1]>>1)&15
    if protocol not in (3,4,5,10,11): raise ValueError('Unsupported non-data/PIO/UDMA protocol')
    # MTTool has sometimes nonzero transfer flags on non-data commands;
    # the protocol, not the sector-count register, determines no-data.
    if protocol==3:
        if data not in (None,0,b''): raise ValueError('Data supplied to non-data command')
        size=0; direction='none'
    else:
        if cdb[2]&3!=2 or not cdb[2]&4:
            raise ValueError('Only sector-count transfers in 512-byte blocks supported')
        direction='in' if protocol in (4,10) else 'out'
        if bool(cdb[2]&8)!=(direction=='in'): raise ValueError('Direction flags conflict')
        count=current[1]+(previous[1]<<8 if extend else 0)
        size=(count or (65536 if extend else 256))*512
        if direction=='in' and (type(data) is not int or data!=size):
            raise ValueError('Read length mismatch')
        if direction=='out' and (not isinstance(data,bytes) or len(data)!=size):
            raise ValueError('Write payload mismatch')
    # Microsoft Windows-driver-samples/storahci/common.h: IN=2, OUT=4,
    # 48BIT_COMMAND=8, USE_DMA=16. DMA and 48-bit addressing are independent.
    flags=1 | (2 if direction=='in' else 4 if direction=='out' else 0) | (8 if extend else 0) | (16 if protocol in (10,11) else 0)
    return Request(current,previous,flags,size,direction,bool(cdb[2]&0x20))

def packet_bytes(request, buffer_address, timeout=30, bits=32):
    if type(timeout) is not int or not 1<=timeout<=600: raise ValueError('Invalid timeout')
    if bits==32:
        header=struct.pack('<HHBBBBIIII',40,request.flags,0,0,0,0,request.size,timeout,0,buffer_address)
    elif bits==64:
        header=struct.pack('<HHBBBBIII4xQ',48,request.flags,0,0,0,0,request.size,timeout,0,buffer_address)
    else: raise ValueError('Unknown ABI')
    return header+request.previous+request.current

def sense_bytes(request, current):
    prev=request.previous
    return bytes([0x72,1,0,0x1d,0,0,0,14,9,12,bool(request.flags&8),current[0],
                  prev[1],current[1],prev[2],current[2],prev[3],current[3],prev[4],current[4],current[5],current[6]])

class PlannedAdapter:
    """Only the supplied, exact ordered plan may reach the injected backend.

    This class is an offline prototype. It is not wired to DeviceIoControl.
    """
    def __init__(self,plan,backend):
        self.plan=tuple((bytes(c),d) for c,d in plan)
        self.backend=backend; self.position=0; self.failed=False
    def command(self,cdb,data,timeout):
        if self.failed: raise RuntimeError('Adapter stopped after a failure')
        try:
            if self.position>=len(self.plan) or (bytes(cdb),data)!=self.plan[self.position]:
                raise RuntimeError('Command/payload differs from approved plan')
            request=decode(cdb,data)
            current,raw=self.backend(request,data,timeout)
            if len(current)!=8 or current[6]&0xa9 or not current[6]&0x40:
                raise OSError('ATA command did not complete successfully')
            if request.direction=='in' and (not isinstance(raw,bytes) or len(raw)!=request.size):
                raise OSError('Incomplete native read')
            self.position+=1
            return (2 if request.ck_cond else 0,
                    raw if request.direction=='in' else None,
                    sense_bytes(request,current) if request.ck_cond else None)
        except Exception:
            self.failed=True
            raise
