# Missing package resource: xor_0561_data.bin

Status: observed locally; not yet reported upstream.

At commit `15a8a845566903e2b1824bcf8da043848a187fdc`, the installed package on
Windows failed to import phison_s11.algorithm because the resource
`phison_s11/xor_0561_data.bin` was absent from site-packages. A source checkout
containing the resource worked after correcting the package-data inclusion.

Observed exception: FileNotFoundError when algorithm.py evaluates
`resources.files(__package__).joinpath("xor_0561_data.bin").read_bytes()`.

Suggested investigation: build a wheel, install it into a clean environment,
and check the wheel file list and the algorithm import. This was observed on
Windows with Python 3.15; other versions have not been established as affected.

The local packaging workaround was:

```toml
[tool.setuptools.package-data]
phison_s11 = ["xor_0561_data.bin"]
```

This workaround resolved the local import failure. A clean wheel build and
installation should confirm the fix before submitting an upstream patch.
