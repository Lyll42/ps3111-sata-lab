# Single-device hardware observations

One GIGABYTE 240 GB SSD using a PS3111-S11-13 controller was tested on Windows
with direct SATA/AHCI. Serial numbers, PNP instance IDs, controller die IDs and
local paths are omitted. No inference of universal compatibility is intended.

| Observation | Result |
|---|---|
| ROM identification | PS3111 / SBRM02.0 |
| NAND identity | Toshiba-family ID prefix `98 3e 98 b3 76 e3` |
| Geometry reported | Four CEs, two channels, 3916 blocks/CE, two planes/die |
| Native SATA RAM experiment | 80-command SBBM61.3 load and verification completed |
| Additional RAM variants | SBBM61.5 and SBBQ61L3 were identified |
| Early bad-block counters | 934–951 per plane, identical across those variants |
| Capacity-dependent limit at 240 GB | 177 per plane; factory initialization stopped |
| Limit at 120 GB | 998 per plane; early scan and preformat proceeded |
| Normal 120 GB identity | Original model/serial and SBFM61.3, 234441648 sectors |
| After a power cycle | Windows recognized the reduced capacity |
| Distributed sample | 64 MiB write, flush and read comparison passed |
| Full-capacity attempt | Failed after 4,969,201,664 bytes written; no full read verification |
| Windows evidence | Error 433 (device does not exist) and a storahci reset event |
| Subsequent identification | No valid ATA response; buffered data was all zero |
| 60 GB proposal | Prepared offline, never applied because identification was unavailable |

The transport experiment succeeded; the device did not become reliable storage.
A short sample was insufficient to establish sustained-write stability. Software
observations alone do not prove that the underlying physical fault is irreversible.

The public CLI contains only IDENTIFY. RAM loading, factory programming and the
destructive write tests described above are historical observations, not public
commands provided by this package. Offline simulations used explicitly synthetic
health values to inspect control flow; these were never used as hardware results.
