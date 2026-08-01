import re,struct,pickle
from pe import *
strs={}
for line in open('strings.txt'):
    va,sec,t=line.rstrip('\n').split('\t',2)
    strs[int(va,16)]=(sec,t)
TEXTLO=0x401000; TEXTHI=0x401000+0x5a1a10
# scan .rdata/.data for pattern: [ptr->lowercase string][ptr into .text]
cands=[]
for s in SECS:
    if s['name'] not in ('.rdata','.data'): continue
    blob=data[s['rawptr']:s['rawptr']+s['rawsz']]
    for i in range(0,len(blob)-8,4):
        a,b=struct.unpack_from('<II',blob,i)
        if a in strs and TEXTLO<=b<TEXTHI:
            sec,t=strs[a]
            if re.fullmatch(r'[a-z][a-z0-9_]{2,}',t):
                cands.append((s['va']+i,t,b))
print("cands",len(cands))
# cluster contiguous by stride
cands.sort()
clusters=[];cur=[cands[0]]
for c in cands[1:]:
    if c[0]-cur[-1][0]<=32: cur.append(c)
    else:
        if len(cur)>=5: clusters.append(cur)
        cur=[c]
if len(cur)>=5: clusters.append(cur)
for cl in clusters:
    print("=== TABLE at %#x  count=%d stride~%d"%(cl[0][0],len(cl),(cl[1][0]-cl[0][0]) if len(cl)>1 else 0))
pickle.dump(clusters,open('bi.pkl','wb'),2)
