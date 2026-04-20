#!/usr/bin/env python3
"""
Extract AEG463 (DLC) model -> item mapping from ERR regulation.bin.

Chain: AEG463_NNN -> AssetEnvironmentGeometryParam row 463NNN
       -> pickUpItemLotParamId -> ItemLotParam_map -> goods ID

Output: data/aeg463_item_mapping.json
"""

import os
import sys
import json
import tempfile
import shutil

sys.path.insert(0, os.path.dirname(__file__))

from pythonnet import load
load("coreclr")
import clr
from System import Array, Object
from System import Type as SysType
from System.IO import File as SysFile
from System.Reflection import Assembly, BindingFlags

import config

dll_path = str(config.SOULSFORMATS_DLL)
asm = Assembly.LoadFrom(dll_path)
clr.AddReference(dll_path)
import SoulsFormats

_str_type = SysType.GetType("System.String")


def _get_read(type_name):
    return asm.GetType(type_name).GetMethod(
        "Read", BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
        None, Array[SysType]([_str_type]), None)


_param_read = _get_read("SoulsFormats.PARAM")
_fmg_read = asm.GetType("SoulsFormats.FMG").GetMethod(
    "Read", BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None)


def read_param(bnd_file):
    tmp = os.path.join(tempfile.gettempdir(), "_p.bin")
    SysFile.WriteAllBytes(tmp, bnd_file.Bytes.ToArray())
    p = _param_read.Invoke(None, Array[Object]([tmp]))
    os.unlink(tmp)
    return p


def read_fmg(bnd_file):
    tmp = os.path.join(tempfile.gettempdir(), "_f.fmg")
    SysFile.WriteAllBytes(tmp, bnd_file.Bytes.ToArray())
    fmg = _fmg_read.Invoke(None, Array[Object]([tmp]))
    os.unlink(tmp)
    return fmg


def main():
    src = config.GAME_DIR / "oo2core_6_win64.dll"
    for d in [config.LIB_DIR, tempfile.gettempdir(), os.getcwd()]:
        dst = os.path.join(str(d), "oo2core_6_win64.dll")
        if not os.path.exists(dst):
            shutil.copy2(str(src), dst)

    paramdefs = {}
    for xml_path in config.PARAMDEF_DIR.glob("*.xml"):
        try:
            pdef = SoulsFormats.PARAMDEF.XmlDeserialize(str(xml_path), False)
            if pdef and pdef.ParamType:
                paramdefs[str(pdef.ParamType)] = pdef
        except:
            pass

    reg_path = config.require_err_mod_dir() / "regulation.bin"
    bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(reg_path))

    # Read AssetEnvironmentGeometryParam for AEG463 rows (463xxx) and row 777800
    print("Reading AssetEnvironmentGeometryParam for AEG463...")
    aeg463_data = {}
    for f in bnd.Files:
        if "AssetEnvironmentGeometryParam" not in str(f.Name):
            continue
        param = read_param(f)
        pt = str(param.ParamType)
        if pt in paramdefs:
            param.ApplyParamdef(paramdefs[pt])
        for row in param.Rows:
            if not ((463000 <= row.ID <= 464000) or row.ID == 777800):
                continue
            fields = {}
            for cell in row.Cells:
                n = str(cell.Def.InternalName)
                v = cell.Value
                if not hasattr(v, "Length"):
                    fields[n] = v
            lot = fields.get("pickUpItemLotParamId", 0)
            if lot and lot != 0 and lot != -1:
                aeg463_data[row.ID] = {
                    "lot": int(lot),
                    "repick": bool(fields.get("isEnableRepick", 0)),
                    "breakOnPickUp": bool(fields.get("isBreakOnPickUp", 0)),
                    "hiddenOnRepick": bool(fields.get("isHiddenOnRepick", 0)),
                }
        break

    print(f"  {len(aeg463_data)} models with pickup")

    # Read ItemLotParam_map
    print("Reading ItemLotParam_map...")
    target_lots = {v["lot"] for v in aeg463_data.values()}
    lot_items = {}
    for f in bnd.Files:
        if "ItemLotParam_map" not in str(f.Name):
            continue
        param = read_param(f)
        pt = str(param.ParamType)
        if pt in paramdefs:
            param.ApplyParamdef(paramdefs[pt])
        for row in param.Rows:
            if row.ID not in target_lots:
                continue
            fields = {}
            for cell in row.Cells:
                n = str(cell.Def.InternalName)
                v = cell.Value
                if not hasattr(v, "Length"):
                    fields[n] = v
            gid = int(fields.get("lotItemId01", 0))
            num = int(fields.get("lotItemNum01", 0))
            if gid:
                lot_items[row.ID] = (gid, num)
        break

    # Read GoodsName FMG
    print("Reading GoodsName FMG...")
    goods_names = {}
    _bnd4_read = _get_read("SoulsFormats.BND4")
    for bnd_path in (config.require_err_mod_dir() / "msg" / "engus").glob("*.msgbnd.dcx"):
        bnd2 = _bnd4_read.Invoke(None, Array[Object]([str(bnd_path)]))
        for f in bnd2.Files:
            if "GoodsName" in str(f.Name):
                fmg = read_fmg(f)
                for entry in fmg.Entries:
                    goods_names[int(entry.ID)] = str(entry.Text) if entry.Text else ""

    # Build results
    results = []
    for row_id, info in sorted(aeg463_data.items()):
        gid, num = lot_items.get(info["lot"], (0, 0))
        name = goods_names.get(gid, "???")
        if name == "???" or not name:
            continue

        if 463000 <= row_id < 464000:
            model_name = f"AEG463_{row_id - 463000}"
        elif row_id == 777800:
            model_name = f"AEG777_800"
        else:
            model_name = f"row_{row_id}"

        results.append({
            "model": model_name,
            "rowId": row_id,
            "goodsId": gid,
            "primaryItem": name,
            "primaryNum": num,
            "primaryGoodsId": gid,
            "isEnableRepick": info["repick"],
            "isHiddenOnRepick": info["hiddenOnRepick"],
            "isBreakOnPickUp": info["breakOnPickUp"],
            "pickUpItemLotParamId": info["lot"],
        })

    out_path = config.DATA_DIR / "aeg463_item_mapping.json"
    with open(out_path, "w") as f:
        json.dump(results, f, indent=2)
    print(f"Saved {len(results)} entries to {out_path}")


if __name__ == "__main__":
    main()
