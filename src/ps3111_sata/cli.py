"""Dry-run default; explicit flag permits exactly one read-only ATA EC."""
import argparse
import json
from pathlib import Path
from .identify import Target, identify_once, inventory

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", type=Path, help="Private JSON fingerprint; see examples")
    parser.add_argument("--list", action="store_true", help="Read Windows metadata only, without opening raw devices")
    parser.add_argument("--execute-identify", action="store_true", help="Explicitly send one ATA IDENTIFY after target checks")
    args = parser.parse_args(argv)
    if args.list:
        if args.target or args.execute_identify:
            parser.error("--list is a separate metadata-only operation")
        print(json.dumps(inventory(), indent=2)); return 0
    if not args.target:
        parser.error("--target is required; hardware is never selected automatically")
    try:
        target = Target(**json.loads(args.target.read_text(encoding="utf-8")))
        if not args.execute_identify:
            print(json.dumps({"executed": False, "command": "ATA IDENTIFY DEVICE 0xEC", "read_bytes": 512, "write_commands": 0}, indent=2))
            return 0
        actual = identify_once(target)
        actual["serial"] = "[redacted]"
        print(json.dumps({"executed": True, "identity_matched": True, "identity": actual, "commands_sent": 1, "write_commands": 0}, indent=2))
        return 0
    except (OSError, ValueError, TypeError) as error:
        print(json.dumps({"stopped": True, "error": str(error)})); return 1

if __name__ == "__main__":
    raise SystemExit(main())
