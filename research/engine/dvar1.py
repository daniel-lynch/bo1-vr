import sys,pickle,struct,re
from capstone import *
from capstone.x86 import *
from pe import *
md=Cs(CS_ARCH_X86,CS_MODE_32); md.detail=True
idx=pickle.load(open('xref.pkl','rb'))
strs={}
for line in open('strings.txt'):
    va,sec,t=line.rstrip('\n').split('\t',2); strs.setdefault(t,[]).append(int(va,16))
name=sys.argv[1]
for sva in strs.get(name,[]):
    for site in idx.get(sva,[]):
        if secof(site)!='.text': continue
        o=va2off(site-1)
        ins=list(md.disasm(data[o:o+120],site-1))
        # find mov [imm32],eax within next few insns after a call
        out=[]
        for i in ins[:20]:
            out.append('%08x %s %s'%(i.address,i.mnemonic,i.op_str))
        print('--- str %08x ref@%08x'%(sva,site))
        print('\n'.join(out[:14]))
