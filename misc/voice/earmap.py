import json,glob,os
W={os.path.basename(p).split('_')[-1][:-4].lstrip('0'):p for p in glob.glob('luigi_zip/*/*.wav')}
G={os.path.basename(p).split(' - ')[-1][:-4]:p for p in glob.glob('galaxy/Luigi/*/*.wav')}
L={os.path.basename(p)[:-4]:p for p in glob.glob('lm/Luigi/*American*/*.wav')}
def w(k):return W[k]
def g(k):return G[k]
def l(k):return L[k]
# Picked from Whisper transcripts of every clip ("what is said") plus length; Mario's line in comments.
M={
 1:w('7C7'),     # Yeah!
 5:w('7A8'),6:w('7AA'),7:w('765'),8:w('7B3'),9:w('7AB'),          # Oh! / Ow!
 10:g('SE_PV_BURN_RUN'),11:w('7C0'),12:w('761'),                     # fire run: ow-ow / yowww / ya-ya-ya
 13:w('7B5'),14:w('7B6'),15:w('7B7'),                                # hurt in water
 16:w('7BE'),17:w('7BC'),18:w('7B8'),19:w('7CE'),                    # Oh no! / oh oh oh / waaah / no no oh
 20:w('7B9'),21:w('7BB'),22:w('A9E'),                                # KO in water: NOOO / whoa / oh no no no
 23:w('7BD'),24:l('490'),25:w('751'),26:w('7CB'),                    # poison: Ewww / Oooh / Aaargh / Oh!
 27:w('7BA'),                                                        # pit
 43:w('7CD'),44:w('7C1'),45:w('A74'),46:w('7CC'),                    # Moon: Yahoo / Wahoo / multi-moon cheer / Yahoo
 47:w('705'),                                                        # launch: Wahoo!
 48:w('742'),49:w('746'),50:w('734'),51:w('740'),52:w('703'),53:w('741'),   # grabs: Yup / Woop / Huh / Ha
 54:w('793'),55:w('792'),56:w('752'),57:w('753'),                    # heavy grab grunts
 58:l('528'),59:w('731'),60:w('707'),61:w('743'),                    # dive: Woohoo / Yay / Ha / Hoo
 62:w('719'),63:w('6FE'),64:w('700'),65:w('702'),66:w('721'),67:w('768'),   # 1st jump: Hoo / Ho / Hup / Ha
 68:w('738'),69:w('759'),                                            # 2nd jump: Ho!
 70:g('SE_PV_JUMP_TURN_02'),                                         # long jump: Woohoo!
 71:w('704'),72:w('758'),73:w('7C3'),                                # 3rd jump (two you liked) + Haha!
 74:w('718'),75:w('716'),76:w('722'),77:w('7A0'),78:w('725'),79:w('732'),   # flips: Waha / Ho ho / Whoo / Yo-ho / Hoo / Whoa
 80:l('512'),                                                        # landing: Yes!
 81:w('7AC'),82:w('7A4'),83:w('7DD'),84:w('7AF'),85:w('A69'),        # hard landing: Oh / Woah!
 86:w('728'),87:w('7D8'),88:w('701'),89:w('763'),90:w('745'),91:w('736'),92:w('748'),   # kicks: Ha / Ya / Yah / Huh
 93:w('733'),94:w('7D0'),95:w('7D1'),96:w('7D2'),97:w('797'),        # pushed back: Whoa / Oh / Ah
 98:g('SE_PV_RELEAVED'),99:g('SE_PV_YAWN'),100:['SNORE',g('SE_PV_LIE_WAIT')],101:['SNORE',g('SE_PV_SLEEP_1')],
 102:l('522'),103:['SNORE',g('SE_PV_SLEEP_2')],104:['SNORE',l('485')],105:w('A6C'),          # sleep: hm / yawn / mmm / snores / ugh
 106:w('6FF'),107:w('794'),108:w('7D9'),109:w('712'),110:w('720'),111:w('739'),        # throws: Ho / Yo / Ya / Yeah
 112:g('SE_PV_TIRED_01'),113:g('SE_PV_TIRED_03'),114:l('534'),115:g('SE_PV_TIRED_02'),116:g('SE_PV_BURN_RECOVERY'),117:l('473'),   # tired: Ahh / Ugh / Huh?!
 118:l('487'),119:l('484'),120:l('548'),121:g('SE_PV_LAST_DAMAGE_WATER'),122:l('486'),123:l('523'),124:w('A6B'),125:w('796'),   # cold: b-b-b / g-g-g / brrr / oh
}
old={int(k):v for k,v in json.load(open('mapping.json')).items()}
assert set(M)==set(old),(set(old)-set(M),set(M)-set(old))
flat=[v if isinstance(v,str) else v[1] for v in M.values()]
d=[x for x in set(flat) if flat.count(x)>1]; assert not d,d
json.dump(M,open('mapping.json','w'),indent=0,sort_keys=True); print('ok',len(M))
