#!/usr/bin/env python3
"""Extract localized enemy NAMES (FromSoft / community-wiki data) for every
language, to embed as the non-ERR builds' enemy-label source.

We pull ONLY enemy-name entries - ids following the model-name convention
`model*1000 + variant*100 + 4` (model 1000-9999, variant 0-9) - and strip the
leading list-number prefix ("118. Margit, the Fell Omen" -> "Margit, the Fell
Omen"). Tutorial tips and any category-header entries are NOT extracted. The
output is a plain enemy-name table; consumers never reference where the source
msgbnd came from.

Output (committed, profile-independent): data/enemy_names_i18n.json
  { "<id>": { "engus": "Margit, the Fell Omen", "jpnjp": "...", ... }, ... }
"""
import sys, io, os, re, json, tempfile
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

PREFIX = re.compile(r'^\d+[a-z]?\.\s*')

# English-only spelling corrections to the community/wiki-canonical form, applied
# after extraction (the source strings use a shortened form for these). Verified
# against the Fandom/Fextralife enemy lists. Other languages keep their string.
EN_SUBSTR = [(" Tear Scarab", " Teardrop Scarab")]  # c4191 = "Teardrop Scarab"
EN_EXACT = {"Catacomb Sorcerer": "Catacombs Sorcerer"}

def fix_english(name):
    for a, b in EN_SUBSTR:
        name = name.replace(a, b)
    return EN_EXACT.get(name, name)

def is_enemy_id(i):
    if i % 100 != 4:
        return False
    model = i // 1000
    variant = (i % 1000) // 100
    return 1000 <= model <= 9999 and variant <= 9

def main():
    src = config.require_err_mod_dir()
    msg_root = src / 'msg'
    langs = sorted(p.name for p in msg_root.iterdir() if p.is_dir())
    print(f'languages: {langs}')

    table = {}  # id -> {lang: name}
    for lang in langs:
        mp = msg_root / lang / 'menu_dlc02.msgbnd.dcx'
        if not mp.exists():
            mp = msg_root / lang / 'menu.msgbnd.dcx'
        if not mp.exists():
            print(f'  {lang}: no menu msgbnd, skipped'); continue
        msg = _br.Invoke(None, Array[Object]([str(mp)]))
        count = 0
        for f in msg.Files:
            if not os.path.basename(str(f.Name)).startswith('TutorialTitle'):
                continue
            tmp = os.path.join(tempfile.gettempdir(), f'{os.getpid()}_{lang}_tt.fmg')
            SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
            fmg = _fr.Invoke(None, Array[Object]([tmp]))
            for e in fmg.Entries:
                i = int(e.ID)
                if not is_enemy_id(i):
                    continue
                t = str(e.Text) if e.Text else ''
                if not t or t == '[ERROR]':
                    continue
                name = PREFIX.sub('', t).strip()
                if not name or '<' in name:   # skip any markup-bearing entries
                    continue
                if lang == 'engus':
                    name = fix_english(name)
                table.setdefault(str(i), {})[lang] = name
                count += 1
        print(f'  {lang}: {count} enemy names')

    out = config.PROJECT_DIR / 'data' / 'enemy_names_i18n.json'
    with open(out, 'w', encoding='utf-8') as f:
        json.dump(table, f, ensure_ascii=False, indent=0)
    # coverage stats
    engus = sum(1 for v in table.values() if 'engus' in v)
    print(f'wrote {out}: {len(table)} ids, {engus} with English, langs/id avg '
          f'{sum(len(v) for v in table.values())/max(1,len(table)):.1f}')

if __name__ == '__main__':
    main()
