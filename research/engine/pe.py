import struct, sys, re, bisect, os
PATH=os.environ.get("BO1_EXE") or next((p for p in [
    os.path.expanduser("~/.local/share/Steam/steamapps/common/Call of Duty Black Ops/BlackOps.exe"),
    "/mnt/games/steam/steamapps/common/Call of Duty Black Ops/BlackOps.exe",
] if os.path.exists(p)), None)
if not PATH: sys.exit("BlackOps.exe not found; set BO1_EXE=/path/to/BlackOps.exe")
data=open(PATH,'rb').read()
e_lfanew=struct.unpack_from('<I',data,0x3c)[0]
assert data[e_lfanew:e_lfanew+4]==b'PE\0\0'
coff=e_lfanew+4
machine,nsec,tds,ptrsym,nsym,optsz,chars=struct.unpack_from('<HHIIIHH',data,coff)
opt=coff+20
magic=struct.unpack_from('<H',data,opt)[0]
imagebase=struct.unpack_from('<I',data,opt+28)[0]
entry=struct.unpack_from('<I',data,opt+16)[0]
secoff=opt+optsz
SECS=[]
for i in range(nsec):
    o=secoff+i*40
    name=data[o:o+8].rstrip(b'\0').decode('latin1')
    vsz,va,rawsz,rawptr=struct.unpack_from('<IIII',data,o+8)
    scharacter=struct.unpack_from('<I',data,o+36)[0]
    SECS.append(dict(name=name,vsz=vsz,va=va+imagebase,rawsz=rawsz,rawptr=rawptr,ch=scharacter))
def va2off(va):
    for s in SECS:
        if s['va']<=va<s['va']+max(s['vsz'],s['rawsz']):
            d=va-s['va']
            if d<s['rawsz']: return s['rawptr']+d
    return None
def off2va(off):
    for s in SECS:
        if s['rawptr']<=off<s['rawptr']+s['rawsz']:
            return s['va']+(off-s['rawptr'])
    return None
def read(va,n):
    o=va2off(va)
    if o is None: return None
    return data[o:o+n]
def secof(va):
    for s in SECS:
        if s['va']<=va<s['va']+max(s['vsz'],s['rawsz']): return s['name']
    return None
TEXT=[s for s in SECS if s['ch']&0x20000000][0]
if __name__=='__main__':
    print("imagebase %#x entry %#x nsec %d"%(imagebase,imagebase+entry,nsec))
    for s in SECS: print("%-9s va=%#010x vsz=%#010x raw=%#010x rawsz=%#010x ch=%#010x"%(s['name'],s['va'],s['vsz'],s['rawptr'],s['rawsz'],s['ch']))
