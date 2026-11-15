#!/usr/bin/env python3
"""Install into a disposable pacman root; never alter the host installation."""
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile


def digest_stream(f):
    h = hashlib.sha256()
    while data := f.read(4 * 1024 * 1024):
        h.update(data)
    return h.hexdigest()


def main():
    package, deb = (Path(p).resolve(strict=True) for p in sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix='codex-pacman-root-') as temp:
        root = Path(temp)
        db = root / 'var/lib/pacman'
        hooks = root / 'empty-hooks'
        db.mkdir(parents=True)
        hooks.mkdir()
        command = ['fakeroot', 'pacman', '--root', str(root), '--dbpath', str(db),
                   '--logfile', str(root / 'pacman.log'), '--hookdir', str(hooks),
                   '-U', '--nodeps', '--nodeps', '--noscriptlet', '--noconfirm', str(package)]
        subprocess.run(command, check=True)
        query = subprocess.run(['pacman', '--root', str(root), '--dbpath', str(db),
                                '-Q', 'chatgpt'], check=True, capture_output=True, text=True)
        assert query.stdout.strip() == 'chatgpt 26.1111.21308-1'
        assert (root / 'usr/bin/chatgpt').is_symlink()
        assert (root / 'usr/bin/chatgpt').resolve().is_relative_to(root)
        subprocess.run(['desktop-file-validate', str(root / 'usr/share/applications/chatgpt.desktop')], check=True)
        linked = subprocess.run(['ldd', str(root / 'usr/lib/chatgpt/ChatGPT')],
                                check=True, capture_output=True, text=True)
        assert 'not found' not in linked.stdout, linked.stdout
        # Compare every shipped application file against the source DEB. This
        # catches accidental makepkg stripping, purging or static-library removal.
        source = subprocess.Popen(['ar', 'p', str(deb), 'data.tar.xz'], stdout=subprocess.PIPE)
        compared = 0
        with tarfile.open(fileobj=source.stdout, mode='r|xz') as archive:
            for member in archive:
                name = member.name.removeprefix('./')
                if not name.startswith('usr/lib/chatgpt/') or not member.isfile():
                    continue
                target = root / name
                assert target.is_file(), f'Missing upstream file: {name}'
                expected = digest_stream(archive.extractfile(member))
                with target.open('rb') as f:
                    actual = digest_stream(f)
                assert actual == expected, f'Changed upstream file: {name}'
                compared += 1
        assert source.wait() == 0
        assert compared > 100
        env = os.environ.copy()
        env['ELECTRON_RUN_AS_NODE'] = '1'
        runtime = subprocess.run([str(root / 'usr/lib/chatgpt/ChatGPT'), '--version'],
                                 env=env, check=True, capture_output=True, text=True)
        assert runtime.stdout.strip() == '26.1111.21308', runtime.stdout
        print(f'PASS: isolated pacman installation, version, launcher, desktop entry, shared libraries, application version ({runtime.stdout.strip()}), {compared} unmodified upstream files')


if __name__ == '__main__':
    main()
