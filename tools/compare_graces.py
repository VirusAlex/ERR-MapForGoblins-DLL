#!/usr/bin/env python3
"""Compare specific graces between vanilla and ERR.

Loads BonfireWarpParam from both regulations, resolves grace names via
the appropriate PlaceName/sample FMG, and reports the deltas for the
user-supplied list of grace names (substring match)."""
import sys, io, os, tempfile, json
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

from extract_all_items import (load_paramdefs, read_param, param_to_dict,
                               _read_from_bytes, _bnd4_read, _fmg_read,
                               _msbe_read)


def find_msb_entity_by_id(root, area, gx, gz, entity_id):
    """Find an MSB Part/Region with the given EntityID across all suffix tiles.
    Returns (map_name, kind, name, pos) or None."""
    import os, tempfile
    from System import Array, Type as SysType, Object
    from System.IO import File as SysFile
    _str = SysType.GetType('System.String')
    msb_dir = root / 'map' / 'MapStudio'
    pattern = f'm{area:02d}_{gx:02d}_{gz:02d}_*.msb.dcx'
    for path in sorted(msb_dir.glob(pattern)):
        data = SoulsFormats.DCX.Decompress(str(path)).ToArray()
        tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_cg.msb')
        SysFile.WriteAllBytes(tmp, data)
        msb = _msbe_read.Invoke(None, Array[Object]([tmp]))
        for cat in ('Assets','DummyAssets','Enemies','DummyEnemies'):
            coll = getattr(msb.Parts, cat, None) or []
            for p in coll:
                try:
                    if int(getattr(p, 'EntityID', 0) or 0) == entity_id:
                        disabled = int(getattr(p, 'GameEditionDisable', 0) or 0) == 1
                        return (path.name.replace('.msb.dcx',''),
                                f'Part.{cat}', str(p.Name),
                                (float(p.Position.X), float(p.Position.Y), float(p.Position.Z)),
                                str(getattr(p, 'ModelName', '')),
                                disabled)
                except Exception:
                    pass
    return None


TARGETS = [
    'Reeling Shack',
    'Inner Aeonia',
    'Primeval Sorcerer Azur',
    'Fortified Manor, First Floor',
    'Second Floor Chamber',
    "Midra's Library",
    'Fissure Cross',
]


def load_regulation(path):
    return SoulsFormats.SFUtil.DecryptERRegulation(str(path))


def collect_placenames(root):
    """Load PlaceName FMG from item.msgbnd + item_dlc01 + item_dlc02."""
    out = {}
    for fname in ('item.msgbnd.dcx', 'item_dlc01.msgbnd.dcx',
                  'item_dlc02.msgbnd.dcx'):
        p = root / 'msg' / 'engus' / fname
        if not p.exists():
            p = root / 'msg' / 'enus' / fname
        if not p.exists():
            continue
        try:
            bnd = _read_from_bytes(_bnd4_read,
                SoulsFormats.DCX.Decompress(str(p)), '.msgbnd')
        except Exception:
            continue
        for f in bnd.Files:
            name = str(f.Name).replace('\\', '/').lower()
            # PlaceName.fmg (base), or PlaceName_dlcXX.fmg in DLC bundles
            if 'placename' not in name or not name.endswith('.fmg'):
                continue
            fmg = _read_from_bytes(_fmg_read, f.Bytes, '.fmg')
            for e in fmg.Entries:
                try:
                    tid = int(e.ID)
                    txt = str(e.Text) if e.Text is not None else ''
                    if txt:
                        out[tid] = txt
                except Exception:
                    pass
    return out


def main():
    paramdefs = load_paramdefs()

    # Full set - pick from the schema dump
    FIELDS = {
        'disableParam_NT','disableParamReserve1','disableParamReserve2',
        'eventflagId','bonfireEntityId','bonfireSubCategorySortId',
        'forbiddenIconId','dispMinZoomStep','selectMinZoomStep',
        'bonfireSubCategoryId','clearedEventFlagId','iconId',
        'dispMask00','dispMask01','dispMask02',
        'areaNo','gridXNo','gridZNo','posX','posY','posZ',
        'textId1','textEnableFlagId1','textDisableFlagId1',
        'textId2','textEnableFlagId2','textDisableFlagId2',
        'altIconId','altForbiddenIconId',
        'noIgnitionSfxId_0','noIgnitionSfxDmypolyId_0',
        'noIgnitionSfxId_1','noIgnitionSfxDmypolyId_1',
    }

    print('Loading vanilla regulation...')
    van_bnd = load_regulation(config.GAME_DIR / 'regulation.bin')
    van_bwp = read_param(van_bnd, 'BonfireWarpParam', paramdefs)
    van_data = param_to_dict(van_bwp, FIELDS)

    print('Loading ERR regulation...')
    err_bnd = load_regulation(config.ERR_MOD_DIR / 'regulation.bin')
    err_bwp = read_param(err_bnd, 'BonfireWarpParam', paramdefs)
    err_data = param_to_dict(err_bwp, FIELDS)

    print('Loading PlaceName FMG (vanilla and ERR)...')
    van_names = collect_placenames(config.GAME_DIR)
    err_names = collect_placenames(config.ERR_MOD_DIR)
    print(f'  vanilla PlaceName entries: {len(van_names)}')
    print(f'  ERR PlaceName entries: {len(err_names)}')

    # Index by name → list of (rid, tid1, source)
    def index_by_name(data, names):
        idx = {}  # lowercase substring → list of (rid, row, name_text)
        for rid, row in data.items():
            tid = int(row.get('textId1', 0) or 0)
            if tid <= 0: continue
            text = names.get(tid, '')
            if not text: continue
            idx.setdefault(text, []).append((rid, row))
        return idx

    van_idx = index_by_name(van_data, van_names)
    err_idx = index_by_name(err_data, err_names)

    print()
    print('=' * 80)
    for target in TARGETS:
        print()
        print(f'### {target}')

        # Try exact then case-insensitive substring
        def lookup(idx, q):
            ql = q.lower()
            # exact
            for name, rows in idx.items():
                if name.lower() == ql:
                    return name, rows
            # substring
            hits = []
            for name, rows in idx.items():
                if ql in name.lower():
                    hits.append((name, rows))
            if len(hits) == 1: return hits[0]
            if len(hits) > 1:
                return f'AMBIGUOUS: {[h[0] for h in hits]}', None
            return None, None

        vname, vrows = lookup(van_idx, target)
        ename, erows = lookup(err_idx, target)

        if vrows is None and erows is None:
            print(f'  NOT FOUND in either vanilla or ERR (vname={vname}, ename={ename})')
            continue

        def fmt_row(rid, row):
            f = row.get
            return (f"  rid={rid} area={f('areaNo',0)} grid=({f('gridXNo',0)},"
                    f"{f('gridZNo',0)}) pos=({f('posX',0):+.2f},"
                    f"{f('posY',0):+.2f},{f('posZ',0):+.2f}) "
                    f"flag={f('eventflagId',0)} tid1={f('textId1',0)} "
                    f"entity={f('bonfireEntityId',0)} "
                    f"masks=[00={f('dispMask00',0)} 01={f('dispMask01',0)} "
                    f"02={f('dispMask02',0)}] "
                    f"cleared={f('clearedEventFlagId',0)} "
                    f"disableParam_NT={f('disableParam_NT',0)}")

        print(f'  vanilla name: {vname}')
        if vrows:
            for rid, row in vrows: print(fmt_row(rid, row))
        else:
            print('    (no rows)')

        print(f'  ERR name: {ename}')
        if erows:
            for rid, row in erows: print(fmt_row(rid, row))
        else:
            print('    (no rows)')

        # Per-rid diff
        if vrows and erows:
            van_by = {rid: row for rid, row in vrows}
            err_by = {rid: row for rid, row in erows}
            common = sorted(set(van_by) & set(err_by))
            only_v = sorted(set(van_by) - set(err_by))
            only_e = sorted(set(err_by) - set(van_by))
            for rid in common:
                vr, er = van_by[rid], err_by[rid]
                diffs = []
                for k in sorted(set(vr) | set(er)):
                    vv = vr.get(k); ev = er.get(k)
                    if vv != ev:
                        diffs.append(f'{k}: {vv} → {ev}')
                if diffs:
                    print(f'  DIFFS @ rid={rid}: ' + '; '.join(diffs))
                else:
                    print(f'  IDENTICAL @ rid={rid}')
            if only_v:
                print(f'  ONLY-IN-VANILLA rids: {only_v}')
            if only_e:
                print(f'  ONLY-IN-ERR rids: {only_e}')

        # MSB asset lookup for any ERR rows we have
        for rid, row in (erows or []):
            eid = int(row.get('bonfireEntityId', 0) or 0)
            if not eid:
                continue
            area = int(row.get('areaNo', 0) or 0)
            gx = int(row.get('gridXNo', 0) or 0)
            gz = int(row.get('gridZNo', 0) or 0)
            print(f'  MSB lookup for ERR rid={rid} entity={eid}:')
            r_err = find_msb_entity_by_id(config.ERR_MOD_DIR, area, gx, gz, eid)
            r_van = find_msb_entity_by_id(config.GAME_DIR,    area, gx, gz, eid)
            if r_van:
                m,k,n,p,mdl,dis = r_van
                print(f'    vanilla: map={m} {k} {n} model={mdl} pos=({p[0]:+.2f},{p[1]:+.2f},{p[2]:+.2f}) disabled={dis}')
            else:
                print(f'    vanilla: NOT FOUND')
            if r_err:
                m,k,n,p,mdl,dis = r_err
                print(f'    ERR    : map={m} {k} {n} model={mdl} pos=({p[0]:+.2f},{p[1]:+.2f},{p[2]:+.2f}) disabled={dis}')
            else:
                print(f'    ERR    : NOT FOUND')


if __name__ == '__main__':
    main()
