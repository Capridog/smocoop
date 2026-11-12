import librosa,numpy as np,glob,os,json,pickle
def feat(p):
    y,sr=librosa.load(p,sr=16000); y,_=librosa.effects.trim(y,top_db=35)
    m=librosa.feature.mfcc(y=y,sr=sr,n_mfcc=20,hop_length=160,n_fft=512)[1:]
    m=(m-m.mean(1,keepdims=True))/(m.std(1,keepdims=True)+1e-6)
    return np.nan_to_num(m)+1e-3*np.random.RandomState(0).randn(*m.shape),len(y)/sr
def load(pat):
    d={}
    for p in glob.glob(pat):
        k=int(os.path.basename(p).split('_')[-1][:-4],16); d[k]=(p,)+feat(p)
    return d
M=load('mario3dw/*/SeResourceStd2nd_*.wav'); L=load('luigi_zip/*/SeResourceStd2nd_*.wav')
def cost(a,b):
    D,wp=librosa.sequence.dtw(a[1],b[1],metric='cosine')
    return D[-1,-1]/len(wp)+0.35*abs(np.log((a[2]+.05)/(b[2]+.05)))
out={}
for name,mk,lk in (('main',[k for k in sorted(M) if k<0xA00],[k for k in sorted(L) if k<0xA00]),
                   ('A',[k for k in sorted(M) if k>=0xA00],[k for k in sorted(L) if k>=0xA00])):
    C=np.array([[cost(M[a],L[b]) for b in lk] for a in mk])
    out[name]=(mk,lk,C)
    print(name,C.shape,flush=True)
pickle.dump(out,open('align_cost.pkl','wb'))
