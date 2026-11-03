#!/usr/bin/env python3
"""Linux/GCC paired 0.31 baseline comparison; results go outside the SDK."""
from pathlib import Path
import argparse,subprocess,sys,os
p=argparse.ArgumentParser();p.add_argument('--baseline',type=Path,required=True);p.add_argument('--out',type=Path,required=True);p.add_argument('--cmake',default='cmake');a=p.parse_args()
here=Path(__file__).resolve().parent;sdk=here.parent;old=a.baseline.resolve();out=a.out.resolve()
if out==sdk or sdk in out.parents or out==old or old in out.parents:raise SystemExit('output must be outside both SDKs')
out.mkdir(parents=True,exist_ok=True)
if any(out.iterdir()):raise SystemExit('output must be empty')
def run(cmd,log):
 r=subprocess.run(cmd,capture_output=True,text=True);(out/log).write_text(r.stdout+r.stderr);r.check_returncode()
for n in (64,128,256):
 for label,src in [('old',old),('new',sdk)]:
  cmd=['g++','-std=c++20','-O2','-DNDEBUG','-fno-strict-aliasing',f'-DELEMENTS={n}','-I'+str(src/'include'),'-I'+str(src/'third_party/hlog/include'),'-c',str(here/'compile.cpp'),'-o',str(out/f'{label}{n}.o')]
  subprocess.run([sys.executable,str(here/'measure_compile.py'),'--record',str(out/f'{label}{n}'),'--']+cmd,check=True)
for label,src in [('old',old),('new',sdk)]:
 build=out/(label+'-build');run([a.cmake,'-S',str(src),'-B',str(build),'-DCMAKE_BUILD_TYPE=Release','-DREGFW_BUILD_TESTS=OFF'],label+'-config.log');run([a.cmake,'--build',str(build),'-j3'],label+'-build.log')
 common=['g++','-std=c++20','-O3','-DNDEBUG','-fno-strict-aliasing','-I'+str(src/'include'),'-I'+str(src/'third_party/hlog/include')]
 link=[str(build/'libregfw.a'),'-L'+str(src/'third_party/hlog/lib'),'-Wl,-rpath,'+str(src/'third_party/hlog/lib'),'-lhlog','-pthread']
 for mixed in (0,1):run(common+['-DREGFW_STATIC_BENCH',f'-DREGFW_BENCH_MIXED={mixed}','-DREGFW_BENCH_STRIDE=4',str(here/'mmio.cpp')]+link+['-o',str(out/f'{label}-{mixed}')],f'{label}-{mixed}-build.log')
 for name,source in [('init','init.cpp'),('digest','plan_digest.cpp')]:run(common+[str(here/source)]+link+['-o',str(out/f'{label}-{name}')],f'{label}-{name}-build.log')
 run([str(out/f'{label}-digest')],label+'-digest.log')
assert (out/'old-digest.log').read_bytes()==(out/'new-digest.log').read_bytes()
subprocess.run([sys.executable,str(here/'parallel.py'),'--sdk',str(sdk),'--build',str(out/'new-build'),'--out',str(out/'parallel')],check=True)
subprocess.run([sys.executable,str(here/'measure_mmio.py'),str(out)],check=True)
subprocess.run([sys.executable,str(here/'measure_init.py')],env=dict(os.environ,REGFW_INIT_OUTPUT=str(out)),check=True)

subprocess.run([sys.executable,str(here/'measure_noise.py'),str(out)],check=True)
