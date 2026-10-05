#!/usr/bin/env python3
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('runtime', Path(__file__).with_name('prepare_test_runtime.py'))
runtime = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runtime)


class MetadataTest(unittest.TestCase):
    def test_legacy_environment_and_labels_escaping_is_preserved(self):
        text = r'[[ENVIRONMENT]] [[MOBILEGL_BACKEND_TYPE=DirectVulkan\;MOBILEGL_TRANSPORT=inproc]] [[LABELS]] [[gpu\;split]]'
        fixed = runtime.normalize(text, '', '/new', '/cmake')
        self.assertEqual(fixed, text)

    def test_other_escaped_values_are_preserved(self):
        text = r'COMMAND "argument\;with-semicolon"'
        self.assertEqual(runtime.normalize(text, '', '/new', '/cmake'), text)

    def test_relocation_and_cmake_modules(self):
        text = '"/old/build/test" "/old/tool/bin/cmake" "/old/tool/share/cmake/Modules/GoogleTest.cmake"'
        fixed = runtime.normalize(text, '/old', '/new', '/usr/share/cmake')
        self.assertIn('"/new/build/test"', fixed)
        self.assertIn('"cmake"', fixed)
        self.assertIn('/usr/share/cmake/Modules/GoogleTest.cmake', fixed)

    def test_hardware_replaces_icd_and_removes_software_overrides(self):
        text = 'ENVIRONMENT "VK_ICD_FILENAMES=/lvp.json;LIBGL_ALWAYS_SOFTWARE=1;MESA_GL_VERSION_OVERRIDE=3.3;MESA_GLSL_VERSION_OVERRIDE=330;EGL_PLATFORM=surfaceless"'
        fixed = runtime.normalize(text, '', '/new', '/cmake', True, '/dzn.json')
        self.assertIn('VK_ICD_FILENAMES=/dzn.json', fixed)
        self.assertIn('EGL_PLATFORM=surfaceless', fixed)
        self.assertNotIn('SOFTWARE', fixed)
        self.assertNotIn('VERSION_OVERRIDE', fixed)
        self.assertIn('LIBGL_ALWAYS_SOFTWARE=1', runtime.normalize(text, '', '/new', '/cmake'))

    def test_hardware_driver_change_preserves_legacy_separators(self):
        text = r'ENVIRONMENT "A=x\;VK_ICD_FILENAMES=/lvp.json\;LIBGL_ALWAYS_SOFTWARE=1\;B=y"'
        fixed = runtime.normalize(text, '', '/new', '/cmake', True, '/dzn.json')
        self.assertEqual(fixed, r'ENVIRONMENT "A=x\;VK_ICD_FILENAMES=/dzn.json\;B=y"')

    def test_preparation_is_idempotent_and_drops_discovery_cache(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / '.ci-source-root').write_text('/old\n')
            (root / 'tests.cmake').write_text('ENVIRONMENT "BACKEND=x\\;TRANSPORT=y"\n"/old/build"')
            (root / 'x_tests.json').write_text('{}')
            runtime.prepare(root, Path('/new'), '/cmake', False, None)
            first = (root / 'tests.cmake').read_text()
            runtime.prepare(root, Path('/new'), '/cmake', False, None)
            self.assertEqual(first, (root / 'tests.cmake').read_text())
            self.assertFalse((root / 'x_tests.json').exists())

    @unittest.skipUnless(os.name == 'posix' and shutil.which('cmake'), 'requires POSIX CMake')
    def test_real_ctest_discovery_separates_environment(self):
        version = subprocess.check_output(['cmake', '--version'], text=True).splitlines()[0]
        self.assertIn('3.31.10', version, 'CI must use its pinned legacy GoogleTest discovery version')
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            probe = root / 'probe.py'
            probe.write_text('#!/usr/bin/env python3\nimport sys, os\n'
                             'if "--gtest_list_tests" in sys.argv:\n print("Probe.\\n  Environment")\n'
                             'else:\n assert os.environ.get("MOBILEGL_BACKEND_TYPE") == "DirectVulkan", repr(dict(os.environ))\n assert os.environ.get("MOBILEGL_TRANSPORT") == "inproc"\n')
            probe.chmod(0o755)
            (root / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.22)\nproject(EnvProbe NONE)\nenable_testing()\ninclude(GoogleTest)\n'
                f'add_executable(probe IMPORTED)\nset_target_properties(probe PROPERTIES IMPORTED_LOCATION "{probe}")\n'
                'gtest_discover_tests(probe DISCOVERY_MODE PRE_TEST PROPERTIES '
                'ENVIRONMENT "MOBILEGL_BACKEND_TYPE=DirectVulkan\\;MOBILEGL_TRANSPORT=inproc" LABELS "gpu\\;split")\n')
            build = root / 'build'
            subprocess.run(['cmake', '-S', str(root), '-B', str(build)], check=True, capture_output=True)
            info = subprocess.check_output(['cmake', '--system-information'], text=True)
            cmake_root = re.search(r'^CMAKE_ROOT "([^"]+)"', info, re.M)[1]
            runtime.prepare(build, root, cmake_root, False, None)
            inventory = json.loads(subprocess.check_output(['ctest', '--test-dir', str(build), '--show-only=json-v1'], text=True))
            props = {p['name']: p['value'] for p in inventory['tests'][0]['properties']}
            self.assertEqual(props['ENVIRONMENT'], ['MOBILEGL_BACKEND_TYPE=DirectVulkan', 'MOBILEGL_TRANSPORT=inproc'])
            self.assertEqual(set(props['LABELS']), {'gpu', 'split'})
            result = subprocess.run(['ctest', '--test-dir', str(build), '--output-on-failure'], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
