#!/usr/bin/env python3
"""Extract TutorialTitle (codex) tables from the menu msgbnd.

Outputs (into config.DATA_DIR, profile-aware):
  - tutorial_title_ids.json      [ids] every non-empty TutorialTitle entry
  - tutorial_title_names.json    {id: title text}
  - enemy_tutorial_mapping.json  {"cNNNN": NNNN*1000+4} for ids matching the
        codex naming convention (base enemy entry = model digits * 1000 + 4;
        variants add 100 per variant). ERR's codex follows this convention;
        vanilla has only a handful of tutorial entries, so for the vanilla
        profile these tables are small and the enemy mapping near-empty
        (vanilla enemy labels come from NpcName / BloodMsg instead).

Consumed by generate_loot_massedit.py / generate_pieces_massedit.py for
enemy-drop name lines.
"""
import sys
import io
import os
import re
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


def main():
    src = config.require_err_mod_dir()
    out = config.DATA_DIR
    out.mkdir(parents=True, exist_ok=True)

    msg_path = src / 'msg' / 'engus' / 'menu_dlc02.msgbnd.dcx'
    if not msg_path.exists():
        msg_path = src / 'msg' / 'engus' / 'menu.msgbnd.dcx'

    names = {}
    msg = _br.Invoke(None, Array[Object]([str(msg_path)]))
    for f in msg.Files:
        nm = os.path.basename(str(f.Name))
        if not nm.startswith('TutorialTitle'):
            continue
        tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_tt.fmg')
        SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
        fmg = _fr.Invoke(None, Array[Object]([tmp]))
        for e in fmg.Entries:
            t = str(e.Text) if e.Text else ''
            if t and t != '[ERROR]':
                # ERR codex titles carry a numeric list prefix ("118. Margit,
                # the Fell Omen", variants "219a. Putrid Tree Spirit"). Strip
                # it: consumers match these names against PlaceName text to
                # resolve boss VARIANTS, and the comparison needs the bare name.
                names.setdefault(int(e.ID), re.sub(r'^\d+[a-z]?\.\s*', '', t).strip())

    ids = sorted(names)
    # Codex naming convention: enemy model cNNNN -> base entry NNNN*1000+4,
    # variants at +100 each. Map a model when ANY family member exists - some
    # entries ship variant-only (e.g. only NNNN104 with no NNNN004 base);
    # consumers do their own variant resolution from the base id, and a
    # missing base simply falls through to the text-match strategy.
    families = {}
    for tid in ids:
        if tid % 100 == 4 and (tid // 100) % 10 <= 9:
            model = tid // 1000
            variant = (tid % 1000) // 100
            if 1000 <= model <= 9999 and variant <= 9:
                families.setdefault(model, tid)
    mapping = {f'c{model}': model * 1000 + 4 for model in sorted(families)}

    with open(out / 'tutorial_title_ids.json', 'w', encoding='utf-8') as f:
        json.dump(ids, f)
    with open(out / 'tutorial_title_names.json', 'w', encoding='utf-8') as f:
        json.dump({str(k): names[k] for k in ids}, f, ensure_ascii=False)
    with open(out / 'enemy_tutorial_mapping.json', 'w', encoding='utf-8') as f:
        json.dump(mapping, f)
    print(f'  tutorial_title_ids: {len(ids)}, names: {len(names)}, enemy mapping: {len(mapping)}')


if __name__ == '__main__':
    main()
