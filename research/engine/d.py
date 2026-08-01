import sys,struct
from capstone import *
from capstone.x86 import *
from pe import *
md=Cs(CS_ARCH_X86,CS_MODE_32); md.detail=True
strs={}
for line in open('strings.txt'):
    va,sec,t=line.rstrip('\n').split('\t',2); strs[int(va,16)]=t
def ann(i):
    out=[]
    for op in i.operands:
        if op.type==X86_OP_IMM:
            v=op.imm
            if v in strs: out.append('"%s"'%strs[v][:70])
        elif op.type==X86_OP_MEM and op.mem.base==0 and op.mem.index==0:
            v=op.mem.disp & 0xffffffff
            if v in strs: out.append('->"%s"'%strs[v][:70])
    return ('  ; '+' '.join(out)) if out else ''
def dis(va,n=200,stop=True):
    o=va2off(va); buf=data[o:o+n*8]
    cnt=0
    for i in md.disasm(buf,va):
        print("%08x  %-22s %s%s"%(i.address,i.mnemonic+' '+i.op_str,'',ann(i)))
        cnt+=1
        if cnt>=n: break
        if stop and i.mnemonic in ('ret','retn') : print('---'); break
if __name__=='__main__':
    va=int(sys.argv[1],16); n=int(sys.argv[2]) if len(sys.argv)>2 else 200
    stop = (len(sys.argv)<4)
    dis(va,n,stop)
