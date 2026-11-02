#!/usr/bin/env python3
"""Black-box tests of the compiled manager; no compiler builds or public network."""
import hashlib
import http.server
import io
import json
import os
from pathlib import Path
import platform
import pty
import re
import select
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import unittest

TEST, PRODUCTION, UPDATE = map(lambda p: str(Path(p).resolve()), sys.argv[1:4])
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
        self.slow = {}
        self.requests = []
        parent = self
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                parent.requests.append(self.path)
                value = parent.routes.get(self.path)
                if value is None:
                    self.send_error(404); return
                data = json.dumps(value).encode() if isinstance(value, (dict, list)) else value
                declared = None
                if isinstance(data, tuple): data, declared = data
                slow = parent.slow.get(self.path)
                self.send_response(200)
                if slow is None or not slow[1]:
                    self.send_header('Content-Length', str(len(data) if declared is None else declared))
                self.end_headers()
                if slow:
                    split = len(data) // 2
                    self.wfile.write(data[:split]); self.wfile.flush()
                    slow[0].wait(10)
                    self.wfile.write(data[split:])
                else:
                    self.wfile.write(data)
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

    def package(self, tag='nightly', identity=SHA, missing=None, extra=(), version=None, compatibility=None):
        rolling = tag in ('nightly', 'preview')
        version = version or ('0.1.0' if rolling else tag[1:].split('-beta.')[0])
        stem = 'gti-' + tag if rolling else 'gti-v' + version
        asset = stem + '-' + HOST + '.tar.gz'
        data = {'bin/gti': '#!/bin/sh\nprintf "compiler\\n"\nprintf "<%s>\\n" "$@"\n',
                'bin/gti_lsp': '#!/bin/sh\nprintf "lsp\\n"\nprintf "<%s>\\n" "$@"\n',
                'share/gti/VERSION': version, 'share/gti/BUILD_ID': identity,
                'share/gti/PREVIEW_SOURCE': identity,
                'share/gti/preview.json': json.dumps({'schema': 1, 'features': [{'task': 'fixture'}]})}
        if compatibility is not None:
            data['share/gti/LSP_COMPATIBILITY.json'] = json.dumps({'schema': 1, 'compatibility': compatibility})
            data['share/gti/LSP_SOURCE'] = identity
            data['share/gti/parser/gti.so'] = 'matching parser fixture'
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

    def lsp_component(self, compatibility='c' * 64, run=123, source='b' * 40,
                      embedded=None, body=None):
        tag = f'lsp-{compatibility}-{run}'
        identity = {'schema': 1, 'kind': 'gti-lsp', 'compatibility': compatibility,
                    'source': source, 'run_id': run, 'attempt': 1, 'platform': HOST}
        entries = {'bin/gti_lsp': b'#!/bin/sh\necho patched-lsp\n',
                   'share/gti/LSP_COMPONENT.json': json.dumps(embedded or identity).encode()}
        stream = io.BytesIO()
        with tarfile.open(fileobj=stream, mode='w:gz') as archive:
            for name, value in entries.items():
                info = tarfile.TarInfo(name); info.size = len(value)
                info.mode = 0o755 if name.startswith('bin/') else 0o644
                archive.addfile(info, io.BytesIO(value))
        data = stream.getvalue() if body is None else body
        asset = f'gti-lsp-{HOST}.tar.gz'
        manifest = {k:v for k,v in identity.items() if k != 'platform'}
        manifest['platforms'] = {HOST: {'asset': asset, 'sha256': hashlib.sha256(data).hexdigest()}}
        self.routes['/api/releases?per_page=100&page=1'] = [
            {'tag_name': tag, 'target_commitish': source, 'draft': False, 'prerelease': True}]
        self.routes['/api/compare/' + SHA + '...' + source] = {'status': 'ahead'}
        base = '/downloads/' + tag + '/'
        self.routes[base + 'lsp.json'] = manifest
        self.routes[base + asset] = data
        return base, manifest

    def test_compatible_lsp_update_preserves_compiler_and_resources(self):
        self.package('preview', compatibility='c' * 64)
        self.run_cli('default', 'preview'); self.run_cli('self-install')
        before = self.state()
        original = self.home / 'toolchains' / before['toolchains']['preview']['directory']
        self.lsp_component()
        result = self.run_cli('update', 'lsp')
        self.assertIn('Updated LSP for preview', result.stdout)
        after = self.state(); self.assertEqual(before['default'], after['default'])
        current = self.home / 'toolchains' / after['toolchains']['preview']['directory']
        for name in ('bin/gti', 'share/gti/VERSION', 'share/gti/BUILD_ID', 'share/gti/parser/gti.so'):
            self.assertEqual((original / name).read_bytes(), (current / name).read_bytes())
            self.assertEqual((original / name).stat().st_ino, (current / name).stat().st_ino)
        self.assertNotEqual((original / 'bin/gti_lsp').stat().st_ino, (current / 'bin/gti_lsp').stat().st_ino)
        self.assertNotIn(b'patched-lsp', (original / 'bin/gti_lsp').read_bytes())
        self.assertEqual(self.run_cli('run', 'preview', 'gti_lsp', '--version').stdout, 'patched-lsp\n')
        self.assertIn('LSP component ' + 'b' * 40, self.run_cli('show').stdout)
        requests = len(self.requests)
        self.assertEqual(self.run_cli('--version', binary=str(self.home / 'bin/gti_lsp')).stdout, 'patched-lsp\n')
        self.assertIn('compiler', self.run_cli('--version', binary=str(self.home / 'bin/gti')).stdout)
        self.assertEqual(requests, len(self.requests))
        self.assertIn('up to date', self.run_cli('update', 'lsp', 'preview').stdout)
        self.lsp_component(run=122)
        self.assertIn('up to date', self.run_cli('update', 'lsp').stdout)
        self.assertEqual(self.state(), after)
        self.run_cli('gc')
        self.assertFalse(original.exists())
        self.assertEqual(self.run_cli('run', 'preview', 'gti_lsp').stdout, 'patched-lsp\n')
        self.run_cli('update', 'preview')
        self.assertNotIn('lsp', self.state()['toolchains']['preview'])
        self.assertNotIn('patched-lsp', self.run_cli('run', 'preview', 'gti_lsp').stdout)

    def test_lsp_update_rejects_mismatches_without_activation(self):
        self.package('preview', compatibility='c' * 64); self.run_cli('default', 'preview')
        before = self.state()
        for case in ('compatibility', 'source', 'run_id', 'attempt', 'asset', 'checksum', 'embedded', 'archive', 'missing'):
            with self.subTest(case=case):
                base, manifest = self.lsp_component()
                asset = f'gti-lsp-{HOST}.tar.gz'
                if case in ('compatibility', 'source'): manifest[case] = 'd' * (64 if case == 'compatibility' else 40)
                elif case == 'run_id': manifest['run_id'] = 122
                elif case == 'attempt': manifest['attempt'] = 0
                elif case == 'asset': manifest['platforms'][HOST]['asset'] = '../untrusted'
                elif case == 'checksum': self.routes[base + asset] = b'corrupted'
                elif case == 'embedded': self.lsp_component(embedded={'schema': 1, 'platform': 'wrong'})
                elif case == 'archive': self.lsp_component(body=b'not an archive')
                elif case == 'missing': del manifest['platforms'][HOST]
                self.run_cli('update', 'lsp', ok=False)
                self.assertEqual(self.state(), before)
                self.assertFalse(list((self.home / 'toolchains').glob('.gtiup-*')))
        self.lsp_component(compatibility='d' * 64)
        self.assertIn('No compatible', self.run_cli('update', 'lsp').stdout)
        self.assertEqual(self.state(), before)

    def test_lsp_update_cannot_downgrade_or_replace_unmerged_preview(self):
        self.package('preview', compatibility='c' * 64)
        self.run_cli('default', 'preview')
        before = self.state()
        self.lsp_component()
        for status in ('behind', 'diverged'):
            self.routes['/api/compare/' + SHA + '...' + 'b' * 40] = {'status': status}
            result = self.run_cli('update', 'lsp', ok=False)
            self.assertIn('older than or diverges', result.stderr)
            self.assertEqual(before, self.state())

    def test_lsp_update_uses_project_selection_and_handles_legacy_toolchains(self):
        self.package('nightly'); self.run_cli('default', 'nightly')
        self.assertIn('predates independent LSP', self.run_cli('update', 'lsp', ok=False).stderr)
        self.package('preview', compatibility='c' * 64); self.run_cli('install', 'preview')
        (self.cwd / 'gti-toolchain.toml').write_text('[toolchain]\nchannel = "preview"\n')
        self.lsp_component(); self.run_cli('update', 'lsp')
        self.assertEqual(self.state()['default'], 'nightly')
        self.assertIn('lsp', self.state()['toolchains']['preview'])
        self.assertNotIn('lsp', self.state()['toolchains']['nightly'])

    def test_all_channels_and_pinned_versions(self):
        for tag in ('preview', 'nightly', 'v0.2.0', 'v0.3.0-beta.2', 'v0.1.0'): self.package(tag)
        for channel in ('preview', 'nightly', 'beta', 'stable', '0.1.0', '0.3.0-beta.2'):
            self.run_cli('install', channel)
        state = self.state()
        self.assertEqual(state['toolchains']['stable']['tag'], 'v0.2.0')
        self.assertEqual(state['toolchains']['beta']['tag'], 'v0.3.0-beta.2')
        self.assertEqual(len(state['toolchains']), 6)
        self.assertIn('preview', self.run_cli('list').stdout)

    def test_download_stages_are_on_stderr_without_terminal_controls(self):
        self.package('preview')
        raw = subprocess.run([TEST, 'default', 'preview'], cwd=self.cwd, env=self.env,
                             capture_output=True, timeout=15)
        self.assertEqual(raw.returncode, 0, raw.stderr)
        self.assertNotIn(b'\r', raw.stderr); self.assertNotIn(b'\x1b', raw.stderr)
        stderr, stdout = raw.stderr.decode(), raw.stdout.decode()
        stages = ('Checking preview release', 'Downloading preview', 'Downloaded preview',
                  'Verifying archive checksum', 'Extracting preview', 'Verifying toolchain identity',
                  'Activating preview')
        for stage in stages:
            self.assertIn(stage, stderr)
        positions = [stderr.index(stage) for stage in stages]
        self.assertEqual(positions, sorted(positions))
        self.assertTrue(stdout.startswith('Installed preview:'))
        self.assertNotIn('Downloading', stdout)
        self.assertEqual(self.run_cli('default', 'preview').stderr, '')

    def test_live_terminal_progress_before_download_finishes(self):
        for unknown_size in (False, True):
            with self.subTest(unknown_size=unknown_size):
                path = self.package('preview')
                release = threading.Event()
                self.slow[path] = (release, unknown_size)
                master, slave = pty.openpty()
                process = subprocess.Popen([TEST, 'update', 'preview'], cwd=self.cwd, env=self.env,
                                           stdout=subprocess.PIPE, stderr=slave)
                os.close(slave)
                output = b''
                try:
                    deadline = time.monotonic() + 6
                    # Hold the second half of the response until progress is visible.
                    pattern = rb'MiB - [1-9][0-9]*s' if unknown_size else rb'MiB \(50%\)'
                    while time.monotonic() < deadline and not re.search(pattern, output):
                        if select.select([master], [], [], 0.2)[0]:
                            output += os.read(master, 8192)
                    self.assertIsNone(process.poll(), output)
                    self.assertIn(b'Downloading preview', output)
                    self.assertRegex(output, pattern)
                    self.assertNotIn(b'Installed', output)
                    if unknown_size:
                        self.assertNotIn(b'%)', output)
                finally:
                    release.set()
                    stdout, _ = process.communicate(timeout=10)
                    os.close(master)
                self.assertEqual(process.returncode, 0, output)
                self.assertIn(b'Installed preview', stdout)

    def test_interrupted_download_never_reports_success(self):
        path = self.package('preview')
        self.routes[path] = (b'partial', 100)
        result = self.run_cli('default', 'preview', ok=False)
        self.assertIn('Downloading preview', result.stderr)
        self.assertIn('download failed', result.stderr)
        self.assertNotIn('Downloaded preview', result.stderr)
        self.assertNotIn('Activating', result.stderr)
        self.assertNotIn('Installed', result.stdout)
        self.assertFalse((self.home / 'settings.json').exists())

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

    def manager_release(self, tag='v99.0.0', body=None):
        self.routes['/manager-api/releases/latest'] = {'tag_name': tag, 'draft': False, 'prerelease': False}
        base = '/manager-downloads/' + tag + '/gtiup-' + HOST
        body = Path(UPDATE).read_bytes() if body is None else body
        self.routes[base] = body
        self.routes[base + '.sha256'] = (hashlib.sha256(body).hexdigest() + '  gtiup-' + HOST + '\n').encode()
        self.routes[base + '.licenses.txt'] = b'Updated manager license notice\n'
        return base

    def installed_manager(self):
        self.run_cli('self-install')
        return str(self.home / 'bin/gtiup')

    def manager_snapshot(self):
        files = ('bin/gtiup', 'manager.json', 'settings.json', 'share/gtiup/licenses.txt', 'env')
        return {name: (self.home / name).read_bytes() if (self.home / name).exists() else None for name in files}

    def test_self_update_preserves_toolchains_proxies_and_settings(self):
        self.package('preview'); self.run_cli('default', 'preview')
        binary = self.installed_manager()
        (self.home / 'env').write_text('user shell settings')
        before = self.manager_snapshot()
        base = self.manager_release()
        result = self.run_cli('self', 'update', binary=binary)
        self.assertIn('-> 99.0.0', result.stdout)
        self.assertIn('Checking for gtiup updates', result.stderr)
        self.assertIn('Downloading gtiup 99.0.0', result.stderr)
        self.assertIn('Verifying gtiup checksum and version', result.stderr)
        self.assertEqual(self.run_cli('--version', binary=binary).stdout.strip(), 'gtiup 99.0.0')
        after = self.manager_snapshot()
        self.assertEqual(after['settings.json'], before['settings.json'])
        self.assertEqual(after['env'], before['env'])
        self.assertEqual(after['bin/gtiup'], Path(UPDATE).read_bytes())
        metadata = json.loads(after['manager.json'])
        self.assertEqual(metadata['version'], '99.0.0')
        self.assertEqual(metadata['sha256'], hashlib.sha256(after['bin/gtiup']).hexdigest())
        self.assertEqual(after['share/gtiup/licenses.txt'], self.routes[base + '.licenses.txt'])
        for tool in ('gti', 'gti_lsp'):
            proxy = self.home / 'bin' / tool
            self.assertEqual(os.readlink(proxy), 'gtiup')
            self.run_cli('--version', binary=str(proxy))
        requests = len(self.requests)
        self.assertIn('already up to date', self.run_cli('self', 'update', binary=binary).stdout)
        self.assertEqual(self.requests[requests:], ['/manager-api/releases/latest'])
        self.assertEqual(after, self.manager_snapshot())
        self.assertFalse(list((self.home / 'bin').glob('.gtiup-*')))

    def test_self_update_rejects_bad_downloads_without_replacement(self):
        binary = self.installed_manager(); before = self.manager_snapshot()
        cases = ('checksum', 'filename', 'missing', 'interrupted', 'version', 'executable', 'license', 'timeout', 'output')
        for case in cases:
            with self.subTest(case=case):
                base = self.manager_release()
                if case == 'checksum': self.routes[base] = b'corrupt download'
                if case == 'filename': self.routes[base + '.sha256'] = (('0' * 64) + '  other-binary\n').encode()
                if case == 'missing': del self.routes[base]
                if case == 'interrupted': self.routes[base] = (b'partial', 100)
                if case == 'version': self.manager_release(body=Path(TEST).read_bytes())
                if case == 'executable': self.manager_release(body=b'not an executable')
                if case == 'license': del self.routes[base + '.licenses.txt']
                if case == 'timeout': self.manager_release(body=b'#!/bin/sh\nexec sleep 10\n')
                if case == 'output': self.manager_release(body=b'#!/bin/sh\nwhile :; do echo excessive-output; done\n')
                self.run_cli('self', 'update', binary=binary, ok=False)
                self.assertEqual(before, self.manager_snapshot())
                self.run_cli('--version', binary=binary)
                self.assertFalse(list((self.home / 'bin').glob('.gtiup-*')))

    def test_self_update_rollback_after_each_replacement(self):
        binary = self.installed_manager(); self.manager_release()
        license = self.home / 'share/gtiup/licenses.txt'; license.parent.mkdir(parents=True)
        for previous_license in (None, b'Original licenses\n'):
            if previous_license is None: license.unlink(missing_ok=True)
            else: license.write_bytes(previous_license)
            before = self.manager_snapshot()
            for step in ('binary', 'metadata', 'licenses'):
                result = self.run_cli('self', 'update', binary=binary, ok=False,
                                      env={**self.env, 'GTIUP_TEST_UPDATE_FAILURE': step})
                self.assertIn('injected manager update failure', result.stderr)
                self.assertEqual(before, self.manager_snapshot())
                self.run_cli('--version', binary=binary)

    def test_self_update_release_admission_and_no_downgrade(self):
        binary = self.installed_manager(); before = self.manager_snapshot()
        current = self.run_cli('--version').stdout.strip().split()[-1]
        for tag in ('v' + current, 'v0.0.1'):
            self.manager_release(tag=tag); start = len(self.requests)
            self.assertIn('already up to date', self.run_cli('self', 'update', binary=binary).stdout)
            self.assertEqual(self.requests[start:], ['/manager-api/releases/latest'])
        for field, value in (('tag_name', '../bad'), ('tag_name', 'v99.0.0-beta.1'),
                             ('draft', True), ('prerelease', True)):
            self.manager_release(); self.routes['/manager-api/releases/latest'][field] = value
            self.assertIn('invalid stable manager release', self.run_cli('self', 'update', binary=binary, ok=False).stderr)
            self.assertEqual(before, self.manager_snapshot())
        self.routes['/manager-api/releases/latest'] = {}
        self.run_cli('self', 'update', binary=binary, ok=False)
        self.assertEqual(before, self.manager_snapshot())

    def test_self_update_requires_owned_installation(self):
        self.run_cli('self', 'update', ok=False)
        self.assertEqual(self.requests, [])
        binary = self.installed_manager(); self.manager_release()
        metadata = self.home / 'manager.json'
        original = metadata.read_bytes()
        changed = json.loads(original); changed['sha256'] = '0' * 64
        metadata.write_text(json.dumps(changed))
        self.assertIn('not the recorded manager', self.run_cli('self', 'update', binary=binary, ok=False).stderr)
        metadata.write_bytes(original)
        license = self.home / 'share/gtiup/licenses.txt'; license.parent.mkdir(parents=True)
        unrelated = self.root / 'user-license'; unrelated.write_text('user content')
        license.symlink_to(unrelated)
        self.assertIn('license path', self.run_cli('self', 'update', binary=binary, ok=False).stderr)
        self.assertEqual(unrelated.read_text(), 'user content')
        self.assertEqual(Path(binary).read_bytes(), Path(TEST).read_bytes())
        for arguments in (('self',), ('self', 'update', 'nightly'), ('self', 'other')):
            self.assertIn('usage: gtiup self update', self.run_cli(*arguments, binary=binary, ok=False).stderr)

    def test_concurrent_self_updates_install_once(self):
        binary = self.installed_manager(); base = self.manager_release()
        processes = [subprocess.Popen([binary, 'self', 'update'], cwd=self.cwd, env=self.env,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE) for _ in range(2)]
        for process in processes:
            stdout, stderr = process.communicate(timeout=20)
            self.assertEqual(process.returncode, 0, stderr)
        self.assertEqual(self.requests.count(base), 1)
        self.assertEqual(self.run_cli('--version', binary=binary).stdout.strip(), 'gtiup 99.0.0')
        self.assertEqual(json.loads((self.home / 'manager.json').read_text())['sha256'], hashlib.sha256(Path(binary).read_bytes()).hexdigest())

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
        self.assertNotIn(b'GTIUP_TEST_UPDATE_FAILURE',Path(PRODUCTION).read_bytes())
        self.assertNotIn(b'http://127.0.0.1:',Path(PRODUCTION).read_bytes())


if __name__ == '__main__': unittest.main()
