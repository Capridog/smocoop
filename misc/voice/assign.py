import json,re,numpy as np,collections,glob,os
from scipy.optimize import linear_sum_assignment
exec(open('match.py').read().split('res={}')[0])
skip=re.compile(r'Dummy|OpeningStandUp|Mofumofu|Demo002|Demo021|HomeLaunch')
groups=collections.defaultdict(list)
for mk in M:
    nm=mk[4:-4]
    if skip.search(nm): continue
    g=re.sub(r'\d+$','',nm); groups[g].append(mk)
Lk=list(L)
cache={}
def cost(mk,lk):
    mm,md,_=M[mk];lm,ld,_=L[lk]
    D,wp=librosa.sequence.dtw(mm,lm,metric='cosine')
    return D[-1,-1]/len(wp)+0.25*abs(np.log((ld+.05)/(md+.05)))
mapping={}
for g,mks in groups.items():
    C=np.array([[cost(m,l) for l in Lk] for m in mks])
    r,c=linear_sum_assignment(C)
    for i,j in zip(r,c): mapping[int(mks[i][:3])]=Lk[j]
json.dump(mapping,open('mapping.json','w'),indent=0,sort_keys=True)
for k in sorted(mapping): print(k,[m for m in M if int(m[:3])==k][0][4:-4],mapping[k][-8:-4])
print(len(mapping),'slots,',len(set(mapping.values())),'clips')
