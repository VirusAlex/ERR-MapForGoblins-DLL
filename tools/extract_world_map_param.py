#!/usr/bin/env python3
"""
Extract WorldMapPointParam from ERR regulation.bin using SoulsFormats.
Exports to CSV for comparison with MASSEDIT files.
"""

import csv
import json
import sys
from pathlib import Path

import config

# Setup .NET runtime
import os
import tempfile
from pythonnet import load
load('coreclr')
import clr
from System.Reflection import Assembly, BindingFlags
from System.IO import File as SysFile

dll_path = str(config.SOULSFORMATS_DLL)
asm = Assembly.LoadFrom(dll_path)
clr.AddReference(dll_path)

import SoulsFormats
from System import Array, Type as SysType, Object

GAME_DIR = config.require_game_dir()
ERR_MOD_DIR = config.require_err_mod_dir()
OUTPUT_DIR = config.DATA_DIR
PARAMDEF_DIR = config.PARAMDEF_DIR

# Andre.SoulsFormats: Read(string) via reflection
_str_type = SysType.GetType('System.String')
def _get_read_str(type_name):
    cls = asm.GetType(type_name)
    return cls.GetMethod('Read',
        BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
        None, Array[SysType]([_str_type]), None)

_param_read_str = _get_read_str('SoulsFormats.PARAM')

def _read_from_bytes(read_method, data, suffix='.bin'):
    tmp = os.path.join(tempfile.gettempdir(), f'_mfg_tmp{suffix}')
    if hasattr(data, 'ToArray'):
        SysFile.WriteAllBytes(tmp, data.ToArray())
    else:
        SysFile.WriteAllBytes(tmp, data)
    result = read_method.Invoke(None, Array[Object]([tmp]))
    os.unlink(tmp)
    return result

FIELDS_OF_INTEREST = [
    "eventFlagId", "distViewEventFlagId", "iconId", "bgmPlaceType",
    "isAreaIcon", "isOverrideDistViewMarkPos", "isEnableNoText",
    "areaNo", "gridXNo", "gridZNo",
    "clearedEventFlagId", "dispMask00", "dispMask01",
    "distViewIconId", "angle",
    "posX", "posY", "posZ",
    "textId1", "textEnableFlagId1", "textDisableFlagId1",
    "textId2", "textEnableFlagId2", "textDisableFlagId2",
    "textId3", "textId4", "textId5", "textId6", "textId7", "textId8",
    "textType1", "textType2", "textType3", "textType4",
    "selectMinZoomStep", "dispMinZoomStep", "entryFEType",
]


def load_regulation(reg_path):
    print(f"Loading regulation.bin from {reg_path}...")
    bnd = SFUtil.DecryptERRegulation(str(reg_path))
    print(f"  {bnd.Files.Count} files in regulation")
    return bnd


def find_param(bnd, param_name):
    for f in bnd.Files:
        name = str(f.Name) if f.Name else ""
        if param_name in name:
            print(f"  Found: {name} ({f.Bytes.Length} bytes)")
            return f
    return None


def load_paramdefs(paramdef_dir):
    defs = {}
    for xml_path in paramdef_dir.glob("*.xml"):
        try:
            pdef = SoulsFormats.PARAMDEF.XmlDeserialize(str(xml_path), False)
            if pdef and pdef.ParamType:
                defs[str(pdef.ParamType)] = pdef
        except Exception:
            pass

    print(f"  Loaded {len(defs)} paramdefs")
    return defs


def extract_world_map_point_param(bnd, paramdefs):
    param_file = find_param(bnd, "WorldMapPointParam")
    if not param_file:
        print("ERROR: WorldMapPointParam not found in regulation!")
        return []

    param = _read_from_bytes(_param_read_str, param_file.Bytes, '.param')
    param_type = str(param.ParamType) if param.ParamType else ""
    print(f"  ParamType: {param_type}, Rows: {param.Rows.Count}")

    if paramdefs and param_type in paramdefs:
        param.ApplyParamdef(paramdefs[param_type])
    elif paramdefs:
        for key, pdef in paramdefs.items():
            if "WorldMapPoint" in key:
                try:
                    param.ApplyParamdef(pdef)
                    print(f"  Applied paramdef: {key}")
                    break
                except Exception:
                    pass

    rows = []
    for row in param.Rows:
        entry = {"ID": int(row.ID), "Name": str(row.Name) if row.Name else ""}
        if row.Cells:
            for cell in row.Cells:
                field_name = str(cell.Def.InternalName)
                try:
                    entry[field_name] = cell.Value
                except Exception:
                    entry[field_name] = None
        rows.append(entry)

    return rows


def export_csv(rows, output_path, fields=None):
    if not rows:
        print("No rows to export!")
        return

    if fields:
        header = ["ID", "Name"] + [f for f in fields if f in rows[0]]
    else:
        header = list(rows[0].keys())

    with open(output_path, 'w', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, fieldnames=header, extrasaction='ignore')
        w.writeheader()
        for row in rows:
            clean = {}
            for k in header:
                v = row.get(k, "")
                if hasattr(v, 'ToString'):
                    v = str(v)
                elif v is True:
                    v = "1"
                elif v is False:
                    v = "0"
                elif v is None:
                    v = ""
                clean[k] = v
            w.writerow(clean)

    print(f"Exported {len(rows)} rows to {output_path}")


def main():
    reg_path = ERR_MOD_DIR / "regulation.bin"
    if not reg_path.exists():
        reg_path = GAME_DIR / "regulation.bin"

    if not reg_path.exists():
        print(f"ERROR: regulation.bin not found!")
        sys.exit(1)

    bnd = load_regulation(reg_path)

    print("\nLoading paramdefs...")
    paramdefs = load_paramdefs(PARAMDEF_DIR)

    print("\nExtracting WorldMapPointParam...")
    rows = extract_world_map_point_param(bnd, paramdefs)

    if rows:
        csv_out = OUTPUT_DIR / "WorldMapPointParam.csv"
        export_csv(rows, csv_out, FIELDS_OF_INTEREST)

        json_out = OUTPUT_DIR / "WorldMapPointParam.json"
        json_rows = []
        for r in rows:
            clean = {}
            for k, v in r.items():
                if hasattr(v, 'ToString'):
                    clean[k] = str(v)
                elif isinstance(v, bool):
                    clean[k] = v
                else:
                    clean[k] = v
            json_rows.append(clean)
        with open(json_out, 'w', encoding='utf-8') as f:
            json.dump(json_rows, f, indent=2, default=str)
        print(f"Exported {len(json_rows)} rows to {json_out}")

    print(f"\n{len(rows)} WorldMapPointParam entries extracted.")


if __name__ == "__main__":
    main()
