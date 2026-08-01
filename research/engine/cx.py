import struct,sys,pickle,os
from pe import *
CACHE='calls.pkl'
if os.path.exists(CACHE):
    calls=pickle.load(open(CACHE,'rb'))
else:
    calls={}
    s=TEXT; blob=data[s['rawptr']:s['rawptr']+s['rawsz']]; base=s['va']
    for i in range(len(blob)-4):
        b=blob[i]
        if b in (0xe8,0xe9):
            rel=struct.unpack_from('<i',blob,i+1)[0]
            tgt=(base+i+5+rel)&0xffffffff
            if s['va']<=tgt<s['va']+s['vsz']:
                calls.setdefault(tgt,[]).append((base+i,'call' if b==0xe8 else 'jmp'))
    pickle.dump(calls,open(CACHE,'wb'),2)
if __name__=='__main__':
    t=int(sys.argv[1],16)
    for a,k in calls.get(t,[]): print("%08x %s"%(a,k))
    print("total",len(calls.get(t,[])))
