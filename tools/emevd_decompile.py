#!/usr/bin/env python3
"""EMEVD decompiler - DarkScript3-style listings of Elden Ring event scripts.

Usage:
  py emevd_decompile.py <map>                   # decompile all events in ERR
  py emevd_decompile.py <map> --event 30082550  # one specific event
  py emevd_decompile.py <map> --events 30082550 30082501
  py emevd_decompile.py <map> --grep 30080460   # events whose body references this value
  py emevd_decompile.py <map> --source vanilla  # vanilla regulation instead of ERR
  py emevd_decompile.py <map> --diff            # side-by-side ERR vs vanilla diff

Resolves:
  - Instruction names + arg types via er-common.emedf.json (in DarkScript3 Resources)
  - Enum values (Death State, Comparison Type, BOOL, ...) via EMEDF enums
  - Named entity IDs via er-common.CharaName.txt (Tanith, Vyke, ...)
  - Model names (c4020 → Royal Revenant) via er-common.ModelName.txt
  - Dev comments per event via emeld_er/<map>.emeld.dcx.txt
  - Anonymous enemies via MSB lookup (c4020_9000 etc. → eid → human-friendly)

Param substitution placeholders shown as Xsrc_len (e.g. X0_4 = "4 bytes from caller offset 0").

Why this exists: when answering "what does event X do" questions, ALWAYS run this
first instead of guessing semantics from raw 2003:69 / 4:0 / etc. byte dumps.
"""
import sys, io, os, tempfile, struct, json, argparse
from pathlib import Path
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

import config
from pythonnet import load
load('coreclr')
import clr
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object
from System.IO import File as SysFile

clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats
asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))

_str = SysType.GetType('System.String')
_emevd_read = asm.GetType('SoulsFormats.EMEVD').GetMethod(
    'Read', BindingFlags.Public|BindingFlags.Static|BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str]), None)
_msbe_read = asm.GetType('SoulsFormats.MSBE').GetMethod(
    'Read', BindingFlags.Public|BindingFlags.Static|BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str]), None)


# DarkScript3 Resources/ (optional). If configured, used for entity / model /
# event-comment name resolution. The core EMEDF JSON ships in the repo at
# tools/er-common.emedf.json, so instruction decoding works without it.
import config as _goblin_config
DARKSCRIPT_RESOURCES = _goblin_config.DARKSCRIPT_RESOURCES
_LOCAL_EMEDF = Path(__file__).parent / 'er-common.emedf.json'

# EMEDF arg-type sizes (bytes). Verified against 4:0, 4:5, 4:7 instructions and
# SoulsFormats EMEVD output. Each type aligns to its natural width.
TYPE_SIZE = {0: 1, 1: 2, 2: 4, 3: 1, 4: 4, 5: 4, 6: 4}


# ── EMEDF & resources ──
def load_emedf():
    # Prefer the repo-local copy so the script works without DarkScript3 installed.
    emedf_path = _LOCAL_EMEDF
    if not emedf_path.exists() and DARKSCRIPT_RESOURCES:
        emedf_path = DARKSCRIPT_RESOURCES / 'er-common.emedf.json'
    emedf = json.load(open(emedf_path, encoding='utf-8'))
    instr_by_id = {}
    for cls in emedf['main_classes']:
        for ins in cls['instrs']:
            instr_by_id[(int(cls['index']), int(ins['index']))] = ins
    enums_by_name = {e['name']: {int(k): v for k, v in e['values'].items()}
                     for e in emedf.get('enums', [])}
    return instr_by_id, enums_by_name


def load_name_map(filename):
    """Read DarkScript3 resource file: '<id> <name>' per line.

    Returns an empty dict if DarkScript3 isn't configured - callers degrade
    gracefully to raw numeric IDs."""
    out = {}
    if not DARKSCRIPT_RESOURCES: return out
    p = DARKSCRIPT_RESOURCES / filename
    if not p.exists(): return out
    for line in p.read_text(encoding='utf-8-sig').splitlines():
        line = line.strip()
        if not line: continue
        sp = line.split(None, 1)
        if len(sp) != 2: continue
        try: out[int(sp[0])] = sp[1]
        except ValueError:
            out[sp[0]] = sp[1]  # for model entries like "c0000"
    return out


def load_event_comments(map_name):
    """Map event_id → 'jp_comment -- english_comment' from emeld_er/."""
    out = {}
    if not DARKSCRIPT_RESOURCES: return out
    p = DARKSCRIPT_RESOURCES / 'emeld_er' / f'{map_name}.emeld.dcx.txt'
    if not p.exists(): return out
    for line in p.read_text(encoding='utf-8-sig').splitlines():
        line = line.strip()
        if not line: continue
        sp = line.split(None, 1)
        if len(sp) != 2: continue
        try: out[int(sp[0])] = sp[1]
        except ValueError: pass
    return out


# ── MSB entity name resolution ──
_msb_entity_cache = {}

def msb_entity_name(map_name, entity_id, model_names):
    """Return 'c4020_9000 (Royal Revenant)' for entity 30080460 in m30_08_00_00."""
    if entity_id <= 0: return None
    if map_name not in _msb_entity_cache:
        _msb_entity_cache[map_name] = {}
        p = config.ERR_MOD_DIR / 'map' / 'MapStudio' / f'{map_name}.msb.dcx'
        if not p.exists():
            return None
        try:
            data = SoulsFormats.DCX.Decompress(str(p)).ToArray()
            tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_dec.msb')
            SysFile.WriteAllBytes(tmp, data)
            msb = _msbe_read.Invoke(None, Array[Object]([tmp]))
        except Exception:
            return None
        for cat in ('Enemies', 'DummyEnemies', 'Assets', 'DummyAssets'):
            coll = getattr(msb.Parts, cat, None) or []
            for part in coll:
                try:
                    eid = int(getattr(part, 'EntityID', 0) or 0)
                    if eid > 0:
                        _msb_entity_cache[map_name][eid] = (str(part.Name), str(getattr(part, 'ModelName', '')))
                except Exception: pass
        # Also Regions
        for rname in dir(msb.Regions):
            if rname.startswith('_'): continue
            try: coll = getattr(msb.Regions, rname)
            except: continue
            if not hasattr(coll, '__iter__'): continue
            try:
                for r in coll:
                    try:
                        eid = int(getattr(r, 'EntityID', 0) or 0)
                        if eid > 0:
                            _msb_entity_cache[map_name][eid] = (str(r.Name), f'Region.{rname}')
                    except: pass
            except: pass
    info = _msb_entity_cache[map_name].get(entity_id)
    if not info: return None
    part_name, model_or_kind = info
    pretty = model_names.get(model_or_kind, '')
    if pretty:
        return f'{part_name} ({pretty})'
    return part_name


# ── EMEVD I/O ──
def load_emevd(path):
    data = SoulsFormats.DCX.Decompress(str(path)).ToArray()
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_dec.emevd')
    SysFile.WriteAllBytes(tmp, data)
    return _emevd_read.Invoke(None, Array[Object]([tmp]))


def event_emevd_path(map_name, source='err'):
    root = config.ERR_MOD_DIR if source == 'err' else config.GAME_DIR
    return root / 'event' / f'{map_name}.emevd.dcx'


# ── Decoding ──
def get_param_substitutions(event):
    """Build {(instr_idx, byte_offset): placeholder_str} from event.Parameters."""
    subs = {}
    try:
        if not event.Parameters: return subs
        for prm in event.Parameters:
            ii = int(prm.InstructionIndex)
            tgt = int(prm.TargetStartByte)
            src = int(prm.SourceStartByte)
            n = int(prm.ByteCount)
            subs[(ii, tgt)] = f'X{src}_{n}'
    except Exception: pass
    return subs


def decode_args(args, spec_args, instr_idx, subs, enums, chara_names, model_names, map_name):
    """Decode a single instruction's ArgData into a list of (name, formatted_value) pairs."""
    out = []
    offset = 0
    for a in spec_args:
        t = a['type']
        sz = TYPE_SIZE.get(t, 4)
        # align to natural width (min 1)
        align = sz if sz <= 4 else 4
        if offset % align != 0:
            offset = ((offset + align - 1) // align) * align
        if offset + sz > len(args):
            out.append((a['name'], '?(truncated)'))
            break
        sub_key = (instr_idx, offset)
        if sub_key in subs:
            out.append((a['name'], subs[sub_key]))
            offset += sz
            continue
        raw = args[offset:offset+sz]
        if t == 6:
            val = struct.unpack('<f', raw)[0]
            val_str = f'{val:.3g}f'
        elif t in (5, 2, 4):
            val = struct.unpack('<i', raw)[0] if t == 5 else struct.unpack('<I', raw)[0]
            val_str = format_int(val, a, t, chara_names, model_names, map_name)
        elif t == 1:
            val = struct.unpack('<h', raw)[0]
            val_str = str(val)
        elif t == 3:
            val = struct.unpack('<b', raw)[0]
            val_str = format_int(val, a, t, chara_names, model_names, map_name)
        elif t == 0:
            val = raw[0]
            val_str = format_int(val, a, t, chara_names, model_names, map_name)
        else:
            val_str = '?(unknown_type)'
        out.append((a['name'], val_str))
        offset += sz
    return out


def format_int(val, arg_spec, type_id, chara_names, model_names, map_name):
    enum_name = arg_spec.get('enum_name')
    if enum_name and enum_name in _ENUMS:
        emap = _ENUMS[enum_name]
        if val in emap:
            return f'{emap[val]}({val})'
    # Entity ID resolution
    if type_id == 2 and val > 1000:  # likely a real entity ID, not slot
        if val in chara_names:
            return f'{val} /* {chara_names[val]} */'
        if val == 10000:
            return '10000 /* Player */'
        nm = msb_entity_name(map_name, val, model_names)
        if nm:
            return f'{val} /* {nm} */'
    return str(val)


# Globals populated in main(), used by format_int via closure (gross but fine)
_ENUMS = {}


def render_event(event, instr_by_id, enums, chara_names, model_names, comments, map_name):
    eid = int(event.ID)
    rest = str(event.RestBehavior) if hasattr(event, 'RestBehavior') else '?'
    comment = comments.get(eid, '').strip()
    header_comment = f'  // {comment}' if comment else ''
    out = [f'$Event({eid}, {rest}, function() {{{header_comment}']

    subs = get_param_substitutions(event)
    for i, ins in enumerate(event.Instructions):
        bank = int(ins.Bank); iid = int(ins.ID)
        spec = instr_by_id.get((bank, iid))
        args_bytes = bytes(ins.ArgData)
        if not spec:
            out.append(f'    {bank}:{iid} args(len={len(args_bytes)})  // unknown instruction')
            continue
        name = spec['name'].replace(' ', '').replace('/', '')  # CamelCase-ish
        decoded = decode_args(args_bytes, spec['args'], i, subs, enums,
                              chara_names, model_names, map_name)
        arg_str = ', '.join(f'{v}' for _, v in decoded)
        out.append(f'    [{i:2}] {name}({arg_str});')
    out.append('});')
    return '\n'.join(out)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('map_name')
    p.add_argument('--event', type=int)
    p.add_argument('--events', type=int, nargs='+')
    p.add_argument('--grep', type=int, help='show events whose body contains this int value')
    p.add_argument('--source', choices=['err', 'vanilla'], default='err')
    p.add_argument('--all', action='store_true', help='dump every event (default if no filter)')
    p.add_argument('--diff', action='store_true', help='side-by-side ERR vs vanilla for matching events')
    args = p.parse_args()

    instr_by_id, enums = load_emedf()
    global _ENUMS
    _ENUMS = enums
    chara_names = load_name_map('er-common.CharaName.txt')
    model_names = load_name_map('er-common.ModelName.txt')
    comments = load_event_comments(args.map_name)

    if args.diff:
        for src in ('vanilla', 'err'):
            path = event_emevd_path(args.map_name, src)
            print(f'\n{"="*78}\n>>> SOURCE: {src.upper()} ({path.name})\n{"="*78}')
            if not path.exists():
                print(f'(missing: {path})')
                continue
            dump_emevd(path, args, instr_by_id, enums, chara_names, model_names, comments)
        return

    path = event_emevd_path(args.map_name, args.source)
    if not path.exists():
        print(f'No file: {path}'); return
    dump_emevd(path, args, instr_by_id, enums, chara_names, model_names, comments)


def dump_emevd(path, args, instr_by_id, enums, chara_names, model_names, comments):
    em = load_emevd(path)
    targets = set()
    if args.event is not None: targets.add(args.event)
    if args.events: targets.update(args.events)
    if args.grep is not None:
        for ev in em.Events:
            for ins in ev.Instructions:
                a = bytes(ins.ArgData)
                vals = [struct.unpack_from('<i', a, o)[0] for o in range(0, len(a)-3, 4)]
                if args.grep in vals:
                    targets.add(int(ev.ID)); break
    if not targets and not (args.all or args.grep is not None or args.event or args.events):
        targets = {int(ev.ID) for ev in em.Events}
    elif args.all:
        targets = {int(ev.ID) for ev in em.Events}

    print(f'// File: {path.name}, {em.Events.Count} events')
    rendered = 0
    for ev in em.Events:
        if int(ev.ID) not in targets: continue
        print()
        print(render_event(ev, instr_by_id, enums, chara_names, model_names,
                          comments, args.map_name))
        rendered += 1
    if rendered == 0:
        print('// (no matching events)')


if __name__ == '__main__':
    main()
