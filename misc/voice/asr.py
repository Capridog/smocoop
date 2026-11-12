import glob,os,json,sys
from faster_whisper import WhisperModel
m=WhisperModel('small.en',device='cpu',compute_type='int8')
files=sorted(glob.glob('mario_wav/*.wav'))+sorted(glob.glob('luigi_zip/*/*.wav'))+sorted(glob.glob('galaxy/Luigi/*/*.wav'))+sorted(glob.glob('lm/Luigi/*American*/*.wav'))+sorted(glob.glob('lm/Luigi/*American*/(Ending Cutscene)/*.wav'))
out={}
for i,f in enumerate(files):
    import librosa; y,_=librosa.load(f,sr=16000); segs,info=m.transcribe(y,language='en',beam_size=5,vad_filter=False,condition_on_previous_text=False,temperature=0)
    segs=list(segs)
    out[f]=dict(text=' '.join(s.text.strip() for s in segs),nsp=round(min([s.no_speech_prob for s in segs] or [1]),2))
    if i%40==0: print(i,len(files),f[-40:],out[f],flush=True)
json.dump(out,open('asr.json','w'),indent=0)
