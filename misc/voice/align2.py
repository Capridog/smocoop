import librosa,numpy as np,glob,os,pickle
def feat(p):
    y,sr=librosa.load(p,sr=16000); y,_=librosa.effects.trim(y,top_db=35)
    if len(y)<2048: y=np.pad(y,(0,2048-len(y)))
    f0,vf,vp=librosa.pyin(y,fmin=80,fmax=1000,sr=sr,frame_length=1024,hop_length=160)
    rms=librosa.feature.rms(y=y,frame_length=1024,hop_length=160)[0]
    db=librosa.amplitude_to_db(rms+1e-6,ref=np.max)
    st=12*np.log2(np.where(np.isnan(f0),np.nan,f0))
    med=np.nanmedian(st) if np.any(~np.isnan(st)) else 0
    st=np.nan_to_num(st-med,nan=0.0)
    n=min(len(st),len(db))
    F=np.vstack([st[:n]/4,db[:n]/12,vp[:n]*1.0])
    return F,len(y)/sr
def load(pat):
    return {int(os.path.basename(p).split('_')[-1][:-4],16):feat(p) for p in glob.glob(pat)}
M=load('mario3dw/*/SeResourceStd2nd_*.wav'); L=load('luigi_zip/*/SeResourceStd2nd_*.wav')
def cost(a,b):
    D,wp=librosa.sequence.dtw(a[0],b[0],metric='euclidean')
    return D[-1,-1]/len(wp)+0.5*abs(np.log((a[1]+.05)/(b[1]+.05)))
out={}
for name,mk,lk in (('main',[k for k in sorted(M) if k<0xA00],[k for k in sorted(L) if k<0xA00]),
                   ('A',[k for k in sorted(M) if k>=0xA00],[k for k in sorted(L) if k>=0xA00])):
    C=np.array([[cost(M[a],L[b]) for b in lk] for a in mk]);out[name]=(mk,lk,C);print(name,flush=True)
pickle.dump(out,open('align_cost2.pkl','wb'))
