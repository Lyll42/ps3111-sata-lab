"""Redirect transport imports in hash-pinned local vendor copies. No execution."""
import sys,struct,json,hashlib,argparse
from pathlib import Path
import pefile
ROOT=Path('.')
OUT=Path('.')
HOOKS={'CreateFileA':'_GuardCreateFileA@28','CreateFileW':'_GuardCreateFileW@28',
       'DeviceIoControl':'_GuardDeviceIoControl@32','GetProcAddress':'_GuardGetProcAddress@8',
       'CloseHandle':'_GuardCloseHandle@4'}
EXPECTED={'SSDMPTool.exe':'9ccafbd3f29294d6d9debe41c770d752d30d08caf4eb98f83cf83869f5fd2239',
          'MP_API.dll':'6f608d8d58afd35b071897e39af7369af9ecb052238db2f7e3b43374ad3f90c3'}

def align(n,a):return (n+a-1)//a*a
def build(name,diagnostic=False):
    original=(ROOT/name).read_bytes()
    if hashlib.sha256(original).hexdigest()!=EXPECTED[name]:raise RuntimeError('Source changed: '+name)
    pe=pefile.PE(data=original);base=pe.OPTIONAL_HEADER.ImageBase
    if pe.FILE_HEADER.Machine!=0x14c:raise RuntimeError('Expected x86 image')
    sec_alignment=pe.OPTIONAL_HEADER.SectionAlignment;file_alignment=pe.OPTIONAL_HEADER.FileAlignment
    rva=align(max(s.VirtualAddress+max(s.Misc_VirtualSize,s.SizeOfRawData) for s in pe.sections),sec_alignment)
    raw_offset=align(len(original),file_alignment)
    header_offset=pe.sections[-1].get_file_offset()+40
    if header_offset+40>min(s.PointerToRawData for s in pe.sections if s.PointerToRawData):
        raise RuntimeError('No room for an extra section header')
    # Group contiguous IAT slots with the same DLL. Keep original addresses;
    # split ranges only where a raw-device import changes DLL.
    entries=[];redirected=[]
    for module in pe.DIRECTORY_ENTRY_IMPORT:
        for imp in module.imports:
            dll=module.dll
            import_name=imp.name
            hook=(HOOKS.get(import_name.decode()) if dll.lower()==b'kernel32.dll' and import_name else None)
            if diagnostic and dll.lower()==b'user32.dll' and import_name==b'EnableWindow':hook='_GuardEnableWindow@8'
            if hook:
                redirected.append({'old':import_name.decode(),'new':hook,
                                   'iat_rva':hex(imp.address-base)})
                dll=b'TargetSataGuard.dll' if diagnostic else b'SataGuard.dll';import_name=hook.encode()
            entries.append((dll,import_name,imp.ordinal,imp.address-base))
    groups=[]
    for item in entries:
        if groups and groups[-1][-1][0]==item[0] and groups[-1][-1][3]+4==item[3]:
            groups[-1].append(item)
        else:groups.append([item])
    scanner=diagnostic and name=='SSDMPTool.exe'
    data=bytearray(20*(len(groups)+1+int(scanner)))
    def append(blob,alignment=1):
        data.extend(bytes(align(len(data),alignment)-len(data)))
        offset=len(data);data.extend(blob);return rva+offset
    strings={}
    scanner_iat=None
    if scanner:
        scanner_iat=append(bytes(8),4)
        extra=(b'TargetSataGuard.dll',b'_TargetPopulateDiscovery@4',None,scanner_iat)
        entries.append(extra);groups.append([extra])
    for i,group in enumerate(groups):
        dll=group[0][0];iat=group[0][3]
        if dll not in strings:strings[dll]=append(dll+b'\0')
        thunks=[]
        for _,import_name,ordinal,_ in group:
            thunks.append(append(b'\0\0'+import_name+b'\0',2) if import_name else 0x80000000|ordinal)
        ilt=append(struct.pack('<'+'I'*(len(thunks)+1),*thunks,0),4)
        # Windows also consults FirstThunk when deciding whether an import
        # range is empty. A newly allocated IAT must mirror the lookup table
        # on disk, rather than contain only zeroes. Existing slots already do.
        if scanner and iat==scanner_iat:
            struct.pack_into('<II',data,iat-rva,thunks[0],0)
        struct.pack_into('<IIIII',data,i*20,ilt,0,0,strings[dll],iat)
    scanner_patch=None
    if scanner:
        stub_rva=rva+align(len(data),4)
        # Position-independent x86 adapter: thiscall ECX -> stdcall argument.
        stub=b'\xe8\0\0\0\0\x58\x05'+struct.pack('<i',scanner_iat-(stub_rva+5))+b'\x51\xff\x10\xc3'
        append(stub,4)
        entry_rva=0x13440;offset=pe.get_offset_from_rva(entry_rva)
        if original[offset:offset+5]!=bytes.fromhex('558bec6aff'):raise RuntimeError('Unknown scanner prologue')
        scanner_patch={'entry_rva':entry_rva,'stub_rva':stub_rva,'iat_rva':scanner_iat}
    pe.FILE_HEADER.NumberOfSections+=1
    pe.OPTIONAL_HEADER.SizeOfImage=align(rva+len(data),sec_alignment)
    pe.OPTIONAL_HEADER.DATA_DIRECTORY[1].VirtualAddress=rva
    pe.OPTIONAL_HEADER.DATA_DIRECTORY[1].Size=20*(len(groups)+1)
    for index in (4,11):
        pe.OPTIONAL_HEADER.DATA_DIRECTORY[index].VirtualAddress=0
        pe.OPTIONAL_HEADER.DATA_DIRECTORY[index].Size=0
    pe.OPTIONAL_HEADER.CheckSum=0
    output=bytearray(pe.write());output.extend(bytes(raw_offset-len(output)))
    section=struct.pack('<8sIIIIIIHHI',b'.satlab\0',len(data),rva,align(len(data),file_alignment),raw_offset,0,0,0,0,0xe0000060 if scanner else 0xc0000040)
    output[header_offset:header_offset+40]=section
    output.extend(data);output.extend(bytes(align(len(output),file_alignment)-len(output)))
    if scanner:
        offset=pe.get_offset_from_rva(scanner_patch['entry_rva'])
        output[offset:offset+5]=b'\xe9'+struct.pack('<i',scanner_patch['stub_rva']-(scanner_patch['entry_rva']+5))
        # Main dialog 102, English resource, Start button 0x430. The source
        # hash and exact old style pin this change to the reviewed executable.
        for kind in pe.DIRECTORY_ENTRY_RESOURCE.entries:
            if kind.id!=5:continue
            for dialog in kind.directory.entries:
                if dialog.id!=102:continue
                for language in dialog.directory.entries:
                    if language.id!=1033:continue
                    button_offset=pe.get_offset_from_rva(language.data.struct.OffsetToData)+3336
                    if struct.unpack_from('<I',output,button_offset+20)[0]!=0x430 or struct.unpack_from('<I',output,button_offset+8)[0]!=0x50010000:
                        raise RuntimeError('Unknown Start button resource')
                    struct.pack_into('<I',output,button_offset+8,0x58010000)
    final=pefile.PE(data=bytes(output));final.OPTIONAL_HEADER.CheckSum=final.generate_checksum();output=final.write()
    checked=pefile.PE(data=output)
    if sum(len(module.imports) for module in checked.DIRECTORY_ENTRY_IMPORT)!=len(entries):
        raise RuntimeError('Incomplete import-table parse after patching')
    for module in checked.DIRECTORY_ENTRY_IMPORT:
        for imp in module.imports:
            if module.dll.lower()==b'kernel32.dll' and imp.name and imp.name.decode() in HOOKS:
                raise RuntimeError('Raw import remains')
    destination=OUT/(('SSDMPTool_SATA_DIAGNOSTIC.exe' if name=='SSDMPTool.exe' else name) if diagnostic else name+'.analisis.bin');destination.write_bytes(output)
    return {'name':name,'source_sha256':EXPECTED[name],'patched_sha256':hashlib.sha256(output).hexdigest(),
            'redirected_imports':redirected,'import_count':len(entries),'hardware_enabled':diagnostic,'identify_only':diagnostic,'scanner_patch':scanner_patch}

if __name__ == '__main__':
    parser=argparse.ArgumentParser(description='Patch local vendor copies for the SATA bridge; never executes them.')
    parser.add_argument('--vendor-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--gui', action='store_true', help='Also create the diagnostic GUI; Start stays disabled')
    args=parser.parse_args()
    ROOT=args.vendor_dir.resolve(); OUT=args.output.resolve(); OUT.mkdir(parents=True,exist_ok=True)
    results=[build('MP_API.dll', diagnostic=True)]
    if args.gui:
        results.append(build('SSDMPTool.exe', diagnostic=True))
    (OUT/'vendor-patch.json').write_text(json.dumps(results,indent=2)+'\n',encoding='utf-8')
    print('Patched local copies. No program was executed.')
