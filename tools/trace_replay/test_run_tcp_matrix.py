#!/usr/bin/env python3
"""Small process/fixture stubs only: no GL, network peer, adb or production trace."""
import contextlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import textwrap
import types
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_tcp_matrix as matrix

STUB = r"""
import json,os,sys,time
from pathlib import Path
values = dict(arg[2:].split('=',1) for arg in sys.argv[1:] if arg.startswith('-D') and '=' in arg)
work=Path(values['TRACE_OUTPUT_DIR']);(work/'output').mkdir(parents=True,exist_ok=True);(work/'input').mkdir()
behavior=os.environ.get('CASE_BEHAVIOR','success')
if behavior=='active':
 for n in range(16):
  with (work/'output/progress.log').open('a') as log:log.write(str(n)+'\n')
  time.sleep(.05)
if behavior!='no-result':
 actual=work/'output/actual.png';actual.write_bytes(b'fixture-image')
 endpoint=os.environ['MOBILEGL_IPC_CONTROL'].removeprefix('tcp://')
 arm=os.environ.get('CASE_ARM','armed')
 if arm=='respect-env':arm='armed' if os.environ.get('MOBILEGL_IPC_RUN_AHEAD')=='1' and os.environ.get('MOBILEGL_IPC_VERB_BARRIER')=='1' else 'lockstep'
 line={'armed':'run-ahead ARMED\n','lockstep':'running lockstep\n','disarmed':'run-ahead ARMED\nrun-ahead DISARMED\n','server-only':''}[arm]
 (work/'output/mobilegl.client.log').write_text('control=tcp data=stream server='+endpoint+' pid=123 dial=connect\n'+line)
 if arm=='server-only':(work/'output/mobilegl.server.log').write_text('run-ahead ARMED\n')
 result={'passed':True,'statusCode':0,'backend':values['TRACE_BACKEND'],
         'targetCall':int(values['TRACE_TARGET_CALL']),'tracePath':str(work/'input'/values['TRACE_FILE']),
         'actualPath':str(actual),'matchedGoldenPath':values['TRACE_GOLDEN'],'ssim':1.0}
 (work/'output/result.json').write_text(json.dumps(result))
if behavior=='timeout':time.sleep(60)
raise SystemExit(7 if behavior=='failed' else 0)
"""


@unittest.skipUnless(sys.platform == 'linux', 'Linux/WSL process-group driver')
class MatrixTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='tcp-matrix-test-')
        self.root = Path(self.temp.name)
        self.source = self.root/'source'
        self.tool = self.source/'tools/trace_replay'
        self.tool.mkdir(parents=True)
        (self.tool/'run_trace_case.cmake').write_text('# stub script')
        self.fixtures = self.root/'fixtures';self.fixtures.mkdir()
        (self.fixtures/'fixture.tgz').write_bytes(b'fixture archive')
        (self.fixtures/'golden.png').write_bytes(b'golden image')
        self.library = self.root/'libMobileGL.so';self.library.write_bytes(b'library-v1')
        self.stub = self.root/'stub.py';self.stub.write_text(STUB)
        self.catalog = self.root/'catalog.json'
        self.out = self.root/'results'
        self.manifest = self.tool/'trace_cases.json'
        self.write_inputs()

    def tearDown(self):
        self.temp.cleanup()

    def write_inputs(self, behavior='success', extra_cases=None):
        cases=[{'name':'Example','trace_archive':'fixture.tgz','trace_file':'trace.trace',
                'golden':'golden.png','target_call':42,'width':16,'height':16,'timeout_seconds':.05}]
        if extra_cases:cases.extend(extra_cases)
        self.manifest.write_text(json.dumps({'defaults':{'ssim_threshold':.99},'cases':cases}))
        command=[sys.executable,str(self.stub),'-DTRACE_REPLAY_EXE='+sys.executable,
                 '-DMOBILEGL_LIBRARY=/old/library','-DTRACE_CASE_NAME=Example','-DTRACE_BACKEND=DirectGLES',
                 '-DTRACE_ARCHIVE='+str(self.fixtures/'fixture.tgz'),'-DTRACE_OUTPUT_DIR=/old/output',
                 '-P',str(self.tool/'run_trace_case.cmake')]
        self.catalog.write_text(json.dumps({'kind':'ctestInfo','tests':[{
            'name':'MobileGLTraceReplay.Example.DirectGLES','command':command,
            'properties':[{'name':'ENVIRONMENT','value':['CASE_BEHAVIOR='+behavior]},
                          {'name':'TIMEOUT','value':.05},{'name':'WORKING_DIRECTORY','value':str(self.root)}]}]}))

    def run_main(self, *extra):
        args=['--catalog',str(self.catalog),'--source',str(self.source),'--library',str(self.library),
              '--runner',sys.executable,'--endpoint','tcp://127.0.0.1:9','--token','not-saved-in-checkpoint',
              '--out',str(self.out),'--idle-seconds','1','--max-seconds','5',*extra]
        with contextlib.redirect_stdout(io.StringIO()):
            return matrix.main(args)

    def checkpoint(self):
        return matrix.read_json(self.out/'checkpoint.json')['runs']

    def set_arm(self, arm, extra=None):
        catalog=matrix.read_json(self.catalog)
        catalog['tests'][0]['properties'][0]['value'] += ['CASE_ARM='+arm,*(extra or [])]
        self.catalog.write_text(json.dumps(catalog))

    def test_required_arm_forces_both_environment_knobs_and_records_actual_arm(self):
        self.set_arm('respect-env',['MOBILEGL_IPC_RUN_AHEAD=0','MOBILEGL_IPC_VERB_BARRIER=0'])
        self.assertEqual(self.run_main('--require-run-ahead'),0)
        row=self.checkpoint()[0]
        self.assertEqual(row['requested_arm'],{'MOBILEGL_IPC_RUN_AHEAD':'1','MOBILEGL_IPC_VERB_BARRIER':'1'})
        self.assertEqual(row['actual_arm'],{'armed':True,'lockstep':False,'disarmed':False})
        self.assertEqual(self.run_main('--require-run-ahead','--resume'),0)
        self.assertEqual(len(self.checkpoint()),1)

    def test_required_arm_rejects_lockstep_disarmed_and_server_only_marker(self):
        for arm in ('lockstep','disarmed','server-only'):
            with self.subTest(arm=arm):
                self.write_inputs()
                self.set_arm(arm)
                self.assertEqual(self.run_main('--require-run-ahead'),1)
                row=self.checkpoint()[-1]
                self.assertEqual(row['status'],'failed')
                self.assertIn('required run-ahead',row['error'])
                self.assertIsInstance(row['actual_arm'],dict)

    def test_required_arm_does_not_resume_a_former_lockstep_image_pass(self):
        self.set_arm('lockstep')
        self.assertEqual(self.run_main(),0)
        self.assertTrue(self.checkpoint()[0]['actual_arm']['lockstep'])
        self.assertEqual(self.run_main('--require-run-ahead','--resume'),1)
        self.assertEqual(len(self.checkpoint()),2)
        self.assertEqual(self.checkpoint()[-1]['status'],'failed')

    def test_success_is_resumed_only_while_its_evidence_is_intact(self):
        self.assertEqual(self.run_main(),0)
        self.assertEqual(self.run_main('--resume'),0)
        self.assertEqual(len(self.checkpoint()),1)
        record=self.checkpoint()[0]
        self.assertNotIn('not-saved-in-checkpoint',(self.out/'checkpoint.json').read_text())
        Path(record['result_path']).write_text('{"passed":false}')
        self.assertEqual(self.run_main('--resume'),0)
        self.assertEqual(len(self.checkpoint()),2)
        self.assertNotEqual(self.checkpoint()[0]['work'],self.checkpoint()[1]['work'])

    def test_nonzero_exit_with_passed_json_never_becomes_a_checkpoint_pass(self):
        self.write_inputs('failed')
        self.assertEqual(self.run_main(),1)
        self.assertEqual(self.run_main('--resume'),1)
        self.assertEqual(len(self.checkpoint()),2)
        self.assertTrue(all(row['status']=='failed' and row['returncode']==7 for row in self.checkpoint()))

    def test_timeout_with_passed_json_is_not_resumed_as_success(self):
        self.write_inputs('timeout')
        self.assertEqual(self.run_main('--idle-seconds','.2'),1)
        self.assertEqual(self.run_main('--idle-seconds','.2','--resume'),1)
        self.assertEqual(len(self.checkpoint()),2)
        self.assertTrue(all(row['status']=='timeout' and row['returncode']==124 for row in self.checkpoint()))

    def test_stale_scratch_result_cannot_satisfy_a_new_attempt(self):
        old=self.out/'Example/output/result.json';old.parent.mkdir(parents=True)
        old.write_text('{"passed":true,"statusCode":0}')
        legacy='[{"case":"Example","returncode":0}]'
        (self.out/'results.json').write_text(legacy)
        self.write_inputs('no-result')
        self.assertEqual(self.run_main('--resume'),1)
        row=self.checkpoint()[0]
        self.assertEqual(row['status'],'failed')
        self.assertEqual(row['returncode'],1)
        self.assertEqual(json.loads(old.read_text())['passed'],True)  # Old evidence is preserved, not promoted.
        backups=list(self.out.glob('results.legacy-*.json'))
        self.assertEqual(len(backups),1)
        self.assertEqual(backups[0].read_text(),legacy)

    def test_artifact_change_invalidates_a_successful_checkpoint(self):
        self.assertEqual(self.run_main(),0)
        self.library.write_bytes(b'library-v2')
        self.assertEqual(self.run_main('--resume'),0)
        self.assertEqual(len(self.checkpoint()),2)
        self.assertNotEqual(self.checkpoint()[0]['identity'],self.checkpoint()[1]['identity'])

    def test_active_logs_outlive_old_local_catalog_and_manifest_timeout(self):
        self.write_inputs('active')
        self.assertEqual(self.run_main('--idle-seconds','.3','--max-seconds','4'),0)
        row=self.checkpoint()[0]
        self.assertGreater(row['seconds'],.6)  # Both obsolete local timeouts are .05 seconds.
        self.assertIsNone(row['timeout_kind'])
        self.assertEqual(row['status'],'passed')

    def test_timeout_kills_and_reaps_a_child_that_ignores_term_after_parent_exits(self):
        work=self.root/'group';work.mkdir()
        pidfile=work/'child.pid'
        child=f"import os,signal,time;from pathlib import Path;signal.signal(signal.SIGTERM,signal.SIG_IGN);Path({str(pidfile)!r}).write_text(str(os.getpid()));time.sleep(60)"
        parent=f"import subprocess,sys,time;subprocess.Popen([sys.executable,'-c',{child!r}]);time.sleep(60)"
        outcome=matrix.supervise([sys.executable,'-c',parent],dict(os.environ),str(work),work,work/'runner.log',
                                 idle_seconds=1,max_seconds=4,poll_seconds=.05,terminate_grace=.1)
        self.assertEqual(outcome['timeout_kind'],'idle')
        self.assertEqual(outcome['remaining_processes'],[])
        pid=int(pidfile.read_text())
        self.assertFalse(Path('/proc',str(pid)).exists(),f'orphan/zombie {pid} survived cleanup')

    def test_case_environment_starts_from_fresh_base_and_owned_transport_wins(self):
        args=types.SimpleNamespace(endpoint='tcp://127.0.0.1:9',token='x',credit=2)
        first={'properties':[{'name':'ENVIRONMENT','value':['CASE_ONLY=one','MOBILEGL_IPC_CONTROL=tcp://stale:1']}]}
        base={'BASE':'same'}
        a=matrix.case_environment(first,base,args)
        b=matrix.case_environment({'properties':[]},base,args)
        self.assertEqual(a['CASE_ONLY'],'one')
        self.assertNotIn('CASE_ONLY',b)
        self.assertEqual(a['MOBILEGL_IPC_CONTROL'],args.endpoint)
        self.assertEqual(base,{'BASE':'same'})

    def test_current_manifest_default_and_explicit_non_ci_selection(self):
        common={'trace_archive':'fixture.tgz','trace_file':'trace.trace','golden':'golden.png',
                'target_call':42,'width':16,'height':16}
        self.write_inputs(extra_cases=[dict(common,name='Extra',ci=False),dict(common,name='OptOut',split=False)])
        self.assertEqual([c['name'] for c in matrix.select_cases(self.manifest,None)],['Example'])
        self.assertEqual(matrix.select_cases(self.manifest,['Extra'])[0]['name'],'Extra')
        with self.assertRaises(ValueError):matrix.select_cases(self.manifest,['OptOut'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
