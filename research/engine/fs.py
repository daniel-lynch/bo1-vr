import sys,bisect,struct
from capstone import Cs,CS_ARCH_X86,CS_MODE_32
from capstone.x86 import *
from pe import *
from fn import starts,fstart
md=Cs(CS_ARCH_X86,CS_MODE_32); md.detail=True
strs={}
for line in open('strings.txt'):
    va,sec,t=line.rstrip('\n').split('\t',2); strs[int(va,16)]=t
NAMES={0x5bd300:'AngleVectors',0x4625c0:'FireBulletCore',0x651a30:'nop_stub',0x462860:'Scr_GetVector',
 0x5c6da0:'Scr_GetNumParam',0x644900:'Scr_Error',0x642f10:'Scr_GetEntity',0x5c25c0:'BG_GetWeaponDef'}
def summarize(fva,maxb=0x2000):
    i=bisect.bisect_right(starts,fva)
    end=starts[i] if i<len(starts) else fva+maxb
    end=min(end,fva+maxb)
    o=va2off(fva); buf=data[o:o+(end-fva)]
    lines=[]
    for ins in md.disasm(buf,fva):
        note=''
        for op in ins.operands:
            if op.type==X86_OP_IMM and op.imm in strs: note+=' ;"%s"'%strs[op.imm][:60]
            if op.type==X86_OP_MEM and op.mem.base==0 and op.mem.index==0 and (op.mem.disp&0xffffffff) in strs:
                note+=' ;->"%s"'%strs[op.mem.disp&0xffffffff][:60]
        if ins.mnemonic=='call':
            for op in ins.operands:
                if op.type==X86_OP_IMM and op.imm in NAMES: note+=' ;;%s'%NAMES[op.imm]
        lines.append("%08x  %-34s%s"%(ins.address,ins.mnemonic+' '+ins.op_str,note))
    return end,lines
if __name__=='__main__':
    f=int(sys.argv[1],16)
    filt=sys.argv[2] if len(sys.argv)>2 else None
    end,L=summarize(f)
    print("### func %08x .. %08x"%(f,end))
    for l in L:
        if filt is None or filt in l or 'call' in l: print(l)
