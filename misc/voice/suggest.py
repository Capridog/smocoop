import json,glob,os,numpy as np,librosa,re
exec(open('unique.py').read().split("names=json.load")[0])   # G, W, fixed (Galaxy counterpart per move)
gal_for={s:v for s,v in fixed.items() if isinstance(v,str) and v.startswith('galaxy/')}
names=json.load(open('pvnames.json'))
cur={int(k):v for k,v in json.load(open('mapping.json')).items()}
def feat(p):
    y,sr=librosa.load(p,sr=16000); y,_=librosa.effects.trim(y,top_db=35)
    if len(y)<1024: y=np.pad(y,(0,1024-len(y)))
    m=librosa.feature.mfcc(y=y,sr=sr,n_mfcc=20,hop_length=160,n_fft=512)[1:]
    m=(m-m.mean(1,keepdims=True))/(m.std(1,keepdims=True)+1e-6)
    return np.nan_to_num(m)+1e-3*np.random.RandomState(0).randn(*m.shape),len(y)/sr
def cost(a,b):
    D,wp=librosa.sequence.dtw(a[0],b[0],metric='cosine'); return D[-1,-1]/len(wp)+0.6*abs(np.log((b[1]+.05)/(a[1]+.05)))
skip=('FILE_SEL','MODE_APPEAR','THANK_YOU','No Label')
pool={}
for k,p in W.items(): pool['w:'+k]=p
for k,p in G.items():
    if not any(x in p for x in skip): pool['g:'+k]=p
for p in glob.glob('lm/Luigi/*American*/*.wav'): pool['m:'+os.path.basename(p)[:-4]]=p
PF={k:feat(p) for k,p in pool.items()}
mario={int(os.path.basename(p)[:3]):p for p in glob.glob('mario_wav/*.wav')}
sug={}
for s in sorted(cur):
    mf=feat(mario[s]); gf=feat(gal_for[s]) if s in gal_for else None
    sc={}
    for k,f in PF.items():
        x=cost(mf,f)
        if gf is not None: x=0.5*x+cost(gf,f)
        sc[k]=x
    rank=sorted(sc,key=sc.get)
    w=[k for k in rank if k.startswith('w:')][:5]; g=[k for k in rank if k.startswith('g:')][:2]; m=[k for k in rank if k.startswith('m:')][:1]
    sug[s]=w+g+m
json.dump(sug,open('suggest.json','w'))
print(len(sug))
