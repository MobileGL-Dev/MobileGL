#!/usr/bin/env python3
"""Relocate packaged CTest metadata and select the executing runner's drivers."""
import argparse
import os
from pathlib import Path
import re
import subprocess


def normalize(text, old_root, new_root, cmake_root, hardware=False, icd=None):
    if old_root:
        text = text.replace(old_root, new_root)
    # Artifact builds and test hosts can have different CMake installations.
    text = re.sub(r'"[^"\n]*/bin/cmake"', '"cmake"', text)
    text = re.sub(r'/[^"\[\]\s;()]*/Modules/(GoogleTest(?:AddTests)?\.cmake|GoogleTest/LaunchTest\.cmake)',
                  lambda m: cmake_root + '/Modules/' + m[1], text)
    lines = []
    for line in text.splitlines(keepends=True):
        if re.search(r'ENVIRONMENT|LABELS', line):
            # GoogleTest discovery receives list properties, not one value with
            # escaped separators. Otherwise BACKEND becomes "DirectVulkan;...".
            line = line.replace('\\;', ';')
        if icd:
            line = re.sub(r'VK_ICD_FILENAMES=[^;"\]\s]+', 'VK_ICD_FILENAMES=' + icd, line)
        if hardware:
            for value in ('LIBGL_ALWAYS_SOFTWARE=1', 'MESA_GL_VERSION_OVERRIDE=3.3',
                          'MESA_GLSL_VERSION_OVERRIDE=330'):
                line = line.replace(value + ';', '').replace(';' + value, '').replace(value, '')
        lines.append(line)
    return ''.join(lines)


def prepare(build, workspace, cmake_root, hardware, icd):
    marker = build / '.ci-source-root'
    old_root = marker.read_text().strip() if marker.exists() else str(workspace)
    changed = 0
    for path in list(build.rglob('*.cmake')) + list(build.rglob('runtime-mode.json')):
        original = path.read_text()
        revised = normalize(original, old_root, str(workspace), cmake_root, hardware, icd)
        if original != revised:
            path.write_text(revised)
            changed += 1
    # Drop discovery caches so the repaired properties are used on this host.
    for path in build.rglob('*_tests*.json'):
        path.unlink()
    marker.write_text(str(workspace) + '\n')
    print(f'Prepared {build}: {changed} metadata files; hardware={hardware}; ICD={icd}')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('build', nargs='+', type=Path)
    args = parser.parse_args()
    info = subprocess.check_output(['cmake', '--system-information'], text=True)
    cmake_root = re.search(r'^CMAKE_ROOT "([^"]+)"', info, re.M)[1]
    workspace = Path(os.environ.get('GITHUB_WORKSPACE', os.getcwd())).resolve()
    for build in args.build:
        prepare(build, workspace, cmake_root, os.environ.get('MOBILEGL_CI_NATIVE_GPU') == '1',
                os.environ.get('VK_ICD_FILENAMES'))


if __name__ == '__main__':
    main()
