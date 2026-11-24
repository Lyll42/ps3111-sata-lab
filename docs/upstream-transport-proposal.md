# Native Windows SATA backend: prototype and PS3111 observations

The proposal was submitted as [psychite issue #2](https://github.com/trulycrisp/psychite/issues/2).
The issue links to this document for the detailed technical findings.

Hi! We investigated direct Windows SATA access on one PS3111-S11 SSD using
psychite at commit 15a8a845566903e2b1824bcf8da043848a187fdc.

An experimental SAT12/SAT16-to-ATA_PASS_THROUGH_DIRECT route worked on that
SATA/AHCI setup. We verified IDENTIFY, NAND/controller queries and an 80-command
Burner RAM sequence. The SSD itself subsequently failed sustained writes, so
this is transport evidence rather than a successful repair claim.

A few findings might be useful upstream:

- The native Windows structure is 40 bytes for x86 and 48 bytes for x64 callers.
  The Windows 48-bit and DMA flags are 8 and 16 respectively.
- One vendor helper passed a zero-padded SAT12 CDB with CdbLength=16.
- Inserting IDENTIFY between a VUC SET_PARAMETER and READ_REGISTER replaced
  the pending response on real SBBM61.3 hardware. Target checks should avoid
  injecting ATA commands in the middle of a stateful transaction.
- A successful DeviceIoControl return alone did not establish success: later
  attempts returned an unchanged 0xec field and all-zero identity data.
- Completion snapshots cannot substitute for live asynchronous register polling.

Would a native Windows SATA backend be welcome, and what interface/tests would
you prefer? We prepared an independent codec, a guarded IDENTIFY-only reader,
offline tests and anonymized notes. No vendor binaries or firmware are bundled.

Could you also clarify the project's license? We did not find one in the root
tree at that commit and have not redistributed psychite source.

Prototype repository: [PS3111 SATA Lab](https://github.com/Lyll42/ps3111-sata-lab).
