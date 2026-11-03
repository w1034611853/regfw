#!/usr/bin/env python3
"""regfw 0.35 formula-backend benchmark driver.

Measures compile time / compiler peak RSS, layout install time and MMIO TPS.
There is no word-table backend to compare with in-tree; the frozen 0.34.0
numbers are kept in docs/FormulaBackend.md for historical context.
"""
import json,os,platform,statistics,subprocess,time
from pathlib import Path

ROOT=Path(__file__).resolve().parent.parent
BUILD=ROOT/os.environ.get('REGFW_BENCH_BUILD','build-local')
CXX=os.environ.get('CXX','clang++')
CXX_VERSION=subprocess.run([CXX,'--version'],capture_output=True,text=True,check=True).stdout.lower()
BASE_FLAGS=['-std=c++20','-O1','-fno-strict-aliasing','-I'+str(ROOT/'include'),
            '-I'+str(ROOT/'third_party/hlog/include'),'-I'+str(ROOT/'benchmarks'),
            ]
RSS_DIV=1024 if platform.system()=='Darwin' else 1
# /usr/bin/time reports peak RSS as '-l' (macOS) or '-v' (Linux); both write
# to stderr, macOS in 512-byte units and Linux in KiB.
TIME_RSS_FLAG='-l' if platform.system()=='Darwin' else '-v'
def _time_rss_kib(stderr:str):
    for line in stderr.splitlines():
        if 'maximum resident set size' in line.lower():
            # macOS prints the value before the label (bytes); Linux /usr/bin/time
            # -v prints "Maximum resident set size (kbytes): N" (KiB).
            tokens=[t for t in line.replace(':',' ').split() if t.isdigit()]
            if not tokens:continue
            value=int(tokens[0])
            return value//1024 if platform.system()=='Darwin' else value
    return None

def compile_case(name,source,extra=()):
    out=BUILD/f'cbench_{name}.o'; walls,rsss=[],[]
    for _ in range(3):
        started=time.perf_counter()
        r=subprocess.run(['/usr/bin/time',TIME_RSS_FLAG,CXX,*BASE_FLAGS,*extra,'-c',str(ROOT/source),'-o',str(out)],capture_output=True,text=True)
        walls.append(time.perf_counter()-started)
        if r.returncode: return {'name':name,'failed':r.stderr[-400:]}
        if (rss:=_time_rss_kib(r.stderr)) is not None:
            rsss.append(rss)
    return {'name':name,'wall_s_median':statistics.median(walls),
            'maxrss_kib':max(rsss),'object_bytes':out.stat().st_size}

def run_binary(name,args,parse):
    r=subprocess.run([str(BUILD/f'bench_{name}'),*args],capture_output=True,text=True,
                     env={**os.environ,'DYLD_LIBRARY_PATH':str(ROOT/'third_party/hlog/target/release')})
    if r.returncode: return {'name':name,'args':args,'failed':r.stderr[-400:]}
    return {'name':name,'args':args,**parse(r.stdout)}

def median_of(fields,text):
    rows=[line.split() for line in text.splitlines() if line.startswith('RESULT')]
    out={}
    for i,field in enumerate(fields):
        values=[float(row[3+i]) for row in rows[1:]]
        out[field]=statistics.median(values) if values else None
    return out

def main():
    BUILD.mkdir(parents=True,exist_ok=True)
    report={'compiler':subprocess.run([CXX,'--version'],capture_output=True,text=True).stdout.splitlines()[0],
            'flags':' '.join(BASE_FLAGS),'sections':{}}
    sec={}
    for name,source in [('dag_formula','benchmarks/compile.cpp'),
                        ('poe_formula','benchmarks/poe_layout.cpp'),
                        ('scale_formula','benchmarks/formula_scale.cpp')]:
        sec[name]=compile_case(name,source)
        print('compile',name,sec[name],flush=True)
    report['sections']['compile']=sec

    init_fields=['construct_ns','resolver_ns','finish_ns','describe_ns','wall_construct_ns','wall_total_ns']
    sec={'init_formula':run_binary('init_formula',['dag','6'],
        lambda t: median_of(init_fields,t)|{'maxrss_kib':max(float(l.split()[-1]) for l in t.splitlines() if l.startswith('RESULT'))//RSS_DIV})}
    report['sections']['init']=sec

    def mmio_parse(text):
        out={}
        for line in text.splitlines():
            if not line.startswith('RESULT'):continue
            parts=line.split(); key=' '.join(parts[1:7])
            # The benchmark already reports a sorted median in column 7.
            # Remaining columns are unsorted per-round samples and must not
            # be indexed as if they were a sorted distribution.
            out[key]=float(parts[7])
        return out
    combos=[]
    for callbacks in ('0','1'):
        for pattern in ('hot','random','mapped'):
            for writing in ('0','1'):
                combos.append(('4','1',callbacks,pattern,writing))
    sec={}
    for name in ['mmio_dynamic','mmio_formula']:
        sec[name]={}
        for combo in combos:
            r=run_binary(name,['3',*combo],mmio_parse)
            if 'failed' in r: sec[name]=r;break
            sec[name].update({k:v for k,v in r.items() if k not in ('name','args')})
        print('mmio',name,len(sec.get(name,{})),flush=True)
    report['sections']['mmio']=sec

    sec=run_binary('scale_formula',[],lambda t: {'stdout':t.strip()})
    report['sections']['scale']=sec
    (BUILD/'formula-bench.json').write_text(json.dumps(report,indent=1))

    md=['# Formula backend benchmark','','Compiler: '+report['compiler'],'',
        '## Compile (median of 3, compiler peak RSS)','',
        '| case | wall s | peak RSS MiB | object |','|---|---|---|---|']
    for name,v in report['sections']['compile'].items():
        if 'failed' in v: md.append(f'| {name} | FAILED | | |');continue
        md.append(f"| {name} | {v['wall_s_median']} | {v['maxrss_kib']/1024:.0f} | {v['object_bytes']/1024:.0f} KiB |")
    md+=['','## Init (DAG, median ns)','','| backend | construct | finish | total wall | peak RSS MiB |','|---|---|---|---|---|']
    for name,v in report['sections']['init'].items():
        if 'failed' in v: md.append(f'| {name} | FAILED | | | |');continue
        md.append(f"| {name} | {v['construct_ns']:.0f} | {v['finish_ns']:.0f} | {v['wall_total_ns']:.0f} | {v['maxrss_kib']/1024:.0f} |")
    md+=['','## MMIO hot path (median ns/access)','']
    keys=set()
    for v in report['sections']['mmio'].values():keys.update(k for k in v if k!='failed')
    md+=['| workload | dynamic | formula |','|---|---|---|']
    for k in sorted(keys):
        row=[k]
        for backend in ['mmio_dynamic','mmio_formula']:
            v=report['sections']['mmio'][backend]
            row.append(f"{v[k]:.1f}" if isinstance(v,dict) and k in v else '-')
        md.append('| '+' | '.join(row)+' |')
    md+=['','## Scale (8 VF, 262144 mapped u512 words)','', '```',
         str(report['sections']['scale'].get('stdout','FAILED')), '```']
    (BUILD/'formula-bench.md').write_text('\n'.join(md)+'\n')
    print('wrote',BUILD/'formula-bench.md')

if __name__=='__main__':main()
