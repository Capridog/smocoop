import json,glob,os,re,pickle,numpy as np,librosa
from scipy.optimize import linear_sum_assignment
exec(open('unique.py').read().split("names=json.load")[0])   # gets G, W, A74, fixed (Galaxy counterparts per slot)
galaxy_for={s:v for s,v in fixed.items() if isinstance(v,str) and '/galaxy/' in '/'+v}
names=json.load(open('pvnames.json'))
cur={int(k):v for k,v in json.load(open('mapping.json')).items()}
skip=re.compile(r'Dummy|OpeningStandUp|Mofumofu|Demo002|Demo021|HomeLaunch')
sleep={98,99,100,101,102,103,104,105}
slots=[s for s in sorted(cur) if s not in sleep]
def feat(p):
    y,sr=librosa.load(p,sr=16000); y,_=librosa.effects.trim(y,top_db=35)
    if len(y)<1024: y=np.pad(y,(0,1024-len(y)))
    m=librosa.feature.mfcc(y=y,sr=sr,n_mfcc=20,hop_length=160,n_fft=512)[1:]
    m=(m-m.mean(1,keepdims=True))/(m.std(1,keepdims=True)+1e-6)
    return np.nan_to_num(m)+1e-3*np.random.RandomState(0).randn(*m.shape),len(y)/sr
def cost(a,b):
    D,wp=librosa.sequence.dtw(a[0],b[0],metric='cosine'); return D[-1,-1]/len(wp)+0.6*abs(np.log((b[1]+.05)/(a[1]+.05)))
Wk=sorted(W,key=lambda k:int(k,16)); WF={k:feat(W[k]) for k in Wk}
mario={int(os.path.basename(p)[:3]):p for p in glob.glob('mario_wav/*.wav')}
MF={s:feat(mario[s]) for s in slots}
GF={s:feat(galaxy_for[s]) for s in slots if s in galaxy_for}
# alignment partner via 3DW Mario anchor (from earlier steps)
A1=pickle.load(open('align_cost.pkl','rb'));A2=pickle.load(open('align_cost2.pkl','rb'));al=json.load(open('align2.json'))
o2w=json.load(open('o2w.json'))
part={}
for name in ('main','A'):
    for k,(v,agree) in al[name].items(): part[int(k,16)]=(int(v,16),agree)
anchor={}
for ok,c in o2w.items():
    s=int(ok[:3]);cst,wk,_=c[0]
    if cst<0.45 and int(wk,16) in part: anchor[s]=(part[int(wk,16)][0],part[int(wk,16)][1] and cst<0.3,cst)
S=np.zeros((len(slots),len(Wk)))
for i,s in enumerate(slots):
    for j,k in enumerate(Wk):
        x=0.5*cost(MF[s],WF[k])
        if s in GF: x+=1.0*cost(GF[s],WF[k])          # same speaker: Luigi Galaxy clip for this move
        else: x+=0.5*cost(MF[s],WF[k])
        if s in anchor and int(k,16)==anchor[s][0]: x-=0.6+(0.3 if anchor[s][1] else 0)
        S[i,j]=x
# user feedback: 'Oh yeah' (3DW 7A1) not on jumps
for i,s in enumerate(slots):
    if 62<=s<=79: S[i,Wk.index('7A1')]+=5
r,c=linear_sum_assignment(S)
m={}
for i,j in zip(r,c): m[slots[i]]=W[Wk[j]]
for s in sleep: m[s]=cur[s]
for s in sorted(m):
    v=m[s]; print(s,names[str(s)],os.path.basename(v if isinstance(v,str) else v[1])[-12:-4], 'anchor' if s in anchor and isinstance(v,str) and int(os.path.basename(v)[-12:-4],16)==anchor[s][0] else '')
json.dump(m,open('mapping.json','w'),indent=0,sort_keys=True)
