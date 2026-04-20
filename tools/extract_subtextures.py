#!/usr/bin/env python3
"""
Extract sub-textures from Scaleform sprite sheets for FFDEC preview.

Reads layout XMLs from sblytbnd.dcx, crops sub-textures from DDS sprite sheets,
and saves as TGA files next to the GFX file so FFDEC can display them.

Processes both vanilla and ERR mod textures.
"""

import os
import sys
import json
import xml.etree.ElementTree as ET
import tempfile
import shutil

sys.path.insert(0, os.path.dirname(__file__))

from PIL import Image
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


def _get_read(tn):
    return asm.GetType(tn).GetMethod(
        "Read", BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
        None, Array[SysType]([_str_type]), None)


def process_sblytbnd(sblytbnd_path, dds_dir, out_dir):
    """Read sblytbnd, crop sub-textures from DDS, save as TGA."""
    _bnd4_read = _get_read("SoulsFormats.BND4")
    bnd = _bnd4_read.Invoke(None, Array[Object]([sblytbnd_path]))

    total = 0
    for f in bnd.Files:
        base = os.path.basename(str(f.Name)).replace(".layout", "")
        layout_data = bytes(f.Bytes.ToArray()).decode("utf-8")

        dds_path = os.path.join(dds_dir, f"{base}.dds")
        if not os.path.exists(dds_path):
            continue

        try:
            sheet = Image.open(dds_path)
        except:
            continue

        root = ET.fromstring(layout_data)
        for sub in root.findall(".//SubTexture"):
            name = sub.get("name", "").replace(".png", "")
            x = int(sub.get("x", 0))
            y = int(sub.get("y", 0))
            w = int(sub.get("width", 0))
            h = int(sub.get("height", 0))
            if w == 0 or h == 0:
                continue

            tga_path = os.path.join(out_dir, f"{name}.tga")
            if os.path.exists(tga_path):
                continue

            icon = sheet.crop((x, y, x + w, y + h))
            icon.save(tga_path)
            total += 1

    return total


def main():
    project_dir = config.PROJECT_DIR
    out_dir = str(project_dir / "assets" / "menu")
    os.makedirs(out_dir, exist_ok=True)

    src = config.GAME_DIR / "oo2core_6_win64.dll"
    for d in [config.LIB_DIR, tempfile.gettempdir(), os.getcwd()]:
        dst = os.path.join(str(d), "oo2core_6_win64.dll")
        if not os.path.exists(dst):
            shutil.copy2(str(src), dst)

    # 1. Vanilla textures
    vanilla_sblyt = str(config.GAME_DIR / "menu" / "hi" / "01_common.sblytbnd.dcx")
    vanilla_dds_dir = str(config.GAME_DIR / "menu" / "hi" / "01_common-tpf-dcx")
    if os.path.exists(vanilla_sblyt) and os.path.exists(vanilla_dds_dir):
        count = process_sblytbnd(vanilla_sblyt, vanilla_dds_dir, out_dir)
        print(f"Vanilla sub-textures: {count}")

    # 2. ERR mod textures
    err_sblyt = str(config.require_err_mod_dir() / "menu" / "hi" / "01_common.sblytbnd.dcx")
    if os.path.exists(err_sblyt):
        # Extract ERR TPF to temp
        err_tpf_path = str(config.require_err_mod_dir() / "menu" / "hi" / "01_common.tpf.dcx")
        if os.path.exists(err_tpf_path):
            _tpf_read = _get_read("SoulsFormats.TPF")
            tpf = _tpf_read.Invoke(None, Array[Object]([err_tpf_path]))
            err_dds_dir = os.path.join(tempfile.gettempdir(), "err_tpf")
            os.makedirs(err_dds_dir, exist_ok=True)
            for tex in tpf.Textures:
                name = str(tex.Name)
                dds_path = os.path.join(err_dds_dir, f"{name}.dds")
                with open(dds_path, "wb") as out:
                    out.write(bytes(tex.Bytes))

            count = process_sblytbnd(err_sblyt, err_dds_dir, out_dir)
            print(f"ERR sub-textures: {count}")

            # 3. ERR MapCursor_ERR icons (cut from ERR SB_MapCursor.dds using ERR layout)
            _bnd4_read = _get_read("SoulsFormats.BND4")
            bnd = _bnd4_read.Invoke(None, Array[Object]([err_sblyt]))
            for f in bnd.Files:
                if "MapCursor_ERR" not in str(f.Name):
                    continue
                layout = bytes(f.Bytes.ToArray()).decode("utf-8")
                root = ET.fromstring(layout)
                # These icons are at coordinates within ERR's SB_MapCursor.dds
                err_mc = os.path.join(err_dds_dir, "SB_MapCursor.dds")
                if not os.path.exists(err_mc):
                    break
                sheet = Image.open(err_mc)
                err_count = 0
                for sub in root.findall(".//SubTexture"):
                    sname = sub.get("name", "").replace(".png", "")
                    x = int(sub.get("x", 0))
                    y = int(sub.get("y", 0))
                    w = int(sub.get("width", 0))
                    h = int(sub.get("height", 0))
                    if w == 0 or h == 0:
                        continue
                    tga_path = os.path.join(out_dir, f"{sname}.tga")
                    icon = sheet.crop((x, y, x + w, y + h))
                    icon.save(tga_path)
                    err_count += 1
                print(f"ERR MapCursor icons: {err_count}")
                break

    print("Done.")


if __name__ == "__main__":
    main()
