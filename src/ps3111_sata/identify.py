"""A single, target-validated ATA IDENTIFY; no vendor or write API is exposed."""
from dataclasses import dataclass
import ctypes
import json
import os
from pathlib import Path
import struct
import subprocess

@dataclass(frozen=True)
class Target:
    index: int
    pnp_id: str
    model: str
    firmware: str
    serial: str
    sectors: int

    def __post_init__(self):
        if type(self.index) is not int or not 0 <= self.index <= 255:
            raise ValueError("Explicit disk index required")
        if not isinstance(self.pnp_id, str) or not self.pnp_id or "REPLACE_" in self.pnp_id or any(x in self.pnp_id for x in "*?\x00\n\r"):
            raise ValueError("Exact PNP instance ID required; no placeholders or wildcards")
        if not self.pnp_id.upper().startswith(("SCSI\\", "IDE\\")):
            raise ValueError("Expected a Windows disk instance ID")
        if not all(isinstance(v, str) for v in (self.model, self.firmware, self.serial)) or not self.model or not self.firmware:
            raise ValueError("Expected model, firmware, and serial must be explicit")
        if type(self.sectors) is not int or not 0 < self.sectors < 2**48:
            raise ValueError("Expected sector count required")

    @property
    def path(self):
        return rf"\\.\PhysicalDrive{self.index}"

def select_metadata(target, inventory):
    matches = [row for row in inventory["drives"] if str(row.get("PNPDeviceID", "")).casefold() == target.pnp_id.casefold()]
    if len(matches) != 1:
        raise ValueError("Target absent or ambiguous; no disk opened")
    row = matches[0]
    if row.get("Index") != target.index or str(row.get("DeviceID", "")).casefold() != target.path.casefold():
        raise ValueError("Disk index/path changed; no disk opened")
    for field, expected in (("Model", target.model), ("FirmwareRevision", target.firmware), ("SerialNumber", target.serial)):
        if str(row.get(field) or "").strip() != expected:
            raise ValueError("Cached identity differs; no disk opened")
    disks = [d for d in inventory["disks"] if d.get("Number") == target.index]
    if len(disks) != 1 or disks[0].get("IsBoot") is not False or disks[0].get("IsSystem") is not False:
        raise ValueError("Boot/system/unknown disk blocked")
    if str(disks[0].get("BusType", "")).casefold() != "sata":
        raise ValueError("Direct SATA required")
    return dict(row), dict(disks[0])

def inventory():
    if os.name != "nt":
        raise OSError("Windows is required for hardware operations")
    script = r"""
$ErrorActionPreference='Stop'
[Console]::OutputEncoding=[System.Text.UTF8Encoding]::new($false)
$d=@(Get-CimInstance Win32_DiskDrive | Select-Object Index,Model,FirmwareRevision,SerialNumber,PNPDeviceID,DeviceID)
$s=@(Get-Disk | Select-Object Number,IsBoot,IsSystem,@{Name='BusType';Expression={$_.BusType.ToString()}})
@{drives=$d;disks=$s}|ConvertTo-Json -Depth 4 -Compress
"""
    shell = Path(os.environ["SystemRoot"]) / "System32/WindowsPowerShell/v1.0/powershell.exe"
    result = subprocess.run([str(shell), "-NoProfile", "-NonInteractive", "-Command", script], capture_output=True, text=True, encoding="utf-8-sig", check=True, timeout=30)
    return json.loads(result.stdout)

class Packet(ctypes.Structure):
    _fields_ = [("length", ctypes.c_uint16), ("flags", ctypes.c_uint16),
                ("path_id", ctypes.c_uint8), ("target_id", ctypes.c_uint8), ("lun", ctypes.c_uint8), ("reserved_byte", ctypes.c_uint8),
                ("transfer", ctypes.c_uint32), ("timeout", ctypes.c_uint32), ("reserved_long", ctypes.c_uint32),
                ("buffer", ctypes.c_void_p), ("previous", ctypes.c_uint8 * 8), ("current", ctypes.c_uint8 * 8)]

def parse_identity(data):
    if len(data) != 512:
        raise ValueError("Incomplete IDENTIFY data")
    if data[510] == 0xa5 and sum(data) & 255:
        raise ValueError("IDENTIFY checksum failed")
    def text(offset, size):
        return bytes(data[offset + (i ^ 1)] for i in range(size)).decode("ascii", errors="strict").strip()
    supports48 = bool(struct.unpack_from("<H", data, 166)[0] & 1024)
    sectors = struct.unpack_from("<Q", data, 200)[0] if supports48 else struct.unpack_from("<I", data, 120)[0]
    if not 0 < sectors < 2**48:
        raise ValueError("Invalid IDENTIFY capacity")
    return {"model": text(54, 40), "firmware": text(46, 8), "serial": text(20, 20), "sectors": sectors}

def validate_result(target, status, returned, transfer, data):
    if returned < ctypes.sizeof(Packet) or transfer != 512 or status & 0xa9 or not status & 0x40:
        raise OSError("ATA result incomplete or unsuccessful; no fallback/retry")
    actual = parse_identity(data)
    if any(actual[name] != getattr(target, name) for name in ("model", "firmware", "serial", "sectors")):
        raise ValueError("Actual ATA identity differs from the selected target")
    return actual

def identify_once(target):
    if os.name != "nt":
        raise OSError("Windows is required for hardware operations")
    if not ctypes.windll.shell32.IsUserAnAdmin():
        raise PermissionError("Administrator privileges are required")
    before = select_metadata(target, inventory())
    if before != select_metadata(target, inventory()):
        raise ValueError("Target changed before opening")
    from ctypes import wintypes as w
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, ctypes.c_void_p, w.DWORD, w.DWORD, w.HANDLE]
    kernel.CreateFileW.restype = w.HANDLE
    kernel.CloseHandle.argtypes = [w.HANDLE]
    kernel.DeviceIoControl.argtypes = [w.HANDLE, w.DWORD, ctypes.c_void_p, w.DWORD, ctypes.c_void_p, w.DWORD, ctypes.POINTER(w.DWORD), ctypes.c_void_p]
    kernel.DeviceIoControl.restype = w.BOOL
    handle = kernel.CreateFileW(target.path, 0xc0000000, 3, None, 3, 0x80, None)
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        backing = ctypes.create_string_buffer(512 + 65535)
        address = (ctypes.addressof(backing) + 65535) & ~65535
        ctypes.memset(address, 0xa5, 512)
        packet = Packet()
        packet.length = ctypes.sizeof(Packet)
        packet.flags, packet.transfer, packet.timeout, packet.buffer = 3, 512, 10, address
        packet.current[1], packet.current[5], packet.current[6] = 1, 0xa0, 0xec
        returned = w.DWORD()
        if not kernel.DeviceIoControl(handle, 0x4d030, ctypes.byref(packet), ctypes.sizeof(packet), ctypes.byref(packet), ctypes.sizeof(packet), ctypes.byref(returned), None):
            raise ctypes.WinError(ctypes.get_last_error())
        return validate_result(target, packet.current[6], returned.value, packet.transfer, ctypes.string_at(address, 512))
    finally:
        kernel.CloseHandle(handle)
