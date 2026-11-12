# Luigi voice archive

`P2LuigiPv.bfwar` goes in the mod's romfs at `SoundData/P2LuigiPv.bfwar`. It holds one clip per
slot of Mario's player-voice wave-sound file (file 101 of SeData.bfsar, 126 slots; names in
`pvnames.json`). Empty slots keep Mario's clip. The mod's GetWaveFileOfWaveSound hook serves these
clips for sounds requested by Luigi's SeKeepers.

Steps:
1. `pvtool.py SeResourceStd2nd.aras mario_pv.bfwar mario_pv.map`: pull Mario's voice archive
   (file 148) out of the game's group.
2. `dspdec.py`: decode Mario's clips (DSP ADPCM) to `mario_wav/`.
3. Unzip the Super Mario 3D World Luigi voice set (The Sounds Resource) into `luigi_zip/`.
4. `assign.py`: match each slot to a Luigi clip (MFCC + DTW, distinct clips within each voice
   group) -> `mapping.json`. Edit that file by hand to change any slot.
5. `build.py`: write `P2LuigiPv.bfwar` (PCM16, loudness matched to Mario's clip per slot).
