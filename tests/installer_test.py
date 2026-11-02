#!/usr/bin/env python3
"""Exercise the shipped bootstrap with fake transport and isolated install homes."""
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

INSTALLER = Path(__file__).resolve().parents[1] / 'install.sh'

class Installer(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name); self.bin = self.root / 'tools'; self.bin.mkdir()
        self.payload = self.root / 'payload'
        self.payload.write_text('#!/bin/sh\nprintf "%s\\n" "$@" > "$TEST_CALLED"\n')
        self.called = self.root / 'called'; self.urls = self.root / 'urls'
        curl = self.bin / 'curl'
        curl.write_text('''#!/usr/bin/env python3
import hashlib,os,sys
from pathlib import Path
args=sys.argv[1:]
assert args[args.index('--proto')+1]=='=https'
assert args[args.index('--proto-redir')+1]=='=https'
url=args[args.index('--output')-1]; dest=Path(args[args.index('--output')+1])
with open(os.environ['TEST_URLS'],'a') as out: out.write(url+'\\n')
asset=url.rsplit('/',1)[-1]
payload=Path(os.environ['TEST_PAYLOAD']).read_bytes()
if asset.endswith('.sha256'):
    digest=hashlib.sha256(payload).hexdigest()
    if os.environ.get('TEST_BAD_HASH'): digest='0'*64
    filename=asset[:-7]
    if os.environ.get('TEST_BAD_NAME'): filename='unexpected'
    dest.write_text(digest+'  '+filename+'\\n')
elif asset.endswith('.licenses.txt'): dest.write_text('fixture license')
else: dest.write_bytes(payload)
''')
        curl.chmod(0o755)
        self.env={**os.environ,'PATH':str(self.bin)+os.pathsep+os.environ['PATH'],
                  'GTIUP_HOME':str(self.root/'home with spaces'), 'TEST_PAYLOAD':str(self.payload),
                  'TEST_CALLED':str(self.called),'TEST_URLS':str(self.urls)}
        self.env.pop('GTIUP_VERSION',None)

    def run_install(self,*args,ok=True,**extra):
        p=subprocess.run(['sh',str(INSTALLER),*args],env={**self.env,**extra},capture_output=True,text=True,timeout=15)
        self.assertEqual(p.returncode==0,ok,p.stdout+p.stderr)
        return p

    def test_default_and_argument_forwarding(self):
        self.run_install('--default-toolchain','preview')
        self.assertEqual(self.called.read_text().splitlines(),['self-install','--default-toolchain','preview'])
        self.assertTrue(all('/releases/latest/download/' in x for x in self.urls.read_text().splitlines()))
        self.assertEqual((Path(self.env['GTIUP_HOME'])/'share/gtiup/licenses.txt').read_text(),'fixture license')

    def test_pinned_manager_and_manager_only(self):
        self.run_install(GTIUP_VERSION='v0.1.0')
        self.assertEqual(self.called.read_text().splitlines(),['self-install'])
        self.assertTrue(all('/releases/download/v0.1.0/' in x for x in self.urls.read_text().splitlines()))

    def test_bad_checksum_never_executes(self):
        self.run_install(ok=False,TEST_BAD_HASH='1')
        self.assertFalse(self.called.exists())

    def test_wrong_checksum_filename_never_executes(self):
        self.run_install(ok=False,TEST_BAD_NAME='1')
        self.assertFalse(self.called.exists())

    def test_invalid_arguments_and_version_do_not_download(self):
        self.run_install('--unknown',ok=False)
        self.run_install(ok=False,GTIUP_VERSION='../other')
        self.assertFalse(self.urls.exists()); self.assertFalse(self.called.exists())

    def test_help_does_not_download(self):
        self.assertIn('Usage:',self.run_install('--help').stdout)
        self.assertFalse(self.urls.exists())

if __name__=='__main__': unittest.main()
