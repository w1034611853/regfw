#!/usr/bin/env python3
"""Package one regfw SDK, then verify its extraction, examples and installed consumer."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tarfile
import tempfile
import tomllib

root = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--hlog-root', type=Path, required=True)
p.add_argument('--output-dir', type=Path, required=True)
p.add_argument('--jobs', type=int, default=2)
p.add_argument('--offline', action='store_true', help='Use only the existing Cargo dependency cache')
args = p.parse_args()
if args.jobs < 1:
    p.error('--jobs must be positive')
version = re.search(r'project\(register_framework_cpp VERSION ([0-9.]+)', (root / 'CMakeLists.txt').read_text())[1]
name = f'regfw-{version}-{platform.system().lower()}-{platform.machine()}'
hlog, output = args.hlog_root.resolve(), args.output_dir.resolve()
if not (hlog / 'Cargo.toml').is_file():
    p.error('--hlog-root must contain the full hlog source SDK')
archive = output / (name + '.tar.gz')
for suffix in ['.tar.gz', '.tar.gz.sha256', '.json']:
    if (output / (name + suffix)).exists():
        p.error(f'refusing to overwrite release: {name + suffix}')
for tool in ['cmake', 'ctest', 'cargo', 'g++', 'bash']:
    if not shutil.which(tool):
        raise SystemExit(f'missing release prerequisite: {tool}')
work = Path(tempfile.mkdtemp(prefix='regfw-release-'))
print('Verification workspace:', work, flush=True)
env = dict(os.environ)
if args.offline:
    env['CARGO_NET_OFFLINE'] = 'true'
env.setdefault('CARGO_TARGET_DIR', str(work / 'cargo-target'))
env.setdefault('HLOG_BUILD_JOBS', str(args.jobs))


def run(command, cwd=None, environment=None, capture=False):
    command = [str(x) for x in command]
    print('+', ' '.join(command), flush=True)
    return subprocess.run(command, cwd=cwd or work, env=environment or env, check=True, text=True,
                          stdout=subprocess.PIPE if capture else None).stdout


def test_build(build, environment=None):
    listing = json.loads(run(['ctest', '--test-dir', build, '--show-only=json-v1'], capture=True))
    names = [x['name'] for x in listing['tests']]
    if not names or len(names) != len(set(names)):
        raise SystemExit('release requires nonzero, uniquely named CTests')
    if not {'example_quickstart', 'example_pf_vf', 'documentation'}.issubset(names):
        raise SystemExit('release requires the current examples and documentation checks')
    run(['ctest', '--test-dir', build, '--output-on-failure'], environment=environment)
    return names


run(['python3', root / 'scripts/update_docs.py', '--check'])
stage = work / 'stage' / f'regfw-{version}'
ignored = shutil.ignore_patterns('build', 'target', 'dist', 'logs', '__pycache__', '.git', '.cache')
shutil.copytree(root, stage, ignore=ignored)
if (stage / 'third_party/hlog').exists():
    shutil.rmtree(stage / 'third_party/hlog')
shutil.copytree(hlog, stage / 'third_party/hlog', ignore=ignored, symlinks=True)
run(['bash', stage / 'third_party/hlog/scripts/install.sh'])
# Refresh the manifest after rebuilding the bundled dependency.
manifest = stage / 'SHA256SUMS'
manifest.write_text(''.join(
    hashlib.sha256(path.read_bytes()).hexdigest() + '  ' + path.relative_to(stage).as_posix() + '\n'
    for path in sorted(stage.rglob('*'))
    if path.is_file() and not path.is_symlink() and path != manifest))
# Archive before testing. Runtime/Cargo tests can create logs, but never in the payload.
candidate = work / (name + '.tar.gz')
with tarfile.open(candidate, 'w:gz') as tar:
    tar.add(stage, arcname=stage.name)
unpacked = work / 'unpacked'
with tarfile.open(candidate) as tar:
    forbidden = {'build', 'target', 'dist', 'logs', '__pycache__', '.git', '.cache'}
    for member in tar.getmembers():
        if forbidden.intersection(Path(member.name).parts):
            raise SystemExit('generated artifact in package: ' + member.name)
    tar.extractall(unpacked, filter='data')
source = unpacked / stage.name
sdk = source / 'third_party/hlog'
hlog_version = tomllib.loads((sdk / 'runtime/Cargo.toml').read_text())['package']['version']
if hlog_version != tomllib.loads((sdk / 'rust-client/Cargo.toml').read_text())['package']['version']:
    raise SystemExit('bundled hlog runtime/client versions differ')
abi = int(re.search(r'#define HLOG_ABI_VERSION (\d+)', (sdk / 'include/hlog.h').read_text())[1])
run(['bash', sdk / 'scripts/verify_runtime.sh', sdk / 'lib/libhlog.so'])
link_env = dict(env, LD_LIBRARY_PATH=str(sdk / 'lib') + os.pathsep + env.get('LD_LIBRARY_PATH', ''))
cargo = run(['cargo', 'test', '--workspace', '--locked', '--jobs', args.jobs], cwd=sdk,
            environment=link_env, capture=True)
print(cargo, flush=True)
rust_passed = sum(map(int, re.findall(r'test result: ok\. (\d+) passed', cargo)))
if not rust_passed:
    raise SystemExit('hlog verification ran zero Rust tests')
run(['bash', sdk / 'tests/test_hexdump_headers.sh'])
run(['bash', sdk / 'tests/test_headers_loader.sh'])
for test in ['test_hexdump_runtime.py', 'test_console_runtime.py']:
    run(['python3', sdk / 'tests' / test, sdk / 'lib/libhlog.so'])
run(['python3', source / 'scripts/update_docs.py', '--check'])
# Test the default top-level configuration; do not force TESTS=ON and hide an empty default suite.
build = work / 'build'
run(['cmake', '-S', source, '-B', build, '-DCMAKE_BUILD_TYPE=Release',
     '-DCMAKE_CXX_FLAGS=-Wall -Wextra -Werror'])
run(['cmake', '--build', build, '--parallel', args.jobs])
names = test_build(build)
run(['python3', source / 'scripts/update_docs.py', '--check', '--build-dir', build])
# Match the documented examples-only configuration, independently of test targets.
examples_build = work / 'examples-only'
run(['cmake', '-S', source, '-B', examples_build, '-DCMAKE_BUILD_TYPE=Release',
     '-DREGFW_BUILD_TESTS=OFF', '-DREGFW_BUILD_EXAMPLES=ON', '-DCMAKE_CXX_FLAGS=-Wall -Wextra -Werror'])
run(['cmake', '--build', examples_build, '--parallel', args.jobs])
examples = {path.name for path in examples_build.glob('regfw_*') if path.is_file() and os.access(path, os.X_OK)}
if examples != {'regfw_quickstart', 'regfw_pf_vf'}:
    raise SystemExit('examples-only build contains unexpected programs: ' + repr(examples))
for example in sorted(examples):
    run([examples_build / example])
run(['python3', source / 'scripts/update_docs.py', '--check', '--build-dir', examples_build])
# Verify install exports through a relocated prefix and a separate CMake project.
prefix = work / 'install'
run(['cmake', '--install', build, '--prefix', prefix])
relocated = work / 'relocated-install'
prefix.rename(relocated)
consumer = work / 'consumer'
consumer.mkdir()
(consumer / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.20)
project(release_consumer LANGUAGES CXX)
find_package(regfw CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE regfw::regfw fmt::fmt)
''')
(consumer / 'main.cpp').write_text('''#include <regfw/regfw.hpp>
#include <hlog.hpp>
#include <array>
#include <stdexcept>
int main(){
  hlog::Logger logger("release_consumer",HLOG_LEVEL_DEBUG);
  reg::RegisterMap map(0x100,"CONSUMER",logger);
  auto value=map.create_reg<std::uint64_t>(0,"VALUE");map.finalize();
  if(!map.mmio_write_sized(0,0x1122334455667788ull,8).success()||value.ref()!=0x1122334455667788ull)throw std::runtime_error("consumer write failed");
  HLOG_INFO(logger,"value={:#x}",value.ref());
  struct {unsigned count:19;} bits{321};HLOG_DEBUG(logger,"count={}",bits.count);
  std::array<unsigned char,5> data{1,2,3,4,5};HLOG_HEXDUMP_DEBUG(logger,hlog::u32,data);
  if(map.explain_address(0,8).find("VALUE")==std::string::npos)throw std::runtime_error("consumer explain failed");
}
''')
run(['cmake', '-S', consumer, '-B', consumer / 'build', f'-DCMAKE_PREFIX_PATH={relocated}',
     f'-DHLog_ROOT={sdk}', '-DCMAKE_CXX_FLAGS=-Wall -Wextra -Werror'])
run(['cmake', '--build', consumer / 'build', '--parallel', args.jobs])
run([consumer / 'build/consumer'], cwd=consumer)
# Sanitizers use the same extracted sources and the same CTest list.
asan = work / 'sanitizers'
run(['cmake', '-S', source, '-B', asan, '-DCMAKE_BUILD_TYPE=Debug',
     '-DCMAKE_CXX_FLAGS=-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie',
     '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined -no-pie'])
run(['cmake', '--build', asan, '--parallel', args.jobs])
sanitizer_env = dict(env, ASAN_OPTIONS='detect_leaks=0:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1')
if names != test_build(asan, sanitizer_env):
    raise SystemExit('sanitizer and Release test suites differ')
output.mkdir(parents=True, exist_ok=True)
digest = hashlib.file_digest(candidate.open('rb'), 'sha256').hexdigest()
manifest = {
    'version': version, 'platform': platform.platform(), 'archive': archive.name, 'sha256': digest,
    'bundled_hlog': {'version': hlog_version, 'abi': abi, 'rust_tests_passed': rust_passed},
    'public_examples': sorted(examples), 'tests': names,
    'sanitizers': ['address', 'undefined'], 'leak_detection': False,
    'checks': ['clean-extracted-payload', 'default-nonzero-ctest', 'examples-only-build',
               'relocated-install-consumer', 'hlog-cargo-workspace', 'real-fmt-c++17-c++20-bitfields',
               'hlog-real-runtime-hexdump-console', 'generated-docs', 'local-documentation-links',
               'compiled-layout-and-explain-snapshots'],
    'verification_workspace': str(work),
}
shutil.copy2(candidate, archive)
(output / (name + '.tar.gz.sha256')).write_text(digest + '  ' + archive.name + '\n')
(output / (name + '.json')).write_text(json.dumps(manifest, indent=2) + '\n')
print('RELEASE VERIFIED:', archive, flush=True)
print('SHA256:', digest, flush=True)
