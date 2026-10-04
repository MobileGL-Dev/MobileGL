import sys, collections
f = sys.argv[1]; tids = sys.argv[2].split(','); t0off = float(sys.argv[3]); span = float(sys.argv[4])
samples = []; cur = None
for line in open(f, encoding='utf-8', errors='replace'):
    s = line.strip()
    if s == 'sample:':
        if cur: samples.append(cur)
        cur = {'ev': '', 'tid': '', 'time': 0, 'syms': []}; continue
    if cur is None: continue
    if s.startswith('event_type:'): cur['ev'] = s.split()[1]
    elif s.startswith('thread_id:'): cur['tid'] = s.split()[1]
    elif s.startswith('time:'): cur['time'] = int(s.split()[1])
    elif s.startswith('symbol:'): cur['syms'].append(s.split(None, 1)[1])
if cur: samples.append(cur)
samples = [x for x in samples if x['tid'] in tids]
samples.sort(key=lambda x: x['time'])
T0 = samples[0]['time'] + int(t0off * 1e6)
# collapse into intervals per tid: state on/off; print transitions
last = {}
for x in samples:
    if x['time'] < T0 or x['time'] > T0 + span * 1e6: continue
    st = 'OFF' if x['ev'].startswith('sched') else 'on '
    k = [s for s in x['syms'] if 'kallsyms' not in s][:1]
    key = (st, x['syms'][0] if st=='on ' else ','.join(s for s in x['syms'][1:12] if s in ('do_sys_poll','futex_wait_queue','unix_stream_read_generic','binder_thread_read','do_nanosleep','__wake_up_sync_key')))
    if last.get(x['tid']) != key:
        print('%9.3f ms  %-6s %s %s' % ((x['time'] - T0) / 1e6, x['tid'], st, key[1][:60]))
        last[x['tid']] = key
