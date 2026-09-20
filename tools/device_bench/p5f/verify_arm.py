#!/usr/bin/env python3
"""Read-only verdict for the prepared public-pixel arm; never invokes adb."""
import pathlib
import re
import sys
import xml.etree.ElementTree as ET

d = pathlib.Path(sys.argv[1])
def hashes(path):
    return {name.removeprefix('./'): digest for digest, name in
            (line.split(maxsplit=1) for line in path.read_text().splitlines() if line.strip())}
assert hashes(d / 'sha256.txt') == hashes(d / 'device-sha256.txt'), 'host/device artifact hash mismatch'
arm = dict(re.findall(r'(backend|transport|role)=(\S+)', (d / 'arm.txt').read_text()))
assert (d / 'process-exit.txt').read_text().strip() == '0', 'process failed'
cases = ET.parse(d / 'xml').getroot().findall('.//testcase')
expected_cases = {
    'ClipDistanceScenario.AnEnabledClipDistanceRemovesTheNegativeHalf',
    'ClipDistanceScenario.ADisabledClipDistanceRemovesNothing',
    'ClipDistanceScenario.TheEnablesAreIndependentPerDistance',
    'CopyImageLayeredScenario.ArrayToArrayCopiesEverySlice',
    'CopyImageLayeredScenario.ArrayToArrayHonoursDifferentLayerOffsets',
    'CopyImageLayeredScenario.VolumeToVolumeHonoursNonZeroZ',
    'CopyImageLayeredScenario.VolumeToVolumeAtNonZeroMipLevel',
    'CopyImageLayeredScenario.ArrayToVolumeCopiesEverySlice',
    'CopyImageLayeredScenario.VolumeToArrayCopiesEverySlice',
    'P5fPublicMipmapScenario.UploadedRgba8BaseGeneratesRedMipChain',
    'P5fPublicMipmapScenario.GpuWrittenBaseGeneratesGreenMipChainWithoutStaleCpuUpload',
}
case_names = [c.get('classname', '') + '.' + c.get('name', '') for c in cases]
assert len(cases) == len(expected_cases), f'expected 11 selected cases, got {len(cases)}'
assert len(case_names) == len(set(case_names)), ('duplicate test cases', case_names)
assert set(case_names) == expected_cases, (
    'selected case set mismatch',
    {'missing': sorted(expected_cases - set(case_names)),
     'extra': sorted(set(case_names) - expected_cases)})
# The two pre-existing Vulkan per-distance-enable limitations are explicit,
# matched by exact case and reason, and never counted as executed passes.
allowed_skips = {
    'ClipDistanceScenario.ADisabledClipDistanceRemovesNothing': 'clips by a DISABLED gl_ClipDistance',
    'ClipDistanceScenario.TheEnablesAreIndependentPerDistance': 'clips by every declared gl_ClipDistance regardless of the enables',
} if arm['backend'] == 'DirectVulkan' else {}
skipped = []
for c in cases:
    name = c.get('classname', '') + '.' + c.get('name', '')
    assert c.get('status') == 'run', ('case did not execute', name, c.attrib)
    assert c.find('failure') is None and c.find('error') is None, c.attrib
    skip = c.find('skipped')
    if skip is not None:
        assert c.get('result') == 'skipped', ('invalid executed-skip result', name, c.attrib)
        text = ' '.join(skip.itertext()) + ' ' + skip.get('message', '')
        assert name in allowed_skips and allowed_skips[name] in text, ('unexpected skip', c.attrib, text)
        skipped.append(name)
    else:
        assert c.get('result') == 'completed', ('case did not complete', name, c.attrib)
stdout = (d / 'stdout.txt').read_text(errors='replace')
log = (d / 'library.log').read_text(errors='replace')
assert f'backend={arm["backend"]}' in stdout
assert 'AImageReader window' in stdout, 'not the Android window harness'
assert not re.search(r'Fatal\{|SIGSEGV|SIGABRT|AddressSanitizer', stdout + log)
stats = [line for line in log.splitlines() if 'MGPipe stats:' in line and re.search(r'\bframes=\d+', line)]
assert stats, 'no actual stats windows'
assert all(re.search(r'\brsp=0(?:\s|$)', line) for line in stats), 'nonzero/missing rsp'
assert any(int(re.search(r'\bframes=(\d+)', line)[1]) > 0 for line in stats), 'no present frames'
vbs = [int(re.search(r'\bvbs=(\d+)', line)[1]) for line in stats]
if arm['transport'] == 'inproc':
    assert any(v > 0 for v in vbs), 'no applied verb boundaries; transport not proven'
    assert re.search(r'Config: IPC .*strict=1 .*role-split-state=' + arm['role'] + r'\b', log)
    # Current Magma publishes the capability after its queued-state migration.
    # Historical P5f bundles should be checked with their matching source verifier.
    assert 'run-ahead ARMED' in log, 'missing runtime capability evidence'
else:
    assert all(v == 0 for v in vbs), 'monolith control has applied verb boundaries'
print(f'PASS {arm}: {len(cases)-len(skipped)} passed, {len(skipped)} named legacy feature skips, {len(stats)} stats windows, rsp=0, vbs={sum(vbs)}')
for name in skipped: print('  preserved limitation:', name)
