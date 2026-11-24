"""Native SATA RAM loading and Burner diagnostics. Offline unless --execute."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import struct

from ps3111_sata.codec import decode, packet_bytes
from ps3111_sata.identify import Packet, Target, inventory, parse_identity, select_metadata
from prepare import FIRMWARE_SHA256, validate_profile


def attach(interface):
    from phison_s11 import drive
    from phison_s11.scsi import drive as scsi
    result = drive.Drive.__new__(drive.Drive)
    result._scsi = scsi.Drive.__new__(scsi.Drive)
    result._scsi._interface = interface
    return result


def identity(drive):
    from phison_s11.ata import command
    data, _ = drive.command(command.CommandRegisters(command=command.Command.IDENTIFY_DEVICE), 512)
    def field(offset, length):
        return bytes(data[offset+(i^1)] for i in range(length)).decode('ascii').strip()
    return field(54, 40), field(46, 8)


def sequence(drive, burner):
    if identity(drive) != ('PS3111', 'SBRM02.0'):
        raise ValueError('RAM loading requires PS3111/SBRM02.0 ROM mode')
    for section in burner.iram_sections:
        drive.vuc_program_pram(section.burner_address, section.code)
    drive.vuc_jump()
    if burner.sram_section_crypt:
        drive.vuc_program_pram_icode(burner.sram_section_crypt)
    result = identity(drive)
    if result[1] != 'SBBM61.3':
        raise ValueError('Expected SBBM61.3 after RAM loading')
    return result


class Recorder:
    """Synthetic RAM readbacks for constructing a command plan, never a device backend."""
    def __init__(self):
        self.plan = []; self.blocks = {}; self.identifications = 0

    def command(self, cdb, data, timeout):
        self.plan.append((bytes(cdb), data))
        request = decode(cdb, data); opcode = request.current[6]; feature = request.current[0]
        if opcode == 0xec:
            raw = bytearray(512)
            fw = 'SBRM02.0' if not self.identifications else 'SBBM61.3'
            model = 'PS3111' if not self.identifications else 'SATABURN   SB'
            self.identifications += 1
            for offset, size, text in ((46, 8, fw), (54, 40, model)):
                value = text.encode().ljust(size, b' ')
                raw[offset:offset+size] = bytes(value[i^1] for i in range(size))
            return 0, bytes(raw), None
        if opcode == 0xe0:
            return 0, None, None
        if opcode == 0x31 and feature in (0x24, 0x40, 0x0f, 0xbd):
            if feature == 0xbd:
                self.blocks[request.current[4]] = data
            return 0, None, None
        if opcode == 0x21 and feature == 0x30:
            raw = bytearray(data); struct.pack_into('<I', raw, 8, 0x55aa55aa)
            return 0, bytes(raw), None
        if opcode == 0x21 and feature == 0xbe:
            return 0, self.blocks[request.current[4]], None
        raise ValueError('Unexpected command in RAM plan')


class Native:
    def __init__(self, profile, mode, plan=()):
        if os.name != 'nt' or ctypes.sizeof(ctypes.c_void_p) != 8:
            raise OSError('The Python recovery backend requires 64-bit Windows')
        if not ctypes.windll.shell32.IsUserAnAdmin():
            raise PermissionError('Administrator privileges are required')
        expected = ('PS3111', 'SBRM02.0') if mode == 'ram' else ('SATABURN   SB', 'SBBM61.3')
        target = Target(profile['disk_index'], profile['pnp_id'], *expected, '', 40961)
        # Windows may cache the ROM descriptor after the RAM transition.
        rows = inventory()
        candidates = [d for d in rows['drives'] if str(d.get('PNPDeviceID', '')).casefold() == target.pnp_id.casefold()]
        if len(candidates) != 1:
            raise ValueError('Target missing or ambiguous')
        row = candidates[0]
        cached = (str(row.get('Model', '')).strip(), str(row.get('FirmwareRevision', '')).strip())
        if cached not in (expected, ('PS3111', 'SBRM02.0')):
            raise ValueError('Unexpected cached device identity')
        target = Target(target.index, target.pnp_id, *cached, str(row.get('SerialNumber', '')).strip(), 40961)
        before = select_metadata(target, rows)
        if select_metadata(target, inventory()) != before:
            raise ValueError('Target changed before opening')
        from ctypes import wintypes as w
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, ctypes.c_void_p, w.DWORD, w.DWORD, w.HANDLE]
        kernel.CreateFileW.restype = w.HANDLE
        kernel.CloseHandle.argtypes = [w.HANDLE]
        kernel.DeviceIoControl.argtypes = [w.HANDLE, w.DWORD, ctypes.c_void_p, w.DWORD, ctypes.c_void_p, w.DWORD, ctypes.POINTER(w.DWORD), ctypes.c_void_p]
        kernel.DeviceIoControl.restype = w.BOOL
        self.kernel = kernel; self.mode = mode; self.plan = tuple(plan); self.position = 0; self.failed = False
        self.handle = kernel.CreateFileW(target.path, 0xc0000000, 3, None, 3, 0x80, None)
        if self.handle == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            raw = self._send(bytes.fromhex('a1080e0001000000a0ec0000'), 512, 10)[1]
            parsed = parse_identity(raw)
            if (parsed['model'], parsed['firmware']) != expected:
                raise ValueError('Actual ATA identity differs from the required session state')
            self.initial_identity = raw
        except BaseException:
            self.close(); raise

    def _send(self, cdb, data, timeout):
        from ctypes import wintypes as w
        request = decode(cdb, data); size = request.size
        if size > 65536:
            raise ValueError('Transfer exceeds 64 KiB')
        backing = ctypes.create_string_buffer(size+65535) if size else None
        address = (ctypes.addressof(backing)+65535)&~65535 if backing is not None else 0
        if isinstance(data, bytes):
            ctypes.memmove(address, data, size)
        packet = Packet.from_buffer_copy(packet_bytes(request, address, min(timeout, 30), bits=64))
        if packet.current[5]&0x10:
            raise ValueError('Unexpected ATA device selector')
        packet.current[5] = 0xaf if packet.current[6] == 0xe0 else packet.current[5]|0xa0
        returned = w.DWORD()
        if not self.kernel.DeviceIoControl(self.handle, 0x4d030, ctypes.byref(packet), 48, ctypes.byref(packet), 48, ctypes.byref(returned), None):
            raise ctypes.WinError(ctypes.get_last_error())
        if returned.value < 48 or packet.transfer != size or packet.current[6]&0xa9 or not packet.current[6]&0x40:
            raise OSError('Incomplete or unsuccessful ATA command; no retry')
        return 0, ctypes.string_at(address, size) if type(data) is int else None, None

    def command(self, cdb, data, timeout):
        try:
            if self.failed:
                raise ValueError('Session stopped after an error')
            if self.mode == 'ram':
                if self.position >= len(self.plan) or self.plan[self.position] != (bytes(cdb), data):
                    raise ValueError('Command or payload differs from the RAM plan')
            else:
                check_diagnostic(cdb, data, self.mode)
            result = self._send(cdb, data, timeout)
            self.position += 1
            return result
        except BaseException:
            self.failed = True; raise

    def close(self):
        if self.handle is not None:
            self.kernel.CloseHandle(self.handle); self.handle = None


def check_diagnostic(cdb, data, mode):
    request = decode(cdb, data); tf = request.current; opcode = tf[6]; feature = tf[0]
    if request.previous != bytes(8):
        raise ValueError('Extended task-file registers are not allowed in diagnostics')
    lba = int.from_bytes(tf[2:5], 'little')
    if opcode == 0xec and request.size == 512 and tf[:5] == bytes([0, 1, 0, 0, 0]):
        return
    if opcode == 0xe0 and tf == bytes.fromhex('006ffeeffa00e000') and request.size == 0:
        return
    if opcode == 0x21 and feature in (0x13, 0x15, 0x28, 0xc5) and data == 512 and (lba == 0 or (feature == 0x13 and lba == 1)):
        return
    if opcode == 0x31 and mode == 'diagnose-unlock' and feature in (0xc4, 0xc6, 0xc7) and lba == 0 and isinstance(data, bytes) and len(data) == 512:
        if feature == 0xc6 or data == bytes(512):
            return
    if mode == 'controller':
        registers = (0x04002440, 0x0400247c, 0x040024bc, 0x0400303e)
        if opcode == 0x31 and feature == 0x24 and lba == 0 and isinstance(data, bytes) and len(data) == 512:
            if int.from_bytes(data[:4], 'little') in registers and data[4:] == bytes(508):
                return
        if opcode == 0x31 and feature == 0x60 and lba == 0x400 and isinstance(data, bytes) and len(data) == 512:
            if data[:4] == struct.pack('<I', 0x04002400) and int.from_bytes(data[4:8], 'little') in (0, 3) and data[8:] == bytes(504):
                return
        if opcode == 0x21 and feature == 0x61 and data == 512 and lba in (0x100, 0x400):
            return
    raise ValueError('Command outside the diagnostic policy')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=('ram', 'diagnose', 'diagnose-unlock', 'controller'))
    parser.add_argument('--profile', required=True, type=Path)
    parser.add_argument('--firmware', type=Path)
    parser.add_argument('--output', type=Path, default=Path('build/session'))
    parser.add_argument('--execute', action='store_true')
    args = parser.parse_args()
    profile = validate_profile(json.loads(args.profile.read_text(encoding='utf-8-sig')))
    image = None; plan = ()
    if args.operation == 'ram':
        if args.firmware is None:
            parser.error('ram requires --firmware')
        raw = args.firmware.read_bytes()
        if hashlib.sha256(raw).hexdigest() != FIRMWARE_SHA256:
            raise ValueError('Unexpected firmware hash')
        from phison_s11 import firmware
        image = firmware.Firmware.from_bytes(raw)
        recorder = Recorder(); sequence(attach(recorder), image.burner_code); plan = recorder.plan
    if not args.execute:
        print(json.dumps({'executed': False, 'operation': args.operation, 'ram_plan_steps': len(plan),
                          'note': 'RAM jumps, VUC unlocks and register access require --execute; factory operations use the separate runner.'}, indent=2))
        return
    args.output.mkdir(parents=True, exist_ok=True)
    interface = Native(profile, args.operation, plan)
    try:
        drive = attach(interface)
        if image is not None:
            (args.output/'identify-rom.bin').write_bytes(interface.initial_identity)
            sequence(drive, image.burner_code)
            if interface.position != len(plan):
                raise ValueError('Incomplete RAM plan')
        elif args.operation == 'controller':
            # A failed transfer stops the session; do not continue sending
            # commands after an uncertain transport failure.
            try:
                drive.vuc_write_register(0x04002400, struct.pack('<I', 3))
                values = {hex(a): drive.vuc_read_register(a, 1 if a == 0x0400303e else 4).hex()
                          for a in (0x04002440, 0x0400247c, 0x040024bc, 0x0400303e)}
                (args.output/'controller.json').write_text(json.dumps(values, indent=2))
            finally:
                if not interface.failed:
                    drive.vuc_write_register(0x04002400, bytes(4))
                else:
                    print('Register session failed; access latch may remain enabled. Power-cycle before further work.')
        else:
            if args.operation == 'diagnose-unlock':
                drive.vuc_unlock()
            for feature, name in ((0x13, 'system-info.bin'), (0x15, 'nand-id.bin'), (0x28, 'infoblock.bin')):
                (args.output/name).write_bytes(drive.vuc(feature, 512))
        print(json.dumps({'completed': True, 'operation': args.operation, 'commands': interface.position}))
    finally:
        interface.close()


if __name__ == '__main__':
    main()
