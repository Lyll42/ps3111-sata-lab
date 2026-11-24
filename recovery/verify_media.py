"""Destructive, target-specific write/flush/read test. Defaults to an offline plan."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import time

from prepare import validate_profile
from ps3111_sata.identify import Target, Packet, identify_once, inventory, select_metadata, validate_result

BLOCK = 1024*1024


def regions(size, full=False):
    if size < BLOCK or size % 512:
        raise ValueError('Capacity must be sector aligned and at least 1 MiB')
    if full:
        for offset in range(0, size, BLOCK):
            yield offset, min(BLOCK, size-offset)
    else:
        for i in range(64):
            yield ((size-BLOCK)*i//63)//512*512, BLOCK


def pattern(offset, length, size):
    if offset < 0 or length <= 0 or offset % 512 or length % 512 or offset+length > size:
        raise ValueError('Invalid test range')
    return hashlib.shake_256(b'PS3111-SATA-VERIFY-v1'+struct.pack('<QI',offset,length)).digest(length)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', type=Path, required=True)
    parser.add_argument('--full', action='store_true', help='Write and read every logical sector; default is 64 distributed samples')
    parser.add_argument('--execute', action='store_true')
    parser.add_argument('--accept-data-loss', action='store_true')
    parser.add_argument('--output', type=Path, default=Path('build/media-test.json'))
    args = parser.parse_args()
    p = validate_profile(json.loads(args.profile.read_text(encoding='utf-8-sig')))
    size = p['sectors']*512
    report = {'executed':args.execute,'mode':'full' if args.full else 'sample','capacity_bytes':size,
              'written_bytes':0,'read_verified_bytes':0,'outcome':'offline_only'}
    if not args.execute:
        report['planned_bytes_per_phase'] = sum(n for _,n in regions(size,args.full))
        print(json.dumps(report,indent=2)); return
    if not args.accept_data_loss:
        parser.error('--execute requires --accept-data-loss')
    target = Target(p['disk_index'],p['normal_pnp_id'],p['model'],'SBFM61.3',p['serial'],p['sectors'])
    # Full ATA identity/capacity check; one exact target, never an automatic fallback.
    identify_once(target)
    before = select_metadata(target, inventory())
    if select_metadata(target, inventory()) != before:
        raise ValueError('Target changed before testing')
    # Require the owner to take this disk offline first; never modify another disk.
    shell = Path(os.environ['SystemRoot'])/'System32/WindowsPowerShell/v1.0/powershell.exe'
    command = f"$ErrorActionPreference='Stop'; Get-Disk -Number {target.index} | Select-Object Number,IsOffline,IsBoot,IsSystem | ConvertTo-Json -Compress"
    row = json.loads(subprocess.run([str(shell),'-NoProfile','-NonInteractive','-Command',command],capture_output=True,text=True,check=True,timeout=30).stdout)
    if row.get('Number') != target.index or row.get('IsOffline') is not True or row.get('IsBoot') is not False or row.get('IsSystem') is not False:
        raise ValueError('The exact target must be offline and must not be a boot/system disk')
    from ctypes import wintypes as w
    kernel = ctypes.WinDLL('kernel32',use_last_error=True)
    kernel.CreateFileW.argtypes = [w.LPCWSTR,w.DWORD,w.DWORD,ctypes.c_void_p,w.DWORD,w.DWORD,w.HANDLE]
    kernel.CreateFileW.restype = w.HANDLE
    kernel.CloseHandle.argtypes = [w.HANDLE]
    kernel.DeviceIoControl.argtypes = [w.HANDLE,w.DWORD,ctypes.c_void_p,w.DWORD,ctypes.c_void_p,w.DWORD,ctypes.POINTER(w.DWORD),ctypes.c_void_p]
    kernel.DeviceIoControl.restype = w.BOOL
    kernel.SetFilePointerEx.argtypes = [w.HANDLE,ctypes.c_longlong,ctypes.c_void_p,w.DWORD]
    kernel.SetFilePointerEx.restype = w.BOOL
    for name in ('ReadFile','WriteFile'):
        function = getattr(kernel,name)
        function.argtypes = [w.HANDLE,ctypes.c_void_p,w.DWORD,ctypes.POINTER(w.DWORD),ctypes.c_void_p]
        function.restype = w.BOOL
    kernel.FlushFileBuffers.argtypes = [w.HANDLE]; kernel.FlushFileBuffers.restype = w.BOOL
    handle = kernel.CreateFileW(target.path,0xc0000000,3,None,3,0x80,None)
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    backing = ctypes.create_string_buffer(BLOCK+65535)
    address = (ctypes.addressof(backing)+65535)&~65535
    start = time.monotonic(); last = start
    args.output.parent.mkdir(parents=True,exist_ok=True)
    try:
        # Recheck on the actual handle that will receive writes.
        packet = Packet(); packet.length = ctypes.sizeof(packet); packet.flags = 3
        packet.transfer = 512; packet.timeout = 10; packet.buffer = address
        packet.current[1] = 1; packet.current[5] = 0xa0; packet.current[6] = 0xec
        count = w.DWORD()
        if not kernel.DeviceIoControl(handle,0x4d030,ctypes.byref(packet),ctypes.sizeof(packet),ctypes.byref(packet),ctypes.sizeof(packet),ctypes.byref(count),None):
            raise ctypes.WinError(ctypes.get_last_error())
        validate_result(target,packet.current[6],count.value,packet.transfer,ctypes.string_at(address,512))
        for phase in ('write','read'):
            for offset,length in regions(size,args.full):
                expected = pattern(offset,length,size)
                if phase == 'write':
                    ctypes.memmove(address,expected,length)
                else:
                    ctypes.memset(address,0xa5,length)
                if not kernel.SetFilePointerEx(handle,offset,None,0):
                    raise ctypes.WinError(ctypes.get_last_error())
                count = w.DWORD(); function = kernel.WriteFile if phase == 'write' else kernel.ReadFile
                if not function(handle,address,length,ctypes.byref(count),None):
                    raise ctypes.WinError(ctypes.get_last_error())
                if count.value != length:
                    raise OSError('Partial '+phase+' at '+str(offset))
                if phase == 'read' and ctypes.string_at(address,length) != expected:
                    raise OSError('Data mismatch at '+str(offset))
                report['written_bytes' if phase == 'write' else 'read_verified_bytes'] += length
                if time.monotonic()-last >= 5:
                    report.update(phase=phase,offset=offset,elapsed_seconds=round(time.monotonic()-start,2))
                    args.output.write_text(json.dumps(report,indent=2)); last = time.monotonic()
            if phase == 'write' and not kernel.FlushFileBuffers(handle):
                raise ctypes.WinError(ctypes.get_last_error())
        report['outcome'] = 'write_flush_read_verified'
    except BaseException as error:
        report.update(outcome='stopped_on_error',error=str(error))
        raise
    finally:
        kernel.CloseHandle(handle)
        report['elapsed_seconds'] = round(time.monotonic()-start,2)
        args.output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__ == '__main__':
    main()
