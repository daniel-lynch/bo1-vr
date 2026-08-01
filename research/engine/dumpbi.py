import struct,re
from pe import *
strs={}
for line in open('strings.txt'):
    va,sec,t=line.rstrip('\n').split('\t',2); strs[int(va,16)]=t
TEXTLO,TEXTHI=0x401000,0x401000+0x5a1a10
def walk(start,stride=12):
    out=[];va=start
    # walk backwards first
    while True:
        p=va-stride
        d=read(p,8)
        if not d: break
        a,b=struct.unpack('<II',d)
        if a in strs and TEXTLO<=b<TEXTHI and re.fullmatch(r'[a-z][a-z0-9_]{2,}',strs[a]): va=p
        else: break
    res=[]
    while True:
        d=read(va,stride)
        if not d or len(d)<8: break
        a,b=struct.unpack_from('<II',d,0)
        if a in strs and TEXTLO<=b<TEXTHI and re.fullmatch(r'[a-z][a-z0-9_]{2,}',strs[a]):
            extra=struct.unpack_from('<I',d,8)[0] if stride>=12 else 0
            res.append((va,strs[a],b,extra)); va+=stride
        else: break
    return res
tables=[0xa4e7b8,0xa52058,0xa521a8,0xa52a80,0xa52edc,0xa535c8,0xa54218,0xa542d8,0xa54d40,0xa551d8,0xa5552c,0xa55e40,0xa606e8,0xb71de0,0xb71fd8,0xb75ee0,0xb767f8,0xb76ef4,0xb84988]
seen=set(); allr=[]
with open('gsc_builtins.txt','w') as f:
    for t in tables:
        r=walk(t)
        if not r: continue
        key=r[0][0]
        if key in seen: continue
        seen.add(key)
        f.write("### TABLE %#x  n=%d\n"%(r[0][0],len(r)))
        for va,n,fn,ex in r:
            f.write("%08x %-40s fn=%08x %d\n"%(va,n,fn,ex)); allr.append(n)
print("total builtins",len(allr),"unique",len(set(allr)))
