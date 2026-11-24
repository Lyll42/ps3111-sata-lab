"""Prepare a private, target-specific recovery build. Never opens a device."""
import argparse
import configparser
import hashlib
import json
from pathlib import Path
import shutil
import struct

FIRMWARE_SHA256 = 'ea0e88c42cf070af2e367cb1880c940092c29672824dd4200b9a8d4ff8e4b9f0'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def c_string(value):
    if not isinstance(value, str) or any(ord(c) < 32 or ord(c) > 126 for c in value):
        raise ValueError('Configuration strings must be printable ASCII')
    return json.dumps(value)


def validate_profile(p):
    for key in ('pnp_id', 'normal_pnp_id'):
        value = p[key]
        if not value.startswith(('SCSI\\', 'IDE\\')) or any(c in value for c in '*?\r\n\0') or 'REPLACE' in value:
            raise ValueError('An exact local PNP instance ID is required: ' + key)
        c_string(value)
    if type(p['disk_index']) is not int or not 1 <= p['disk_index'] <= 255:
        raise ValueError('The native vendor scanner supports explicit disk indices 1..255')
    if not p['serial'] or len(p['serial']) > 20 or not p['model'] or len(p['model']) > 40:
        raise ValueError('Supply the original ATA model and serial')
    c_string(p['serial']); c_string(p['model'])
    if type(p['sectors']) is not int or not 1 <= p['sectors'] <= 468862128:
        raise ValueError('Capacity must be within the original 240 GB case limit')
    for key in ('controller_id0', 'controller_id1'):
        if type(p[key]) is not int or not 0 <= p[key] <= 0xffffffff:
            raise ValueError('A measured 32-bit controller identity is required')
    if type(p['controller_revision']) is not int or not 0 <= p['controller_revision'] <= 255:
        raise ValueError('A measured controller revision is required')
    if len(p['nand_id_prefix'].split()) != 6 or len(bytes.fromhex(p['nand_id_prefix'])) != 6:
        raise ValueError('Supply the six-byte NAND ID prefix')
    return p


def array(name, data):
    rows = [f'static const U8 {name}[{len(data)}]={{']
    rows += [','.join(str(v) for v in data[i:i+64])+',' for i in range(0, len(data), 64)]
    return '\n'.join(rows + ['};'])


def configuration(p):
    return '\n'.join([
        '/* Private, generated target configuration. Do not publish. */',
        '#define RECOVERY_PNP L' + c_string(p['pnp_id']),
        '#define RECOVERY_NORMAL_PNP L' + c_string(p['normal_pnp_id']),
        '#define RECOVERY_DISK_INDEX ' + str(p['disk_index']) + 'UL',
        '#define RECOVERY_MODEL ' + c_string(p['model']),
        '#define RECOVERY_SERIAL ' + c_string(p['serial']),
        '#define RECOVERY_SECTORS ' + str(p['sectors']) + 'UL',
        '#define RECOVERY_CONTROLLER_ID0 ' + hex(p['controller_id0']) + 'UL',
        '#define RECOVERY_CONTROLLER_ID1 ' + hex(p['controller_id1']) + 'UL',
        '#define RECOVERY_CONTROLLER_REVISION ' + str(p['controller_revision']),
        '',
    ])


def prepare(profile, firmware_path, info_path, preformat_path, nand_path, vendor_dir, output):
    p = validate_profile(json.loads(profile.read_text(encoding='utf-8-sig')))
    raw = firmware_path.read_bytes()
    if sha(raw) != FIRMWARE_SHA256:
        raise ValueError('Only the investigated SBFM61.3_11032021.BIN is supported')
    # psychite remains a separately installed dependency, with its own license.
    from phison_s11 import firmware, info_block
    image = firmware.Firmware.from_bytes(raw)
    if str(image.version) != 'SBFM61.3':
        raise ValueError('Unexpected firmware version')
    original = info_path.read_bytes()
    if len(original) != 512:
        raise ValueError('InfoBlock must be 512 bytes')
    ib = info_block.InfoBlock.from_bytes(original)
    if bytes(ib) != original or (ib.model, ib.serial) != (p['model'], p['serial']):
        raise ValueError('InfoBlock does not match the selected device')
    if p['sectors'] > ib.addressable_sectors:
        raise ValueError('Requested capacity exceeds the saved original capacity')
    ib.addressable_sectors = p['sectors']
    info = bytes(ib)
    if any(original[i] != info[i] for i in range(512) if i not in range(100, 104)):
        raise ValueError('InfoBlock change is not limited to capacity')
    template = preformat_path.read_bytes()
    if len(template) != 1024 or template[:512] != original:
        raise ValueError('Supply the original matching vendor-generated preformat payload')
    nand = nand_path.read_bytes()
    if len(nand) != 512 or bytes.fromhex(p['nand_id_prefix']) not in nand:
        raise ValueError('NAND identity is empty, invalid or differs from the profile')
    normal = bytes(image.normal_code)
    seed = bytearray(normal[:firmware.Seed.SIZE]); code = normal[firmware.Seed.SIZE:]
    end = max(s.burner_address + len(s.code) for s in image.normal_code.sections if s)
    for i, section in enumerate(image.normal_code.sections):
        if section is None:
            struct.pack_into('<I', seed, i*8, end)
    if not code or (len(code)+65535)//65536 >= 32:
        raise ValueError('Normal code exceeds the native policy bit mask')
    output.mkdir(parents=True, exist_ok=True)
    for source in Path(__file__).with_name('native').glob('*.c'):
        shutil.copyfile(source, output/source.name)
    shutil.copyfile(Path(__file__).with_name('native')/'factory_policy.h', output/'factory_policy.h')
    (output/'recovery_config.h').write_text(configuration(p), encoding='ascii')
    header = ['/* Private firmware data generated from local inputs. */',
              '#define FACTORY_SECTORS RECOVERY_SECTORS',
              '#define FACTORY_SYSTEM_VERSION_OFFSET 344', '#define FACTORY_VUC_MODE_OFFSET 426']
    for name, data in {'factory_info': info, 'factory_preformat_data': info+template[512:],
                       'factory_seed': bytes(seed), 'factory_code': code, 'factory_nand_ids': nand}.items():
        header.append(array(name, data))
    (output/'factory_data.h').write_text('\n'.join(header)+'\n', encoding='ascii')
    shutil.copyfile(profile, output/'profile.json')
    shutil.copyfile(vendor_dir/'FlashInfo.dat', output/'FlashInfo.dat')
    ini = configparser.ConfigParser(interpolation=None); ini.optionxform = str
    ini.read(vendor_dir/'param.ini', encoding='ascii')
    # Preserve vendor geometry/flash settings from the user's matching profile.
    required = ('ConfigID', 'General Info', 'Flash Info', 'Device Info', 'Options', 'EraseAll')
    if any(section not in ini for section in required):
        raise ValueError('Incomplete matching vendor param.ini')
    if ini['Flash Info'].get('Flh_ID', '').lower().split() != p['nand_id_prefix'].lower().split():
        raise ValueError('Vendor configuration NAND ID differs from the profile')
    ini['General Info'].update(ModelName=p['model'], SerialNum=p['serial'], TestStationSel='5', MapType='0')
    ini['Device Info'].update(ICFWVersion='SBFM61.3', **{'3111SBISP_CodeVersion':'M61.3',
        '3111SBISP_MPBINCode':'.\\Bin\\SBFM61.3_11032021.BIN', '3111SBISP_RDTBINCode':''})
    ini['Options'].update(SetSSDSize=str(p['sectors']*512//10**9), SetSSDSizeLBA=str(p['sectors']), DownLoadInfo='1')
    ini['InfoBlock'] = {'InfoB_FileName': '.\\infoblock.bin'}
    ini['EraseAll']['Do_Erase_First'] = '1'; ini['RDT'] = {'Do_RDT': '0'}
    with (output/'param.ini').open('w', encoding='ascii') as stream:
        ini.write(stream, space_around_delimiters=False)
    (output/'infoblock.bin').write_bytes(info)
    (output/'Bin').mkdir(exist_ok=True); (output/'BINInfo').mkdir(exist_ok=True)
    shutil.copyfile(firmware_path, output/'Bin/SBFM61.3_11032021.BIN')
    evidence = {'hardware_executed': False, 'sectors': p['sectors'], 'firmware_sha256': sha(raw),
                'inputs': {str(path.name): sha(path.read_bytes()) for path in
                           (profile, info_path, preformat_path, nand_path)},
                'bad_block_limits_overridden': False}
    (output/'preparation.json').write_text(json.dumps(evidence, indent=2)+'\n')
    return evidence


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('profile', 'firmware', 'infoblock', 'preformat', 'nand-id', 'vendor-dir', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    a = parser.parse_args()
    print(json.dumps(prepare(a.profile, a.firmware, a.infoblock, a.preformat, a.nand_id, a.vendor_dir, a.output), indent=2))


if __name__ == '__main__':
    main()
