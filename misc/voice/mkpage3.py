import glob,os,json,base64,subprocess,soundfile as sf
def mp3(p):
    out=subprocess.run(['ffmpeg','-v','error','-i',p,'-ac','1','-ar','22050','-b:a','40k','-f','mp3','-'],capture_output=True).stdout
    return 'data:audio/mpeg;base64,'+base64.b64encode(out).decode()
def dur(p): i=sf.info(p); return i.frames/i.samplerate
ASR=json.load(open('asr.json'))
def said(p):
    t=ASR.get(p,{}).get('text','').strip()
    return (' “'+(t[:24]+'…' if len(t)>24 else t)+'”') if t else ''
names=json.load(open('pvnames.json')); groups=json.load(open('groups.json'))
mapping={int(k):v for k,v in json.load(open('mapping.json')).items()}
friendly={'vJump':'Jump (1st)','vJump2nd':'Jump (2nd)','vJump3rd':'Jump (3rd)','vJumpBroad':'Long jump','vJumpTurn':'Backflip / side flip','vJumpTurn2':'Backflip / side flip (2)',
 'vLandJump':'Landing jump','vLandStiffen':'Hard landing','vLandStiffen2':'Hard landing (big)','vGrab':'Grab / ledge','vGrabHeavy':'Grab (heavy)','vHeadSlide':'Dive / belly slide',
 'vKick':'Kick','vThrow':'Throw','vThrowHeavy':'Throw (heavy)','vPushedBack':'Pushed back','vDamage':'Hurt','vDamageFireRun':'Burned (running)','vDamageInWater':'Hurt in water',
 'vDamageLast':'Knocked out','vDamageLastCrushed':'Crushed','vDamageLastInWater':'Knocked out in water','vDamageLastPoison':'Poison','vDeadFallDown':'Fell in a pit',
 'vDemoGetShine':'Got a Moon','vDemoGetGrandShine00':'Got a Multi Moon','vDemoGetGrandShine01':'Got a Multi Moon (2)','vSleep0':'Sleep 1','vSleep1':'Sleep 2','vSleep2':'Sleep 3','vSleep3':'Sleep 4',
 'vSleep4':'Sleep 5','vSleep5':'Sleep 6','vSleep6':'Sleep 7','vSleep7':'Sleep 8','vTired':'Out of breath','vWaitCold':'Cold','vWaitCold2':'Cold (2)','vRcGetPrize':'RC car prize','vPayToHome':'Pay to go home'}
clips={}
def add(cid,label,group,path):
    if cid not in clips: clips[cid]=dict(label=label,group=group,src=mp3(path))
for p in sorted(glob.glob('galaxy/Luigi/*/*.wav')+glob.glob('galaxy/Luigi/*/*/*.wav')):
    nm=os.path.basename(p).split(' - ')[-1][:-4]
    if 'No Label' in nm: nm='Unused '+os.path.basename(p).split(']')[0].split('[')[-1]
    add('g:'+nm,'%s%s (%.1fs)'%(nm,said(p),dur(p)),'Super Mario Galaxy',p)
for p in sorted(glob.glob('luigi_zip/*/*.wav')):
    k=os.path.basename(p).split('_')[-1][:-4].lstrip('0')
    add('w:'+k,'3DW %s%s (%.1fs)'%(k,said(p),dur(p)),'Super Mario 3D World',p)
for p in sorted(glob.glob('lm/Luigi/*American*/*.wav')+glob.glob('lm/Luigi/*American*/(Ending Cutscene)/*.wav'),key=lambda p:int(os.path.basename(p)[:-4])):
    k=os.path.basename(p)[:-4]; tag=' ending' if 'Ending' in p else ''
    add('m:'+k,'LM %s%s%s (%.1fs)'%(k,tag,said(p),dur(p)),"Luigi's Mansion",p)
mario={int(os.path.basename(p)[:3]):p for p in glob.glob('mario_wav/*.wav')}
sug=json.load(open('suggest.json'))
rows=[]
for s in sorted(mapping):
    nm=names[str(s)]; g=groups.get(nm,'')
    v=mapping[s]
    if isinstance(v,list): cur={'CUT':'edited clip','SEQ':'several clips in a row'}.get(v[0],'looped hums or snores')
    else:
        b=os.path.basename(v); cur=('Galaxy '+b.split(' - ')[-1][:-4]) if ' - ' in b else '3DW '+b.split('_')[-1][:-4].lstrip('0')
    cid=None
    if isinstance(v,str):
        b=os.path.basename(v); cid=('g:'+b.split(' - ')[-1][:-4]) if ' - ' in b else ('w:'+b.split('_')[-1][:-4].lstrip('0'))
    rows.append(dict(slot=s,label=friendly.get(g,g or nm),name=nm+'  ·  Mario: '+ASR.get(mario[s],{}).get('text','')[:30],cur=cur,curId=cid,sug=[x for x in sug.get(str(s),[]) if x in clips],mario=mp3(mario[s]),now=mp3('slotwav/%03d.wav'%s)))
data=dict(clips=clips,rows=rows)
tpl=open('page_tpl3.html').read()
open('aud/luigi_voice_picker.html','w').write(tpl.replace('/*DATA*/',json.dumps(data)))
print(len(clips),len(rows),os.path.getsize('aud/luigi_voice_picker.html'))
