#!/usr/bin/env python3
"""Package only the manager; never builds or embeds a GTI compiler."""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import tarfile

p=argparse.ArgumentParser(); p.add_argument('--build',type=Path,required=True);p.add_argument('--platform',choices=['linux-x64','darwin-arm64'],required=True);p.add_argument('--output',type=Path,default=Path('dist'));a=p.parse_args()
root=Path(__file__).resolve().parents[1]
binary=a.build/'gtiup'
version=subprocess.check_output([str(binary),'--version'],text=True).strip().split()[-1]
if not version or '/' in version: raise SystemExit('invalid manager version')
a.output.mkdir(parents=True,exist_ok=True)
name='gtiup-'+a.platform;target=a.output/name;shutil.copyfile(binary,target);target.chmod(0o755)
licenses=[root/'LICENSE',root/'licenses/json.txt',root/'licenses/zlib.txt',a.build/'_deps/libarchive-src/COPYING']
if a.platform=='linux-x64': licenses += [a.build/'_deps/curl-src/COPYING',root/'licenses/openssl.txt',root/'licenses/musl.txt',root/'licenses/gcc-gpl3.txt',root/'licenses/gcc-runtime-exception.txt']
notice='\n\n'.join('===== '+str(path.relative_to(root) if path.is_relative_to(root) else path.name)+' =====\n'+path.read_text() for path in licenses)
(a.output/(name+'.licenses.txt')).write_text(notice)
archive=a.output/(f'gtiup-v{version}-{a.platform}.tar.gz')
with tarfile.open(archive,'w:gz') as stream:
 stream.add(target,arcname='bin/gtiup'); stream.add(a.output/(name+'.licenses.txt'),arcname='LICENSES.txt');stream.add(root/'README.md',arcname='README.md')
for path in (target,archive):
 (a.output/(path.name+'.sha256')).write_text(hashlib.sha256(path.read_bytes()).hexdigest()+'  '+path.name+'\n')
print(target,archive)
