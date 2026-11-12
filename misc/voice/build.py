import json,struct,numpy as np,wave,glob,os
mapping={int(k):v for k,v in json.load(open('mapping.json')).items()}
def rd(p):
    f=wave.open(p);a=np.frombuffer(f.readframes(f.getnframes()),'<i2').astype(np.float32);r=f.getframerate()
    if f.getnchannels()>1:a=a.reshape(-1,f.getnchannels()).mean(1)
    return r,a
def rms(a):return float(np.sqrt(np.mean(a**2))+1e-9)
mario={int(os.path.basename(p)[:3]):p for p in glob.glob('mario_wav/*.wav')}
lpath={os.path.basename(p):p for p in glob.glob('luigi_zip/*/*.wav')}
def fwav(rate,pcm):
    data=pcm.astype('<i2').tobytes()
    info=struct.pack('<BBHIIII',1,0,0,rate,0,len(pcm),0)
    info+=struct.pack('<I',1)+struct.pack('<HHi',0x7100,0,0xc)
    info+=struct.pack('<HHi',0x1f00,0,0x18)+struct.pack('<HHi',0,0,-1)+struct.pack('<I',0)
    info=b'INFO'+struct.pack('<I',0x60)+info; info+=b'\0'*(0x60-len(info))
    dat=b'DATA'+struct.pack('<I',8+0x18+len(data))+b'\0'*0x18+data
    dat+=b'\0'*((-len(dat))%0x20)
    total=0x40+len(info)+len(dat)
    h=b'FWAV'+struct.pack('<HHIIHH',0xfeff,0x40,0x00010200,total,2,0)
    h+=struct.pack('<HHII',0x7000,0,0x40,len(info))+struct.pack('<HHII',0x7001,0,0x40+len(info),len(dat))
    h+=b'\0'*(0x40-len(h))
    return h+info+dat
waves={};cache={}
def src(v,slot):
    if isinstance(v,list):  # ['SNORE', clip1, clip2]: alternate with gaps up to Mario's length
        _,m=rd(mario[slot]); target=len(m)/48000.0
        clips=[rd(x) for x in v[1:]]; r=max(c[0] for c in clips); parts=[];t=0.0;i=0
        while t<target or i<2:
            cr,ca=clips[i%len(clips)]
            ca=np.interp(np.arange(0,len(ca),cr/r),np.arange(len(ca)),ca) if cr!=r else ca
            parts+= [ca,np.zeros(int(r*0.45),np.float32)]; t+=len(ca)/r+0.45; i+=1
            if i>40: break
        return r,np.concatenate(parts)
    return rd(v if os.path.exists(v) else lpath[v])
for slot,lk in mapping.items():
    r,a=src(lk,slot)
    nz=np.nonzero(np.abs(a)>300)[0]
    if len(nz):a=a[max(0,nz[0]-int(r*0.005)):]
    _,m=rd(mario[slot])
    g=rms(m)/rms(a); a=a*g
    pk=np.abs(a).max()
    if pk>31000:a*=31000/pk
    key=(json.dumps(lk),round(g,2))
    if key not in cache: cache[key]=fwav(r,np.round(a).astype(np.int16))
    waves[slot]=key
N=126
order=list(dict.fromkeys(waves.values()))
body=b'';offs={}
for k in order:
    body+=b'\0'*((-len(body))%0x40)
    offs[k]=len(body);body+=cache[k]
infosz=(8+4+12*N+0x1f)&~0x1f
PRE=(-(0x40+infosz+8))%0x40
info=b'INFO'+struct.pack('<II',infosz,N)
for i in range(N):
    if i in waves:k=waves[i];info+=struct.pack('<HHiI',0x1f00,0,offs[k]+PRE,len(cache[k]))
    else: info+=struct.pack('<HHiI',0,0,-1,0)
info+=b'\0'*(infosz-len(info))
fileblk=b'FILE'+struct.pack('<I',8+PRE+len(body))+b'\0'*PRE+body
total=0x40+len(info)+len(fileblk)
h=b'FWAR'+struct.pack('<HHIIHH',0xfeff,0x40,0x10000,total,2,0)
h+=struct.pack('<HHII',0x6800,0,0x40,len(info))+struct.pack('<HHII',0x6801,0,0x40+len(info),len(fileblk))
h+=b'\0'*(0x40-len(h))
out=h+info+fileblk
open('/mnt/user-data/outputs/deploy/P2LuigiPv.bfwar','wb').write(out)
print('slots',len(waves),'unique',len(order),'bytes',len(out), 'filebody abs', hex(0x40+len(info)+8))
