#!/usr/bin/env python3
"""Run one compiler in a fresh process: child peak RSS is measured independently."""
import argparse,json,resource,subprocess,time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--record',required=True);p.add_argument('command',nargs=argparse.REMAINDER);a=p.parse_args();cmd=a.command
if cmd and cmd[0]=='--':cmd=cmd[1:]
t=time.monotonic();r=subprocess.run(cmd,capture_output=True,text=True);seconds=time.monotonic()-t
record={'command':cmd,'exit_code':r.returncode,'wall_seconds':seconds,'peak_rss_kib':resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss}
Path(a.record+'.log').write_text(r.stdout+r.stderr);Path(a.record+'.json').write_text(json.dumps(record,indent=2));print(json.dumps(record),flush=True)

raise SystemExit(r.returncode)
