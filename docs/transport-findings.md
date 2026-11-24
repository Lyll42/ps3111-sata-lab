# Windows native SATA transport findings

The experimental route translated SAT12/SAT16 task-file registers into
`ATA_PASS_THROUGH_DIRECT` and used `IOCTL_ATA_PASS_THROUGH_DIRECT`. This avoids
depending on a USB bridge accepting SAT CDBs. One Windows SATA/AHCI setup was tested.

## ABI and flags

The Windows structure is 40 bytes in a 32-bit caller and 48 bytes in a 64-bit
caller. The data pointer is at offset 20/24 and the current task file at 32/40.
DMA and extended addressing are independent: DRDY=1, DATA_IN=2, DATA_OUT=4,
48BIT_COMMAND=8, USE_DMA=16. Sector counts and direction must match the payload.

Sources: [Windows structure](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddscsi/ns-ntddscsi-_ata_pass_through_direct),
[Microsoft storahci definitions](https://github.com/microsoft/Windows-driver-samples/blob/main/storage/miniports/storahci/src/common.h).
Internal miniport flags are not additional public ATA-pass-through flags.

## Vendor framing and state

- One vendor helper declared a zero-padded SAT12 CDB as 16 bytes. The C wrapper
  accepts that form only if the four trailing bytes are zero. The Python codec
  keeps its strict SAT12/SAT16 input contract.
- Native buffer alignment matters. The Python identifier uses an aligned buffer.
- In a real SBBM61.3 experiment, injecting IDENTIFY between VUC SET_PARAMETER
  and READ_REGISTER replaced the pending register response with IDENTIFY data.
  A future stateful backend should preserve a verified handle and avoid hidden
  ATA commands inside such a transaction. This public reader has no VUC API.
- Later calls returned a successful Windows IOCTL, an unchanged command/status
  field of `0xec`, and no valid identity data. That was treated as failure.

## Completion versus live register polling

The C `BridgeReturnResponse` exposes only an actual completed-command snapshot.
It rejects missing results and BSY/DRQ states. It is **not** an implementation
of live SAT protocol-15 register polling. An asynchronous factory operation must
not be made successful by returning a constant READY value.

SAT references: [T10 proposal 04-262r8](https://www.t10.org/ftp/t10/document.04/04-262r8.pdf),
[08-344r2](https://www.t10.org/ftp/t10/document.08/08-344r2.pdf).
