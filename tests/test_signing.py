import sys
sys.dont_write_bytecode = True
import datetime
import importlib.util
from pathlib import Path
import unittest
spec = importlib.util.spec_from_file_location('signing', Path(__file__).parents[1] / 'scripts/sign-passkey-build.py')
signing = importlib.util.module_from_spec(spec); spec.loader.exec_module(signing)

class SigningTests(unittest.TestCase):
    def profile(self):
        return {'ExpirationDate':datetime.datetime.now(datetime.timezone.utc)+datetime.timedelta(days=1),'Entitlements':{'com.apple.application-identifier':'TEST.local.yobro.browser','com.apple.developer.team-identifier':'TEST','com.apple.developer.web-browser.public-key-credential':True}}
    def test_approved_exact_profile(self):
        self.assertTrue(signing.validate(self.profile(),'local.yobro.browser')['com.apple.developer.web-browser.public-key-credential'])
    def test_reject_missing_approval_wrong_app_and_expired(self):
        profile=self.profile(); profile['Entitlements'].pop('com.apple.developer.web-browser.public-key-credential')
        with self.assertRaises(ValueError): signing.validate(profile,'local.yobro.browser')
        with self.assertRaises(ValueError): signing.validate(self.profile(),'wrong.app')
        profile=self.profile(); profile['ExpirationDate']=datetime.datetime(2000,1,1)
        with self.assertRaises(ValueError): signing.validate(profile,'local.yobro.browser')
