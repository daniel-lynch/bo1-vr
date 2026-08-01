import struct,pickle,sys
from pe import *
# index every 4-byte aligned-at-any-offset value in .text and .rdata that falls in image VA range
lo=0x400000; hi=0x400000+0x04680000
idx={}
for s in SECS:
    if s['rawsz']==0 or s['name'] not in ('.text','.rdata','.data'): continue
    blob=data[s['rawptr']:s['rawptr']+s['rawsz']]
    base=s['va']
    for i in range(len(blob)-3):
        v=blob[i]|(blob[i+1]<<8)|(blob[i+2]<<16)|(blob[i+3]<<24)
        if lo<v<hi:
            idx.setdefault(v,[]).append(base+i)
pickle.dump(idx,open('xref.pkl','wb'),2)
print("entries",len(idx))
