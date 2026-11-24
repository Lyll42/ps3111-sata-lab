# Full SATA recovery workflow

This directory contains the source for the SATA adaptation used in the case
study, including the factory path. It provides RAM loading, Burner diagnostics,
the MPTool import/scanner patcher, a guarded native bridge and the factory runner.

The original implementation was tied to one SSD. This version replaces its
private PNP instance IDs, serial and controller identity with a local profile.
It remains an experimental port of that implementation, not a universal SSD
repair tool. The configurable version has been checked offline, not rerun on
hardware. The original drive failed sustained writes after initialization.

## Dependencies and local inputs

- 64-bit Windows for Python hardware operations; an elevated console is required.
- Python 3.11 or later for the recovery workflow, plus this project's package.
- [psychite](https://github.com/trulycrisp/psychite), installed separately. The
  implementation was developed against commit
  `15a8a845566903e2b1824bcf8da043848a187fdc`. Confirm its license with its author.
- `python -m pip install -r recovery/requirements.txt` for the PE import patcher.
- TinyCC 0.9.27 **x86** to build the native bridge and factory runner.
- Your own matching MPTool package, including `MP_API.dll`, `SSDMPTool.exe`,
  `FlashInfo.dat` and `param.ini`. Only the reviewed MPTool v2.2.22 binaries
  are accepted by the patcher:

| File | SHA256 |
|---|---|
| MP_API.dll | `6f608d8d58afd35b071897e39af7369af9ecb052238db2f7e3b43374ad3f90c3` |
| SSDMPTool.exe | `9ccafbd3f29294d6d9debe41c770d752d30d08caf4eb98f83cf83869f5fd2239` |

- Your own `SBFM61.3_11032021.BIN`, SHA256
  `ea0e88c42cf070af2e367cb1880c940092c29672824dd4200b9a8d4ff8e4b9f0`.
- Original 512-byte InfoBlock, complete 512-byte NAND-ID response and a matching
  1024-byte vendor-generated preformat payload. Its first 512 bytes must equal
  the original InfoBlock. The remaining command-table bytes must come from the
  matching vendor configuration. The tool does not invent a flash table.
- Original model/serial, ROM and normal PNP instance IDs, Windows disk index,
  and measured controller register values at `0400247c`, `040024bc` and `0400303e`.

Vendor binaries, firmware and per-device payloads are **not included**. They
must be obtained separately from their authors or an existing local package.
There is no verified official public download to recommend. This project's MIT
license covers its own code; it grants no rights to those third-party files.

Keep all inputs, profiles, dumps and generated builds under `build/` or another
private directory. Never upload them with a pull request.

## 1. Configure the target

Copy `profile.example.json` to `build/my-profile.json`. Replace every placeholder
and use the controller values read from your own drive. Example zeros are not
valid measurements. `sectors` selects the requested logical capacity; it does
not change NAND geometry or override bad-block limits.

The factory source follows the reviewed SBBM61.3/SBFM61.3 flow. Reusing it with
other firmware or NAND requires source review and new tests. The scanner uses
logical vendor port 1, mapped to the exact configured PNP device. This is distinct
from the Windows disk index. The native vendor adapter supports indices 1..255.

## 2. RAM load and diagnostics

These commands display plans by default. `--execute` sends commands to the
configured SSD. RAM loading executes Burner code; unlocking and controller
queries change access state. They do not initialize NAND.

```powershell
python recovery/session.py ram --profile build/my-profile.json --firmware C:\inputs\SBFM61.3_11032021.BIN
# Append --execute to load and run the Burner in RAM.

python recovery/session.py diagnose --profile build/my-profile.json
# If the current engineering mode needs unlocking, use diagnose-unlock instead.
# Append --execute and --output build/diagnostics to save real responses.

python recovery/session.py controller --profile build/my-profile.json
# Append --execute to read the controller identity using the access latch.
```

The unlock route calls psychite's implementation through a restricted transport;
engineering-mode transitions using unrelated ATA commands are rejected. The
controller route restores the access latch to zero after successful reads.
After a transport failure it stops; power-cycle before further work because the
latch may remain enabled. Unexpected identity or ATA status stops the session.

## 3. Prepare a private factory package

This step reads local files only. Supply the original matching vendor settings,
including the correct NAND geometry. A preformat payload may be obtained from
an offline capture of the matching vendor initialization flow. Capture emulation
is separate research tooling; do not substitute fabricated device responses.

```powershell
python recovery/prepare.py --profile build/my-profile.json --firmware C:\inputs\SBFM61.3_11032021.BIN --infoblock C:\inputs\original-info.bin --preformat C:\inputs\preformat.bin --nand-id C:\inputs\nand-id.bin --vendor-dir C:\inputs\MPTool --output build/factory
.\recovery\build.ps1 -Package build/factory -Compiler C:\tools\tcc-x86\tcc.exe
python recovery/patch_vendor.py --vendor-dir C:\inputs\MPTool --output build/factory
python recovery/launch.py --package build/factory --seal
```

Preparation verifies the firmware hash, model/serial, original InfoBlock,
capacity-only changes, NAND prefix and preformat prefix. The factory guard pins
the entire NAND response and outgoing firmware/configuration payloads. Compiling
and sealing do not run the factory runner. The smoke test runs only in memory.

## 4. Factory preflight and initialization

These operations require the matching Burner already running in RAM.

```powershell
# Check the package hashes and display a plan; no hardware access.
python recovery/launch.py --package build/factory

# Elevated console: controller/NAND checks and vendor preflight.
# This includes VUC access and the temporary controller access latch.
python recovery/launch.py --package build/factory --operation preflight --execute

# Destructive: initialize NAND and install the prepared firmware/configuration.
python recovery/launch.py --package build/factory --operation initialize --execute --accept-data-loss

# Destructive: manufacturer erase first, then initialization.
python recovery/launch.py --package build/factory --operation initialize-with-erase --execute --accept-data-loss
```

The launcher checks package hashes and Windows target metadata, including SATA
bus and boot/system flags. The native guard checks the exact PNP identity,
Windows index, actual ATA state, controller identity, NAND response, command
sequence and payloads. Initialization uses MP_API's real health checks and polls
the device; no synthetic health values or constant READY responses are used.
Any vendor message aborts the runner. Package hashes detect accidental changes;
they are not a security boundary against a modified local build.

After completion, follow the tool's power-cycle instructions and verify the
reported identity and capacity. A successful initialization does not establish
drive reliability: perform a complete write/read test before trusting it.

## Diagnostic GUI

The patcher can also create the GUI used for SATA identification/NAND queries.
Prepare a separate directory and build its guard with `-Mode Nand` (or
`-Mode Identify`), then patch with `--gui`. Launch the generated
`SSDMPTool_SATA_DIAGNOSTIC.exe` manually as administrator.

The GUI's Start button stays disabled. The factory workflow uses the separate
runner above; enabling Start would bypass that launcher's checks. Never replace
a diagnostic guard with a factory guard and assume the GUI has the same policy.

## Source map

| File | Purpose |
|---|---|
| `session.py` | Native Python RAM plan, NAND/InfoBlock diagnostics and controller query |
| `prepare.py` | Per-device profile and firmware/configuration generation |
| `patch_vendor.py` | Hash-pinned PE import and scanner patching |
| `native/sata_guard.c` | SAT12/SAT16 conversion and guarded Windows imports |
| `native/target_guard.c` | Exact-device selection and native command dispatch |
| `native/factory_policy.h` | Factory command state machine and payload checks |
| `native/factory_runner.c` | Vendor preflight, optional erase and initialization |
| `build.ps1` | x86 compilation and memory-only smoke test |
| `launch.py` | Package sealing, metadata checks and explicit execution |
| `verify_media.py` | Destructive sample or full write/flush/read verification |
| `offline/` | Synthetic-device capture source and private fixture preparation |

The default read-only CLI in `src/` remains separate from these recovery tools.

## Write/read verification

After a normal boot, use the same profile with its normal PNP identity and
configured capacity. Take only the selected SSD offline in Windows first.
The test requires an offline SATA disk and verifies model, serial, firmware
and capacity, including a fresh ATA check on the handle used for writes.

```powershell
# Display a plan for 64 distributed 1 MiB samples.
python recovery/verify_media.py --profile build/my-profile.json

# Destructive: write, flush and read the entire configured logical capacity.
python recovery/verify_media.py --profile build/my-profile.json --full --execute --accept-data-loss
```

A failed read, write, flush or comparison stops the test and saves the byte
counts and error to `build/media-test.json`. Passing a sample is not a
substitute for a full test. The test overwrites existing data.

## Offline capture of vendor-generated payloads

`offline/` contains the capture implementation used to inspect MP_API's factory
flow. It supplies a synthetic handle and intercepts device operations entirely
in memory. It includes synthetic health responses for branch exploration;
they must never be used as evidence of a working SSD or copied into a hardware
guard. The factory runtime above uses real device responses instead.

Place private measured files in a fixture directory:
`identify-rom.bin` (512 bytes), `nand-id.bin` (512), `infoblock.bin` (512),
`system-info.bin` (512, SBBM61.3), `flash-header.bin` (4096). Optional
`nand-limits.bin` and `nand-limits-240.bin` provide the previously read limit
responses. The profile supplies the controller register values. An unavailable
fixture stops the relevant simulated query; it does not justify inventing
hardware results.

```powershell
python recovery/offline/prepare_capture.py --profile build/my-profile.json --firmware C:\inputs\SBFM61.3_11032021.BIN --fixtures C:\inputs\fixtures --vendor-dir C:\inputs\MPTool --output build/capture
.\recovery\offline\build.ps1 -Package build/capture -Compiler C:\tools\tcc-x86\tcc.exe
python recovery/patch_vendor.py --vendor-dir C:\inputs\MPTool --output build/capture
```

To run the offline capture, open `build/capture` as the working directory and
run `.\capture_runner.exe --pretest-simulation`. It checks the capture-only DLL
marker before loading MP_API. Use only the capture guard in this directory;
never substitute the real factory guard. The emitted
`offline-payload-*-feature-08.bin` is the 1024-byte preformat template used by
`prepare.py`. Keep captures separate from live recovery packages.
