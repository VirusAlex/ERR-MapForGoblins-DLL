#!/usr/bin/env python3
"""Generate World - Hero's Tomb Statues.MASSEDIT.

Each statue uses the common_func template 90005683 ("英雄の墓_指示像" =
Hero's Tomb Instruction Statue) - when the player activates it, an SFX
arrow appears pointing toward a hidden Hero's Tomb cave entrance.

Template call signature (params, byte-offset N in args block):
  X0_4   visgate flag (prereq for the statue to be usable)
  X4_4   statue entity ID (the AEG099_055 / equivalent asset)
  X8_4   SFX dmypoly id (where the arrow effect anchors)
  X12_4  activated flag (set ON when the player presses the action btn 9260)
  X16_4  duplicate of X12_4 in the templates we've seen

We mark the statue at its MSB position and hide the marker on X0_4 (the flag
that DELETES the statue in-world once its content is resolved). NOT X12_4: that
"activated" flag is set on examine but the same X0_4 branch clears it back OFF,
so a marker keyed on X12_4 would linger after the statue is gone.

The same model (AEG099_055) can also appear as non-interactive decoration
in DLC/world - those instances are NOT in the EMEVD scan so they don't
become markers.
"""
import sys, io, os, tempfile, struct
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

from massedit_common import (OUT_DIR, UNDERGROUND_AREAS, DLC_AREAS,
                             OVERWORLD_AREAS, get_disp_mask,
                             resolve_location_id_at)

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
_str = SysType.GetType('System.String')
_msbe = asm.GetType('SoulsFormats.MSBE').GetMethod('Read',
    BindingFlags.Public|BindingFlags.Static|BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str]), None)
_emevd = asm.GetType('SoulsFormats.EMEVD').GetMethod('Read',
    BindingFlags.Public|BindingFlags.Static|BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str]), None)


HERO_TOMB_TEMPLATE = 90005683
import json
import icon_registry
import row_id_registry
ICON_ID = icon_registry.iconid("hero_tomb_statues")  # single-layer MENU_MAP_85 (char 109)
ROW_START = row_id_registry.base("World - Hero's Tomb Statues")  # z-order slot; see row_id_registry


def load_grave_clear_flags():
    """Map each grave's map-point eventFlagId -> that grave's CLEARED flag.

    The statue template's X0_4 (params[0]) is the eventFlagId of the target Hero's
    Grave's world-map point (a vanilla area-30/60 row in WorldMapPointParam). That
    map point is the flag block 62xxx = "grave entrance DISCOVERED", which the game
    also uses to delete the statue - i.e. the statue vanishes when you FIND the
    grave, not when you finish it. Verified live: those 62xxx flags stay OFF after
    clearing the dungeon, so keying our marker on X0_4 hides it too late/never for
    a player who cares about "have I done this grave".

    The grave's map-point row also encodes its CLEARED transition flag in
    textDisableFlagId4 (the standard 30XXX800 dungeon-complete flag; falls back to
    textId1*1000+800 for the one row that lays the fields out differently). Keying
    the statue marker on THAT hides it exactly when the target grave is cleared.
    Fully data-derived from the extracted param - no geometry, no hardcoding.
    """
    path = config.DATA_DIR / 'WorldMapPointParam.json'
    out = {}
    for r in json.load(open(path, encoding='utf-8')):
        efid = r.get('eventFlagId') or 0
        if efid <= 0:
            continue
        t1 = r.get('textId1') or 0
        clear = r.get('textDisableFlagId4') or 0
        if clear <= 0 and t1 > 0:
            clear = t1 * 1000 + 800  # uniform dungeon-complete flag pattern
        if clear > 0:
            out[efid] = clear
    return out


def _read(reader, path, suf='.tmp'):
    data = SoulsFormats.DCX.Decompress(str(path)).ToArray()
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_ht' + suf)
    SysFile.WriteAllBytes(tmp, data)
    return reader.Invoke(None, Array[Object]([tmp]))


def parse_tile(name):
    parts = name.replace('m', '').split('_')
    if len(parts) < 4: return None
    try: return int(parts[0]), int(parts[1]), int(parts[2])
    except ValueError: return None


def main():
    err = config.require_err_mod_dir()
    ev_dir = err / 'event'
    msb_dir = err / 'map' / 'MapStudio'

    # Pass 1: collect (entity, flag, source_tile) from every 90005683 call
    print('Scanning EMEVD for Hero\'s Tomb statue calls...')
    calls = []
    seen = set()  # dedup by entity (phase variants _00/_10 share the same EID)
    for p in sorted(ev_dir.glob('m*.emevd.dcx')):
        try:
            em = _read(_emevd, p)
        except Exception:
            continue
        src_tile = p.name.replace('.emevd.dcx', '')
        for evt in em.Events:
            for inst in evt.Instructions:
                if int(inst.Bank) != 2000 or int(inst.ID) != 6:
                    continue
                ab = bytes(inst.ArgData) if inst.ArgData else b''
                if len(ab) < 8: continue
                if struct.unpack_from('<I', ab, 4)[0] != HERO_TOMB_TEMPLATE:
                    continue
                params = [struct.unpack_from('<I', ab, off)[0]
                          for off in range(8, len(ab) - 3, 4)]
                if len(params) < 4: continue
                # X0_4 (params[0]) is the target grave's map-point eventFlagId. The game
                # deletes the statue when X0_4 turns ON = when the grave entrance is
                # DISCOVERED (flag block 62xxx), NOT when the grave is cleared. We remap it
                # to the grave's CLEARED flag below (load_grave_clear_flags) so the marker
                # hides when the player finishes the grave. X12_4 is the "examined" flag -
                # set on the action button before you've done the grave - so it hides too
                # early. See load_grave_clear_flags() for the derivation.
                entity, x0_4 = params[1], params[0]
                if entity in seen: continue
                seen.add(entity)
                calls.append((src_tile, entity, x0_4))
    print(f'  {len(calls)} Hero\'s Tomb statues found')

    # Pass 2: resolve entity → MSB position (entity may live in a different
    # supertile than the caller's tile, e.g. cross-tile asset prefixes).
    # Build a global entity index from every MSB once.
    print('Building MSB entity index...')
    eid_index = {}
    for p in sorted(msb_dir.glob('*.msb.dcx')):
        try:
            msb = _read(_msbe, p, '.msb')
        except Exception:
            continue
        tile_name = p.name.replace('.msb.dcx', '')
        tinfo = parse_tile(tile_name)
        if tinfo is None: continue
        area, gx, gz = tinfo
        for cat in (msb.Parts.Assets, getattr(msb.Parts, 'DummyAssets', []) or []):
            for a in cat:
                try:
                    e = int(a.EntityID)
                    if e > 0 and e not in eid_index:
                        eid_index[e] = {
                            'name': str(a.Name), 'model': str(a.ModelName),
                            'x': float(a.Position.X), 'y': float(a.Position.Y),
                            'z': float(a.Position.Z),
                            'msb': tile_name,
                            'area': area, 'gx': gx, 'gz': gz,
                        }
                except Exception: pass
    print(f'  {len(eid_index)} indexed entities')

    # Grave map-point eventFlagId -> that grave's CLEARED flag (see loader docstring).
    grave_clear = load_grave_clear_flags()
    remapped = 0

    # Pass 3: emit MASSEDIT rows
    lines = []
    row_id = ROW_START
    written = 0
    for src_tile, entity, x0_4 in calls:
        part = eid_index.get(entity)
        if not part: continue
        # Remap X0_4 (grave DISCOVERED) -> grave CLEARED flag; fall back to X0_4 if the
        # grave has no map-point row (keeps the old behaviour rather than never hiding).
        flag = grave_clear.get(x0_4, x0_4)
        if flag != x0_4:
            remapped += 1
        area = part['area']
        gx = part['gx']
        gz = part['gz']

        # Cross-tile prefix remap: if MSB name has prefix like "m60_43_39_00-",
        # the asset logically belongs to that fine tile.
        nm = part['name']
        if nm.startswith('m'):
            import re as _re
            m = _re.match(r'^m(\d{2})_(\d{2})_(\d{2})_(\d{2})-', nm)
            if m:
                own_area, own_gx, own_gz, _ = (int(g) for g in m.groups())
                if own_area == area:
                    # Apply remap (same logic as extract_all_items.py)
                    SUFFIX_SCALE = {'01': 2, '02': 4, '12': 4}
                    cur_suffix = part['msb'][-2:]
                    scale = SUFFIX_SCALE.get(cur_suffix)
                    if scale is not None:
                        FINE = 256
                        agg = FINE * scale
                        offset_x = gx * agg + agg / 2 - own_gx * FINE - FINE / 2
                        offset_z = gz * agg + agg / 2 - own_gz * FINE - FINE / 2
                        part['x'] += offset_x
                        part['z'] += offset_z
                        gx = own_gx
                        gz = own_gz

        disp = get_disp_mask(area)
        lines.append(f'param WorldMapPointParam: id {row_id}: iconId: = {ICON_ID};')
        lines.append(f'param WorldMapPointParam: id {row_id}: {disp}: = 1;')
        lines.append(f'param WorldMapPointParam: id {row_id}: areaNo: = {area};')
        if area in OVERWORLD_AREAS or area in DLC_AREAS or gx > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: gridXNo: = {gx};')
            lines.append(f'param WorldMapPointParam: id {row_id}: gridZNo: = {gz};')
        lines.append(f'param WorldMapPointParam: id {row_id}: posX: = {part["x"]:.3f};')
        if part['y'] != 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: posY: = {part["y"]:.3f};')
        lines.append(f'param WorldMapPointParam: id {row_id}: posZ: = {part["z"]:.3f};')
        # Label "Examine statue" - ActionButtonText[7041] via +800M offset.
        # goblin_messages.cpp copies this from menu.msgbnd at runtime so
        # the text follows the player's selected game language.
        lines.append(f'param WorldMapPointParam: id {row_id}: textId1: = {800000000 + 7041};')
        if flag > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: textDisableFlagId1: = {flag};')
        # Location subtitle for non-overworld tiles (most are overworld though).
        map_code = f'm{area:02d}_{gx:02d}_{gz:02d}_00'
        loc_id = resolve_location_id_at(map_code, part['x'], part['y'], part['z'])
        if loc_id > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: textId2: = {loc_id};')
            if flag > 0:
                lines.append(f'param WorldMapPointParam: id {row_id}: textDisableFlagId2: = {flag};')
        lines.append(f'param WorldMapPointParam: id {row_id}: selectMinZoomStep: = 1;')
        row_id += 1
        written += 1

    out_path = OUT_DIR / "World - Hero's Tomb Statues.MASSEDIT"
    with open(out_path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    print(f'Written {written} Hero\'s Tomb statue markers to {out_path.name} '
          f'({remapped} keyed to grave-cleared flag, {written - remapped} fell back to X0_4)')


if __name__ == '__main__':
    main()
