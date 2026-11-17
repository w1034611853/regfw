import bisect
from collections import Counter
from pathlib import Path
import subprocess
import sys

binary, samples = sys.argv[1:]
symbols=[]
for line in subprocess.check_output(['nm','-n','-S','--defined-only','-C',binary], text=True).splitlines():
    parts=line.split(maxsplit=3)
    if len(parts)==4 and parts[2] in ('t','T','W','w'):
        symbols.append((int(parts[0],16),int(parts[1],16),parts[3]))
symbols.sort()
starts=[s[0] for s in symbols]
counts=Counter()
ips=Counter()
for line in Path(samples).read_text().splitlines():
    address, count=line.split()
    address, count=int(address,16),int(count)
    index=bisect.bisect_right(starts,address)-1
    if index>=0 and address<symbols[index][0]+symbols[index][1]:
        start,size,name=symbols[index]
        counts[name]+=count
        ips[(name,address-start,address)]+=count
    else:
        counts['[shared libraries or unresolved]']+=count
total=counts.total()
print('Samples:',total)
for name,count in counts.most_common(25):
    print(f'{count:6d} {100*count/total:6.2f}% {name[:240]}')
print('Top instructions')
for (name,offset,address),count in ips.most_common(35):
    print(f'{count:6d} {address:x} +0x{offset:x} {name[:140]}')
