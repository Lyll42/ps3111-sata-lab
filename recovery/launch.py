"""Seal a private factory package or launch it with target metadata checks."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

from prepare import validate_profile
from ps3111_sata.identify import Target, inventory, select_metadata


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def members(package):
    names = ('factory_runner.exe', 'TargetSataGuard.dll', 'MP_API.dll', 'FlashInfo.dat',
             'param.ini', 'profile.json', 'infoblock.bin', 'recovery_config.h',
             'factory_data.h', 'build-manifest.json', 'Bin/SBFM61.3_11032021.BIN')
    return {name: digest(package/name) for name in names}


def check_target(profile, rows):
    matches = [d for d in rows['drives'] if str(d.get('PNPDeviceID', '')).casefold() == profile['pnp_id'].casefold()]
    if len(matches) != 1:
        raise ValueError('Target missing or ambiguous')
    row = matches[0]
    model = str(row.get('Model', '')).strip(); fw = str(row.get('FirmwareRevision', '')).strip()
    if (model, fw) not in (('PS3111', 'SBRM02.0'), ('SATABURN   SB', 'SBBM61.3')):
        raise ValueError('Expected ROM or cached Burner metadata')
    target = Target(profile['disk_index'], profile['pnp_id'], model, fw,
                    str(row.get('SerialNumber', '')).strip(), 40961)
    return select_metadata(target, rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--seal', action='store_true')
    parser.add_argument('--operation', choices=('preflight', 'initialize', 'initialize-with-erase'), default='preflight')
    parser.add_argument('--execute', action='store_true')
    parser.add_argument('--accept-data-loss', action='store_true')
    args = parser.parse_args()
    package = args.package.resolve()
    profile = validate_profile(json.loads((package/'profile.json').read_text(encoding='utf-8-sig')))
    actual = members(package)
    if args.seal:
        if args.execute:
            parser.error('--seal and --execute are separate operations')
        (package/'sealed-files.json').write_text(json.dumps(actual, indent=2)+'\n')
        print('Package hashes sealed. No hardware access.')
        return
    expected = json.loads((package/'sealed-files.json').read_text())
    if actual != expected:
        raise ValueError('Package changed after sealing')
    if not args.execute:
        print(json.dumps({'executed': False, 'operation': args.operation, 'package_verified': True}))
        return
    if args.operation != 'preflight' and not args.accept_data_loss:
        parser.error('Initialization requires --accept-data-loss')
    if os.name != 'nt':
        raise OSError('Windows is required')
    before = check_target(profile, inventory())
    if check_target(profile, inventory()) != before:
        raise ValueError('Target changed before launch')
    raise SystemExit(subprocess.run([str(package/'factory_runner.exe'), '--'+args.operation], cwd=package).returncode)


if __name__ == '__main__':
    main()
