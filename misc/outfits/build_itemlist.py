"""Build the 1.0.0 romfs files that add the 1.1-1.4 update outfits (Luigi-only in the mod).

usage: python build_itemlist.py <1.0 romfs> <1.4 romfs> <out dir>
Writes SystemData/ItemList.szs (1.0 lists + the update outfits appended at the end) and
LocalizedData/USen/MessageData/SystemMessage.szs (1.0 archive with 1.4 ItemCloth/ItemCap names).
Also copy 1.4 ObjectData/TextureClothes.szs and the Mario<outfit>*.szs models into the mod romfs.
"""
import os, sys, oead
NEW = ['MarioBandman', 'MarioSunshine', 'MarioArmorWestern', 'MarioSatellite', 'MarioBatter',
       'MarioTopper', 'MarioRacer', 'MarioRango', 'MarioHariet', 'MarioSpewart', 'MarioConductor',
       'MarioZombie', 'MarioSanta', 'MarioDot3d']
def load(p): return oead.Sarc(oead.yaz0.decompress(open(p, 'rb').read()))
def main(r10, r14, out):
    A = load(os.path.join(r10, 'SystemData/ItemList.szs'))
    fb = {f.name: oead.byml.from_binary(bytes(f.data)) for f in load(os.path.join(r14, 'SystemData/ItemList.szs')).get_files()}
    w = oead.SarcWriter(endian=oead.Endianness.Little)
    for f in A.get_files():
        raw = bytes(f.data)
        if f.name in ('ItemCloth.byml', 'ItemCap.byml', 'ItemList.byml'):
            v = oead.byml.from_binary(raw)
            have = {(str(x['ItemName']), str(x.get('ItemType', ''))) for x in v}
            arr = oead.byml.Array(list(v))
            for x in fb[f.name]:
                key = (str(x['ItemName']), str(x.get('ItemType', '')))
                if key in have or str(x['ItemName']) not in NEW: continue
                arr.append(oead.byml.Hash({k: x[k] for k in x if k != 'AocKey'}))
            raw = oead.byml.to_binary(arr, big_endian=False, version=3)
        w.files[f.name] = raw
    os.makedirs(os.path.join(out, 'SystemData'), exist_ok=True)
    open(os.path.join(out, 'SystemData/ItemList.szs'), 'wb').write(oead.yaz0.compress(w.write()[1]))
    m = 'LocalizedData/USen/MessageData/SystemMessage.szs'
    S = load(os.path.join(r10, m))
    S4 = {f.name: bytes(f.data) for f in load(os.path.join(r14, m)).get_files()}
    w = oead.SarcWriter(endian=oead.Endianness.Little)
    for f in S.get_files():
        w.files[f.name] = S4[f.name] if f.name in ('ItemCloth.msbt', 'ItemCap.msbt') else bytes(f.data)
    os.makedirs(os.path.dirname(os.path.join(out, m)), exist_ok=True)
    open(os.path.join(out, m), 'wb').write(oead.yaz0.compress(w.write()[1]))
if __name__ == '__main__': main(*sys.argv[1:4])
