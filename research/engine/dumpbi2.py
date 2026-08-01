import struct,re
from pe import *
TEXTLO,TEXTHI=0x401000,0x401000+0x5a1a10
rx=re.compile(r'[a-z][a-z0-9_]{1,60}$')
def getstr(va):
    o=va2off(va)
    if o is None: return None
    e=data.find(b'\0',o,o+80)
    if e<0: return None
    try: t=data[o:e].decode('ascii')
    except: return None
    return t
def isent(va):
    d=read(va,12)
    if not d or len(d)<12: return None
    a,b,c=struct.unpack('<III',d)
    if not (TEXTLO<=b<TEXTHI): return None
    if c>1: return None
    t=getstr(a)
    if not t or not rx.match(t): return None
    return (t,b,c)
# find all entries then group into runs
ents={}
for s in SECS:
    if s['name'] not in ('.rdata','.data'): continue
    for va in range(s['va'],s['va']+s['rawsz']-12,4):
        e=isent(va)
        if e: ents[va]=e
vas=sorted(ents)
runs=[];cur=[vas[0]]
for v in vas[1:]:
    if v-cur[-1]==12: cur.append(v)
    else:
        if len(cur)>=8: runs.append(cur)
        cur=[v]
if len(cur)>=8: runs.append(cur)
tot=0
with open('gsc_builtins.txt','w') as f:
    for r in runs:
        f.write("### TABLE %08x n=%d\n"%(r[0],len(r)))
        for v in r:
            t,fn,dev=ents[v]; f.write("%08x %-44s fn=%08x dev=%d\n"%(v,t,fn,dev)); tot+=1
print("runs",len(runs),"entries",tot)
