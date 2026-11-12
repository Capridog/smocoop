import librosa,numpy as np,glob,os,json
def feat(p):
    y,sr=librosa.load(p,sr=16000)
    y,_=librosa.effects.trim(y,top_db=35)
    m=librosa.feature.mfcc(y=y,sr=sr,n_mfcc=20,hop_length=160,n_fft=512)[1:]
    m=(m-m.mean(1,keepdims=True))/(m.std(1,keepdims=True)+1e-6)
    f0=librosa.yin(y,fmin=70,fmax=900,sr=sr,frame_length=1024,hop_length=160) if len(y)>1024 else np.array([0.])
    m=np.nan_to_num(m)+1e-3*np.random.RandomState(0).randn(*m.shape)
    return m,len(y)/sr,y
M={os.path.basename(p):feat(p) for p in sorted(glob.glob('mario_wav/*.wav'))}
L={os.path.basename(p):feat(p) for p in sorted(glob.glob('luigi_zip/*/*.wav'))}
res={}
for mk,(mm,md,_) in M.items():
    sc=[]
    for lk,(lm,ld,_) in L.items():
        D,wp=librosa.sequence.dtw(mm,lm,metric='cosine')
        c=D[-1,-1]/len(wp)
        dr=abs(np.log((ld+.05)/(md+.05)))
        sc.append((c+0.25*dr,lk,round(ld,2)))
    sc.sort();res[mk]=sc[:5]
json.dump({k:[(round(a,3),b,c) for a,b,c in v] for k,v in res.items()},open('match.json','w'),indent=0)
for k,v in res.items(): print(k, round(M[k][1],2), ' | '.join('%s %.2f %.3f'%(b[-8:-4],c,a) for a,b,c in v[:3]))
