#!/usr/bin/env python3
"""Extract goods category tables from EquipParamGoods (regulation.bin).

Outputs (into config.DATA_DIR, profile-aware):
  - goods_sort_groups.json       {goods_id: sortGroupId} for goodsType==0 rows
  - goods_crafting_ids.json      [ids] goodsType==2  (crafting materials)
  - goods_sorcery_ids.json       [ids] goodsType in (5, 17)   (sorceries)
  - goods_incantation_ids.json   [ids] goodsType in (16, 18)  (incantations)
  - goods_spirit_ash_ids.json    [ids] goodsType==8  (spirit ash summons)

Consumed by generate_loot_massedit.py category filters. Rules were derived by
matching the previously-committed tables against EquipParamGoods: ash/craft
matched their goodsType sets exactly; sorcery/incantation/sort_groups matched
on goodsType with only since-added rows differing (the old tables were a
snapshot of an earlier data version - this extractor keeps them fresh).
"""
import sys
import io
import os
import json
import tempfile
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

import config
from pythonnet import load as _pyload
_pyload('coreclr')
from System.Reflection import Assembly
from System import Array, Type as SysType, Object
from System.IO import File as SysFile

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
import SoulsFormats

_str = SysType.GetType('System.String')
_pr = asm.GetType('SoulsFormats.PARAM').BaseType.GetMethod('Read', Array[SysType]([_str]))


def main():
    src = config.require_err_mod_dir()
    out = config.DATA_DIR
    out.mkdir(parents=True, exist_ok=True)

    defs = {}
    for xml in config.PARAMDEF_DIR.glob('*.xml'):
        try:
            d = SoulsFormats.PARAMDEF.XmlDeserialize(str(xml), False)
            if d and d.ParamType:
                defs[str(d.ParamType)] = d
        except Exception:
            pass

    bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(src / 'regulation.bin'))
    goods = None
    for f in bnd.Files:
        if 'EquipParamGoods' in str(f.Name):
            tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_goods.param')
            SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
            goods = _pr.Invoke(None, Array[Object]([tmp]))
            goods.ApplyParamdef(defs[str(goods.ParamType)])
            break
    if goods is None:
        sys.exit('EquipParamGoods not found in regulation')

    sort_groups = {}
    by_type = {2: [], 5: [], 8: [], 16: [], 17: [], 18: []}
    for r in goods.Rows:
        rid = int(r.ID)
        gtype = sortg = None
        for c in r.Cells:
            nm = str(c.Def.InternalName)
            if nm == 'goodsType':
                gtype = int(c.Value)
            elif nm == 'sortGroupId':
                sortg = int(c.Value)
        if gtype == 0:
            sort_groups[rid] = sortg if sortg is not None else 255
        if gtype in by_type:
            by_type[gtype].append(rid)

    def dump(name, obj):
        with open(out / name, 'w', encoding='utf-8') as f:
            json.dump(obj, f)
        print(f'  {name}: {len(obj)}')

    dump('goods_sort_groups.json', {str(k): v for k, v in sorted(sort_groups.items())})
    dump('goods_crafting_ids.json', sorted(by_type[2]))
    dump('goods_sorcery_ids.json', sorted(by_type[5] + by_type[17]))
    dump('goods_incantation_ids.json', sorted(by_type[16] + by_type[18]))
    dump('goods_spirit_ash_ids.json', sorted(by_type[8]))


if __name__ == '__main__':
    main()
