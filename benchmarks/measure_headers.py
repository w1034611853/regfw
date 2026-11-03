#!/usr/bin/env python3
"""Header-entry cost matrix for direction 3.

Measures one plain consumer TU per entry (light bundle.hpp vs full regfw.hpp),
each without and with the device header precompiled (PCH), plus a parallel
clean build of 8 such TUs. The PCH is created for the device header itself and
loaded through -include <header> (GCC then picks the adjacent .gch), so the
TU's own include of the same header is served from the PCH. Reports wall time
and peak RSS per compile, PCH construction cost and verification that the
compiler actually used the PCH (the -H trace must contain the .gch entry).
All outputs go under --record (create it outside the SDK tree).
"""
import argparse,json,os,subprocess,tempfile,time
from pathlib import Path

def run(cmd,cwd=None):
    """Wall time only, for commands (make) whose work happens in grandchildren
    that no single pid accounts for."""
    t=time.monotonic()
    r=subprocess.run(cmd,cwd=cwd,capture_output=True,text=True)
    return {'command':[str(c) for c in cmd],'exit_code':r.returncode,'wall_seconds':time.monotonic()-t,
            'peak_rss_kib':None,'stderr':r.stderr[-4000:]}

def run_measured(cmd,cwd=None):
    """Wall time and THIS child's peak RSS via os.wait4. getrusage(RUSAGE_CHILDREN)
    would instead report the running maximum over all earlier children."""
    import threading
    t=time.monotonic()
    p=subprocess.Popen(cmd,cwd=cwd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    outputs={}
    def drain(key,stream):outputs[key]=stream.read()
    th=[threading.Thread(target=drain,args=(k,s)) for k,s in (('out',p.stdout),('err',p.stderr))]
    for x in th:x.start()
    _,status,usage=os.wait4(p.pid,0)
    for x in th:x.join()
    return {'command':[str(c) for c in cmd],'exit_code':os.waitstatus_to_exitcode(status),
            'wall_seconds':time.monotonic()-t,'peak_rss_kib':usage.ru_maxrss,'stderr':outputs['err'][-4000:]}

def base_flags(cxx,root,tu,header):
    return [cxx,'-std=c++20','-O2','-DNDEBUG','-fno-strict-aliasing',
            '-I'+str(root/'include'),'-I'+str(root/'third_party/hlog/include'),'-I'+str(root/'benchmarks'),
            '-DDEVICE_HEADER="'+str(header)+'"','-c',str(tu)]

def main():
    p=argparse.ArgumentParser()
    p.add_argument('--record',required=True,help='output directory for JSON records')
    p.add_argument('--sdk',required=True)
    p.add_argument('--cxx',default='g++')
    p.add_argument('--tus',type=int,default=8)
    a=p.parse_args()
    root=Path(a.sdk).resolve();out=Path(a.record).resolve();out.mkdir(parents=True,exist_ok=True)
    cxx=a.cxx
    records={}
    for entry,wrapper in (('light','declare_device_light.hpp'),('full','declare_device_full.hpp')):
        header=root/'benchmarks'/wrapper;tu=root/'benchmarks/declare_tu.cpp'
        r=run_measured(base_flags(cxx,root,tu,header)+['-o',str(out/(entry+'.o'))])
        records[f'{entry}_tu']=r
        # PCH of the device header itself: copy it so the .gch lands in --record.
        pch_dir=out/(entry+'-pch');pch_dir.mkdir(exist_ok=True)
        device=pch_dir/wrapper;device.write_text(header.read_text())
        pch=run_measured([cxx,'-std=c++20','-O2','-DNDEBUG','-fno-strict-aliasing','-x','c++-header',
                 '-I'+str(root/'include'),'-I'+str(root/'third_party/hlog/include'),'-I'+str(root/'benchmarks'),
                 str(device),'-o',str(device)+'.gch'])
        records[f'{entry}_pch_build']=pch
        r=run_measured(base_flags(cxx,root,tu,device)+['-include',str(device),'-H','-o',str(out/(entry+'_pch.o'))])
        r['pch_used']=str(device)+'.gch' in r['stderr']
        records[f'{entry}_tu_pch']=r
    # Parallel clean build of N distinct consumer TUs with the light header + PCH.
    with tempfile.TemporaryDirectory() as temp:
        temp=Path(temp);device=out/'light-pch'/'declare_device_light.hpp'
        sources=[]
        for k in range(a.tus):
            f=temp/f'consumer{k}.cpp'
            f.write_text('#include DEVICE_HEADER\nint touch'+str(k)+'(declare_device::Device& d){return static_cast<int>(d.vf[0].A0.size());}\n')
            sources.append(f)
        makefile=temp/'Makefile'
        objs=' '.join(str(f.with_suffix('.o')) for f in sources)
        rule=('CXX = '+cxx+'\nFLAGS = -std=c++20 -O2 -DNDEBUG -fno-strict-aliasing -I'
              +str(root/'include')+' -I'+str(root/'third_party/hlog/include')+' -I'+str(root/'benchmarks')
              +' -DDEVICE_HEADER=\\"'+str(device)+'\\" -include '+str(device)+'\nall: '+objs+'\n%.o: %.cpp\n\t$(CXX) $(FLAGS) -c $< -o $@\n')
        makefile.write_text(rule)
        r=run(['make','-j',str(min(32,os.cpu_count() or 4)),'-f',str(makefile)],cwd=temp)
        records['parallel_light_pch']=r
    for name,r in records.items():
        print(json.dumps({'case':name,'exit_code':r['exit_code'],'wall_seconds':round(r['wall_seconds'],3),
                          'peak_rss_kib':r['peak_rss_kib'],'pch_used':r.get('pch_used')}))
        (out/(name+'.json')).write_text(json.dumps(r,indent=2))
    bad=[k for k,v in records.items() if v['exit_code']]
    if bad:raise SystemExit('failing cases: '+', '.join(bad))
    if not records['light_tu_pch']['pch_used']:raise SystemExit('light PCH was not used by the compiler')

if __name__=='__main__':main()
