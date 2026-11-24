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
checks do not establish hardware compatibility of the cleaned package. Hardware observations
refer to the earlier single-device investigation in [case-study.md](case-study.md).
