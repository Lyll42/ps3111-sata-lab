# PS3111 SATA Lab

Windows SATA transport tools for Phison S11/PS3111 controllers: a SAT12/SAT16
converter, a read-only IDENTIFY diagnostic and an x86 C bridge.

**Experimental.** SATA communication, NAND queries and RAM transfers worked on
one SSD. That drive later failed sustained writes. Hardware compatibility with
other drives remains untested.

## Contents

- SAT register conversion to Windows ATA structures, with offline tests.
- A CLI that verifies the target and permits only one 512-byte ATA IDENTIFY
  read. The default mode displays a plan without accessing hardware.
- x86 C source for the bridge and a PNP target selector for a
  PS3111 in ROM mode. The native target remains unconfigured until a local
  PNP instance ID is supplied.
- Technical findings, an anonymized case study and proposals for psychite's authors.

The package excludes firmware, MPTool/MP_API, compilers, unlock keys, psychite
source, NAND dumps and identifying information from the test machine. The
diagnostic CLI provides no erase, flash or data-write operations.

## Installation and tests

Python 3.10 or later is required. The tests do not require Windows,
administrator privileges or a connected SSD:

```powershell
python -m pip install -e .
python -m unittest discover -s tests -v
```

## Windows diagnostics

`--list` queries Windows metadata; its output may include serial numbers.
Keep private target configurations in `targets/`, which is excluded from Git.

```powershell
python -m ps3111_sata --list
```

Copy `examples/target.example.json` to a private file and replace all values
with the expected index, PNP instance ID, model, firmware, serial number and
sector count of the selected SSD. The example contains an intentionally
invalid PNP instance ID.

```powershell
# Validate the configuration and display a single-read plan without opening disks.
python -m ps3111_sata --target targets/my-ssd.json

# From an elevated console: perform one read if all target checks pass.
python -m ps3111_sata --target targets/my-ssd.json --execute-identify
```

The CLI rejects boot/system disks, non-SATA connections, ambiguous identities
and target changes. It also checks the actual ATA identity data. An incomplete
response or error stops the operation, with no automatic retries or selection
of another disk. The diagnostic result redacts the serial number.

## C library and relationship to MPTool

The `native/` directory contains the C bridge and its test programs.
`SataGuard.dll` blocks all raw-device access. `TargetSataGuard.dll` permits only
IDENTIFY for a PS3111/SBRM02.0 in ROM mode with an exact PNP instance ID and a
SATA connection.

The sources were compiled with TinyCC 0.9.27 **x86** on Windows. Obtain the
compiler separately from the TinyCC project. These native sources use a
32-bit structure layout; do not compile them as 64-bit code.

```powershell
.\native\build.ps1 -Compiler C:\path\to\tcc-x86\tcc.exe
```

The build script runs a conversion test in memory. To use the hardware
probe, the local `native/target_config.h` must contain your own exact PNP
instance ID.

The bridge is not a complete distribution of MPTool adapted for SATA.
Additional VUC queries and factory initialization require further development
and review. Obtain third-party programs and files from their authors;
they are not redistributed here.

## Documentation

- [Hardware results and limitations](docs/case-study.md)
- [Transport details](docs/transport-findings.md)
- [Source provenance](docs/provenance.md)
- [Package validation](docs/validation.md)
- [Technical proposal for psychite](docs/upstream-transport-proposal.md)
- [Missing package resource](docs/upstream-package-data-issue.md)

## License

The original code included in this repository is licensed under MIT.
External projects and factory tools are subject to their own licenses.
