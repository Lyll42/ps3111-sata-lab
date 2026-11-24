import sys,unittest,struct
from pathlib import Path
from unittest.mock import Mock
from ps3111_sata.codec import decode,packet_bytes,PlannedAdapter

IDENTIFY12=bytes.fromhex('a1 08 0e 00 01 00 00 00 a0 ec 00 00')
IDENTIFY16=bytes.fromhex('85 08 2e 00 00 00 01 00 00 00 00 00 00 00 ec 00')

class Codec(unittest.TestCase):
    def test_matches_actual_dll_native_identify_layout(self):
        # Golden taskfile and flags captured from actual DLL in Unicorn.
        p=packet_bytes(decode(IDENTIFY12,512),0x30080000,60)
        self.assertEqual(p,bytes.fromhex('2800030000000000000200003c0000000000000000000830'+'0000000000000000'+'0001000000a0ec00'))
    def test_48_bit_registers(self):
        c=bytes.fromhex('85 09 0e 12 34 00 01 56 78 9a bc de f0 40 24 00')
        r=decode(c,512)
        self.assertEqual(r.current,bytes.fromhex('340178bcf0402400'))
        self.assertEqual(r.previous,bytes.fromhex('1200569ade000000'))
        self.assertEqual(r.flags,0x0b)
        self.assertEqual(len(packet_bytes(r,0x1122334455667788,bits=64)),48)
    def test_factory_udma_uses_windows_dma_flag_independently_of_48bit(self):
        read=decode(bytes.fromhex('a1140e0080000000e0c80000'),65536)
        write=decode(bytes.fromhex('a116060080000000e0ca0000'),bytes(65536))
        self.assertEqual(read.flags,0x13);self.assertEqual(write.flags,0x15)
        self.assertEqual(read.previous,bytes(8));self.assertEqual(read.size,65536)
    def test_udma_direction_conflicts_are_rejected(self):
        with self.assertRaises(ValueError):decode(bytes.fromhex('a114060080000000e0c80000'),65536)
        with self.assertRaises(ValueError):decode(bytes.fromhex('a1160e0080000000e0ca0000'),bytes(65536))
    def test_non_data_apkey(self):
        r=decode(bytes.fromhex('a1 06 00 00 6f fe ef fa 00 e0 00 00'),None)
        self.assertEqual(r.current,bytes.fromhex('006ffe effa00e000'.replace(' ','')))
        self.assertEqual((r.flags,r.size,r.direction),(1,0,'none'))
    def test_factory_preformat_shape_only_never_sent(self):
        # Derived from DLL VA 0x10032c6a; synthetic zero payload only.
        c=bytes.fromhex('a1 0a 06 08 02 00 10 02 a0 31 00 00')
        r=decode(c,bytes(1024))
        self.assertEqual((r.current[0],r.current[6],r.direction,r.size),(8,0x31,'out',1024))
    def test_invalid_formats_rejected(self):
        for c,d in [(b'\x12'+bytes(11),512),(IDENTIFY12,1024),(IDENTIFY12,bytes(512)),
                    (bytes.fromhex('a10a060802001002a0310000'),bytes(512))]:
            with self.assertRaises(ValueError): decode(c,d)
    def test_guard_blocks_changed_command_and_retries(self):
        backend=Mock()
        a=PlannedAdapter([(IDENTIFY16,512)],backend)
        wrong=bytearray(IDENTIFY16); wrong[14]=0x92
        with self.assertRaises(RuntimeError): a.command(wrong,512,10)
        with self.assertRaises(RuntimeError): a.command(IDENTIFY16,512,10)
        backend.assert_not_called()
    def test_changed_output_payload_never_reaches_backend(self):
        c=bytes.fromhex('a10a060802001002a0310000')
        backend=Mock();a=PlannedAdapter([(c,bytes(1024))],backend)
        with self.assertRaises(RuntimeError):a.command(c,b'X'+bytes(1023),10)
        backend.assert_not_called()
    def test_short_read_and_ata_failure_stop_plan(self):
        for status,raw in [(0x50,bytes(511)),(0x51,bytes(512)),(0x80,bytes(512))]:
            backend=Mock(return_value=(bytes([0,1,0,0,0,0,status,0]),raw))
            a=PlannedAdapter([(IDENTIFY16,512)],backend)
            with self.assertRaises(OSError): a.command(IDENTIFY16,512,10)
            self.assertEqual(a.position,0); self.assertTrue(a.failed)
if __name__=='__main__': unittest.main()
