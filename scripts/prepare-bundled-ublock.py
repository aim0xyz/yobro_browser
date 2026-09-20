#!/usr/bin/env python3
"""Apply the reviewed YoBro engine-detection patch to the official Safari ZIP."""
import hashlib
import io
import json
from pathlib import Path
import sys
import zipfile

source, destination = map(Path, sys.argv[1:3])
original = source.read_bytes()
expected = "f6d5190ad8df4c80d11e19cdf69cd0b0eefb1d1facbc82cb2d5262d3b88cab69"
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
            if manifest["version"] != "2026.914.1325":
                raise SystemExit("Unexpected manifest version")
            manifest["version"] = "2026.914.1325.1"
            manifest["version_name"] = "2026.914.1325 + YoBro WebKit compatibility 1"
            data = (json.dumps(manifest, indent=2) + "\n").encode()
        target.writestr(info, data)
    notice = zipfile.ZipInfo("YOBRO-COMPATIBILITY.txt", (2026, 9, 17, 0, 0, 0))
    target.writestr(notice, "YoBro compatibility patch 1: recognize webkit-extension: as Safari in js/ext.js. Manifest version incremented to reinitialize cached filtering rules. All filtering code and lists otherwise unchanged. Based on official uBlock Origin Lite Safari 2026.914.1325. GPLv3; source: https://github.com/gorhill/uBlock/tree/master/platform/mv3\n")
destination.write_bytes(output.getvalue())
print(hashlib.sha256(output.getvalue()).hexdigest())
