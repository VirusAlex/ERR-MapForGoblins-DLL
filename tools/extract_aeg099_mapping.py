#!/usr/bin/env python3
"""
Extract AEG099 model → item mapping from regulation.bin.

Chain: AEG099_NNN → AssetEnvironmentGeometryParam row 99NNN
       → pickUpItemLotParamId → ItemLotParam_map → goods ID

Output: data/aeg099_item_mapping.json

Usage:
    py extract_aeg099_mapping.py
"""

import json
import os
import shutil
import sys
import tempfile

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


def _get_read_str(type_name):
    return asm.GetType(type_name).GetMethod(
        "Read",
        BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
        None,
        Array[SysType]([_str_type]),
        None,
    )


_param_read = _get_read_str("SoulsFormats.PARAM")
_bnd4_read = _get_read_str("SoulsFormats.BND4")
_fmg_read = asm.GetType("SoulsFormats.FMG").GetMethod(
    "Read",
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None,
    Array[SysType]([_str_type]),
    None,
)


def _read_param_from_bnd_file(bnd_file):
    tmp = os.path.join(tempfile.gettempdir(), "_aeg_param.bin")
    SysFile.WriteAllBytes(tmp, bnd_file.Bytes.ToArray())
    param = _param_read.Invoke(None, Array[Object]([tmp]))
    os.unlink(tmp)
    return param


def _read_fmg_from_bnd_file(bnd_file):
    tmp = os.path.join(tempfile.gettempdir(), "_aeg_fmg.fmg")
    SysFile.WriteAllBytes(tmp, bnd_file.Bytes.ToArray())
    fmg = _fmg_read.Invoke(None, Array[Object]([tmp]))
    os.unlink(tmp)
    return fmg


def _ensure_oo2core():
    """Copy oo2core DLL next to SoulsFormats and to temp dir so Andre can find it."""
    if not config.GAME_DIR:
        return
    src = config.GAME_DIR / "oo2core_6_win64.dll"
    if not src.exists():
        return
    for dst_dir in [config.LIB_DIR, tempfile.gettempdir(), os.getcwd()]:
        dst = os.path.join(str(dst_dir), "oo2core_6_win64.dll")
        if not os.path.exists(dst):
            shutil.copy2(str(src), dst)


def main():
    _ensure_oo2core()
    print("Loading paramdefs...")
    paramdefs = {}
    for xml_path in config.PARAMDEF_DIR.glob("*.xml"):
        try:
            pdef = SoulsFormats.PARAMDEF.XmlDeserialize(str(xml_path), False)
            if pdef and pdef.ParamType:
                paramdefs[str(pdef.ParamType)] = pdef
        except Exception:
            pass
    print(f"  {len(paramdefs)} paramdefs loaded")

    reg_path = config.require_err_mod_dir() / "regulation.bin"
    print(f"Decrypting {reg_path}...")
    bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(reg_path))

    # Step 1: Read AssetEnvironmentGeometryParam
    print("Reading AssetEnvironmentGeometryParam...")
    aeg_data = {}  # model_num -> {pickUpItemLotParamId, isEnableRepick, isBreakOnPickUp, ...}
    for f in bnd.Files:
        if "AssetEnvironmentGeometryParam" not in str(f.Name):
            continue
        param = _read_param_from_bnd_file(f)
        pt = str(param.ParamType)
        if pt in paramdefs:
            param.ApplyParamdef(paramdefs[pt])
        for row in param.Rows:
            if not (99000 <= row.ID <= 99999):
                continue
            fields = {}
            for cell in row.Cells:
                n = str(cell.Def.InternalName)
                v = cell.Value
                if not hasattr(v, "Length"):
                    fields[n] = v
            lot_id = fields.get("pickUpItemLotParamId", 0)
            if lot_id and lot_id != 0 and lot_id != -1:
                aeg_data[row.ID - 99000] = {
                    "pickUpItemLotParamId": lot_id,
                    "pickUpActionButtonParamId": fields.get("pickUpActionButtonParamId", 0),
                    "isEnableRepick": bool(fields.get("isEnableRepick", 0)),
                    "isBreakOnPickUp": bool(fields.get("isBreakOnPickUp", 0)),
                    "isHiddenOnRepick": bool(fields.get("isHiddenOnRepick", 0)),
                }
        break
    print(f"  {len(aeg_data)} models with pickUpItemLotParamId")

    # Step 2: Read ItemLotParam_map
    print("Reading ItemLotParam_map...")
    target_lots = {v["pickUpItemLotParamId"] for v in aeg_data.values()}
    lot_items = {}
    for f in bnd.Files:
        if "ItemLotParam_map" not in str(f.Name):
            continue
        param = _read_param_from_bnd_file(f)
        pt = str(param.ParamType)
        if pt in paramdefs:
            param.ApplyParamdef(paramdefs[pt])
        for row in param.Rows:
            if row.ID not in target_lots:
                continue
            fields = {}
            for cell in row.Cells:
                fields[str(cell.Def.InternalName)] = cell.Value
            items = []
            for i in range(1, 9):
                gid = fields.get(f"lotItemId0{i}", 0)
                cat = fields.get(f"lotItemCategory0{i}", 0)
                num = fields.get(f"lotItemNum0{i}", 0)
                if gid and gid != 0:
                    items.append({"goodsId": gid, "category": cat, "num": num})
            lot_items[row.ID] = items
        break
    print(f"  {len(lot_items)} lots resolved")

    # Step 3: Read GoodsName FMG
    print("Reading GoodsName FMG...")
    goods_names = {}
    msg_path = config.require_err_mod_dir() / "msg" / "engus" / "item_dlc02.msgbnd.dcx"
    tmp = os.path.join(tempfile.gettempdir(), "_aeg_msg.msgbnd.dcx")
    shutil.copy2(str(msg_path), tmp)
    msg_bnd = _bnd4_read.Invoke(None, Array[Object]([tmp]))
    os.unlink(tmp)
    for f in msg_bnd.Files:
        name = os.path.basename(str(f.Name))
        if "GoodsName" in name and "dlc" not in name.lower():
            fmg = _read_fmg_from_bnd_file(f)
            for entry in fmg.Entries:
                text = str(entry.Text) if entry.Text else ""
                if text:
                    goods_names[entry.ID] = text
            break
    print(f"  {len(goods_names)} goods names loaded")

    # Step 4: Build result
    result = []
    for model_num in sorted(aeg_data.keys()):
        data = aeg_data[model_num]
        lot_id = data["pickUpItemLotParamId"]
        items = lot_items.get(lot_id, [])
        for item in items:
            item["name"] = goods_names.get(item["goodsId"], "???")

        entry = {
            "model": f"AEG099_{model_num:03d}",
            "modelNum": model_num,
            "pickUpItemLotParamId": lot_id,
            "isEnableRepick": data["isEnableRepick"],
            "isBreakOnPickUp": data["isBreakOnPickUp"],
            "isHiddenOnRepick": data["isHiddenOnRepick"],
            "items": items,
            "primaryItem": items[0]["name"] if items else "???",
            "primaryGoodsId": items[0]["goodsId"] if items else 0,
            "primaryNum": items[0]["num"] if items else 0,
        }
        result.append(entry)

    # Save
    out_path = config.DATA_DIR / "aeg099_item_mapping.json"
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(result, f, indent=2, ensure_ascii=False)

    print(f"\nSaved {len(result)} entries to {out_path}")

    # Summary
    repick = sum(1 for e in result if e["isEnableRepick"])
    no_repick = len(result) - repick
    print(f"  Respawning (repick): {repick}")
    print(f"  One-time pickup:     {no_repick}")


if __name__ == "__main__":
    main()
