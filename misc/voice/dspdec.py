import struct,sys,json,numpy as np,wave
sys.argv=['x'];exec(open('pvtool.py').read().split("if __name__")[0])
def decode_fwav(fw):
    s=sections(fw);ib=s[0x7000][0]+8;db=s[0x7001][0]+8
    enc,loop=fw[ib],fw[ib+1];rate=u32(fw,ib+4);nsamp=u32(fw,ib+12)
    ct=ib+0x14;cnt=u32(fw,ct);ch=ct+s32(fw,ct+8)
    soff=s32(fw,ch+4);aoff=s32(fw,ch+12)
    data=fw[db+soff:]
    if enc==1: return rate,np.frombuffer(data[:nsamp*2],'<i2')
    coef=struct.unpack_from('<16h',fw,ch+aoff)
    out=np.zeros(nsamp,np.int16);h1=h2=0;i=0;p=0
    while i<nsamp:
        ps=data[p];p+=1;sc=1<<(ps&15);ci=ps>>4;c1,c2=coef[ci*2],coef[ci*2+1]
        for k in range(14):
            if i>=nsamp:break
            b=data[p+k//2];n=(b>>4) if k%2==0 else (b&15)
            if n>=8:n-=16
            v=((n*sc)<<11)+1024+c1*h1+c2*h2;v>>=11
            v=max(-32768,min(32767,v));out[i]=v;h2=h1;h1=v;i+=1
        p+=7
    return rate,out
if __name__=='__main__':
    w=open('/mnt/user-data/uploads/Game-files/mario_pv.bfwar','rb').read()
    names=json.load(open('pvnames.json'))
    n=u32(w,sections(w)[0x6800][0]+8)
    for i in range(n):
        fw=war_wave(w,i)
        if not fw: continue
        r,pcm=decode_fwav(fw)
        f=wave.open('mario_wav/%03d_%s.wav'%(i,names[str(i)]),'wb');f.setnchannels(1);f.setsampwidth(2);f.setframerate(r);f.writeframes(pcm.tobytes());f.close()
    print('ok')
