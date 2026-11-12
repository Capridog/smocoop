import librosa,numpy as np,glob,os,json
def feat(p):
    y,sr=librosa.load(p,sr=16000); y,_=librosa.effects.trim(y,top_db=35)
    m=librosa.feature.mfcc(y=y,sr=sr,n_mfcc=20,hop_length=160,n_fft=512)[1:]
    m=(m-m.mean(1,keepdims=True))/(m.std(1,keepdims=True)+1e-6)
    return np.nan_to_num(m)+1e-3*np.random.RandomState(0).randn(*m.shape),len(y)/sr
O={os.path.basename(p)[:-4]:feat(p) for p in sorted(glob.glob('mario_wav/*.wav'))}
W={os.path.basename(p).split('_')[-1][:-4]:feat(p) for p in sorted(glob.glob('mario3dw/*/*.wav'))}
res={}
for ok,(om,od) in O.items():
    sc=[]
    for wk,(wm,wd) in W.items():
        D,wp=librosa.sequence.dtw(om,wm,metric='cosine');sc.append((D[-1,-1]/len(wp)+0.25*abs(np.log((wd+.05)/(od+.05))),wk,round(wd,2)))
    sc.sort();res[ok]=sc[:3]
    print(ok,round(od,2),' | '.join('%s %.2f %.3f'%(b,c,a) for a,b,c in sc[:3]),flush=True)
json.dump(res,open('o2w.json','w'))
