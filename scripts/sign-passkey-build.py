#!/usr/bin/env python3
"""Sign an YOBRO build only with an Apple-approved browser provisioning profile."""
import datetime
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile


def validate(profile, bundle_id):
    entitlements = profile.get('Entitlements', {})
    if entitlements.get('com.apple.developer.web-browser.public-key-credential') is not True:
        raise ValueError('The profile does not include Apple-approved browser passkey access.')
    application = entitlements.get('com.apple.application-identifier', '')
    team = entitlements.get('com.apple.developer.team-identifier', '')
    if not team or application != team + '.' + bundle_id:
        raise ValueError('The provisioning profile must match the exact YOBRO bundle identifier.')
    expires = profile.get('ExpirationDate')
    if not expires or expires.replace(tzinfo=datetime.timezone.utc) <= datetime.datetime.now(datetime.timezone.utc):
        raise ValueError('The provisioning profile has expired.')
    keys = ['com.apple.application-identifier', 'com.apple.developer.team-identifier',
            'com.apple.developer.web-browser.public-key-credential', 'com.apple.developer.web-browser']
    return {key: entitlements[key] for key in keys if key in entitlements}


def main():
    app = Path(sys.argv[1])
    identity = os.environ.get('YOBRO_SIGN_IDENTITY')
    profile_path = os.environ.get('YOBRO_PROVISIONING_PROFILE')
    if not identity or identity == '-' or not profile_path:
        raise ValueError('Set YOBRO_SIGN_IDENTITY and YOBRO_PROVISIONING_PROFILE for an approved build.')
    info = plistlib.loads((app / 'Contents/Info.plist').read_bytes())
    profile = plistlib.loads(subprocess.check_output(['security', 'cms', '-D', '-i', profile_path]))
    entitlements = validate(profile, info['CFBundleIdentifier'])
    with tempfile.TemporaryDirectory(prefix='yobro-sign-') as folder:
        path = Path(folder) / 'entitlements.plist'
        path.write_bytes(plistlib.dumps(entitlements))
        shutil.copyfile(profile_path, app / 'Contents/embedded.provisionprofile')
        subprocess.run(['codesign', '--force', '--deep', '--options', 'runtime', '--timestamp',
                        '--sign', identity, '--entitlements', str(path), str(app)], check=True)
        subprocess.run(['codesign', '--verify', '--deep', '--strict', str(app)], check=True)


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        sys.exit('Passkey build not signed: ' + str(error))
