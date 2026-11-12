import librosa,numpy as np,glob,os,json
exec(open('w3d.py').read().split("def cost")[0].split("def feat")[1].join(["def feat",""])) if False else None
def feat(p):
    y,sr=librosa.load(p,sr=16000); y,_=librosa.effects.trim(y,top_db=35)
    if len(y)<1024: y=np.pad(y,(0,1024-len(y)))
    m=librosa.feature.mfcc(y=y,sr=sr,n_mfcc=20,hop_length=160,n_fft=512)[1:]
    m=(m-m.mean(1,keepdims=True))/(m.std(1,keepdims=True)+1e-6)
    return np.nan_to_num(m)+1e-3*np.random.RandomState(0).randn(*m.shape),len(y)/sr
def cost(a,b):
    D,wp=librosa.sequence.dtw(a[0],b[0],metric='cosine'); return D[-1,-1]/len(wp)+0.3*abs(np.log((b[1]+.05)/(a[1]+.05)))
pool={}
for p in glob.glob('luigi_zip/*/*.wav'): pool['3DW '+os.path.basename(p).split('_')[-1][:-4].lstrip('0')]=feat(p)
for p in glob.glob('galaxy/Luigi/*/*.wav'): pool['GAL '+os.path.basename(p).split(' - ')[-1][:-4]]=feat(p)
for p in glob.glob('lm/Luigi/*American*/*.wav'): pool['LM '+os.path.basename(p)[:-4]]=feat(p)
res={}
for p in sorted(glob.glob('ox_wav/*.wav')):
    f=feat(p); sc=sorted((cost(f,v),k) for k,v in pool.items())[:2]
    res[os.path.basename(p)]=sc
    print(os.path.basename(p)[:30],round(f[1],2),' | '.join('%s %.2f'%(k,c) for c,k in sc),flush=True)
json.dump(res,open('ox2w.json','w'))
