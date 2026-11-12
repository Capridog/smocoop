# Usage: pvtool.py <sarc-or-aras containing a .bfgrp> <out.bfwar> [<out.map>]
import struct,sys
def u16(d,o):return struct.unpack_from('<H',d,o)[0]
def u32(d,o):return struct.unpack_from('<I',d,o)[0]
def s32(d,o):return struct.unpack_from('<i',d,o)[0]
def sarc_files(f,base):
    f.seek(base);h=f.read(0x20);doff=u32(h,12)
    f.seek(base+0x14);sf=f.read(8);n=u16(sf,6)
    f.seek(base+0x20);tab=f.read(16*n+8+0x4000)
    sfnt=16*n+8;out={}
    for i in range(n):
        hh,a,s,e=struct.unpack_from('<IIII',tab,16*i)
        nm=tab[sfnt+(a&0xffffff)*4:].split(b'\0')[0].decode() if a else str(i)
        out[nm]=(base+doff+s,e-s)
    return out
def sections(d):
    n=u16(d,0x10);r={}
    for i in range(n):
        t,o,sz=u16(d,0x14+12*i),u32(d,0x14+12*i+4),u32(d,0x14+12*i+8);r[t]=(o,sz)
    return r
def find_grp(path):
    f=open(path,'rb')
    fs=sarc_files(f,0)
    for nm,(o,sz) in fs.items():
        if nm.endswith('.aras'):
            fs=sarc_files(f,o);break
    for nm,(o,sz) in fs.items():
        if nm.endswith('.bfgrp'):
            f.seek(o);return f.read(sz)
    raise SystemExit('no bfgrp')
def grp_files(g):
    sec=sections(g);io=sec[0x7800][0]+8;fo=sec[0x7801][0]+8
    n=u32(g,io);files={}
    for i in range(n):
        e=io+s32(g,io+4+8*i+4)
        fid=u32(g,e);typ=u16(g,e+4);off=s32(g,e+8);sz=u32(g,e+12)
        if off!=-1 and sz: files[fid]=g[fo+off:fo+off+sz]
    return files
def wsd_waves(w):
    sec=sections(w);ib=sec[0x6800][0]+8
    wit=ib+s32(w,ib+4);wst=ib+s32(w,ib+12)
    nwid=u32(w,wit);wids=[(u32(w,wit+4+8*i),u32(w,wit+8+8*i)) for i in range(nwid)]
    n=u32(w,wst);res=[]
    for i in range(n):
        d=wst+s32(w,wst+4+8*i+4)
        nt=d+s32(w,d+20)   # 3rd ref: note info ref table
        cnt=u32(w,nt)
        if cnt==0: res.append(None);continue
        ni=nt+s32(w,nt+8)
        res.append(wids[u32(w,ni)])
    return res,wids
def war_wave(war,idx):
    sec=sections(war);ib=sec[0x6800][0]+8;fb=sec[0x6801][0]
    off=u32(war,ib+4+12*idx+4);sz=u32(war,ib+4+12*idx+8)
    return war[fb+8+off:fb+8+off+sz]
if __name__=='__main__':
    g=find_grp(sys.argv[1]);files=grp_files(g)
    print('group files',len(files),sorted(files)[:10],'...')
    wsd=files.get(101)
    if wsd is None: raise SystemExit('file 101 not in group')
    print('wsd101',wsd[:4],len(wsd))
    res,wids=wsd_waves(wsd)
    print('wave sounds',len(res),'waveids',len(wids),'wars',sorted(set(a for a,b in wids)))
    warfile={0x05000000:147,0x05000001:148}
    waves=[]
    for r in res:
        if r is None: waves.append(b'');continue
        war=files[warfile[r[0]]];waves.append(war_wave(war,r[1]))
    print('wave bytes',sum(map(len,waves)),[w[:4] for w in waves[:3]])
    # build FWAR
    infosz=8+4+12*len(waves);infosz=(infosz+0x1f)&~0x1f
    body=b'';ents=[]
    for w in waves:
        pad=(-(8+len(body)))%0x40;body+=b'\0'*pad
        ents.append((len(body)+(0),len(w)));body+=w
    # offsets in GetWaveFile are rel to FILE block +8
    info=b'INFO'+struct.pack('<I',infosz)+struct.pack('<I',len(waves))
    for o,s in ents: info+=struct.pack('<HHiI',0x1f00 if s else 0,0,o if s else -1,s)
    info+=b'\0'*(infosz-len(info))
    hdr=0x40;fileoff=hdr+infosz
    fileblk=b'FILE'+struct.pack('<I',8+len(body))+body
    pre=(-(fileoff+8))%0x40  # align wave data start
    fileblk=b'FILE'+struct.pack('<I',8+pre+len(body))+b'\0'*pre+body
    ents2=[(o+pre,s) for o,s in ents]
    info=b'INFO'+struct.pack('<I',infosz)+struct.pack('<I',len(waves))
    for o,s in ents2: info+=struct.pack('<HHiI',0x1f00 if s else 0,0,o if s else -1,s)
    info+=b'\0'*(infosz-len(info))
    total=hdr+len(info)+len(fileblk)
    h=b'FWAR'+struct.pack('<HHIIH H',0xfeff,0x40,0x10000,total,2,0)
    h+=struct.pack('<HHII',0x6800,0,hdr,len(info))+struct.pack('<HHII',0x6801,0,hdr+len(info),len(fileblk))
    h+=b'\0'*(0x40-len(h))
    open(sys.argv[2],'wb').write(h+info+fileblk)
    print('wrote',total)
    if len(sys.argv)>3:
        open(sys.argv[3],'w').write('\n'.join('%d %s'%(i,r) for i,r in enumerate(res)))
        print('wsd101 size',len(wsd),'count',len(res))
