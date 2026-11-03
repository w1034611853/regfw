"""A/A control: both labels execute the same 0.31 binary; no code changes."""
from pathlib import Path
import subprocess,os,json,statistics,sys,random
out=Path(sys.argv[1]).resolve();cpu=min(os.sched_getaffinity(0));rows=[]
cases=[(0,0,'random',0),(0,0,'random',1),(1,1,'mapped',0)]
env=dict(os.environ,REGFW_BENCH_THREADS='1');env.pop('REGFW_BENCH_LOGGER',None)
for block in range(6):
 order=cases[:];random.Random(111+block).shuffle(order)
 for j,(m,c,p,w) in enumerate(order):
  for label in (['A','B'] if (block+j)%2 else ['B','A']):
   r=subprocess.run(['taskset','-c',str(cpu),str(out/f'old-{m}'),'10','4',str(m),str(c),p,str(w)],capture_output=True,text=True,env=env,check=True)
   values={}
   for line in r.stdout.splitlines():
    a=line.split()
    if a and a[0]=='RESULT':values[a[6]]=float(a[7])
   rows.append(dict(block=block,case=[m,c,p,w],label=label,values=values))
  (out/'noise-results.json').write_text(json.dumps(rows,indent=2))
 print(block,flush=True)
summary=[]
for case in cases:
 d={v:statistics.median(x['values']['regfw'] for x in rows if tuple(x['case'])==case and x['label']==v) for v in ['A','B']};summary.append(dict(case=case,**d,delta_percent=(d['B']/d['A']-1)*100))
(out/'noise-summary.json').write_text(json.dumps(summary,indent=2));print(summary)
