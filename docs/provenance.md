# Source provenance and excluded material

The Python codec, read-only CLI and C laboratory bridge were prepared during
a Windows SATA transport investigation. The public tree contains original
implementation code and Windows API declarations. Microsoft SDK headers and
driver implementations are not vendored.

Related upstream project: [trulycrisp/psychite](https://github.com/trulycrisp/psychite).
The local investigation used commit `15a8a845566903e2b1824bcf8da043848a187fdc`.
Its root tree had README.md, example/, pyproject.toml and src/, with no LICENSE
file, and pyproject.toml did not declare a license. We should ask the author to
clarify licensing before redistributing or incorporating their source. No
psychite source, unlock keys or XOR resource is included here.

Excluded: original or modified MPTool executables, MP_API.dll, FlashInfo.dat,
firmware images, generated firmware byte arrays, NAND dumps, compiler binaries,
device-specific scripts and private configuration/logs. No redistribution
permission for those materials is asserted.

The native bridge exported here has factory and VUC/NAND diagnostic branches
removed. Its target configuration example is deliberately unconfigured.

Hardware validation is historical and single-device. The cleaned public package
is checked offline; no new SSD commands are needed to prepare or test it.
