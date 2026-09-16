#!/usr/bin/env python3
"""c1f mutation gates. Run from the worktree, links ON, after building split.
Every mutation must compile, fail its named test, then restore/build/pass that test.
No git operation, server source edit, or unrelated failure is accepted as red.
"""
import argparse
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
EMIT = 'MobileGL/MG_Remote/Client/EmitTables.cpp'
WIRE = 'MobileGL/MG_Remote/Client/WireTables.cpp'
FILL = 'MobileGL/MG_Impl/Pipe/PipeFill.cpp'
BACKEND = 'MobileGL/MG_Remote/Client/BackendObject_Remote.cpp'
SUITE = 'RemoteClientControls.'

def replace(old, new, count=1):
    def change(src):
        if src.count(old) < count:
            raise RuntimeError('mutation anchor absent: ' + old)
        return src.replace(old, new, count)
    return change

def escape_error(src):
    result, count = re.subn(r'            if \(status == 2\) \{.*?\n            \}', '', src,
                            flags=re.S)
    if count != 2:
        raise RuntimeError(f'expected two escape ERROR blocks, got {count}')
    return result

def helper_size(src):
    return replace('ReadbackBytesPerPixel(format, type);\n        }',
                   'ReadbackBytesPerPixel(format, type) + 16;\n        }')(src)

def cases():
    both = 'RequireReadbackReplyComplete(status, replySize, tight);'
    site = '&MGPipeRouteResourceDestroy);'
    return [
        ('B1-source', FILL, replace(site, '&MGPipeApplyResourceDestroy);'),
         ['PipeCatalogue.FrontendNeverTakesAnApplierAddress'], 'PipeCatalogueTest'),
        ('B1-wire', FILL, replace(site, '&MGPipeApplyResourceDestroy);'),
         [SUITE+'DeleteTexturesAdvancesTheWireOrdinal'], 'RemoteClientTest'),
        ('N2-five-verbs', EMIT, replace('            RequireClientTablesInstalled(slot);', ''),
         [SUITE+'TeardownRefuses'+n+'OnALiveSession' for n in
          ['Clear','DrawArrays','ReadPixels','BlitFramebuffer','Present']], 'RemoteClientTest'),
        ('M2-production-deleted', EMIT, replace(both, '', 2),
         [SUITE+'ShortOkReadPixelsReplyRefusesByName', SUITE+'BounceReadPixelsShortReplyRefusesBeforeScatter',
          SUITE+'ErrorReadPixelsReplyRefusesByName'], 'RemoteClientTest'),
        ('M2-fast-deleted', EMIT, replace(both, ''),
         [SUITE+'ShortOkReadPixelsReplyRefusesByName'], 'RemoteClientTest'),
        ('M2-bounce-deleted', EMIT, replace('            '+both+'\n            Scatter', '            Scatter'),
         [SUITE+'BounceReadPixelsShortReplyRefusesBeforeScatter'], 'RemoteClientTest'),
        ('M3-production-plus16', EMIT, replace('info.DstSize = tight;', 'info.DstSize = tight + 16;'),
         [SUITE+'ReadPixelsPutsTheTightExtentOnTheWire'], 'RemoteClientTest'),
        ('M3-shared-size', EMIT, helper_size,
         [SUITE+'ReadPixelsPutsTheTightExtentOnTheWire'], 'RemoteClientTest'),
        ('M4-escape-errors-deleted', WIRE, escape_error,
         [SUITE+'ResourceRespecifyErrorIsNotADecline', SUITE+'MapPersistentErrorIsNotADecline'], 'RemoteClientTest'),
        ('M5-server-role-guards-deleted', FILL,
         replace('!MG_Remote::Client::RunsAsTheServerRole() && ', '', 2),
         [SUITE+'ServerRoleRespecifyDoesNotEmitClientInitialBytes'], 'RemoteClientTest'),
        ('M5-server-flush-guard-deleted', FILL,
         replace('!MG_Remote::Client::RunsAsTheServerRole() && size != 0', 'size != 0'),
         [SUITE+'ServerRoleFlushDoesNotRunTheClientSubDataFollowup'], 'RemoteClientTest'),
        ('M8-real-pack-binding', EMIT,
         replace('if (MG_State::pGLContext != nullptr &&', 'if (false && MG_State::pGLContext != nullptr &&'),
         [SUITE+'BoundPackBufferOffsetReadRefusesByName'], 'RemoteClientTest'),
        ('codex12-repeat-skip', BACKEND,
         replace('if (draw != EGL_NO_SURFACE && ctx != EGL_NO_CONTEXT) {', 'if (false) {'),
         [SUITE+'RepeatedMakeCurrentAdoptsRepublishedCapsWithoutAPumpOrPresent'], 'RemoteClientTest'),
        ('BlobMissing-optional-to-required', WIRE,
         replace('record.Blob = StageOptional(session, blobBytes, blobByteCount);',
                 'record.Blob = StageRequired(session, "SetDynamicState", blobBytes, blobByteCount);'),
         [SUITE+'HeaderOnlyDynamicStateCrossesWithoutBlobMissing'], 'RemoteClientTest'),
        ('N6-compensating-wrong-row', WIRE,
         replace('gMGPipeScreen.ResourceDestroy = &Wire_ResourceDestroy;',
                 'gMGPipeScreen.FenceCreate = &Wire_ResourceDestroy;'),
         ['PipeRouting.TheInstalledClientArmIsWireAndNotMonolithAndEveryRoutedRowMoved'], 'RemoteClientTest'),
    ]

def command(args):
    p = subprocess.run(args, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, timeout=180)
    return p.returncode, p.stdout

def verdict(build_rc, run_rc, output, names):
    if build_rc:
        return 'HARNESS-FAIL'
    if run_rc and all('[  FAILED  ] '+name+' (' in output for name in names):
        return 'RED'
    return 'WRONG-VERDICT'

def self_test():
    names = ['Own.Test']
    probes = [(1, 1, '[  FAILED  ] Own.Test (0 ms)', 'HARNESS-FAIL'),
              (0, -11, 'UNRELATED_FAILURE\nSegmentation fault', 'WRONG-VERDICT'),
              (0, 0, '[  PASSED  ] 1 test.', 'WRONG-VERDICT'),
              (0, 0, '[  FAILED  ] Own.Test (0 ms)', 'WRONG-VERDICT'),
              (0, 1, '[  FAILED  ] Own.Test (0 ms)', 'RED')]
    for b, rc, out, expected in probes:
        assert verdict(b, rc, out, names) == expected
    print('RUNNER_META 5/5: unrelated crash, all green, build failure, false status, own failure')

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--case', default='')
    args = parser.parse_args()
    self_test()
    if args.self_test:
        return 0
    failures = []
    selected = [case for case in cases() if args.case in case[0]]
    if not selected:
        print('HARNESS-FAIL: no matching mutation')
        return 1
    for label, file, mutate, names, target in selected:
        path = ROOT / file
        original = path.read_bytes()
        binary = ROOT / 'build-split/MobileGL/MG_Test' / ('Pipe' if target == 'PipeCatalogueTest' else 'Wire') / target
        build = ['cmake', '--build', 'build-split', '-j', '24', '--target', target]
        run = [str(binary), '--gtest_filter='+':'.join(names)]
        print('\n=== '+label+' ===', flush=True)
        try:
            brc, out = command(build)
            if brc:
                raise RuntimeError('baseline build failed\n'+out)
            rc, out = command(run)
            if rc or any('[       OK ] '+name+' (' not in out for name in names):
                raise RuntimeError('baseline not green\n'+out)
            path.write_text(mutate(original.decode()))
            brc, out = command(build)
            rc = 0
            if not brc:
                rc, out = command(run)
            result = verdict(brc, rc, out, names)
            print(out, end='')
            print(f'{label}: {result} build={brc} run={rc}', flush=True)
            if result != 'RED':
                failures.append(label)
        except Exception as exc:
            print('HARNESS-FAIL', exc, flush=True)
            failures.append(label)
        finally:
            path.write_bytes(original)
            brc, out = command(build)
            rc, out = command(run) if brc == 0 else (brc, out)
            if rc or any('[       OK ] '+name+' (' not in out for name in names):
                print('RESTORE-FAIL\n'+out, flush=True)
                failures.append(label+' restore')
            else:
                print('RESTORED GREEN: '+', '.join(names), flush=True)
    print('FAILED_CONTROLS='+repr(failures), flush=True)
    return bool(failures)

if __name__ == '__main__':
    sys.exit(main())
