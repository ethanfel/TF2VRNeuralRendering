#!/usr/bin/env python3
"""Identify known NR DLL bytes without loading or enabling neural rendering.

Exit code 0 means a recognized file, not runtime or VR compatibility.
This is not an independent Authenticode verification.
"""
import argparse
import hashlib
import json
from pathlib import Path

EXPECTED = "e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e"
NVIDIA_SDK_2 = "2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f"
KNOWN = {
    EXPECTED: {
        "version": "310.8.0.0",
        "source": "tf2-nr-linux upstream runtime pin",
        "minimum_driver_version_from_file": None,
    },
    NVIDIA_SDK_2: {
        "version": "310.9.16.0",
        "source": "https://d1oqksizpb0e3d.cloudfront.net/rtx-remix-ngx_sdk_dlnr@2.7z",
        "minimum_driver_version_from_file": "615.00",
    },
}
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("dll", type=Path)
args = parser.parse_args()
try:
    with args.dll.open("rb") as f:
        digest = hashlib.file_digest(f, "sha256").hexdigest()
except OSError as exc:
    parser.error(str(exc))
matches = digest == EXPECTED
identity = KNOWN.get(digest)
print(json.dumps({"file": str(args.dll.resolve()), "sha256": digest,
                  "recognized_runtime": identity is not None,
                  "identity": identity,
                  "matches_pinned_310_8_0_0": matches,
                  "matches_nvidia_sdk_2": digest == NVIDIA_SDK_2,
                  "authenticode_verified": False,
                  "vr_compatibility_tested": False}, indent=2))
raise SystemExit(0 if identity is not None else 1)
