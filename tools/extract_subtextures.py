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


# DX10 DXGI_FORMAT -> texture2ddecoder decoder. ER menu textures are commonly
# BC7 (98/99); PIL silently decodes those as garbage static, so we must decode
# them ourselves. (BC1/3/5/6 mapped too in case other sheets use them.)
_DXGI_DECODER = {
    70: "decode_bc1", 71: "decode_bc1", 72: "decode_bc1",
    76: "decode_bc3", 77: "decode_bc3", 78: "decode_bc3",
    79: "decode_bc4", 80: "decode_bc4", 81: "decode_bc4",
    82: "decode_bc5", 83: "decode_bc5", 84: "decode_bc5",
    94: "decode_bc6", 95: "decode_bc6", 96: "decode_bc6",
    97: "decode_bc7", 98: "decode_bc7", 99: "decode_bc7",
}


def load_dds(data):
    """Decode DDS bytes to a PIL RGBA image, handling DX10 BC7/BC-family (which
    PIL renders as garbage). Falls back to PIL for formats it understands
    (uncompressed / classic DXT)."""
    import struct, io
    if data[:4] != b"DDS ":
        return Image.open(io.BytesIO(data)).convert("RGBA")
    h = struct.unpack("<I", data[12:16])[0]
    w = struct.unpack("<I", data[16:20])[0]
    fourcc = data[84:88]
    if fourcc == b"DX10":
        dxgi = struct.unpack("<I", data[128:132])[0]
        dec = _DXGI_DECODER.get(dxgi)
        if dec:
            import texture2ddecoder
            raw = getattr(texture2ddecoder, dec)(data[148:], w, h)
            return Image.frombytes("RGBA", (w, h), raw, "raw", "BGRA")
        return Image.frombytes("RGBA", (w, h), data[148:148 + w * h * 4], "raw", "BGRA")
    # classic FourCC (DXT1/3/5) or uncompressed -> PIL handles these correctly
    return Image.open(io.BytesIO(data)).convert("RGBA")


def load_dds_file(path):
    with open(path, "rb") as fh:
        return load_dds(fh.read())


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
            sheet = load_dds_file(dds_path)
        except Exception:
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


def _ensure_oo2core():
    """Make oo2core_6_win64.dll discoverable for DCX decompression."""
    src = config.GAME_DIR / "oo2core_6_win64.dll"
    if not src.exists():
        return
    for d in [config.LIB_DIR, tempfile.gettempdir(), os.getcwd()]:
        dst = os.path.join(str(d), "oo2core_6_win64.dll")
        if not os.path.exists(dst):
            shutil.copy2(str(src), dst)


def extract_menu_dir(menu_hi_dir, out_dir):
    """Extract sub-textures from a menu/hi dir (vanilla game or a mod overlay)
    into out_dir as <name>.tga, using 01_common.sblytbnd's layouts. Pulls DDS
    sheets from the unpacked `01_common-tpf-dcx/` folder AND every `*.tpf.dcx` in
    the dir - the layouts reference sheets spread across multiple tpfs (e.g. ERR's
    `SB_MapCursor_ERR` map markers, incl. the green "Completed" check, live in
    `05_dummy.tpf.dcx`, not `01_common.tpf.dcx`). Returns sub-textures written
    (0 if no 01_common.sblytbnd). Importable by render_map_icons."""
    import glob
    _ensure_oo2core()
    menu_hi_dir = str(menu_hi_dir)
    os.makedirs(out_dir, exist_ok=True)
    sblyt = os.path.join(menu_hi_dir, "01_common.sblytbnd.dcx")
    if not os.path.exists(sblyt):
        return 0

    # Gather every available DDS sheet into one dir so process_sblytbnd can find
    # whichever sheet each layout references, wherever it's packed.
    dds_dir = os.path.join(tempfile.gettempdir(),
                           f"{os.getpid()}_mfg_tpf_{abs(hash(menu_hi_dir)) % 99999}")
    os.makedirs(dds_dir, exist_ok=True)
    unpacked = os.path.join(menu_hi_dir, "01_common-tpf-dcx")
    if os.path.isdir(unpacked):
        for f in os.listdir(unpacked):
            if f.lower().endswith(".dds") and not os.path.exists(os.path.join(dds_dir, f)):
                shutil.copy2(os.path.join(unpacked, f), os.path.join(dds_dir, f))
    _tpf_read = _get_read("SoulsFormats.TPF")
    for tpf_path in glob.glob(os.path.join(menu_hi_dir, "*.tpf.dcx")):
        try:
            tpf = _tpf_read.Invoke(None, Array[Object]([tpf_path]))
        except Exception:
            continue
        for tex in tpf.Textures:
            dst = os.path.join(dds_dir, f"{str(tex.Name)}.dds")
            if not os.path.exists(dst):
                with open(dst, "wb") as out:
                    out.write(bytes(tex.Bytes))
    return process_sblytbnd(sblyt, dds_dir, out_dir)


def main():
    project_dir = config.PROJECT_DIR
    out_dir = str(project_dir / "assets" / "menu")
    os.makedirs(out_dir, exist_ok=True)

    _ensure_oo2core()

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
            err_dds_dir = os.path.join(tempfile.gettempdir(), str(os.getpid()) + "err_tpf")
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
