#!/usr/bin/env python3
"""Scan every per-tile EMEVD for seal-puzzle template calls and emit
data/seal_puzzles.json.

The puzzle uses two templates in common_func.emevd:
  - 90006050 (controller): one call per puzzle. Params:
      (door_eid, group_id, flag1, flag2, ..., flagN, sub_id)
  - 90006051 (per-seal): one call per seal. Params:
      (seal_eid, activation_flag, sfx_eid, group_id)

A seal belongs to a controller when their `group_id` (param[1] of the
controller, param[-1] of the seal) match.

Output entries include each seal's MSB position so the marker generator
can place icons in-world.
"""
import sys, io, os, tempfile, struct, json
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from pythonnet import load
load('coreclr')
import clr
clr.AddReference(str(config.SOULSFORMATS_DLL))
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object
from System.IO import File as SysFile
import SoulsFormats
asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
_str = SysType.GetType('System.String')
_msbe = asm.GetType('SoulsFormats.MSBE').GetMethod('Read',
    BindingFlags.Public|BindingFlags.Static|BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str]), None)
_emevd = asm.GetType('SoulsFormats.EMEVD').GetMethod('Read',
    BindingFlags.Public|BindingFlags.Static|BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str]), None)

TPL_CONTROLLER = 90006050
TPL_SEAL = 90006051


def _read(reader, path, suf='.tmp'):
    data = SoulsFormats.DCX.Decompress(str(path)).ToArray()
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_sp' + suf)
    SysFile.WriteAllBytes(tmp, data)
    return reader.Invoke(None, Array[Object]([tmp]))


def main():
    ev_dir = config.require_err_mod_dir() / 'event'
    msb_dir = config.require_err_mod_dir() / 'map' / 'MapStudio'
    puzzles_by_tile = {}
    for emfile in sorted(ev_dir.glob('m*.emevd.dcx')):
        tile = emfile.name.replace('.emevd.dcx', '')
        try:
            em = _read(_emevd, emfile)
        except Exception:
            continue
        for evt in em.Events:
            for inst in evt.Instructions:
                if int(inst.Bank) != 2000 or int(inst.ID) != 6:
                    continue
                ab = bytes(inst.ArgData) if inst.ArgData else b''
                if len(ab) < 8:
                    continue
                event_id = struct.unpack_from('<I', ab, 4)[0]
                if event_id not in (TPL_CONTROLLER, TPL_SEAL):
                    continue
                params = [struct.unpack_from('<I', ab, off)[0]
                          for off in range(8, len(ab) - 3, 4)]
                d = puzzles_by_tile.setdefault(tile,
                    {'controllers': [], 'seals': []})
                if event_id == TPL_CONTROLLER:
                    d['controllers'].append(params)
                else:
                    d['seals'].append(params)

    out = []
    for tile in sorted(puzzles_by_tile):
        d = puzzles_by_tile[tile]
        if not d['controllers'] or not d['seals']:
            continue
        msb_path = msb_dir / (tile + '.msb.dcx')
        if not msb_path.exists():
            continue
        msb = _read(_msbe, msb_path, '.msb')
        eid_to_part = {}
        for cat in (msb.Parts.Assets, getattr(msb.Parts, 'DummyAssets', []) or []):
            for a in cat:
                try:
                    eid = int(a.EntityID)
                    if eid > 0:
                        eid_to_part[eid] = {
                            'name': str(a.Name),
                            'model': str(a.ModelName),
                            'x': float(a.Position.X),
                            'y': float(a.Position.Y),
                            'z': float(a.Position.Z),
                        }
                except Exception:
                    pass
        for cparams in d['controllers']:
            door_eid = cparams[0] if cparams else 0
            group_id = cparams[1] if len(cparams) > 1 else 0
            seals = [s for s in d['seals']
                     if len(s) >= 4 and s[-1] == group_id]
            if not seals:
                continue
            out.append({
                'tile': tile,
                'door_eid': door_eid,
                'door_part': eid_to_part.get(door_eid),
                'group_id': group_id,
                'seals': [{
                    'eid': s[0],
                    'flag_activated': s[1],
                    'sfx_eid': s[2] if len(s) > 2 else None,
                    'part': eid_to_part.get(s[0]),
                } for s in seals],
            })

    # Append puzzle types that don't use the 90006050/90006051 template pair
    # but share the same "interact at N spots to unlock something" semantics.
    # Each entry mirrors the seal-puzzle schema so the generator can treat
    # all of them uniformly.
    extra = _extract_extra_puzzles(ev_dir, msb_dir)
    out.extend(extra)

    out_path = config.DATA_DIR / 'seal_puzzles.json'
    with open(out_path, 'w', encoding='utf-8') as f:
        json.dump(out, f, indent=2, ensure_ascii=False)
    seal_count = sum(len(p['seals']) for p in out)
    print(f"  {len(out)} puzzles, {seal_count} individual interact points")
    print(f"  ({len(extra)} extra puzzles from non-template events)")
    print(f"  Saved to {out_path.name}")


# Each entry: (tile, event_id, entity_param_idx, flag_param_idx, label)
# Param indices are 0-based into the params block (X0_4=0, X4_4=1, X8_4=2, ...).
_EXTRA_PUZZLES = [
    # Sellia 3-chalice puzzle (action btn 9520, ForceAnim 60010).
    # Event params: (entity, lit_flag). Entity at X0_4, flag at X4_4.
    ('m60_49_39_00', 1049392302, 0, 1, 'Sellia chalice (big variant)'),
    ('m60_49_39_00', 1049392303, 0, 1, 'Sellia chalice (small variant)'),
    ('m60_50_39_00', 1050392303, 0, 1, 'Sellia chalice (m60_50)'),
    # Snow Town "Seal Release Statue" puzzle (4 statues, action btn 9528).
    # Event params: (entity, sfx_eid, lit_flag). Entity at X0_4, flag at X8_4 (idx 2).
    ('m60_48_57_00', 1048572370, 0, 2, 'Snow Town seal-release statue'),
    # Siofra "Lower/Upper Layer Lantern" ignition (16 lanterns, action btn 9524).
    # Event params: (lit_flag, asset1, asset2). Flag at X0_4 (idx 0), assets at X4_4/X8_4.
    # We use asset1 (idx 1) as the marker anchor - it's the one IFActionButton listens to.
    ('m12_02_00_00', 12022601, 1, 0, 'Siofra lower-layer lantern'),
    ('m12_02_00_00', 12022621, 1, 0, 'Siofra upper-layer lantern'),
]




def _extract_extra_puzzles(ev_dir, msb_dir):
    """Discover non-template puzzles via hardcoded (tile, event_id) handlers.

    For each registered puzzle event, find every InitializeEvent call to it
    and pull the entity ID + lit flag from the configured param slots.
    Match entity IDs to MSB asset positions and emit one puzzle entry per
    event (grouping all its instances under one puzzle group)."""
    from collections import defaultdict
    by_puzzle = defaultdict(list)  # (tile, event_id, label) -> list of (entity, flag)
    for tile, event_id, ent_idx, flag_idx, label in _EXTRA_PUZZLES:
        em_path = ev_dir / (tile + '.emevd.dcx')
        if not em_path.exists(): continue
        try:
            em = _read(_emevd, em_path)
        except Exception:
            continue
        for evt in em.Events:
            for inst in evt.Instructions:
                if int(inst.Bank) != 2000: continue
                ab = bytes(inst.ArgData) if inst.ArgData else b''
                if len(ab) < 8: continue
                ev_target = struct.unpack_from('<I', ab, 4)[0]
                if ev_target != event_id: continue
                params = [struct.unpack_from('<I', ab, off)[0]
                          for off in range(8, len(ab) - 3, 4)]
                if ent_idx >= len(params) or flag_idx >= len(params):
                    continue
                entity = params[ent_idx]
                flag = params[flag_idx]
                if entity and flag:
                    by_puzzle[(tile, event_id, label)].append((entity, flag))

    # Resolve entity IDs to MSB positions per tile
    msb_cache = {}
    def _eid_to_part(tile, eid):
        if tile not in msb_cache:
            p = msb_dir / (tile + '.msb.dcx')
            if not p.exists():
                msb_cache[tile] = {}
                return None
            try:
                msb = _read(_msbe, p, '.msb')
            except Exception:
                msb_cache[tile] = {}
                return None
            d = {}
            for cat in (msb.Parts.Assets, getattr(msb.Parts, 'DummyAssets', []) or []):
                for a in cat:
                    try:
                        e = int(a.EntityID)
                        if e > 0:
                            d[e] = {
                                'name': str(a.Name), 'model': str(a.ModelName),
                                'x': float(a.Position.X), 'y': float(a.Position.Y),
                                'z': float(a.Position.Z),
                            }
                    except Exception: pass
            msb_cache[tile] = d
        return msb_cache[tile].get(eid)

    # Cross-tile lookup: some puzzle assets live in supertiles (e.g., Snow Town
    # AEG110_029 are in m60_24_28_01 with cross-tile prefix m60_48_57_00-).
    # Build a global entity index from ALL m60 supertiles so we can find them.
    global_eid_index = None
    def _global_eid_lookup(eid):
        nonlocal global_eid_index
        if global_eid_index is None:
            global_eid_index = {}
            for p in sorted(msb_dir.glob('m*_*_*_0*.msb.dcx')):
                # Skip already-cached fine tiles to save time
                try:
                    msb = _read(_msbe, p, '.msb')
                except Exception: continue
                for cat in (msb.Parts.Assets, getattr(msb.Parts, 'DummyAssets', []) or []):
                    for a in cat:
                        try:
                            e = int(a.EntityID)
                            if e > 0 and e not in global_eid_index:
                                global_eid_index[e] = {
                                    'name': str(a.Name), 'model': str(a.ModelName),
                                    'x': float(a.Position.X), 'y': float(a.Position.Y),
                                    'z': float(a.Position.Z),
                                    'msb': p.name.replace('.msb.dcx',''),
                                }
                        except Exception: pass
        return global_eid_index.get(eid)

    out = []
    for (tile, event_id, label), pairs in by_puzzle.items():
        seals = []
        for entity, flag in pairs:
            part = _eid_to_part(tile, entity) or _global_eid_lookup(entity)
            if not part: continue
            seals.append({
                'eid': entity, 'flag_activated': flag,
                'sfx_eid': None, 'part': part,
            })
        if not seals: continue
        out.append({
            'tile': tile, 'door_eid': 0, 'door_part': None,
            'group_id': event_id, 'label': label, 'seals': seals,
        })

    return out


if __name__ == '__main__':
    main()
