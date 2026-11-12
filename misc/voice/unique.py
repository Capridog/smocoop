import json,glob,os,numpy as np,librosa
from scipy.optimize import linear_sum_assignment
G={os.path.basename(p).split(' - ')[-1][:-4]:p for p in glob.glob('galaxy/Luigi/*/*.wav')}
W={os.path.basename(p).split('_')[-1][:-4].lstrip('0'):p for p in glob.glob('luigi_zip/*/*.wav')}
A74=W['A74']
fixed={
 68:G['SE_PV_JUMP_M_01'],69:G['SE_PV_JUMP_M_02'],71:W['704'],72:W['758'],73:G['SE_PV_JUMP_L'],
 62:G['SE_PV_JUMP_S_01'],63:G['SE_PV_JUMP_S_02'],64:G['SE_PV_JUMP_S_03'],
 70:G['SE_PV_JUMP_LONG_01'],47:G['SE_PV_JUMP_LONG_02'],74:G['SE_PV_JUMP_TURN_01'],77:G['SE_PV_JUMP_TURN_02'],
 80:G['SE_PV_LAND'],81:G['SE_PV_HIP_DROP_LAND_01'],82:G['SE_PV_HIP_DROP_LAND_02'],83:G['SE_PV_LAND_COOL'],
 48:G['SE_PV_CATCH'],49:G['SE_PV_CLIFF_FALL_HANG'],50:G['SE_PV_CLIFF_CLIMB'],
 54:G['SE_PV_LIFT_UP_01'],55:G['SE_PV_LIFT_UP_02'],56:G['SE_PV_STRAIN'],57:G['SE_SV_LUIGI_LIFT_UP'],
 86:G['SE_PV_KICK_01'],87:G['SE_PV_KICK_02'],88:G['SE_PV_PUNCH_01'],89:G['SE_PV_PUNCH_02'],
 106:G['SE_PV_THROW_01'],107:G['SE_PV_THROW_02'],
 93:G['SE_PV_DAMAGE_S_03'],94:G['SE_PV_DAMAGE_L'],95:G['SE_PV_NEEDLE_DAMAGE'],96:G['SE_SV_LUIGI_TRAMPLED_01'],97:G['SE_SV_LUIGI_TRAMPLED_02'],
 5:G['SE_PV_DAMAGE_S_01'],6:G['SE_PV_DAMAGE_S_02'],7:G['SE_PV_ELEC_DAMAGE'],
 10:G['SE_PV_BURN'],11:G['SE_PV_BURN_RUN'],12:G['SE_PV_BURN_RECOVERY'],
 13:G['SE_PV_DAMAGE_S_WATER_01'],14:G['SE_PV_DAMAGE_S_WATER_02'],15:G['SE_PV_COLD_WATER_DAMAGE'],
 16:G['SE_PV_LAST_DAMAGE'],17:G['SE_PV_DOWN'],18:G['SE_PV_THROWN'],
 20:G['SE_PV_LAST_DAMAGE_WATER'],21:G['SE_PV_DOWN_WATER'],
 23:G['SE_PV_FAINTED'],24:G['SE_PV_DIE_MUD'],25:G['SE_PV_MUD_SINK'],27:G['SE_PV_FALL_DIE'],
 43:G['SE_PV_POWER_STAR_GET'],44:G['SE_SV_LUIGI_POW_STAR_GET_WITH'],45:G['SE_PV_GRAND_STAR_GET'],46:G['SE_PV_TOUCH_GRAND_STAR_1'],
 1:G['SE_PV_JUMP_JOY_01'],
 112:G['SE_PV_TIRED_01'],113:G['SE_PV_TIRED_02'],114:G['SE_PV_TIRED_03'],115:G['SE_SV_LUIGI_TALK_TIRE'],
 98:G['SE_PV_YAWN'],99:['CUT',A74,0,0.95,0],101:['CUT',A74,1.0,3.35,0],102:G['SE_PV_LIE_WAIT'],104:['CUT',A74,4.2,None,0],
 100:['SNORE',G['SE_PV_SLEEP_1']],103:['SNORE',G['SE_PV_SLEEP_2']],105:G['SE_PV_RELEAVED'],
 118:G['SE_PV_FREEZE'],
}
names=json.load(open('pvnames.json'))
old={int(k):v for k,v in json.load(open('mapping.json')).items()}
slots=sorted(old)
fill=[s for s in slots if s not in fixed]
used=set(v if isinstance(v,str) else v[1] for v in fixed.values())
skipnames=('FILE_SEL','MODE_APPEAR','THANK_YOU','No Label','LUIGI_MARIO','SV_LUIGI_HEY','TALK_HELP','SORRY','THANKS','AFRAID','FRIGHTENED')
pool=[p for p in list(G.values())+list(W.values())+glob.glob('lm/Luigi/*American*/*.wav') if p not in used and not any(k in p for k in skipnames)]
print('fill',len(fill),'pool',len(pool))
def feat(p):
    y,sr=librosa.load(p,sr=16000); y,_=librosa.effects.trim(y,top_db=35)
    if len(y)<1024: y=np.pad(y,(0,1024-len(y)))
    m=librosa.feature.mfcc(y=y,sr=sr,n_mfcc=20,hop_length=160,n_fft=512)[1:]
    m=(m-m.mean(1,keepdims=True))/(m.std(1,keepdims=True)+1e-6)
    return np.nan_to_num(m)+1e-3*np.random.RandomState(0).randn(*m.shape),len(y)/sr
mario={int(os.path.basename(p)[:3]):p for p in glob.glob('mario_wav/*.wav')}
MF={s:feat(mario[s]) for s in fill}; PF={p:feat(p) for p in pool}
C=np.zeros((len(fill),len(pool)))
for i,s in enumerate(fill):
    mm,md=MF[s]
    for j,p in enumerate(pool):
        lm,ld=PF[p]; D,wp=librosa.sequence.dtw(mm,lm,metric='cosine')
        C[i,j]=D[-1,-1]/len(wp)+0.6*abs(np.log((ld+.05)/(md+.05)))
r,c=linear_sum_assignment(C)
m=dict(fixed)
for i,j in zip(r,c): m[fill[i]]=pool[j]
for s in fill: print(s,names[str(s)],os.path.basename(m[s])[-40:],round(C[fill.index(s),pool.index(m[s])],2))
flat=[v if isinstance(v,str) else (v[1]+str(v[2:])) for v in m.values()]
print('unique',len(set(flat)),'of',len(flat))
json.dump(m,open('mapping.json','w'),indent=0,sort_keys=True)
