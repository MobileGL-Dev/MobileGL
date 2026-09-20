#!/usr/bin/env python3
"""Prove the primary runtime's compiled shape and actual inproc GPU execution."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
RSP_CASES = {backend + '.Split.P5fRsp.F1WireScenario.EachWireFrameHasZeroResidualPulls'
             for backend in ('DirectGLES', 'DirectVulkan')}


def build_proof(build, mode):
    library = build / 'libMobileGL.so'
    commands = json.loads((build / 'compile_commands.json').read_text())
    production = [row for row in commands
                  if row['file'].replace('\\', '/').endswith('/MobileGL/ConfigLoader.cpp')]
    if not production:
        raise RuntimeError('no ConfigLoader production compilation found')
    definitions = []
    for row in production:
        argv = row.get('arguments') or shlex.split(row['command'])
        flags = {arg[2:].split('=', 1)[0]: arg[2:].split('=', 1)[1] if '=' in arg else '1'
                 for arg in argv if arg.startswith('-D')}
        expected = mode == 'disaggregated'
        if any((flags.get(name) == '1') != expected
               for name in ('MOBILEGL_BUILD_DISAGGREGATED', 'MOBILEGL_PIPE_PUSH')):
            raise RuntimeError(f'production macros disagree with {mode}: {flags}')
        definitions.append({'source': row['file'], 'definitions': flags})
    symbols = subprocess.check_output(['nm', '--defined-only', str(library)], text=True)
    if len(symbols.splitlines()) < 1000:
        raise RuntimeError('library appears stripped; symbol absence would prove nothing')
    remote = sum('MG_Remote' in line for line in symbols.splitlines())
    applier = sum('MGPipeApply' in line for line in symbols.splitlines())
    if (mode == 'disaggregated' and (not remote or not applier)) or (mode == 'monolith' and (remote or applier)):
        raise RuntimeError(f'{mode} symbol mismatch: MG_Remote={remote}, MGPipeApply={applier}')
    return {'mode': mode, 'source_sha': subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip(),
            'library': str(library), 'sha256': hashlib.sha256(library.read_bytes()).hexdigest(),
            'defined_symbols': len(symbols.splitlines()), 'remote_symbols': remote, 'applier_symbols': applier,
            'production_compilation': definitions}


def inproc_log_proof(text):
    marker = 'Config: MOBILEGL_TRANSPORT=inproc - the MGPipe record stream'
    configs = re.findall(r'Config: IPC[^\r\n]*', text)
    required = ('strict=1', 'role-split-state=1', 'run-ahead=1')
    fatals = re.findall(r'^.*Fatal\{.*$', text, re.MULTILINE)
    if marker not in text or not configs or not all(
            all(re.search(r'\b' + re.escape(setting) + r'\b', line) for setting in required) for line in configs) or fatals:
        raise RuntimeError('library log does not prove real inproc / strict / role state / zero Fatal')
    return {'transport': 'inproc', 'ipc_config_lines': configs, 'fatal_count': 0}


def logs_proof(discovery, junit):
    spec = importlib.util.spec_from_file_location('split_log_paths', ROOT / 'MobileGL/MG_IntegrationTest/Harness/split_log_paths.py')
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    lane = json.loads(discovery.read_text())
    names = [test['name'] for test in lane['tests']]
    if len(names) != 2 or set(names) != RSP_CASES:
        raise RuntimeError(f'primary inproc proof must execute both exact backend cases: {names}')
    helper.require_green(lane, str(junit), sorted(RSP_CASES))
    logs = helper.marker_log_paths(lane)
    proof = {}
    for name in sorted(RSP_CASES):
        path = Path(logs[name])
        proof[name] = {'private_log': str(path), **inproc_log_proof(path.read_text(encoding='utf-8', errors='replace'))}
    return proof


def self_test():
    marker = 'Config: MOBILEGL_TRANSPORT=inproc - the MGPipe record stream crosses a real ring'
    config = 'Config: IPC run-ahead=1 strict=1 role-split-state=1'
    good = marker + '\n' + config
    inproc_log_proof(good)
    for bad in ('', 'Config: Accepted env variable: MOBILEGL_TRANSPORT=inproc\n' + config,
                good.replace('strict=1', 'strict=0'), good.replace('role-split-state=1', 'role-split-state=0'),
                good.replace('run-ahead=1', 'run-ahead=0'), good + '\nFatal{ProtocolCorruption, example}'):
        try:
            inproc_log_proof(bad)
        except RuntimeError:
            pass
        else:
            raise AssertionError('invalid runtime log passed')
    with tempfile.TemporaryDirectory(prefix='mobilegl-runtime-proof-') as directory:
        build = Path(directory)
        (build / 'libMobileGL.so').write_bytes(b'checker fixture, not a real runtime')
        normal = ''.join(f'00000000 T fixture{i}\n' for i in range(1000))
        remote = normal + '00000000 T MG_Remote_fixture\n00000000 T MGPipeApply_fixture\n'
        for mode, flags, symbols, succeeds in (
            ('monolith', '', normal, True),
            ('disaggregated', '-DMOBILEGL_BUILD_DISAGGREGATED=1 -DMOBILEGL_PIPE_PUSH=1', remote, True),
            ('disaggregated', '-DMOBILEGL_BUILD_DISAGGREGATED=1', remote, False),
            ('disaggregated', '-DMOBILEGL_BUILD_DISAGGREGATED=1 -DMOBILEGL_PIPE_PUSH=1', normal, False),
            ('monolith', '', remote, False),
            ('monolith', '', '00000000 T stripped\n', False),
        ):
            (build / 'compile_commands.json').write_text(json.dumps([
                {'file': '/fixture/MobileGL/ConfigLoader.cpp', 'command': f'clang++ {flags} -c ConfigLoader.cpp'}]))
            with patch('subprocess.check_output', side_effect=lambda args, **kwargs: symbols if args[0] == 'nm' else 'fixture-sha\n'):
                try:
                    build_proof(build, mode)
                except RuntimeError:
                    if succeeds: raise
                else:
                    if not succeeds: raise AssertionError('invalid compiled shape passed')
    print('runtime mode proof: compiled-shape and actual-transport positive/negative controls passed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('self-test')
    build = sub.add_parser('build')
    build.add_argument('--build-dir', type=Path, required=True)
    build.add_argument('--mode', choices=('disaggregated', 'monolith'), required=True)
    logs = sub.add_parser('logs')
    logs.add_argument('--discovery', type=Path, required=True)
    logs.add_argument('--junit', type=Path, required=True)
    for child in (build, logs): child.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.command == 'self-test':
        self_test()
        return
    proof = build_proof(args.build_dir, args.mode) if args.command == 'build' else logs_proof(args.discovery, args.junit)
    args.output.write_text(json.dumps(proof, indent=2), encoding='utf-8')
    print(f'{args.command} runtime proof written to {args.output}')


if __name__ == '__main__':
    main()
