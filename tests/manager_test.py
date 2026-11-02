#!/usr/bin/env python3
"""Black-box tests of the compiled manager; no compiler builds or public network."""
import hashlib
import http.server
import io
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import tarfile
import tempfile
import threading
import unittest

TEST, PRODUCTION = map(lambda p: str(Path(p).resolve()), sys.argv[1:3])
sys.argv[1:] = []
HOST = 'darwin-arm64' if sys.platform == 'darwin' else 'linux-x64'
SHA = 'a' * 40


class Manager(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.home = self.root / 'home with spaces'
        self.cwd = self.root / 'project'; self.cwd.mkdir()
        self.routes = {}
        self.requests = []
        parent = self
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                parent.requests.append(self.path)
                value = parent.routes.get(self.path)
                if value is None:
                    self.send_error(404); return
                data = json.dumps(value).encode() if isinstance(value, (dict, list)) else value
                self.send_response(200); self.send_header('Content-Length', str(len(data))); self.end_headers(); self.wfile.write(data)
            def log_message(self, *args): pass
        self.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True); self.thread.start()
        self.addCleanup(self.server.server_close); self.addCleanup(self.server.shutdown)
        self.env = {**os.environ, 'GTIUP_HOME': str(self.home), 'GTIUP_TOOLCHAIN': '',
                    'GTIUP_TEST_SERVER': f'http://127.0.0.1:{self.server.server_port}'}
        self.routes['/status'] = {'schema': 1, 'stable': ['0.1.0', '0.2.0'], 'yanked': [], 'withdrawn': []}
        self.routes['/api/releases?per_page=100&page=1'] = [{'tag_name': 'v0.3.0-beta.2', 'draft': False, 'prerelease': True}]

    def run_cli(self, *args, ok=True, binary=TEST, cwd=None, env=None):
        result = subprocess.run([binary, *args], cwd=cwd or self.cwd, env=env or self.env, capture_output=True, text=True, timeout=15)
        if ok: self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else: self.assertNotEqual(result.returncode, 0, result.stdout)
        return result

    def state(self): return json.loads((self.home / 'settings.json').read_text())

    def package(self, tag='nightly', identity=SHA, missing=None, extra=(), version=None):
        rolling = tag in ('nightly', 'preview')
        version = version or ('0.1.0' if rolling else tag[1:].split('-beta.')[0])
        stem = 'gti-' + tag if rolling else 'gti-v' + version
        asset = stem + '-' + HOST + '.tar.gz'
        data = {'bin/gti': '#!/bin/sh\nprintf "compiler\\n"\nprintf "<%s>\\n" "$@"\n',
                'bin/gti_lsp': '#!/bin/sh\nprintf "lsp\\n"\nprintf "<%s>\\n" "$@"\n',
                'share/gti/VERSION': version, 'share/gti/BUILD_ID': identity,
                'share/gti/PREVIEW_SOURCE': identity,
                'share/gti/preview.json': json.dumps({'schema': 1, 'features': [{'task': 'fixture'}]})}
        stream = io.BytesIO()
        with tarfile.open(fileobj=stream, mode='w:gz') as tar:
            for name, value in data.items():
                if name == missing: continue
                info = tarfile.TarInfo(name); encoded = value.encode(); info.size = len(encoded)
                info.mode = 0o755 if name.startswith('bin/') else 0o644
                tar.addfile(info, io.BytesIO(encoded))
            for info, value in extra:
                tar.addfile(info, io.BytesIO(value) if value is not None else None)
        body = stream.getvalue(); base = '/downloads/' + tag + '/'
        self.routes[base + asset] = body
        self.routes[base + asset + '.sha256'] = (hashlib.sha256(body).hexdigest() + '  ' + asset + '\n').encode()
        self.routes[base + 'BUILD_ID'] = SHA.encode()
        self.routes['/api/releases/tags/' + tag] = {'tag_name': tag, 'draft': False, 'prerelease': rolling or '-beta.' in tag}
        return base + asset

    def test_all_channels_and_pinned_versions(self):
        for tag in ('preview', 'nightly', 'v0.2.0', 'v0.3.0-beta.2', 'v0.1.0'): self.package(tag)
        for channel in ('preview', 'nightly', 'beta', 'stable', '0.1.0', '0.3.0-beta.2'):
            self.run_cli('install', channel)
        state = self.state()
        self.assertEqual(state['toolchains']['stable']['tag'], 'v0.2.0')
        self.assertEqual(state['toolchains']['beta']['tag'], 'v0.3.0-beta.2')
        self.assertEqual(len(state['toolchains']), 6)
        self.assertIn('preview', self.run_cli('list').stdout)

    def test_selection_proxy_arguments_and_offline_resolution(self):
        for tag in ('preview', 'nightly'): self.package(tag); self.run_cli('install', tag)
        self.run_cli('default', 'nightly'); self.run_cli('self-install')
        (self.cwd / 'gti-toolchain').write_text('preview\n'); nested = self.cwd / 'nested'; nested.mkdir()
        self.assertIn('preview-', self.run_cli('which', 'gti', cwd=nested).stdout)
        self.assertIn('nightly-', self.run_cli('which', 'gti', cwd=nested, env={**self.env, 'GTIUP_TOOLCHAIN': 'nightly'}).stdout)
        before = len(self.requests)
        result = self.run_cli('+nightly', 'file with spaces.gti', '--help', binary=str(self.home / 'bin/gti'), cwd=nested)
        self.assertEqual(result.stdout, 'compiler\n<file with spaces.gti>\n<--help>\n')
        self.assertIn('lsp', self.run_cli('run', 'preview', 'gti_lsp', '--version').stdout)
        self.assertIn('preview', self.run_cli('show').stdout)
        self.assertEqual(before, len(self.requests))
        (self.cwd / 'gti-toolchain').write_text('nonsense')
        self.assertIn('gti-toolchain: invalid TOML', self.run_cli('which', 'gti', ok=False).stderr)

    def test_toml_channels_and_versions(self):
        for tag in ('preview', 'nightly', 'v0.2.0', 'v0.3.0-beta.2'): self.package(tag)
        for channel in ('preview', 'nightly', 'beta', 'stable', '0.2.0', 'v0.3.0-beta.2'):
            self.run_cli('install', channel)
            (self.cwd / 'gti-toolchain.toml').write_text(f'# Project pin\n[toolchain]\nchannel = "{channel}" # comment\n')
            expected = 'v0.2.0' if channel == '0.2.0' else channel
            self.assertEqual(self.run_cli('show').stdout.splitlines()[0], expected)

    def test_toml_precedence_and_proxies(self):
        for tag in ('preview', 'nightly'): self.package(tag); self.run_cli('install', tag)
        self.run_cli('default', 'nightly'); self.run_cli('self-install')
        (self.cwd / 'gti-toolchain').write_text('nightly\n')
        child = self.cwd / 'child'; child.mkdir()
        nested = child / 'nested'; nested.mkdir()
        config = child / 'gti-toolchain.toml'
        config.write_text("[toolchain]\r\nchannel = 'preview'\r\n")
        before = len(self.requests)
        for tool in ('gti', 'gti_lsp'):
            self.assertIn('preview-', self.run_cli('which', tool, cwd=nested).stdout)
            self.assertIn('nightly-', self.run_cli('which', tool, cwd=nested, env={**self.env, 'GTIUP_TOOLCHAIN': 'nightly'}).stdout)
            result = self.run_cli('argument with spaces', binary=str(self.home / 'bin' / tool), cwd=nested)
            self.assertIn('<argument with spaces>', result.stdout)
        # Legacy filename takes priority in the same directory, even over bad TOML.
        (child / 'gti-toolchain').write_text('nightly\n')
        config.write_text('invalid')
        self.assertEqual(self.run_cli('show', cwd=nested).stdout.splitlines()[0], 'nightly')
        (child / 'gti-toolchain').write_text('[toolchain]\nchannel = "preview"\n')
        self.assertEqual(self.run_cli('show', cwd=nested).stdout.splitlines()[0], 'preview')
        (child / 'gti-toolchain').unlink()
        # Explicit selections bypass a broken project configuration.
        self.run_cli('+nightly', '--version', binary=str(self.home / 'bin/gti'), cwd=nested)
        self.run_cli('run', 'nightly', 'gti_lsp', '--version', cwd=nested)
        self.run_cli('show', cwd=nested, env={**self.env, 'GTIUP_TOOLCHAIN': 'nightly'})
        self.assertEqual(before, len(self.requests))

    def test_toml_syntax_and_invalid_files_fail_closed(self):
        self.package(); self.run_cli('default', 'nightly')
        config = self.cwd / 'gti-toolchain.toml'
        for text in ('toolchain.channel = "nightly"', 'toolchain = { channel = "nightly" }',
                     '["toolchain"]\n"channel" = "night\\u006cy"'):
            config.write_text(text)
            self.assertEqual(self.run_cli('show').stdout.splitlines()[0], 'nightly')
        before = len(self.requests)
        invalid = ('', 'nightly', '[toolchain]', 'channel = "nightly"',
                   '[toolchain]\nchannel = 1', '[toolchain]\nchannel = ["nightly"]',
                   '[toolchain]\nchannel = "nonsense"', '[toolchain]\nchannel = " nightly "',
                   '[toolchain]\nchannel = "nightly"\nchannel = "preview"',
                   '[toolchain]\nchannel = "nightly', '[toolchain]\nchannel = "nightly" trailing',
                   '[[toolchain]]\nchannel = "nightly"', '[other]\nchannel = "nightly"',
                   '[toolchain]\nchannel = "nightly"\n[extra]',
                   '[toolchain]\nchannel = "nightly"\nchanell = "preview"',
                   '#' * (64 * 1024 + 1))
        for text in invalid:
            with self.subTest(text=text[:80]):
                config.write_text(text)
                self.assertIn(str(config), self.run_cli('show', ok=False).stderr)
        for field in ('components', 'targets', 'profile', 'path'):
            config.write_text(f'[toolchain]\nchannel = "nightly"\n{field} = "value"')
            self.assertIn('unsupported toolchain setting: ' + field, self.run_cli('show', ok=False).stderr)
        config.write_text('[toolchain]\nchannel = "preview"')
        self.assertIn('preview is not installed', self.run_cli('show', ok=False).stderr)
        # A broken legacy file must not fall through to the valid TOML sibling.
        (self.cwd / 'gti-toolchain').write_text('broken')
        self.assertIn('gti-toolchain: invalid TOML', self.run_cli('show', ok=False).stderr)
        self.assertEqual(before, len(self.requests))

    def test_update_failure_keeps_previous_selection(self):
        self.package(); self.run_cli('default', 'nightly'); before = self.state()
        self.package(identity='b' * 40)
        self.assertIn('changed during download', self.run_cli('update', 'nightly', ok=False).stderr)
        self.assertEqual(self.state(), before)
        asset = self.package(); self.routes[asset] = b'corrupted'
        self.assertIn('checksum mismatch', self.run_cli('update', 'nightly', ok=False).stderr)
        self.assertEqual(self.state(), before)
        self.assertFalse(list((self.home / 'toolchains').glob('.gtiup-*')))

    def test_revoked_releases_and_missing_metadata_fail_closed(self):
        self.package('v0.2.0'); self.package('v0.3.0-beta.2')
        self.routes['/status']['yanked'] = [{'version': '0.2.0'}]
        self.assertIn('yanked', self.run_cli('install', 'stable', ok=False).stderr)
        self.routes['/status']['withdrawn'] = [{'tag': 'v0.3.0-beta.2'}]
        self.assertIn('withdrawn', self.run_cli('install', 'beta', ok=False).stderr)
        del self.routes['/status']
        self.run_cli('install', '0.2.0', ok=False)
        self.assertFalse((self.home / 'settings.json').exists())

    def test_no_channel_fallback_and_version_identity(self):
        self.routes['/status']['stable'] = []
        self.assertIn('no stable release', self.run_cli('install', 'stable', ok=False).stderr)
        self.routes['/status']['stable'] = ['0.2.0']
        # Correct URL/checksum, but the archive's embedded version is wrong.
        asset = self.package('v0.2.0', version='0.1.0')
        expected = '/downloads/v0.2.0/gti-v0.2.0-' + HOST + '.tar.gz'
        self.routes[expected] = self.routes[asset]
        self.routes[expected + '.sha256'] = (hashlib.sha256(self.routes[asset]).hexdigest() + '  ' + expected.rsplit('/', 1)[1]).encode()
        self.assertIn('version mismatch', self.run_cli('install', 'stable', ok=False).stderr)

    def test_required_tool_and_preview_identity(self):
        self.package(missing='bin/gti_lsp')
        self.assertIn('lacks executable', self.run_cli('install', 'nightly', ok=False).stderr)
        self.package('preview', missing='share/gti/preview.json')
        self.run_cli('install', 'preview', ok=False)
        self.assertFalse((self.home / 'settings.json').exists())

    def test_archive_paths_links_duplicates_and_special_files(self):
        cases = []
        for name in ('../escape', '/absolute'):
            cases.append([(tarfile.TarInfo(name), None)])
        for link in ('../escape', '/absolute'):
            info = tarfile.TarInfo('link'); info.type = tarfile.SYMTYPE; info.linkname = link; cases.append([(info,None)])
        info = tarfile.TarInfo('fifo'); info.type = tarfile.FIFOTYPE; cases.append([(info,None)])
        info = tarfile.TarInfo('hard'); info.type = tarfile.LNKTYPE; info.linkname = 'bin/gti'; cases.append([(info,None)])
        cases.append([(tarfile.TarInfo('bin/gti'),None)])
        info = tarfile.TarInfo('link'); info.type = tarfile.SYMTYPE; info.linkname = 'actual'
        cases.append([(info,None),(tarfile.TarInfo('link/child'),None)])
        for entries in cases:
            with self.subTest(name=entries[0][0].name, target=entries[0][0].linkname):
                self.package(extra=entries); self.run_cli('install','nightly',ok=False)
                self.assertFalse((self.home / 'settings.json').exists())
                self.assertFalse((self.root / 'escape').exists())

    def test_existing_commands_and_manager_are_preserved(self):
        folder = self.home / 'bin'; folder.mkdir(parents=True)
        (folder / 'gti').write_text('user command')
        self.assertIn('refusing to overwrite', self.run_cli('self-install',ok=False).stderr)
        self.assertEqual((folder / 'gti').read_text(),'user command')
        (folder / 'gti').unlink(); (folder / 'gtiup').write_text('unrelated manager')
        self.run_cli('self-install',ok=False)
        self.assertEqual((folder / 'gtiup').read_text(),'unrelated manager')

    def test_self_install_reinstallation_and_no_implicit_download(self):
        self.run_cli('self-install'); self.run_cli('self-install')
        self.assertTrue((self.home / 'bin/gti').is_symlink())
        self.assertEqual(self.requests, [])
        self.run_cli('--version',binary=str(self.home / 'bin/gtiup'))
        self.run_cli('which','gti',ok=False)
        self.assertEqual(self.requests, [])

    def test_retention_and_explicit_gc(self):
        self.package(); self.run_cli('default','nightly'); old = self.run_cli('which','gti').stdout.strip()
        self.package(identity=SHA,extra=[(tarfile.TarInfo('another'),None)]); self.run_cli('update','nightly')
        self.assertTrue(Path(old).exists())
        unmanaged = self.home / 'toolchains/user-files'; unmanaged.mkdir()
        self.run_cli('gc'); self.assertFalse(Path(old).exists()); self.assertTrue(unmanaged.exists())
        self.run_cli('uninstall','nightly',ok=False)
        self.package('preview'); self.run_cli('default','preview'); self.run_cli('uninstall','nightly')
        self.assertNotIn('nightly',self.state()['toolchains'])

    def test_concurrent_installs_do_not_lose_selections(self):
        self.package('nightly'); self.package('preview')
        processes = [subprocess.Popen([TEST,'install',s],cwd=self.cwd,env=self.env,stdout=subprocess.PIPE,stderr=subprocess.PIPE) for s in ('nightly','preview')]
        for p in processes:
            stdout,stderr=p.communicate(timeout=20); self.assertEqual(p.returncode,0,stderr)
        self.assertEqual(set(self.state()['toolchains']),{'nightly','preview'})

    def test_production_binary_has_no_test_endpoint_override(self):
        # An invalid channel fails before a network request on either binary.
        self.run_cli('install','../invalid',binary=PRODUCTION,ok=False)
        self.assertEqual(self.requests,[])
        self.assertNotIn(b'GTIUP_TEST_SERVER',Path(PRODUCTION).read_bytes())
        self.assertNotIn(b'http://127.0.0.1:',Path(PRODUCTION).read_bytes())


if __name__ == '__main__': unittest.main()
