#!/usr/bin/env python3
from pathlib import Path
import argparse,json,os,subprocess,time
p=argparse.ArgumentParser();p.add_argument('--sdk',required=True,type=Path);p.add_argument('--build',required=True,type=Path);p.add_argument('--out',required=True,type=Path);p.add_argument('--jobs',type=int,default=int(os.environ.get('REGFW_PARALLEL_JOBS','32')));a=p.parse_args();sdk=a.sdk.resolve();build=a.build.resolve();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
s=(sdk/'benchmarks/compile.cpp').read_text();start=s.index(' REG_LAYOUT(');end=s.index('\n });',start)+len('\n });');factory=s[start+len(' REG_LAYOUT('):end-2]
s=s[:start]+' REG_LAYOUT_DECL();'+s[end:];s=s[:s.index('\n#ifndef INCLUDE_ONLY')];s=s.replace('REG_BUNDLE(Dag)','REG_BUNDLE_DECL(Dag)').replace('REG_BUNDLE(Module)','REG_BUNDLE_DECL(Module)').replace('REG_BUNDLE(Arrays)','REG_BUNDLE_DECL(Arrays)');s='\n'.join(line for line in s.splitlines() if not line.startswith('std::int64_t cpu_ns()'));(out/'layout.hpp').write_text('#pragma once\n'+s)
(out/'layout.cpp').write_text('#include "layout.hpp"\ntemplate<unsigned N,class T> Arrays<N,T>::Arrays(reg::RegScope scope):reg::RegEntityBundle(std::move(scope)){}\ntemplate<unsigned N,class T> Arrays<N,T>::~Arrays()=default;\nREG_BUNDLE_IMPL(Module);\nREG_BUNDLE_IMPL(Dag);\nREG_LAYOUT_IMPL(Dag, '+factory+');\n')
for i in range(32):(out/f'user{i}.cpp').write_text(f'#include "layout.hpp"\nvoid use{i}(Dag& root){{root.vf[{i%8}].A0.at(0).ref()={i};}}\n')
(out/'main.cpp').write_text('''#include "layout.hpp"
#include <stdexcept>
int main(){reg::RegisterBank<Dag> b(reg::space());b.map().set_identity_resolver(Route,[](auto& r){return r.request_id;});b.finish();if(!b.map().mmio_write(0xc100220000ULL,123).success()||b.map().mmio_read(0xff71f0020000ULL)!=123)throw std::runtime_error("alias");auto s=b.save();b.reset();b.load(s);if(b.map().mmio_read(0xff71f0020000ULL)!=123)throw std::runtime_error("snapshot");}
''')
# Paths in this reproducer must not contain Make-special whitespace.
for x in [sdk,build,out]:
 if any(c.isspace() for c in str(x)):raise SystemExit('use paths without whitespace for this Make fixture')
objects=' '.join(['layout.o','main.o']+[f'user{i}.o' for i in range(32)])
(out/'Makefile').write_text(f'''CXX=g++
CXXFLAGS=-std=c++20 -O2 -DNDEBUG -fno-strict-aliasing -DELEMENTS=128 -I{sdk}/include -I{sdk}/third_party/hlog/include
app: {objects}
\t$(CXX) $^ {build}/libregfw.a -L{sdk}/third_party/hlog/lib -Wl,-rpath,{sdk}/third_party/hlog/lib -lhlog -pthread -o $@
%.o: %.cpp layout.hpp
\t$(CXX) $(CXXFLAGS) -c $< -o $@
''')
def memory():return int(Path('/sys/fs/cgroup/memory.current').read_text())
start=memory();peak=start;t=time.monotonic()
with (out/'make.log').open('w') as f:
 jobs=max(1,min(a.jobs,128));proc=subprocess.Popen(['make','-j'+str(jobs)],cwd=out,stdout=f,stderr=subprocess.STDOUT)
 while proc.poll() is None:peak=max(peak,memory());time.sleep(.1)
elapsed=time.monotonic()-t;run=subprocess.run([str(out/'app')],capture_output=True,text=True) if proc.returncode==0 else None
r={'command':'make -j'+str(jobs),'translation_units':34,'elements':128,'exit_code':proc.returncode,'run_exit_code':None if run is None else run.returncode,'seconds':elapsed,'cgroup_start_bytes':start,'cgroup_peak_bytes':peak,'cgroup_increase_bytes':peak-start,'note':'cgroup includes surrounding environment and cache; not compiler RSS'}
(out/'result.json').write_text(json.dumps(r,indent=2));print(json.dumps(r),flush=True)
if proc.returncode or run.returncode:raise SystemExit(1)
