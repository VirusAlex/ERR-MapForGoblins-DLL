#!/usr/bin/env python3
"""Dump English (engus) strings for every FMG family a marker textId can reference,
into data/<profile>/english_fallback.json keyed by the SAME offset-encoded id the
markers/DLL use. Injected by setup_messages (via goblin_item_fallback.cpp) as the
lowest-priority PlaceName layer, so a marker whose text has no string in the
PLAYER's language falls back to English instead of resolving to nothing (the engine
draws no icon for a fully text-less marker). Overhauls localize only part of their
custom content, so non-English clients otherwise get blank boss/NPC names,
locations, and interact prompts.

Encoding (must match the DLL bands in goblin_messages.cpp + the marker generators):
  PlaceName        -> raw id (<50M, locations)
  NpcName          -> id + 700M   (boss / NPC names; covers the 1600M "Characters" hi-range too)
  ActionButtonText -> id + 800M   (interact prompts)
Item families (Goods/Weapon/Protector/Accessory/Gem) already ship English via
item_icon_table.json -> goblin_item_fallback.cpp, so they're not re-dumped here.
TutorialTitle (900M) / BloodMsg (950M) are injected from our own multi-language
tables, so they already carry every language and need no English fallback.
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
_fr = asm.GetType('SoulsFormats.FMG').BaseType.GetMethod('Read', Array[SysType]([_str]))
_br = asm.GetType('SoulsFormats.BND4').BaseType.GetMethod('Read', Array[SysType]([_str]))

# (msgbnd filename, FMG-name prefix, encode offset). first-wins across the layers
# within a msgbnd (base before _dlc01/_dlc02), like the DLL's layered copy.
SOURCES = [
    ('item_dlc02.msgbnd.dcx', 'PlaceName', 0),
    ('item_dlc02.msgbnd.dcx', 'NpcName', 700000000),
    ('menu_dlc02.msgbnd.dcx', 'ActionButtonText', 800000000),
]


def _dump_family(msg, prefix, offset, out):
    for f in msg.Files:
        nm = os.path.basename(str(f.Name))
        if not nm.startswith(prefix):
            continue
        # Only the base + this family's DLC layers (PlaceName, PlaceName_dlc01, ...),
        # never a different family that merely shares a prefix.
        stem = nm.split('.')[0]
        if stem != prefix and not stem.startswith(prefix + '_'):
            continue
        tmp = os.path.join(tempfile.gettempdir(), f'{os.getpid()}_{prefix}.fmg')
        SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
        fmg = _fr.Invoke(None, Array[Object]([tmp]))
        for e in fmg.Entries:
            t = str(e.Text) if e.Text else ''
            if t and t != '[ERROR]':
                out.setdefault(int(e.ID) + offset, t)


def main():
    src = config.require_err_mod_dir()
    out_dir = config.DATA_DIR
    out_dir.mkdir(parents=True, exist_ok=True)

    table = {}
    cache = {}
    for fname, prefix, offset in SOURCES:
        path = src / 'msg' / 'engus' / fname
        if not path.exists():
            print(f'  SKIP {prefix}: {path} not found')
            continue
        if fname not in cache:
            cache[fname] = _br.Invoke(None, Array[Object]([str(path)]))
        before = len(table)
        _dump_family(cache[fname], prefix, offset, table)
        print(f'  {prefix} (+{offset}): {len(table) - before} entries')

    out_path = out_dir / 'english_fallback.json'
    with open(out_path, 'w', encoding='utf-8') as f:
        json.dump({str(k): table[k] for k in sorted(table)}, f, ensure_ascii=False, indent=2)
    print(f'  english_fallback: {len(table)} total entries -> {out_path}')


if __name__ == '__main__':
    main()
