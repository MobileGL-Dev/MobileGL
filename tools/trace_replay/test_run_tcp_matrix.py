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
         'actualPath':str(actual),
         'matchedGoldenPath':os.environ.get('CASE_MATCH_GOLDEN') or values['TRACE_GOLDEN'],
         'ssim':float(os.environ.get('CASE_SSIM','1.0'))}
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

    def write_inputs(self, behavior='success', extra_cases=None, case_overrides=None,
                     backends=('DirectGLES',)):
        case={'name':'Example','trace_archive':'fixture.tgz','trace_file':'trace.trace',
              'golden':'golden.png','target_call':42,'width':16,'height':16,'timeout_seconds':.05}
        case.update(case_overrides or {})
        cases=[case]
        if extra_cases:cases.extend(extra_cases)
        self.manifest.write_text(json.dumps({'defaults':{'ssim_threshold':.99},'cases':cases}))
        tests=[]
        for backend in backends:
            command=[sys.executable,str(self.stub),'-DTRACE_REPLAY_EXE='+sys.executable,
                     '-DMOBILEGL_LIBRARY=/old/library','-DTRACE_CASE_NAME=Example','-DTRACE_BACKEND='+backend,
                     '-DTRACE_ARCHIVE='+str(self.fixtures/'fixture.tgz'),'-DTRACE_OUTPUT_DIR=/old/output',
                     '-P',str(self.tool/'run_trace_case.cmake')]
            tests.append({'name':'MobileGLTraceReplay.Example.'+backend,'command':command,
                          'properties':[{'name':'ENVIRONMENT','value':['CASE_BEHAVIOR='+behavior]},
                                        {'name':'TIMEOUT','value':.05},
                                        {'name':'WORKING_DIRECTORY','value':str(self.root)}]})
        self.catalog.write_text(json.dumps({'kind':'ctestInfo','tests':tests}))

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
        for test in catalog['tests']:
            test['properties'][0]['value'] += ['CASE_ARM='+arm,*(extra or [])]
        self.catalog.write_text(json.dumps(catalog))

    def set_case_env(self, entries):
        catalog=matrix.read_json(self.catalog)
        for test in catalog['tests']:
            test['properties'][0]['value'] += list(entries)
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

    # ------------------------------------------------------------------------------------
    # P7: the same driver runs the DirectVulkan device matrix.
    # ------------------------------------------------------------------------------------
    VULKAN_ONLY={'trace_archive':'fixture.tgz','trace_file':'trace.trace','golden':'golden.png',
                 'target_call':42,'width':16,'height':16,'ci_backends':['DirectVulkan']}
    GLES_ONLY=dict(VULKAN_ONLY,ci_backends=['DirectGLES'])

    def plan_args(self, **extra):
        args=types.SimpleNamespace(source=self.source,library=self.library,fixtures=self.fixtures,
                                   runner=Path(sys.executable),endpoint='tcp://127.0.0.1:9',
                                   token='x',credit=2,require_run_ahead=False,base_env={'BASE':'same'})
        for key,value in extra.items():setattr(args,key,value)
        return args

    def plan_for(self, backend, case_overrides=None):
        self.write_inputs(case_overrides=case_overrides,backends=('DirectGLES','DirectVulkan'))
        catalog={test['name']:test for test in matrix.read_json(self.catalog)['tests']}
        case=matrix.select_cases(self.manifest,None,[backend])[0]
        return matrix.plan_case(case,backend,catalog,self.plan_args(),{})

    def test_selection_honours_per_case_backends_and_names_an_impossible_pairing(self):
        self.write_inputs(extra_cases=[dict(self.VULKAN_ONLY,name='VulkanOnly'),
                                       dict(self.GLES_ONLY,name='GlesOnly')])
        # The default subset is every split case; the per-backend filter happens in main(),
        # so both restricted cases survive selection itself.
        self.assertEqual([c['name'] for c in matrix.select_cases(self.manifest,None,['DirectVulkan'])],
                         ['Example','VulkanOnly','GlesOnly'])
        # A case NAMED on the command line that cannot run the requested backend is an error
        # that says which case and what it does run - not one plan fewer and no message.
        with self.assertRaises(ValueError) as caught:
            matrix.select_cases(self.manifest,['GlesOnly'],['DirectVulkan'])
        self.assertIn('GlesOnly',str(caught.exception))
        self.assertIn('DirectGLES',str(caught.exception))
        self.assertEqual(matrix.select_cases(self.manifest,['GlesOnly'],['DirectGLES'])[0]['name'],'GlesOnly')
        self.assertEqual(matrix.select_cases(self.manifest,['VulkanOnly'],['DirectVulkan'])[0]['name'],'VulkanOnly')

    def test_directvulkan_plans_its_own_golden_and_threshold(self):
        overrides={'backend_overrides':{'DirectVulkan':{'golden':'vulkan-golden.png',
                                                        'ssim_threshold':0.95}}}
        (self.fixtures/'vulkan-golden.png').write_bytes(b'vulkan golden image')
        vulkan=self.plan_for('DirectVulkan',overrides)
        self.assertEqual(Path(vulkan['values']['TRACE_GOLDEN']).name,'vulkan-golden.png')
        self.assertEqual(vulkan['values']['TRACE_SSIM_THRESHOLD'],'0.95')
        self.assertEqual(vulkan['values']['TRACE_ALTERNATE_GOLDEN'],'')
        self.assertTrue(vulkan['key'].startswith('DirectVulkan/'))
        gles=self.plan_for('DirectGLES',overrides)
        self.assertEqual(Path(gles['values']['TRACE_GOLDEN']).name,'golden.png')
        self.assertEqual(gles['values']['TRACE_SSIM_THRESHOLD'],'0.99')
        self.assertTrue(gles['key'].startswith('DirectGLES/'))
        self.assertNotEqual(gles['identity'],vulkan['identity'])
        # Each backend knows which goldens belong to the other one - including the SHARED
        # golden, which stops being neutral the moment one backend replaces it.
        self.assertEqual({Path(p).name for p in gles['foreign_goldens']},{'vulkan-golden.png'})
        self.assertEqual({Path(p).name for p in vulkan['foreign_goldens']},{'golden.png'})

    def test_directgles_planning_is_untouched_by_another_backends_override(self):
        """The 'keep DirectGLES byte-identical' claim, asserted rather than asserted-in-prose."""
        (self.fixtures/'vulkan-golden.png').write_bytes(b'vulkan golden image')
        before=self.plan_for('DirectGLES')
        after=self.plan_for('DirectGLES',{'backend_overrides':{'DirectVulkan':{
            'golden':'vulkan-golden.png','ssim_threshold':0.95}}})
        self.assertEqual(before['values'],after['values'])
        self.assertEqual(before['key'],after['key'])
        # The identity fingerprint is what --resume compares, so it must not move either.
        self.assertEqual(before['identity'],after['identity'])

    def test_a_directgles_golden_never_satisfies_a_directvulkan_run(self):
        """RED-ONCE: the wrong-backend golden is refused, by name."""
        (self.fixtures/'vulkan-golden.png').write_bytes(b'vulkan golden image')
        self.write_inputs(case_overrides={'backend_overrides':{'DirectVulkan':{
            'golden':'vulkan-golden.png'}}},backends=('DirectGLES','DirectVulkan'))
        self.set_case_env(['CASE_MATCH_GOLDEN='+str(self.fixtures/'golden.png')])
        self.assertEqual(self.run_main('--backend','DirectVulkan'),1)
        row=self.checkpoint()[-1]
        self.assertEqual(row['status'],'failed')
        self.assertIn('another backend declares',row['error'])
        self.assertIn('golden.png',row['error'])
        self.assertEqual(row['returncode'],1)
        # The control: the same stub reporting the DirectVulkan golden passes.
        self.write_inputs(case_overrides={'backend_overrides':{'DirectVulkan':{
            'golden':'vulkan-golden.png'}}},backends=('DirectGLES','DirectVulkan'))
        self.set_case_env(['CASE_MATCH_GOLDEN='+str(self.fixtures/'vulkan-golden.png')])
        self.assertEqual(self.run_main('--backend','DirectVulkan'),0)
        row=self.checkpoint()[-1]
        self.assertEqual(row['status'],'passed')
        self.assertEqual(row['backend'],'DirectVulkan')
        self.assertEqual(Path(row['golden']).name,'vulkan-golden.png')

    def test_directvulkan_run_ahead_proof_and_checkpoint_keys_are_per_backend(self):
        self.write_inputs(backends=('DirectGLES','DirectVulkan'))
        self.set_arm('respect-env',['MOBILEGL_IPC_RUN_AHEAD=0','MOBILEGL_IPC_VERB_BARRIER=0'])
        self.assertEqual(self.run_main('--backend','DirectGLES','--backend','DirectVulkan',
                                       '--require-run-ahead'),0)
        rows=self.checkpoint()
        self.assertEqual([row['backend'] for row in rows],['DirectGLES','DirectVulkan'])
        self.assertEqual({row['key'] for row in rows},
                         {'DirectGLES/credit-2/Example','DirectVulkan/credit-2/Example'})
        self.assertTrue(all(row['actual_arm']=={'armed':True,'lockstep':False,'disarmed':False}
                            for row in rows))
        # Each backend keeps its own checkpoint entry: resuming one must not consume the other.
        self.assertEqual(self.run_main('--backend','DirectVulkan','--require-run-ahead','--resume'),0)
        self.assertEqual(len(self.checkpoint()),2)

    def test_a_directvulkan_lockstep_log_is_refused_exactly_as_a_directgles_one_is(self):
        self.write_inputs(backends=('DirectGLES','DirectVulkan'))
        self.set_arm('lockstep')
        self.assertEqual(self.run_main('--backend','DirectVulkan','--require-run-ahead'),1)
        row=self.checkpoint()[-1]
        self.assertEqual(row['backend'],'DirectVulkan')
        self.assertEqual(row['status'],'failed')
        self.assertIn('required run-ahead',row['error'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
