"""Build offline fixture files from private measured inputs. No hardware access."""
import argparse
import configparser
import hashlib
import json
from pathlib import Path
import shutil
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from prepare import FIRMWARE_SHA256, configuration, validate_profile


def fixture_array(name, data):
    return 'static const unsigned char '+name+'['+str(len(data))+']={'+','.join(str(v) for v in data)+'};\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('profile', 'firmware', 'fixtures', 'vendor-dir', 'output'):
        parser.add_argument('--'+name, required=True, type=Path)
    args = parser.parse_args()
    p = validate_profile(json.loads(args.profile.read_text(encoding='utf-8-sig')))
    raw = args.firmware.read_bytes()
    if hashlib.sha256(raw).hexdigest() != FIRMWARE_SHA256:
        raise ValueError('Unexpected SBFM61.3 firmware hash')
    from phison_s11 import firmware, info_block
    fw = firmware.Firmware.from_bytes(raw)
    required = {'identify-rom.bin':512, 'nand-id.bin':512, 'infoblock.bin':512,
                'system-info.bin':512, 'flash-header.bin':4096}
    values = {name:(args.fixtures/name).read_bytes() for name in required}
    if any(len(values[name]) != size for name, size in required.items()):
        raise ValueError('Unexpected fixture length')
    ib = info_block.InfoBlock.from_bytes(values['infoblock.bin'])
    if (ib.model, ib.serial) != (p['model'], p['serial']):
        raise ValueError('InfoBlock identity differs from profile')
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).parent
    for name in ('capture_guard.c', 'capture_runner.c'):
        shutil.copyfile(source/name, out/name)
    shutil.copyfile(source.parent/'native/sata_guard.c', out/'sata_guard.c')
    (out/'recovery_config.h').write_text(configuration(p), encoding='ascii')
    for name, key in (('capture_identify','identify-rom.bin'), ('capture_nand','nand-id.bin')):
        (out/(name+'.h')).write_text(fixture_array(name, values[key]), encoding='ascii')
    identity = bytearray(values['identify-rom.bin'])
    def set_text(raw, offset, size, text):
        value = text.encode().ljust(size, b' ')
        raw[offset:offset+size] = bytes(value[i^1] for i in range(size))
    set_text(identity, 46, 8, 'SBBM61.3'); set_text(identity, 54, 40, 'SATABURN   SB')
    (out/'offline-burner-identify.bin').write_bytes(identity)
    set_text(identity, 46, 8, 'SBFM61.3'); set_text(identity, 54, 40, ib.model)
    set_text(identity, 20, 20, ib.serial)
    struct.pack_into('<I', identity, 120, min(ib.addressable_sectors, 0x0fffffff))
    struct.pack_into('<Q', identity, 200, ib.addressable_sectors)
    for word in (83, 86):
        struct.pack_into('<H', identity, word*2, struct.unpack_from('<H', identity, word*2)[0]|1024)
    (out/'offline-normal-identify.bin').write_bytes(identity)
    system = values['system-info.bin']
    if system.count(b'BSMB163.') != 1:
        raise ValueError('Expected SBBM61.3 SystemInfo fixture')
    (out/'offline-system-info.bin').write_bytes(system)
    (out/'offline-normal-system-info.bin').write_bytes(system.replace(b'BSMB163.', b'BSMF163.'))
    (out/'offline-flash-header.bin').write_bytes(values['flash-header.bin'])
    (out/'offline-start-in-burner.flag').write_bytes(b'1')
    rows = [(0x04002440, 0xd0010000), (0x0400247c, p['controller_id0']),
            (0x040024bc, p['controller_id1']), (0x0400303e, p['controller_revision'])]
    (out/'offline-registers.bin').write_bytes(struct.pack('<I',len(rows))+b''.join(struct.pack('<II',*row) for row in rows))
    for source_name, dest_name in (('nand-limits.bin','offline-nand-limits-lba0.bin'), ('nand-limits-240.bin','offline-nand-limits-240.bin')):
        if (args.fixtures/source_name).exists():
            shutil.copyfile(args.fixtures/source_name, out/dest_name)
    encoded = bytes(fw.burner_code)[firmware.Seed.SIZE:]; offset = 0; layout = []
    for i, section in enumerate(fw.burner_code.sections):
        if section is None:
            continue
        block = encoded[offset:offset+len(section.code)]; offset += len(section.code)
        if i == 6:
            continue
        at = raw.find(block)
        if at < 0 or raw.find(block, at+1) >= 0:
            raise ValueError('Firmware section is not uniquely located')
        layout.append((section.burner_address,len(block),at))
    (out/'offline-iram-layout.bin').write_bytes(struct.pack('<I',len(layout))+b''.join(struct.pack('<III',*row) for row in layout))
    (out/'offline-firmware.bin').write_bytes(raw)
    (out/'infoblock-original.bin').write_bytes(values['infoblock.bin'])
    shutil.copyfile(args.vendor_dir/'FlashInfo.dat', out/'FlashInfo.dat')
    (out/'Bin').mkdir(exist_ok=True); (out/'BINInfo').mkdir(exist_ok=True)
    shutil.copyfile(args.firmware, out/'Bin/SBFM61.3_11032021.BIN')
    ini = configparser.ConfigParser(interpolation=None); ini.optionxform = str
    ini.read(args.vendor_dir/'param.ini', encoding='ascii')
    ini['General Info'].update(ModelName=p['model'],SerialNum=p['serial'],TestStationSel='5')
    ini['Device Info'].update(ICFWVersion='SBFM61.3', **{'3111SBISP_CodeVersion':'M61.3','3111SBISP_MPBINCode':'.\\Bin\\SBFM61.3_11032021.BIN'})
    ini['Options'].update(SetSSDSize=str(ib.addressable_sectors*512//10**9),SetSSDSizeLBA=str(ib.addressable_sectors),DownLoadInfo='1')
    ini['InfoBlock'] = {'InfoB_FileName':'.\\infoblock-original.bin'}
    with (out/'param.ini').open('w', encoding='ascii') as stream:
        ini.write(stream, space_around_delimiters=False)
    (out/'SIMULATION_ONLY.txt').write_text('Offline synthetic responses. Not evidence of hardware health. Never deploy this guard to a real recovery package.\n')
    print('Prepared private offline fixtures. No hardware access.')


if __name__ == '__main__':
    main()
