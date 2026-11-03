from pathlib import Path
import subprocess,json,random,statistics,os,sys
out=Path(sys.argv[1]).resolve();cpu=min(os.sched_getaffinity(0));records=[]
cases=[(m,c,p,w) for m in (0,1) for c in (0,1) for p in ('random','mapped') if m or p=='random' for w in (0,1)]
for logger in (False,True):
 env=dict(os.environ,REGFW_BENCH_THREADS='1');env.pop('REGFW_BENCH_LOGGER',None)
 if logger:env['REGFW_BENCH_LOGGER']='1'
 for block in range(4):
  order=cases[:];random.Random(3200+block).shuffle(order)
  for j,(mixed,cb,pattern,write) in enumerate(order):
   checks=[]
   for version in (('old','new') if (j+block)%2 else ('new','old')):
    cmd=['taskset','-c',str(cpu),str(out/f'{version}-{mixed}'),'10','4',str(mixed),str(cb),pattern,str(write)]
    r=subprocess.run(cmd,capture_output=True,text=True,env=env,check=True);values={};checksum=None
    for line in r.stdout.splitlines():
     a=line.split()
     if a and a[0]=='RESULT':values[a[6]]=float(a[7])
     if a and a[0]=='CHECKSUM':checksum=a[1]
     if a and a[0]=='LIBC_SINGLE_THREADED':assert a[1]=='0'
    assert len(values)==3 and checksum is not None
    checks.append(checksum);records.append(dict(logger=logger,block=block,mixed=mixed,callbacks=cb,pattern=pattern,write=write,version=version,values=values,checksum=checksum))
   assert checks[0]==checks[1]
   (out/'mmio-results.json').write_text(json.dumps(records,indent=2))
  print(logger,block+1,flush=True)
summary=[]
for logger in (False,True):
 for m,c,p,w in cases:
  row=dict(logger=logger,mixed=m,callbacks=c,pattern=p,write=w)
  for v in ('old','new'):row[v+'_ns']=statistics.median(r['values']['regfw'] for r in records if (r['logger'],r['mixed'],r['callbacks'],r['pattern'],r['write'],r['version'])==(logger,m,c,p,w,v))
  row['delta_percent']=(row['new_ns']/row['old_ns']-1)*100;summary.append(row)
(out/'mmio-summary.json').write_text(json.dumps(summary,indent=2));print('DONE',len(records),flush=True)
