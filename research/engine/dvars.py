import struct,re,pickle
from capstone import *
from capstone.x86 import *
from pe import *
from fn import starts
md=Cs(CS_ARCH_X86,CS_MODE_32); md.detail=True
def gs(va):
    o=va2off(va)
    if o is None: return None
    e=data.find(b'\0',o,o+120)
    if e<0: return None
    try: t=data[o:e].decode('ascii')
    except: return None
    return t
# find candidate Dvar_Register funcs: called with a string arg that looks like a dvar name, result stored to .data
from cx import calls
# scan all of .text linearly, track recent pushes of string ptrs, and 'call X ; mov [imm32],eax'
s=TEXT; blob=data[s['rawptr']:s['rawptr']+s['rawsz']]
res=[]
recent=[]
prev=None
for ins in md.disasm(blob,s['va']):
    if ins.mnemonic=='push' and ins.operands and ins.operands[0].type==X86_OP_IMM:
        v=ins.operands[0].imm
        t=gs(v)
        if t and re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]{2,60}',t): recent.append((ins.address,t))
        if len(recent)>12: recent.pop(0)
    if ins.mnemonic=='call' and ins.operands and ins.operands[0].type==X86_OP_IMM:
        prev=(ins.address,ins.operands[0].imm,list(recent)); recent=[]
    elif ins.mnemonic=='mov' and prev and ins.address-prev[0]<=8:
        ops=ins.operands
        if len(ops)==2 and ops[0].type==X86_OP_MEM and ops[0].mem.base==0 and ops[0].mem.index==0 \
           and ops[1].type==X86_OP_REG and ins.reg_name(ops[1].reg)=='eax':
            g=ops[0].mem.disp&0xffffffff
            if 0xb6c000<=g<0x4660000 and prev[2]:
                res.append((g,prev[2][0][1],prev[1],prev[0]))
        prev=None
    elif ins.mnemonic not in ('nop',): prev=prev if (prev and ins.address-prev[0]<=8) else None
pickle.dump(res,open('dvars.pkl','wb'),2)
byg={}
for g,name,fn,site in res: byg.setdefault(g,[]).append((name,fn,site))
with open('dvars.txt','w') as f:
    for g in sorted(byg):
        for name,fn,site in byg[g]:
            f.write("%08x  %-42s reg=%08x site=%08x\n"%(g,name,fn,site))
print("dvar globals",len(byg),"records",len(res))
