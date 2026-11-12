import numpy as np,soundfile as sf,librosa,glob
G={p.split(' - ')[-1][:-4]:p for p in glob.glob('galaxy/Luigi/*/*.wav')}
L={p.split('/')[-1][:-4]:p for p in glob.glob('lm/Luigi/*American*/*.wav')}
SR=32000
def load(p): y,_=librosa.load(p,sr=SR); y,_=librosa.effects.trim(y,top_db=40); return y/(np.abs(y).max()+1e-9)*0.7
def onsets(slot):
    y,_=librosa.load(glob.glob('mario_wav/%03d_*.wav'%slot)[0],sr=16000)
    rms=librosa.feature.rms(y=y,frame_length=1024,hop_length=160)[0];on=rms>0.12*rms.max()
    t=[i/100 for i in range(1,len(on)) if on[i] and not on[i-1]]
    if on[0]: t=[0.0]+t
    return t,len(y)/16000
rng=np.random.RandomState(7)
def snores(slot,clips):
    t,dur=onsets(slot); out=np.zeros(int((dur+1.5)*SR)); last=-9;k=0
    for s in t:
        c=clips[k%len(clips)]
        if s<last: continue
        a=load(c); st=rng.uniform(-0.25,0.25); a=librosa.effects.pitch_shift(a,sr=SR,n_steps=st)*rng.uniform(0.8,1.0)
        i=int(s*SR); out[i:i+len(a)]+=a[:len(out)-i]; last=s+len(a)/SR+0.35; k+=1
    return out[:int((dur+0.3)*SR)]
def one(p): return load(p)
def cat(*xs,gap=0.25): 
    r=[];[r.extend([x,np.zeros(int(gap*SR))]) for x in xs]; return np.concatenate(r)
C={98:cat(one(L['522']),one(L['538'])),99:one(G['SE_PV_YAWN']),102:one(G['SE_PV_LIE_WAIT']),
   100:snores(100,[G['SE_PV_SLEEP_1'],G['SE_PV_SLEEP_2']]),101:snores(101,[G['SE_PV_SLEEP_2'],G['SE_PV_SLEEP_1']]),
   103:snores(103,[G['SE_PV_SLEEP_1'],G['SE_PV_SLEEP_2']]),104:snores(104,[G['SE_PV_SLEEP_2'],L['485'],G['SE_PV_SLEEP_1']]),
   105:cat(one(G['SE_PV_RELEAVED']),one(G['SE_PV_SLEEP_2']),gap=0.6)}
import os; os.makedirs('sleepC',exist_ok=True)
for s,a in C.items(): sf.write('sleepC/%03d.wav'%s,a,SR)
parts=[]
for s in [98,99,102,100,101,104,105]: parts+=[C[s],np.zeros(int(0.8*SR))]
sf.write('sleepdemo/C.wav',np.concatenate(parts),SR)
