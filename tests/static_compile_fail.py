#!/usr/bin/env python3
"""Compile-fail contracts for layouts which must never reach machine init.

Structural errors are diagnosed during consteval layout compilation.
Register/register conflicts are diagnosed by the runtime installer (see
formula_geometry.cpp), so they are not compile-fail cases here.
"""
from pathlib import Path
import os,subprocess,sys,tempfile
root=Path(sys.argv[1]);cxx=sys.argv[2]
flags=[]
if sys.platform=='darwin':
    sdk=os.environ.get('SDKROOT','')
    if not sdk:
        try:
            sdk=subprocess.run(['xcrun','--show-sdk-path'],capture_output=True,text=True,check=True).stdout.strip()
        except Exception:
            sdk=''
    if sdk:flags=['-isysroot',sdk]
def compile_only(source,name,temp):
    file=Path(temp)/(name+'.cpp');file.write_text(source)
    return subprocess.run([cxx,'-std=c++20',*flags,'-fsyntax-only',
        '-I'+str(root/'include'),'-I'+str(root/'third_party/hlog/include'),str(file)],
        capture_output=True,text=True)
prefix='''#include <regfw/regfw.hpp>
struct R:reg::RegEntityBundle {REG_BUNDLE(R);REG(X,std::uint32_t,reg::at(0));REG_SCHEMA(&R::X);};
'''
# Only declaration/type errors remain compile-time contracts. Geometry and
# array-selector errors are validated during native layout initialization.
cases={
 'symbol_capture':('REG_ENTITY(r,R);','[this](auto layout){return layout.address_space("S").map(layout.view("V",64).map(layout.r),0);}','capture-free'),
}
with tempfile.TemporaryDirectory() as temp:
 for name,(decl,factory,message) in cases.items():
  source=prefix+f'struct D:reg::Bundle {{REG_BUNDLE(D);{decl}REG_LAYOUT({factory});}};\nint main(){{}}\n'
  result=compile_only(source,name,temp)
  if result.returncode==0 or message not in result.stderr:
   raise RuntimeError(name+' failed its compile-time contract:\n'+result.stderr[-8000:])
# The light declaration entry provides no layout compiler: REG_LAYOUT in a
# bundle.hpp-only TU must fail loudly instead of silently linking nothing.
light_source='''#include <regfw/bundle.hpp>
struct R:reg::RegEntityBundle {REG_BUNDLE(R);REG(X,std::uint32_t,reg::at(0));};
struct D:reg::Bundle {REG_BUNDLE(D);REG_ENTITY(r,R);REG_LAYOUT([](auto layout){return layout.address_space("S").map(layout.view("V",64).map(layout.r),0);});};
int main(){reg::RegisterBank<D> b(reg::space());}
'''
with tempfile.TemporaryDirectory() as temp:
 result=compile_only(light_source,'light_layout',temp)
 if result.returncode==0 or 'REG_LAYOUT requires <regfw/regfw.hpp>' not in result.stderr:
  raise RuntimeError('light_layout failed its compile-time contract:\n'+result.stderr[-8000:])
print('PASS: 2 declaration-contract failures rejected during compilation')
