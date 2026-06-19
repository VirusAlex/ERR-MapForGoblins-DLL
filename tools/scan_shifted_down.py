#!/usr/bin/env python3
"""Full vanilla-vs-ERR position comparison for every MSB entity.

Walks every map shared between vanilla and ERR; matches Parts and Regions
by Name; flags anything whose Y dropped by more than THRESHOLD units.
Writes a markdown table sorted by drop magnitude."""
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

_str = SysType.GetType('System.String')
_msbe = asm.GetType('SoulsFormats.MSBE').GetMethod('Read',
    BindingFlags.Public|BindingFlags.Static|BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str]), None)


def read_msb(path):
    data = SoulsFormats.DCX.Decompress(str(path)).ToArray()
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_scan.msb')
    SysFile.WriteAllBytes(tmp, data)
    return _msbe.Invoke(None, Array[Object]([tmp]))


def collect(msb):
    """Return {name: (kind, y, model_or_type)}"""
    out = {}
    # Parts
    for cat in ('MapPieces', 'Enemies', 'Players', 'Collisions', 'DummyAssets',
                'DummyEnemies', 'ConnectCollisions', 'Assets'):
        coll = getattr(msb.Parts, cat, None) or []
        for p in coll:
            try:
                name = str(p.Name)
                model = str(getattr(p, 'ModelName', '') or '')
                # Skip disabled placements - they don't spawn
                if int(getattr(p, 'GameEditionDisable', 0) or 0) == 1:
                    continue
                out[name] = (f'Part.{cat}', float(p.Position.Y), model)
            except Exception:
                pass
    # Regions: walk every collection attribute on .Regions
    for rname in dir(msb.Regions):
        if rname.startswith('_'):
            continue
        try:
            coll = getattr(msb.Regions, rname)
        except Exception:
            continue
        if not hasattr(coll, '__iter__'):
            continue
        try:
            for r in coll:
                try:
                    name = str(r.Name)
                    out[name] = (f'Region.{rname}', float(r.Position.Y), '')
                except Exception:
                    pass
        except Exception:
            pass
    return out


def main():
    THRESHOLD = 3.0
    van_dir = config.require_game_dir() / 'map' / 'MapStudio'
    err_dir = config.require_err_mod_dir() / 'map' / 'MapStudio'

    err_files = sorted(p.name for p in err_dir.glob('*.msb.dcx'))
    print(f'Scanning {len(err_files)} ERR MSBs against vanilla...')

    shifts = []
    for i, fname in enumerate(err_files):
        van_p = van_dir / fname
        err_p = err_dir / fname
        if not van_p.exists():
            continue
        try:
            van = read_msb(van_p)
            err = read_msb(err_p)
        except Exception:
            continue
        vc = collect(van)
        ec = collect(err)
        for name, (kind, ey, model) in ec.items():
            if name not in vc:
                continue
            vy = vc[name][1]
            dy = ey - vy
            if dy <= -THRESHOLD:
                shifts.append({
                    'map': fname.replace('.msb.dcx', ''),
                    'name': name,
                    'kind': kind,
                    'model': model,
                    'vanilla_y': vy,
                    'err_y': ey,
                    'dy': dy,
                })
        if (i + 1) % 100 == 0:
            print(f'  [{i+1}/{len(err_files)}]')

    shifts.sort(key=lambda s: s['dy'])  # most negative first

    out_json = config.DATA_DIR / 'shifted_down_full.json'
    with open(out_json, 'w', encoding='utf-8') as f:
        json.dump(shifts, f, ensure_ascii=False, indent=2)

    out_md = config.DATA_DIR / 'shifted_down_full.md'
    with open(out_md, 'w', encoding='utf-8') as f:
        f.write(f'# Shifted-down entities in ERR (dy <= -{THRESHOLD:.1f})\n\n')
        f.write(f'Total: {len(shifts)} entries\n\n')
        f.write('| # | dy | map | kind | name | model | vanilla_Y | ERR_Y |\n')
        f.write('|---|----|-----|------|------|-------|-----------|-------|\n')
        for i, s in enumerate(shifts, 1):
            f.write(f"| {i} | {s['dy']:+.2f} | {s['map']} | {s['kind']} | "
                    f"`{s['name']}` | {s['model']} | "
                    f"{s['vanilla_y']:+.2f} | {s['err_y']:+.2f} |\n")
    print(f'Wrote {out_json.name} and {out_md.name} ({len(shifts)} entries).')


if __name__ == '__main__':
    main()
