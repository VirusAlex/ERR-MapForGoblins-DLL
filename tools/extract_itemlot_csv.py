#!/usr/bin/env python3
"""
Extract ItemLotParam_map from ERR regulation.bin and save as CSV.
Uses Andre.SoulsFormats.dll + local paramdefs (no external tools needed).

Usage:
    python extract_itemlot_csv.py [path/to/regulation.bin]

If no path given, uses ERR_MOD_DIR from config or prompts.
Output: ../data/ItemLotParam_map.csv
"""

import csv
import sys
import os
import tempfile
from pathlib import Path

import config

SCRIPT_DIR = Path(__file__).parent
DATA_DIR = config.DATA_DIR
LIB_DIR = config.LIB_DIR
PARAMDEF_DIR = config.PARAMDEF_DIR

# .NET init
from pythonnet import load
load('coreclr')
import clr
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object
from System.IO import File as SysFile

dll_path = str(LIB_DIR / "Andre.SoulsFormats.dll")
asm = Assembly.LoadFrom(dll_path)
clr.AddReference(dll_path)
import SoulsFormats

# PARAM.Read(string) via reflection (Andre.SoulsFormats uses Memory<byte>, not byte[])
_param_cls = asm.GetType('SoulsFormats.PARAM')
_str_type = SysType.GetType('System.String')
_param_read_str = _param_cls.GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None)


def load_paramdefs():
    """Load all PARAMDEF XMLs from tools/paramdefs."""
    defs = {}
    for xml_path in PARAMDEF_DIR.glob("*.xml"):
        try:
            pdef = SoulsFormats.PARAMDEF.XmlDeserialize(str(xml_path), False)
            if pdef and pdef.ParamType:
                defs[str(pdef.ParamType)] = pdef
        except Exception:
            pass
    return defs


def read_param(bnd, param_name, paramdefs):
    for f in bnd.Files:
        if param_name in str(f.Name):
            tmp = os.path.join(tempfile.gettempdir(), f'{param_name}.param')
            SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
            param = _param_read_str.Invoke(None, Array[Object]([tmp]))
            os.unlink(tmp)
            pt = str(param.ParamType) if param.ParamType else ''
            if pt in paramdefs:
                param.ApplyParamdef(paramdefs[pt])
            return param
    return None


def main():
    if len(sys.argv) > 1:
        reg_path = Path(sys.argv[1])
    else:
        default = config.require_err_mod_dir() / 'regulation.bin'
        if default.exists():
            reg_path = default
        else:
            print("Usage: python extract_itemlot_csv.py <path/to/regulation.bin>")
            sys.exit(1)

    print(f"Loading regulation.bin from {reg_path}...")
    bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(reg_path))
    print(f"  {bnd.Files.Count} files")

    print("Loading paramdefs...")
    paramdefs = load_paramdefs()
    print(f"  {len(paramdefs)} paramdefs")

    print("Reading ItemLotParam_map...")
    ilp = read_param(bnd, 'ItemLotParam_map', paramdefs)
    if not ilp:
        print("ERROR: ItemLotParam_map not found in regulation.bin")
        sys.exit(1)

    field_names = []
    if ilp.Rows.Count > 0:
        first_row = ilp.Rows[0]
        if first_row.Cells:
            for cell in first_row.Cells:
                field_names.append(str(cell.Def.InternalName))

    print(f"  {ilp.Rows.Count} rows, {len(field_names)} fields per row")

    # Only output lots containing Rune Piece (800010) or Ember Piece (850010)
    output_path = DATA_DIR / "ItemLotParam_map.csv"
    DATA_DIR.mkdir(parents=True, exist_ok=True)

    header = ["ID", "Name"] + field_names
    written = 0

    with open(output_path, 'w', newline='', encoding='utf-8') as f:
        writer = csv.writer(f)
        writer.writerow(header)

        for row in ilp.Rows:
            row_id = int(row.ID)
            row_name = str(row.Name) if row.Name else ''

            # Check if this lot contains goods 800010 or 850010
            has_piece = False
            values = {}
            if row.Cells:
                for cell in row.Cells:
                    fn = str(cell.Def.InternalName)
                    val = cell.Value
                    if hasattr(val, 'ToString'):
                        val = str(val)
                    values[fn] = val

                for slot in range(1, 9):
                    item_id_str = values.get(f'lotItemId0{slot}', '0')
                    try:
                        item_id = int(item_id_str)
                    except (ValueError, TypeError):
                        item_id = 0
                    if item_id in (800010, 850010):
                        has_piece = True
                        break

            if not has_piece:
                continue

            csv_row = [row_id, row_name]
            for fn in field_names:
                csv_row.append(values.get(fn, ''))
            writer.writerow(csv_row)
            written += 1

    print(f"\nWritten {written} rows to {output_path}")


if __name__ == "__main__":
    main()
