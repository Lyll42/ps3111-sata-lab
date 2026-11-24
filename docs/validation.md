# Local release validation

Checked on Windows with Python 3.15, without accessing an SSD:

- 19 offline unit tests passed from the source tree.
- A wheel was built successfully with isolated local build dependencies.
- The same 19 tests passed when importing directly from the built wheel.
- CLI help loaded from the wheel.
- Native x86 sources compiled with TinyCC 0.9.27.
- The native memory-only smoke test passed (DLL loading, conversion and rejection).

The source archive is assembled from Git's tracked-file list. Build outputs,
private target configuration, firmware, vendor programs and local logs are
excluded. Public sources were checked for known private device identities
and local user paths.

The [initial GitHub Actions run](https://github.com/Lyll42/ps3111-sata-lab/actions/runs/45472586857)
passed all four combinations of Windows/Ubuntu and Python 3.11/3.13.
Other Python versions and Linux have not been tested locally. These offline
checks do not establish hardware compatibility of the public package. Hardware observations
refer to the earlier single-device investigation in [case-study.md](case-study.md).

## Recovery source export

The separate `recovery/` workflow was checked without opening a real device:

- 26 offline Python tests passed, including target rejection, diagnostic command
  boundaries and media-test ranges/patterns.
- Local preparation produced all five firmware/configuration payloads byte for
  byte equal to those in the original 120 GB build. Private inputs stayed local.
- The generated native factory guard and runner compiled as Windows x86, and
  the memory-only bridge smoke test passed.
- Four existing CPU-emulator checks passed against the newly compiled factory
  guard: capacity/payload pinning, the full firmware write/readback sequence,
  wrong-device rejection and capacity-only InfoBlock changes. These used local
  historical fixtures, which are not distributed.
- The RAM planner reproduced the 80-command sequence using the separately
  installed psychite source and the local pinned firmware. No RAM load ran.
- Vendor import/scanner patching, package sealing and the launcher's default
  offline plan completed using local copies. No vendor executable was run.
- Offline capture sources compiled. The capture program was not executed.

The configurable Python recovery entry points and media writer have not been
run against hardware. The factory C sources come from the original workflow
with per-device values moved into generated configuration. Offline validation
does not extend the single-device hardware results to other SSDs.
