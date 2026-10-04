import sys, re, subprocess, collections, os
f, tid, so, base, sub, target = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4],16), sys.argv[5], sys.argv[6]
SYM = os.path.expandvars(r'$LOCALAPPDATA/Android/Sdk/ndk/27.2.12479018/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-symbolizer.exe')
S=[]; cur=None
for line in open(f, encoding='utf-8', errors='replace'):
    s=line.strip()
    if s=='sample:':
        if cur: S.append(cur)
        cur={'ev':'','tid':'','fr':[]}; continue
    if cur is None: continue
    if s.startswith('event_type:'): cur['ev']=s.split()[1]
    elif s.startswith('thread_id:'): cur['tid']=s.split()[1]
    elif s.startswith('vaddr_in_file:'): cur['fr'].append([s.split()[1],''])
    elif s.startswith('file:') and cur['fr']: cur['fr'][-1][1]=s.split(None,1)[1]
if cur: S.append(cur)
S=[x for x in S if x['tid']==tid and not x['ev'].startswith('sched')]
addrs=sorted({int(a,16)+base for x in S for a,d in x['fr'] if sub in d})
names={}
for i in range(0,len(addrs),3000):
    part=addrs[i:i+3000]
    out=subprocess.run([SYM,'--obj='+so,'-C','-f','--inlining=false']+['0x%x'%a for a in part],capture_output=True,text=True).stdout
    for a,b in zip(part,out.strip().split('\n\n')): names[a]=re.sub(r'\(.*','',b.split('\n')[0]).split('::')[-1]
agg=collections.Counter(); tot=0
for x in S:
    ch=[names.get(int(a,16)+base,'?') for a,d in x['fr'] if sub in d]
    if target in ch:
        k=ch.index(target); tot+=1
        agg[' <- '.join(ch[k:k+4])]+=1
print('samples with', target, tot, 'of', len(S))
for k,v in agg.most_common(12): print(v, k)
