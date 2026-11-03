#!/usr/bin/env python3
"""Generate/check documentation against the two compiled public examples."""
import argparse
from pathlib import Path
import re
import subprocess
import textwrap
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parents[1]
PUBLIC_EXAMPLES = {'quickstart.cpp', 'pf_vf.cpp', 'pf_vf_layout.hpp'}


def snippet(file, marker):
    source = (ROOT / file).read_text()
    pattern = rf'^[ \t]*// DOC-BEGIN {marker}\n(.*?)^[ \t]*// DOC-END {marker}$'
    matches = re.findall(pattern, source, re.M | re.S)
    if len(matches) != 1:
        raise SystemExit(f'expected one DOC block {marker} in {file}')
    return '```cpp\n' + textwrap.dedent(matches[0]).rstrip() + '\n```'


def titles_table():
    titles = dict(re.findall(r'std::string_view\s+(\w+)\s*=\s*"([^"]+)";',
                            (ROOT / 'include/regfw/layout_titles.hpp').read_text()))
    descriptions = {
        'bundle': 'Summary：地址 View 与共享实体引用；Full 同样保留。数组 REG 仅在 Full 的紧凑表显示',
        'aliases': 'Full：静态别名、地址公式和权限',
        'shared': 'Summary：共享存储及各个入口',
        'canonical': 'Full：规范地址区间及共享入口',
        'paths': 'Summary/Full：汇合到同一存储区间的连线',
        'identity': 'Full：身份窗口、domain 和 resolver',
        'index': 'Full：数组下标规则、条件与 fallback',
        'callbacks': 'Full：resolver 名称、绑定状态和注册位置',
        'effective': 'Summary/Full：动态实例的符号路径，不执行 resolver',
        'warnings': '有警告时显示：诊断 code、原因与声明位置',
    }
    if titles.keys() != descriptions.keys():
        raise SystemExit('layout section descriptions must match layout_titles.hpp')
    return '| 输出 | 内容 |\n|---|---|\n' + '\n'.join(
        f'| {title} | {descriptions[key]} |' for key, title in titles.items())


def update(file, marker, body, check):
    path = ROOT / file
    old = path.read_text()
    begin, end = f'<!-- GENERATED {marker} BEGIN -->', f'<!-- GENERATED {marker} END -->'
    if old.count(begin) != 1 or old.count(end) != 1:
        raise SystemExit(f'expected one GENERATED {marker} block in {file}')
    before, rest = old.split(begin)
    _, after = rest.split(end)
    new = before + begin + '\n' + body + '\n' + end + after
    if check and new != old:
        raise SystemExit(f'stale generated documentation: {file}; run scripts/update_docs.py --build-dir BUILD')
    if not check:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(new)


def check_structure():
    files = {str(p.relative_to(ROOT / 'examples')) for p in (ROOT / 'examples').rglob('*') if p.is_file()}
    if files != PUBLIC_EXAMPLES:
        raise SystemExit(f'public examples must be exactly {sorted(PUBLIC_EXAMPLES)}, got {sorted(files)}')
    for file in PUBLIC_EXAMPLES:
        if re.search(r'REG_(?:REPEAT_)?APERTURE|reg::aperture\(|reg::window\(|reg::Window', (ROOT / 'examples' / file).read_text()):
            raise SystemExit(f'obsolete API in public example: {file}')
    version = re.search(r'project\(register_framework_cpp VERSION ([0-9.]+)',
                        (ROOT / 'CMakeLists.txt').read_text())[1]
    if (ROOT / 'README.md').read_text().splitlines()[0] != '# regfw ' + version:
        raise SystemExit('README version differs from CMake project version')
    if f'## {version}\n' not in (ROOT / 'CHANGELOG.md').read_text():
        raise SystemExit('CHANGELOG is missing the current version')


def check_links():
    # Only SDK-owned Markdown, not the independently released hlog documentation.
    files = [ROOT / 'README.md', ROOT / 'AGENT.md', ROOT / 'CHANGELOG.md', *(ROOT / 'docs').rglob('*.md')]
    for file in files:
        markdown = re.sub(r'^```[^\n]*\n.*?^```[ \t]*$', '', file.read_text(), flags=re.M | re.S)
        for target in re.findall(r'(?<!!)\[[^\]\n]+\]\(([^)\n]+)\)', markdown):
            if re.match(r'[a-zA-Z][\w+.-]*:', target):
                continue
            relative = unquote(target.split('#', 1)[0].strip('<>'))
            path = (file.parent / relative).resolve() if relative else file
            if not path.is_relative_to(ROOT) or not path.is_file():
                raise SystemExit(f'broken local documentation link in {file.relative_to(ROOT)}: {target}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--build-dir', type=Path, help='Generate/check output snapshots using compiled examples')
    args = parser.parse_args()
    check_structure()
    blocks = [
        ('README.md', 'QUICKSTART', 'examples/quickstart.cpp', 'quickstart'),
        ('docs/AddressViews.md', 'ADDRESS VIEW EXAMPLE', 'docs/snippets/address_views.cpp', 'address-view-example'),
        ('docs/RegEntity-RegView.md', 'ENTITIES', 'examples/pf_vf_layout.hpp', 'entities'),
        ('docs/AddressViews.md', 'POE VIEWS', 'examples/pf_vf_layout.hpp', 'windows'),
        ('docs/Device.md', 'CALLBACKS', 'examples/pf_vf.cpp', 'callbacks'),
        ('docs/Device.md', 'ACCESS', 'examples/pf_vf.cpp', 'access'),
        ('docs/Device.md', 'STATE', 'examples/pf_vf.cpp', 'state'),
        ('docs/Diagnostics.md', 'EXPLAIN', 'examples/pf_vf.cpp', 'explain'),
    ]
    for file, marker, source, source_marker in blocks:
        update(file, marker, snippet(source, source_marker), args.check)
    update('docs/Diagnostics.md', 'LAYOUT SECTIONS', titles_table(), args.check)
    if args.build_dir:
        build = args.build_dir.resolve()
        for option, file in [('--layout', 'pf-vf-layout.txt'), ('--layout-full', 'pf-vf-layout-full.txt'), ('--explain', 'pf-vf-explain.txt')]:
            actual = subprocess.check_output([str(build / 'regfw_pf_vf'), option], cwd=build, text=True)
            # source_location keeps absolute paths (whose spelling depends on
            # the configure-time source dir and symlinks, e.g. /tmp vs
            # /private/tmp on macOS). Reduce every public example file to its
            # bare name so snapshots are identical after extraction or
            # relocation; line numbers stay checked.
            for name in PUBLIC_EXAMPLES:
                actual = re.sub(rf'\S*/(examples/)?{re.escape(name)}', f'examples/{name}', actual)
            path = ROOT / 'docs/output' / file
            if args.check:
                if not path.is_file() or path.read_text() != actual:
                    raise SystemExit(f'stale compiled output: docs/output/{file}; regenerate with --build-dir BUILD')
            else:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(actual)
    check_links()
    print('Documentation verified' if args.check else 'Documentation updated')


if __name__ == '__main__':
    main()
