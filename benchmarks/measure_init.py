from pathlib import Path
import subprocess,os,json,random
r=Path(os.environ["REGFW_INIT_OUTPUT"]).resolve();cpu=min(os.sched_getaffinity(0));records=[]
cases=['storage128','storage1024','storage4096','wide256','dag','dag-summary','dag-full']
for block in range(3):
 order=cases[:];random.Random(3000+block).shuffle(order)
 for j,case in enumerate(order):
  versions=['old','new'] if (block+j)%2==0 else ['new','old']
  for version in versions:
   cmd=['taskset','-c',str(cpu),str(r/(version+'-init')),case,'4']
   p=subprocess.run(cmd,text=True,capture_output=True,check=True)
   rows=[]
   for line in p.stdout.splitlines():
    a=line.split()
    if a and a[0]=='RESULT':rows.append(list(map(int,a[2:])))
   if len(rows)!=4:raise RuntimeError(p.stdout)
   records.append({'block':block,'case':case,'version':version,'cpu':cpu,'samples':rows,'command':cmd})
   (r/'init-results.json').write_text(json.dumps(records,indent=2))
  print(block,case,flush=True)
(r/'init-status.json').write_text(json.dumps({'state':'done','processes':len(records),'cpu':cpu}));print('DONE',flush=True)
