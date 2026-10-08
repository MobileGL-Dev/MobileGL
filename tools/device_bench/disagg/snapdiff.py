import sys
def rd(p):
    L=open(p).read().split('\n'); up=float(L[0].split()[0]); gpu=sum(int(x) for x in L[1].split())
    T={}
    for l in L[2:]:
        f=l.split()
        if len(f)>=5 and f[0]=='T': T[f[1]]=(int(f[2]),int(f[3]),int(f[4]),' '.join(f[5:]))
    return up,gpu,T
a=rd(sys.argv[1]); b=rd(sys.argv[2]); wall=b[0]-a[0]
print(f"wall {wall:.2f}s gpu_clock_stats delta {(b[1]-a[1])/1e6:.3f} (units?) per wall-s {(b[1]-a[1])/wall/1e6:.3f}")
rows=[]
for k,v in b[2].items():
    if k in a[2]:
        u=a[2][k]; rows.append(((v[0]-u[0])/1e6,(v[1]-u[1])/1e6,v[2]-u[2],v[3],k))
rows.sort(reverse=True)
for r in rows[:12]: print(f"{r[3]:<22} tid {r[4]:>6} cpu {r[0]:8.1f} ms ({100*r[0]/1e3/wall:5.1f}%)  runq-wait {r[1]:7.1f} ms  switches {r[2]}")
