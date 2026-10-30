import sys,struct,capstone
from capstone import *
B=int(sys.argv[4],16) if len(sys.argv)>4 else 0
d=open(sys.argv[1],'rb').read()
md=Cs(CS_ARCH_ARM,CS_MODE_THUMB); md.detail=True
def w(a): o=a-B; return struct.unpack('<I',d[o:o+4])[0] if 0<=o<len(d)-3 else None
def dis(a,e):
    out=[]
    while a<e:
        ins=list(md.disasm(d[a-B:a-B+4],a,1))
        if not ins: out.append(f"{a:08x}: .short {struct.unpack('<H',d[a-B:a-B+2])[0]:04x}"); a+=2; continue
        i=ins[0]; s=f"{i.address:08x}: {i.mnemonic:8s} {i.op_str}"
        if i.mnemonic.startswith('ldr') and '[pc' in i.op_str:
            try:
                imm=i.operands[1].mem.disp; t=((i.address+4)&~3)+imm; s+=f"   ; ={w(t):#010x} @{t:08x}"
            except Exception: pass
        out.append(s); a+=i.size
    return out
if __name__=='__main__':
    for l in dis(int(sys.argv[2],16),int(sys.argv[3],16)): print(l)
