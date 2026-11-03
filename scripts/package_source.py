#!/usr/bin/env python3
"""Create a clean, self-contained source SDK and verify the SHA256 manifest.

Unlike the full release.py pipeline this only packages source/dependencies; it
never claims to have completed CTest, sanitizer or QEMU integration testing.
"""
import argparse
import hashlib
from pathlib import Path
import re
import shutil
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def ignored(directory: str, names: list[str]) -> set[str]:
    blocked = set()
    for name in names:
        path = Path(directory) / name
        if (name.startswith("._") or name in {".DS_Store", ".git", ".cache", "__pycache__", "logs", "dist", "target"}
                or name == "build" or name.startswith(("build-", "build_", "cmake-build-"))
                or (path.is_file() and path.suffix in {".o", ".obj", ".pyc", ".log"})):
            blocked.add(name)
    return blocked


def sha(path: Path) -> str:
    result = hashlib.sha256()
    with path.open('rb') as fp:
        for data in iter(lambda: fp.read(2 * 1024 * 1024), b''):
            result.update(data)
    return result.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    version = re.search(r'project\(register_framework_cpp VERSION ([0-9.]+)',
                        (ROOT / 'CMakeLists.txt').read_text())[1]
    name = f'regfw-{version}'
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    archive = output / f'{name}-linux-x86_64.tar.gz'
    with tempfile.TemporaryDirectory(prefix='regfw-package-') as scratch:
        stage = Path(scratch) / name
        shutil.copytree(ROOT, stage, symlinks=True, ignore=ignored)
        manifest = stage / 'SHA256SUMS'
        manifest.unlink(missing_ok=True)
        files = sorted(p for p in stage.rglob('*') if p.is_file() and not p.is_symlink())
        manifest.write_text(''.join(f'{sha(p)}  {p.relative_to(stage).as_posix()}\n' for p in files))
        with tarfile.open(archive, 'w:gz', compresslevel=6, dereference=False) as tf:
            tf.add(stage, arcname=name)
        # Independent fresh extraction verifies *all* ordinary packaged files.
        with tempfile.TemporaryDirectory(prefix='regfw-verify-') as fresh:
            with tarfile.open(archive, 'r:gz') as tf:
                tf.extractall(fresh, filter='data')
            extracted = Path(fresh) / name
            for line in (extracted / 'SHA256SUMS').read_text().splitlines():
                digest, filename = line.split('  ', 1)
                p = extracted / filename
                if not p.is_file() or sha(p) != digest:
                    raise RuntimeError(f'manifest verification failed: {filename}')
            if any(p.name.startswith('._') for p in extracted.rglob('*')):
                raise RuntimeError('AppleDouble file found after clean packaging')
    (output / (archive.name + '.sha256')).write_text(f'{sha(archive)}  {archive.name}\n')
    print(f'Created and independently hash-verified: {archive}')
    print(f'SHA256 {sha(archive)}')


if __name__ == '__main__':
    main()
