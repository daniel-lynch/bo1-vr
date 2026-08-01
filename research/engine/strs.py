import re,sys
from pe import *
out=[]
pat=re.compile(rb'[\x20-\x7e]{4,}')
for s in SECS:
    if s['rawsz']==0: continue
    blob=data[s['rawptr']:s['rawptr']+s['rawsz']]
    for m in pat.finditer(blob):
        va=s['va']+m.start()
        out.append((va,s['name'],m.group().decode('latin1')))
with open('strings.txt','w') as f:
    for va,sec,t in out:
        f.write("%08x\t%s\t%s\n"%(va,sec,t))
print(len(out))
