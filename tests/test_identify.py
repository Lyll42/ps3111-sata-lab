import contextlib
import ctypes
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
from ps3111_sata.identify import Target, Packet, select_metadata, validate_result, parse_identity
from ps3111_sata.cli import main

def target():
    return Target(4, r"SCSI\DISK&VEN_PS3111&PROD_\DEMO_INSTANCE", "PS3111", "SBRM02.0", "", 40961)

def inventory():
    t = target()
    row = {"Index": t.index, "PNPDeviceID": t.pnp_id, "DeviceID": t.path, "Model": t.model, "FirmwareRevision": t.firmware, "SerialNumber": t.serial}
    return {"drives": [row], "disks": [{"Number": t.index, "IsBoot": False, "IsSystem": False, "BusType": "SATA"}]}

def data():
    raw = bytearray(512)
    for offset, size, text in [(20, 20, ""), (46, 8, "SBRM02.0"), (54, 40, "PS3111")]:
        text = text.ljust(size).encode()
        raw[offset:offset + size] = bytes(text[i ^ 1] for i in range(size))
    struct.pack_into("<I", raw, 120, 40961)
    return bytes(raw)

class IdentifyTests(unittest.TestCase):
    def test_windows_packet_abi_uses_fixed_width_integers(self):
        if ctypes.sizeof(ctypes.c_void_p) == 8:
            self.assertEqual((ctypes.sizeof(Packet), Packet.buffer.offset, Packet.current.offset), (48, 24, 40))
        else:
            self.assertEqual((ctypes.sizeof(Packet), Packet.buffer.offset, Packet.current.offset), (40, 20, 32))

    def test_exact_target_passes_metadata_and_actual_identity(self):
        t = target()
        self.assertEqual(select_metadata(t, inventory())[0]["DeviceID"], t.path)
        self.assertEqual(validate_result(t, 0x50, ctypes.sizeof(Packet), 512, data())["sectors"], 40961)

    def test_wrong_disk_path_serial_or_ambiguous_pnp_is_blocked(self):
        for field, value in [("Index", 0), ("DeviceID", r"\\.\PhysicalDrive0"), ("SerialNumber", "OTHER")]:
            rows = inventory(); rows["drives"][0][field] = value
            with self.assertRaises(ValueError): select_metadata(target(), rows)
        rows = inventory(); rows["drives"] *= 2
        with self.assertRaises(ValueError): select_metadata(target(), rows)

    def test_boot_system_unknown_and_non_sata_are_blocked(self):
        for field, value in [("IsBoot", True), ("IsSystem", True), ("IsBoot", None), ("BusType", "NVMe")]:
            rows = inventory(); rows["disks"][0][field] = value
            with self.assertRaises(ValueError): select_metadata(target(), rows)

    def test_echoed_command_ata_errors_busy_and_truncated_results_fail(self):
        for status in [0xec, 0x51, 0x80, 0x58, 0]:
            with self.assertRaises(OSError): validate_result(target(), status, ctypes.sizeof(Packet), 512, data())
        with self.assertRaises(OSError): validate_result(target(), 0x50, 4, 512, data())
        with self.assertRaises(OSError): validate_result(target(), 0x50, ctypes.sizeof(Packet), 0, data())
        with self.assertRaises(ValueError): validate_result(target(), 0x50, ctypes.sizeof(Packet), 512, bytes(512))

    def test_valid_transport_but_wrong_actual_capacity_fails(self):
        raw = bytearray(data()); struct.pack_into("<I", raw, 120, 100)
        with self.assertRaises(ValueError): validate_result(target(), 0x50, ctypes.sizeof(Packet), 512, bytes(raw))

    def test_invalid_identify_checksum_is_rejected(self):
        raw = bytearray(data()); raw[510] = 0xa5
        raw[511] = (-sum(raw)) & 255
        self.assertEqual(parse_identity(bytes(raw))["model"], "PS3111")
        raw[511] ^= 1
        with self.assertRaises(ValueError): parse_identity(bytes(raw))

    def test_dry_run_never_calls_hardware(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "target.json"
            path.write_text(json.dumps(target().__dict__))
            with patch("ps3111_sata.cli.identify_once") as hardware, contextlib.redirect_stdout(io.StringIO()) as output:
                self.assertEqual(main(["--target", str(path)]), 0)
                hardware.assert_not_called()
            self.assertFalse(json.loads(output.getvalue())["executed"])

    def test_placeholder_and_wildcard_target_are_rejected(self):
        for pnp in ["", "SCSI\\REPLACE_WITH_YOUR_INSTANCE_ID", "SCSI\\DISK*", "\\\\.\\PhysicalDrive0"]:
            with self.assertRaises(ValueError): Target(4, pnp, "PS3111", "SBRM02.0", "", 40961)

if __name__ == "__main__": unittest.main()
