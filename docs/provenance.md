# Source provenance and excluded material

This repository contains the Python codec, read-only CLI and C bridge developed
for the Windows SATA investigation. It includes Windows API declarations but
does not vendor Microsoft SDK headers or driver implementations.

Related upstream project: [trulycrisp/psychite](https://github.com/trulycrisp/psychite).
The local investigation used commit `15a8a845566903e2b1824bcf8da043848a187fdc`.
Its root tree had README.md, example/, pyproject.toml and src/, with no LICENSE
file, and pyproject.toml did not declare a license. Licensing clarification is
requested in [issue #2](https://github.com/trulycrisp/psychite/issues/2).
No psychite source, unlock keys or XOR resource is included here.

Excluded: original or modified MPTool executables, MP_API.dll, FlashInfo.dat,
firmware images, generated firmware byte arrays, NAND dumps, compiler binaries,
device-specific scripts and private configuration/logs. No redistribution
permission for those materials is asserted.

The public native bridge excludes factory operations and VUC/NAND diagnostics.
Its target configuration example is deliberately unconfigured.

Hardware results come from the single-device investigation. Package tests run
offline; see [validation](validation.md) for the checks performed.
