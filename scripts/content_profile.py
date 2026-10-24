"""Prepare explicit offline AppContent metadata; SKU is a probe setting, not a license check.

Add-ons (DLC): --dlc-dir names a folder holding add-ons already extracted from their packages,
each an ADDCONT<label> folder with sce_sys/param.sfo (the folder itself, or one level below,
for example <dlc-dir>/CUSA12112/ADDCONT...). Those of this title are listed in addcont.txt
(label, tab, folder) for p4d-probe --addcont-list."""
import argparse
import json
import struct
from pathlib import Path
from prepare import sfo


def find_addons(dlc_dir, title_id):
    """[(entitlement label, folder, title)] of the extracted add-ons of title_id under dlc_dir."""
    if not dlc_dir or not Path(dlc_dir).is_dir():
        return []
    root = Path(dlc_dir)
    candidates = [root] + [p for p in root.iterdir() if p.is_dir()]
    candidates += [p for c in candidates[1:] for p in c.iterdir() if p.is_dir()]
    found = {}
    for folder in candidates:
        param = folder / 'sce_sys' / 'param.sfo'
        if not param.is_file():
            continue
        try:
            values = sfo(param.read_bytes())
        except (OSError, ValueError):
            continue
        content_id = values.get('CONTENT_ID', '')
        # CONTENT_ID: UP2611-CUSA12112_00-ADDCONTDLCD0000C; the label is its last 16 characters.
        label = content_id.rsplit('-', 1)[-1] if '-' in content_id else ''
        if values.get('TITLE_ID') != title_id or len(label) != 16 or values.get('CATEGORY') != 'ac':
            continue
        found.setdefault(label, (label, folder.resolve(), values.get('TITLE', label)))
    return sorted(found.values())


def prepare(game, out, sku='full', dlc_dir=None):
    values=sfo((game/'sce_sys/param.sfo').read_bytes())
    params=[values.get(f'USER_DEFINED_PARAM_{i}',0) for i in range(1,5)]
    if any(type(v) is not int or not 0<=v<=0xffffffff for v in params):
        raise ValueError('invalid user-defined parameter')
    addons=find_addons(dlc_dir, values.get('TITLE_ID'))
    profile=dict(title_id=values.get('TITLE_ID'),sku=sku,sku_source='explicit probe setting',
                 user_params=params, mounted_addons=[dict(label=l,folder=str(f),title=t) for l,f,t in addons],
                 boot_attr=0)
    out.mkdir(parents=True,exist_ok=True)
    (out/'content.bin').write_bytes(struct.pack('<8s5I',b'G3CONT01',{'full':3,'trial':1}[sku],*params))
    (out/'content-profile.json').write_text(json.dumps(profile,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
    (out/'addcont.txt').write_text(''.join(f'{l}\t{f}\n' for l,f,_ in addons),encoding='utf-8')
    print(f'AppContent profile: SKU={sku} (probe setting), user params={params}, add-ons={len(addons)}'
          + (f' from {dlc_dir}' if addons else ''))

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('game',type=Path)
    p.add_argument('--out',type=Path,default=Path(__file__).resolve().parent.parent/'out')
    p.add_argument('--sku',choices=['full','trial'],default='full')
    p.add_argument('--dlc-dir',type=Path,default=None)
    a=p.parse_args();prepare(a.game,a.out,a.sku,a.dlc_dir)
