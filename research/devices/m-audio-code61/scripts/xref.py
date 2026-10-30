import sys,struct
B=0x08002400
d=open(sys.argv[1],'rb').read()
targets=[int(x,16) for x in sys.argv[2:]]
# find literal pool slots containing value, and ldr-literal instructions pointing to those slots or the values
def h(o): return struct.unpack('<H',d[o:o+2])[0]
slots={}
for o in range(0,len(d)-3,2):
    v=struct.unpack('<I',d[o:o+4])[0]
    if v in targets: slots[B+o]=v
print('slots',{hex(k):hex(v) for k,v in slots.items()})
for o in range(0,len(d)-3,2):
    a=B+o; x=h(o)
    if (x&0xf800)==0x4800:
        t=((a+4)&~3)+(x&0xff)*4
        if t in slots: print(f"ldr16 {a:08x} r{(x>>8)&7} <- {t:08x} ({slots[t]:#x})")
    if x==0xf8df:
        y=h(o+2); t=((a+4)&~3)+(y&0xfff)
        if t in slots: print(f"ldr32 {a:08x} r{y>>12} <- {t:08x} ({slots[t]:#x})")
