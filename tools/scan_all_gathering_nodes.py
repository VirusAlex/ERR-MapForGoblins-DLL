#!/usr/bin/env python3
"""
Scan all UXM-extracted MSBs for AEG099 and AEG463 gathering nodes.

Output: data/all_gathering_nodes_final.json
"""

import os
import sys
import json
import glob
import re

sys.path.insert(0, os.path.dirname(__file__))

from pythonnet import load
load("coreclr")
import clr
from System import Array, Object
from System import Type as SysType
from System.Reflection import Assembly, BindingFlags

import config
import shutil
import tempfile

dll_path = str(config.SOULSFORMATS_DLL)
asm = Assembly.LoadFrom(dll_path)
clr.AddReference(dll_path)
import SoulsFormats

_str_type = SysType.GetType("System.String")
_msb_read = asm.GetType("SoulsFormats.MSBE").GetMethod(
    "Read", BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None)

src = config.GAME_DIR / "oo2core_6_win64.dll"
for d in [config.LIB_DIR, tempfile.gettempdir(), os.getcwd()]:
    dst = os.path.join(str(d), "oo2core_6_win64.dll")
    if not os.path.exists(dst):
        shutil.copy2(str(src), dst)


def main():
    project_dir = config.PROJECT_DIR
    data_dir = project_dir / "data"

    # Load gathering model sets
    with open(data_dir / "aeg099_item_mapping.json") as f:
        aeg099_models = set(e["model"] for e in json.load(f))
    aeg463_prefix = "AEG463_"

    msb_dir = str(config.GAME_DIR / "map" / "MapStudio")
    pat = re.compile(r"^m(\d+)_(\d+)_(\d+)_(\d+)$")

    all_nodes = []
    errors = 0
    files = sorted(glob.glob(os.path.join(msb_dir, "*.msb.dcx")))
    print(f"Scanning {len(files)} MSBs for AEG099 + AEG463...")

    for i, fpath in enumerate(files):
        fname = os.path.basename(fpath).replace(".msb.dcx", "")
        m = pat.match(fname)
        if not m:
            continue
        area, p1, p2, p3 = int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4))

        try:
            msb = _msb_read.Invoke(None, Array[Object]([fpath]))
            for part in msb.Parts.Assets:
                model = str(part.ModelName)
                is_gather = model in aeg099_models or model.startswith(aeg463_prefix)
                if not is_gather:
                    continue
                pos = part.Position
                all_nodes.append({
                    "model": model, "name": str(part.Name), "map": fname,
                    "area": area, "p1": p1, "p2": p2, "p3": p3,
                    "x": float(pos.X), "y": float(pos.Y), "z": float(pos.Z),
                })
        except:
            errors += 1
        if (i + 1) % 300 == 0:
            print(f"  {i+1}/{len(files)}, {len(all_nodes)} nodes...")

    print(f"Done. {len(all_nodes)} total gathering nodes, {errors} errors")

    out_path = data_dir / "all_gathering_nodes_final.json"
    with open(out_path, "w") as f:
        json.dump(all_nodes, f)
    print(f"Saved to {out_path}")

    from collections import Counter
    by_type = Counter("AEG099" if n["model"].startswith("AEG099") else "AEG463" for n in all_nodes)
    print(f"AEG099: {by_type['AEG099']}, AEG463: {by_type['AEG463']}")


if __name__ == "__main__":
    main()
