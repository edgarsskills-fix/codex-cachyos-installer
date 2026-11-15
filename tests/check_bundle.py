#!/usr/bin/env python3
"""Check release integrity, executable permissions and non-mutating preflight."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zipfile


def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        while data := f.read(4 * 1024 * 1024):
            h.update(data)
    return h.hexdigest()


def run(*args):
    return subprocess.run(list(map(str, args)), capture_output=True, text=True)


def main():
    bundle = Path(sys.argv[1]).resolve(strict=True)
    metadata = json.loads((bundle / 'manifest.json').read_text())
    package = bundle / metadata['arch_package']
    installer = bundle / 'Установить'
    assert sha(package) == metadata['arch_package_sha256']
    assert sha(installer) == metadata['installer_sha256']
    for line in (bundle / 'SHA256SUMS.txt').read_text().splitlines():
        expected, name = line.split('  ', 1)
        assert sha(bundle / name) == expected, name
    assert installer.read_bytes()[:4] == b'\x7fELF'
    assert installer.stat().st_mode & 0o111
    with zipfile.ZipFile(bundle.parent / (bundle.name + '.zip')) as archive:
        entry = archive.getinfo(bundle.name + '/Установить')
        assert (entry.external_attr >> 16) & 0o111, 'ZIP lost executable mode'
        assert all(not Path(n).is_absolute() and '..' not in Path(n).parts
                   for n in archive.namelist())
    info = run('pacman', '-Qip', package)
    assert info.returncode == 0, info.stderr
    assert metadata['version'] + '-1' in info.stdout
    names = run('bsdtar', '-tf', package)
    assert names.returncode == 0, names.stderr
    paths = [p.removeprefix('./') for p in names.stdout.splitlines()]
    allowed = {'.PKGINFO', '.BUILDINFO', '.MTREE', '.INSTALL', 'etc', 'usr'}
    assert all(p.split('/')[0] in allowed for p in paths), 'Unexpected payload root'
    assert 'usr/lib/chatgpt/ChatGPT' in paths
    assert 'usr/share/applications/chatgpt.desktop' in paths
    assert 'usr/bin/chatgpt' in paths
    assert 'etc/apparmor.d/chatgpt' in paths
    assert run(installer, '--check').returncode == 0, 'Valid package preflight failed'
    with tempfile.TemporaryDirectory(prefix='codex-installer-damage-') as temp:
        temp = Path(temp)
        shutil.copy2(installer, temp / installer.name)
        missing = run(temp / installer.name, '--check')
        assert missing.returncode == 1 and 'Распакуйте' in missing.stderr
        (temp / package.name).write_bytes(b'corrupted package')
        damaged = run(temp / installer.name, '--check')
        assert damaged.returncode == 1 and 'Контрольная сумма' in damaged.stderr
    print('PASS: hashes, ZIP permissions, package metadata, payload boundaries, preflight, missing and damaged package rejection')


if __name__ == '__main__':
    main()
