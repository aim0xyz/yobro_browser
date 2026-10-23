#!/usr/bin/env python3
"""Apply the reviewed YoBro engine-detection patch to the official Safari ZIP."""
import hashlib
import io
import json
from pathlib import Path
import sys
import zipfile

# uBOL's filter lists are compiled into each release and never update on their
# own. YouTube breaks old releases within weeks, so follow upstream closely:
# https://github.com/uBlockOrigin/uAssets/issues/30158 names the minimum.
upstream = "2026.1021.1354"
expected = "6c80d19c128cd3f6b5d1ff2ef151fa0ff31565da3c5ef6841443381702b92838"

source, destination = map(Path, sys.argv[1:3])
original = source.read_bytes()
if hashlib.sha256(original).hexdigest() != expected:
    raise SystemExit("Unexpected upstream archive; review the new release before patching")
output = io.BytesIO()
with zipfile.ZipFile(io.BytesIO(original)) as archive, zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as target:
    for info in archive.infolist():
        data = archive.read(info.filename)
        if info.filename == "js/ext.js":
            old = b"if ( extURL.startsWith('safari-web-extension:') ) { return 'safari'; }"
            if data.count(old) != 1:
                raise SystemExit("Engine detection changed; manual review required")
            data = data.replace(old, b"// YoBro: WKWebExtensionController uses webkit-extension, with Safari APIs.\n    if ( /^(?:safari-web-extension|webkit-extension):/.test(extURL) ) { return 'safari'; }")
        elif info.filename == "manifest.json":
            manifest = json.loads(data)
            if manifest["version"] != upstream:
                raise SystemExit("Unexpected manifest version")
            manifest["version"] = f"{upstream}.1"
            manifest["version_name"] = f"{upstream} + YoBro WebKit compatibility 1"
            data = (json.dumps(manifest, indent=2) + "\n").encode()
        target.writestr(info, data)
    notice = zipfile.ZipInfo("YOBRO-COMPATIBILITY.txt", (2026, 10, 22, 0, 0, 0))
    target.writestr(notice, f"YoBro compatibility patch 1: recognize webkit-extension: as Safari in js/ext.js. Manifest version incremented to reinitialize cached filtering rules. All filtering code and lists otherwise unchanged. Based on official uBlock Origin Lite Safari {upstream}. GPLv3; source: https://github.com/gorhill/uBlock/tree/master/platform/mv3\n")
destination.write_bytes(output.getvalue())
print(hashlib.sha256(output.getvalue()).hexdigest())
