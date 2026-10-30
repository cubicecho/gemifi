import sys,struct
d=open(sys.argv[1],'rb').read()
i=0
while True:
    i=d.find(b'MSCF\0\0\0\0',i)
    if i<0: break
    size=struct.unpack_from('<I',d,i+8)[0]
    coff=struct.unpack_from('<I',d,i+16)[0]
    nf,nfi=struct.unpack_from('<HH',d,i+26)
    flags=struct.unpack_from('<H',d,i+30)[0]
    hdr=36
    if flags&4:
        cbres,=struct.unpack_from('<H',d,i+36);hdr+=4+cbres
    fo=i+hdr; comps=[]
    for k in range(nf):
        off,nb,ct=struct.unpack_from('<IHH',d,fo); comps.append(ct&0xf); fo+= 8
    print(f'CAB @0x{i:x} size={size} folders={nf} files={nfi} comp={comps}')
    p=i+coff
    for k in range(nfi):
        usz,uoff,ifo,dt,tm,at=struct.unpack_from('<IIHHHH',d,p); e=d.index(b'\0',p+16)
        print('   ',usz,d[p+16:e].decode('latin1')); p=e+1
    i+=8
