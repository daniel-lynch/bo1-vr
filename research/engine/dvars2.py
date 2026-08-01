import struct,re,pickle
from capstone import *
from capstone.x86 import *
from pe import *
md=Cs(CS_ARCH_X86,CS_MODE_32); md.detail=True
def gs(va):
    o=va2off(va)
    if o is None: return None
    e=data.find(b'\0',o,o+120)
    if e<0: return None
    try: return data[o:e].decode('ascii')
    except: return None
s=TEXT
ins=list(md.disasm(data[s['rawptr']:s['rawptr']+s['rawsz']],s['va']))
print("insns",len(ins))
recs=[]
for i,x in enumerate(ins):
    if x.mnemonic!='mov': continue
    ops=x.operands
    if not(len(ops)==2 and ops[0].type==X86_OP_MEM and ops[0].mem.base==0 and ops[0].mem.index==0
           and ops[1].type==X86_OP_REG and x.reg_name(ops[1].reg)=='eax'): continue
    g=ops[0].mem.disp&0xffffffff
    if not (0xb6c000<=g<0x4660000): continue
    # preceding call within 3 insns
    ci=None
    for j in range(i-1,max(-1,i-4),-1):
        if ins[j].mnemonic=='call': ci=j; break
        if ins[j].mnemonic in ('ret','jmp'): break
    if ci is None: continue
    tgt=ins[ci].operands[0].imm if ins[ci].operands and ins[ci].operands[0].type==X86_OP_IMM else None
    # last push of a string ptr before the call
    nm=None
    for j in range(ci-1,max(-1,ci-30),-1):
        y=ins[j]
        if y.mnemonic=='push' and y.operands and y.operands[0].type==X86_OP_IMM:
            t=gs(y.operands[0].imm)
            if t and re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]{2,60}',t): nm=t; break
        if y.mnemonic=='call': break
    if nm: recs.append((g,nm,tgt,x.address))
pickle.dump(recs,open('dvars.pkl','wb'),2)
with open('dvars.txt','w') as f:
    for g,nm,t,a in sorted(recs):
        f.write("%08x  %-44s reg=%s site=%08x\n"%(g,nm,hex(t) if t else '?',a))
from collections import Counter
print("records",len(recs),"unique globals",len(set(r[0] for r in recs)))
print("top reg funcs",Counter(r[2] for r in recs).most_common(8))
