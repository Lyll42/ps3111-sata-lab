"""Offline checks for target configuration and recovery command boundaries."""
from pathlib import Path
import sys
import unittest

RECOVERY = Path(__file__).resolve().parents[1]/'recovery'
sys.path.insert(0, str(RECOVERY))
import prepare
import session
import launch
import verify_media


def profile():
    return {'pnp_id': 'SCSI\\DISK&VEN_PS3111&PROD_\\TEST_INSTANCE',
            'normal_pnp_id': 'SCSI\\DISK&VEN_&PROD_TEST\\TEST_INSTANCE',
            'disk_index': 4, 'model': 'TEST SSD', 'serial': 'TEST123',
            'sectors': 234441648, 'controller_id0': 123, 'controller_id1': 456,
            'controller_revision': 3, 'nand_id_prefix': '98 3e 98 b3 76 e3'}


def cdb(feature, write=False, lba=0):
    return bytes([0xa1, 10 if write else 8, 6 if write else 14, feature, 1,
                  lba&255, (lba>>8)&255, (lba>>16)&255, 0xa0, 0x31 if write else 0x21, 0, 0])


class RecoveryTests(unittest.TestCase):
    def test_profile_rejects_ambiguous_devices_and_unsupported_capacity(self):
        for key, value in [('pnp_id', 'SCSI\\DISK*'), ('pnp_id', 'SCSI\\REPLACE'),
                           ('disk_index', 0), ('disk_index', True), ('sectors', 999999999),
                           ('controller_revision', 999), ('serial', 'bad\nvalue')]:
            p = profile(); p[key] = value
            with self.assertRaises(ValueError):
                prepare.validate_profile(p)

    def test_configuration_escapes_strings_and_keeps_device_pins(self):
        p = profile(); p['model'] = 'TEST "SSD"'
        text = prepare.configuration(prepare.validate_profile(p))
        self.assertIn('RECOVERY_DISK_INDEX 4UL', text)
        self.assertIn('TEST \\"SSD\\"', text)
        self.assertIn('RECOVERY_CONTROLLER_ID0 0x7bUL', text)

    def test_diagnostic_policy_blocks_factory_and_ram_writes(self):
        for feature in (0x26, 8, 0x80, 0x82, 0x0f, 0x40):
            for mode in ('diagnose', 'diagnose-unlock', 'controller'):
                with self.assertRaises(ValueError):
                    session.check_diagnostic(cdb(feature, True), bytes(512), mode)
        with self.assertRaises(ValueError):
            session.check_diagnostic(cdb(0xc6, True), bytes(512), 'diagnose')
        session.check_diagnostic(cdb(0xc6, True), bytes(512), 'diagnose-unlock')
        session.check_diagnostic(cdb(0x15), 512, 'diagnose')

    def test_controller_policy_blocks_other_registers_and_values(self):
        import struct
        for address, value in [(0x04002400, 4), (0x04002404, 3)]:
            with self.assertRaises(ValueError):
                session.check_diagnostic(cdb(0x60, True, 0x400), struct.pack('<II', address, value)+bytes(504), 'controller')
        session.check_diagnostic(cdb(0x60, True, 0x400), struct.pack('<II', 0x04002400, 3)+bytes(504), 'controller')

    def test_factory_launcher_rejects_system_disk_and_changed_index(self):
        p = profile()
        row = {'Index': 4, 'DeviceID': r'\\.\PhysicalDrive4', 'Model': 'PS3111',
               'FirmwareRevision': 'SBRM02.0', 'SerialNumber': '', 'PNPDeviceID': p['pnp_id']}
        rows = {'drives': [row], 'disks': [{'Number': 4, 'IsBoot': False, 'IsSystem': False, 'BusType': 'SATA'}]}
        launch.check_target(p, rows)
        rows['disks'][0]['IsSystem'] = True
        with self.assertRaises(ValueError):
            launch.check_target(p, rows)
        rows['disks'][0]['IsSystem'] = False; rows['drives'][0]['Index'] = 2
        with self.assertRaises(ValueError):
            launch.check_target(p, rows)

    def test_media_test_covers_capacity_with_aligned_ranges(self):
        size = verify_media.BLOCK*2+512
        full = list(verify_media.regions(size, True))
        self.assertEqual(sum(length for _,length in full), size)
        self.assertEqual(full[-1], (verify_media.BLOCK*2,512))
        samples = list(verify_media.regions(size))
        self.assertEqual(len(samples), 64)
        self.assertEqual(samples[0][0],0)
        self.assertEqual(samples[-1][0]+samples[-1][1],size)

    def test_media_pattern_changes_with_offset_and_rejects_out_of_bounds(self):
        self.assertNotEqual(verify_media.pattern(0,512,2048),verify_media.pattern(512,512,2048))
        for offset,length in ((1,512),(0,513),(2048,512),(-512,512)):
            with self.assertRaises(ValueError):
                verify_media.pattern(offset,length,2048)


if __name__ == '__main__':
    unittest.main()
