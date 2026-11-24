# Native Windows SATA backend: prototype and PS3111 observations

The proposal was submitted as [psychite issue #2](https://github.com/trulycrisp/psychite/issues/2).
The issue links to this document for the detailed technical findings.

The investigation used one PS3111-S11 SSD connected directly to SATA on Windows
and psychite at commit `15a8a845566903e2b1824bcf8da043848a187fdc`.

An experimental SAT12/SAT16-to-ATA_PASS_THROUGH_DIRECT route worked on that
SATA/AHCI setup. Tests covered IDENTIFY, NAND/controller queries and an 80-command
Burner RAM sequence. The SSD itself subsequently failed sustained writes, so
these results establish transport operation on that setup. The drive was not repaired.

## Implementation findings

- The native Windows structure is 40 bytes for x86 and 48 bytes for x64 callers.
  The Windows 48-bit and DMA flags are 8 and 16 respectively.
- One vendor helper passed a zero-padded SAT12 CDB with CdbLength=16.
- Inserting IDENTIFY between a VUC SET_PARAMETER and READ_REGISTER replaced
  the pending response on real SBBM61.3 hardware. Target checks should avoid
  injecting ATA commands in the middle of a stateful transaction.
- A successful DeviceIoControl return alone did not establish success: later
  attempts returned an unchanged 0xec field and all-zero identity data.
- Completion snapshots cannot substitute for live asynchronous register polling.

## Proposed contribution

The prototype includes an independent codec, a guarded IDENTIFY-only reader,
offline tests and anonymized hardware notes. It excludes vendor binaries and
firmware. The upstream issue asks whether a native Windows SATA backend would
be useful and which interface and tests a contribution should use.

The issue also requests license clarification: the psychite root tree and
pyproject.toml had no license declaration at the tested commit. No psychite
source is redistributed here.

Prototype repository: [PS3111 SATA Lab](https://github.com/Lyll42/ps3111-sata-lab).

The repository now also includes the separate [recovery source workflow](../recovery/README.md),
with configurable targets, factory and offline-capture sources, and build
instructions. Vendor binaries, firmware and private device fixtures remain
external inputs. This addition has been validated offline, not on another SSD.
