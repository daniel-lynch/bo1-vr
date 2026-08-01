import struct,bisect,pickle,os,sys
from pe import *
from cx import calls
CACHE='funcs.pkl'
if os.path.exists(CACHE):
    starts=pickle.load(open(CACHE,'rb'))
else:
    s=TEXT; blob=data[s['rawptr']:s['rawptr']+s['rawsz']]; base=s['va']
    st=set(calls.keys())
    # also after int3 padding runs
    i=0
    while i<len(blob)-1:
        if blob[i]==0xcc:
            j=i
            while j<len(blob) and blob[j]==0xcc: j+=1
            if j-i>=2 and j<len(blob): st.add(base+j)
            i=j
        else: i+=1
    starts=sorted(st)
    pickle.dump(starts,open(CACHE,'wb'),2)
def fstart(va):
    i=bisect.bisect_right(starts,va)-1
    return starts[i] if i>=0 else None
if __name__=='__main__':
    for a in sys.argv[1:]:
        v=int(a,16); print("%08x -> func %08x"%(v,fstart(v)))
