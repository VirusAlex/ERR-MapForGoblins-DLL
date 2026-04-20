#!/usr/bin/env python3
"""
Research summoning pool (AEG099_015) placement data in MSB files.
Extracts ALL available fields using reflection to find event flag links.
"""
import sys, io, json, os, tempfile
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

import config
from pythonnet import load
load('coreclr')
import clr
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object
from System.IO import File as SysFile

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats

ERR_MOD_DIR = config.require_err_mod_dir()
MSB_DIR = ERR_MOD_DIR / 'map' / 'MapStudio'

_str_type = SysType.GetType('System.String')
_msbe_read = asm.GetType('SoulsFormats.MSBE').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None)


def rfb(rm, data, suf='.bin'):
    tmp = os.path.join(tempfile.gettempdir(), '_mfg_015_tmp' + suf)
    if hasattr(data, 'ToArray'):
        SysFile.WriteAllBytes(tmp, data.ToArray())
    else:
        SysFile.WriteAllBytes(tmp, data)
    r = rm.Invoke(None, Array[Object]([tmp]))
    os.unlink(tmp)
    return r


def read_msb(path):
    return rfb(_msbe_read, SoulsFormats.DCX.Decompress(str(path)), '.msb')


def convert_value(val):
    """Convert a .NET value to a Python-friendly type."""
    if val is None:
        return None
    # pythonnet may auto-convert primitives to Python types
    if isinstance(val, (int, float, bool, str)):
        return val
    try:
        t = str(val.GetType().Name)
    except:
        return val
    if t in ('Int32', 'UInt32', 'Int64', 'UInt64', 'Int16', 'UInt16', 'Byte', 'SByte'):
        return int(val)
    if t in ('Single', 'Double'):
        return round(float(val), 4)
    if t == 'Boolean':
        return bool(val)
    if t == 'Vector3':
        return {'X': round(float(val.X), 3), 'Y': round(float(val.Y), 3), 'Z': round(float(val.Z), 3)}
    if t == 'String':
        return str(val)
    return str(val)


def get_all_properties(obj):
    """Use reflection to get ALL properties of an object."""
    t = obj.GetType()
    flags = BindingFlags.Public | BindingFlags.Instance | BindingFlags.FlattenHierarchy
    props = t.GetProperties(flags)
    result = {}
    for prop in props:
        name = str(prop.Name)
        try:
            val = prop.GetValue(obj)
            if val is None:
                result[name] = None
            elif isinstance(val, (int, float, bool, str)):
                result[name] = val
            elif hasattr(val, 'X') and hasattr(val, 'Y') and hasattr(val, 'Z'):
                result[name] = {'X': round(float(val.X), 3), 'Y': round(float(val.Y), 3), 'Z': round(float(val.Z), 3)}
            elif hasattr(val, '__iter__') and not isinstance(val, str):
                items = []
                try:
                    for item in val:
                        items.append(convert_value(item))
                except:
                    items = [str(val)]
                result[name] = items
            else:
                result[name] = convert_value(val)
        except Exception as ex:
            result[name] = f"<error: {ex}>"
    return result


def get_all_fields(obj):
    """Use reflection to get ALL fields (including private) of an object."""
    result = {}
    current_type = obj.GetType()
    while current_type is not None and str(current_type) != 'System.Object':
        flags = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.DeclaredOnly
        fields = current_type.GetFields(flags)
        for field in fields:
            name = str(field.Name)
            try:
                val = field.GetValue(obj)
                result[f"{current_type.Name}.{name}"] = convert_value(val) if val is not None else None
            except:
                pass
        current_type = current_type.BaseType
    return result


def main():
    print("=" * 80)
    print("SUMMONING POOL (AEG099_015) MSB RESEARCH")
    print("=" * 80)

    msb_files = sorted(MSB_DIR.glob('*.msb.dcx'))
    print(f"\nScanning {len(msb_files)} MSB files...")

    all_pools = []
    first_pool_obj = None
    pool_count_by_map = {}

    for msb_path in msb_files:
        map_name = msb_path.name.replace('.msb.dcx', '')
        try:
            msb = read_msb(msb_path)
        except:
            continue

        for p in msb.Parts.Assets:
            if str(p.ModelName) != 'AEG099_015':
                continue

            props = get_all_properties(p)

            if first_pool_obj is None:
                first_pool_obj = p

            all_pools.append({
                'map': map_name,
                'props': props,
            })
            pool_count_by_map[map_name] = pool_count_by_map.get(map_name, 0) + 1

    print(f"\nTotal AEG099_015 entries across all MSBs: {len(all_pools)}")
    print(f"Maps containing summoning pools: {len(pool_count_by_map)}")

    # ===================================================================
    # SECTION 1: Property schema via reflection
    # ===================================================================
    print("\n" + "=" * 80)
    print("SECTION 1: ALL PROPERTIES ON ASSET PART TYPE (via reflection)")
    print("=" * 80)

    if first_pool_obj is not None:
        t = first_pool_obj.GetType()
        print(f"\nType: {t}")
        bt = t.BaseType
        bases = []
        while bt is not None and str(bt) != 'System.Object':
            bases.append(str(bt))
            bt = bt.BaseType
        print(f"Base types: {' -> '.join(bases)}")

        flags = BindingFlags.Public | BindingFlags.Instance | BindingFlags.FlattenHierarchy
        all_props = t.GetProperties(flags)
        print(f"\nTotal properties: {len(all_props)}")
        for prop in sorted(all_props, key=lambda p: str(p.Name)):
            name = str(prop.Name)
            ptype = str(prop.PropertyType)
            try:
                val = prop.GetValue(first_pool_obj)
                val_str = str(val) if val is not None else "null"
                if len(val_str) > 100:
                    val_str = val_str[:100] + "..."
            except:
                val_str = "<error>"
            print(f"  {name:45s} {ptype:45s} = {val_str}")

        # Private fields
        print(f"\n--- Private/Internal Fields ---")
        fields_data = get_all_fields(first_pool_obj)
        for k, v in sorted(fields_data.items()):
            v_str = str(v)
            if len(v_str) > 100:
                v_str = v_str[:100] + "..."
            print(f"  {k:55s} = {v_str}")

    # ===================================================================
    # SECTION 2: Detailed data for each summoning pool
    # ===================================================================
    print("\n" + "=" * 80)
    print("SECTION 2: ALL SUMMONING POOLS -- FULL DATA")
    print("=" * 80)

    # Properties that are always the same or struct objects (not useful for per-pool comparison)
    skip_in_detail = {'ModelName', 'DrawGroups', 'DispGroups', 'BackreadGroups',
                      'Scale', 'Name', 'EntityID', 'EntityGroupIDs', 'Position', 'Rotation',
                      'AssetUnk1', 'AssetUnk2', 'AssetUnk3', 'AssetUnk4',
                      'DisplayDataStruct', 'DisplayGroupStruct', 'GparamConfigStruct',
                      'GrassConfigStruct', 'TileLoadConfig', 'UnkStruct8', 'UnkStruct9',
                      'UnkStruct11', 'SibPath', 'PartNames'}

    for pool in all_pools:
        p = pool['props']
        name = p.get('Name', '?')
        eid = p.get('EntityID', -1)
        pos = p.get('Position', {})
        egroups = p.get('EntityGroupIDs', [])
        nonzero_groups = [g for g in (egroups or []) if isinstance(g, int) and g != 0]

        print(f"\n  {pool['map']:25s} | {name:30s} | EID={eid:>10} | "
              f"Pos=({pos.get('X',0):10.2f}, {pos.get('Y',0):10.2f}, {pos.get('Z',0):10.2f})"
              f"{' | Groups=' + str(nonzero_groups) if nonzero_groups else ''}")

        # Print additional non-trivial scalar properties
        extras = []
        for k, v in sorted(p.items()):
            if k in skip_in_detail:
                continue
            if v is None or v == 0 or v == '' or v is False:
                continue
            if isinstance(v, list) and all(x == 0 for x in v):
                continue
            if isinstance(v, dict):
                continue
            if isinstance(v, str) and v.startswith('SoulsFormats'):
                continue
            extras.append(f"{k}={v}")
        if extras:
            print(f"      {', '.join(extras)}")

    # ===================================================================
    # SECTION 3: Part name encoding analysis
    # ===================================================================
    print("\n" + "=" * 80)
    print("SECTION 3: PART NAME ENCODING ANALYSIS")
    print("=" * 80)

    suffixes = {}
    for pool in all_pools:
        name = pool['props'].get('Name', '')
        parts = name.split('_')
        if len(parts) >= 3:
            suffix = '_'.join(parts[2:])
            suffixes.setdefault(suffix, []).append({
                'map': pool['map'],
                'name': name,
                'eid': pool['props'].get('EntityID', -1),
            })

    print(f"\nUnique suffixes: {len(suffixes)}")
    for suffix in sorted(suffixes.keys()):
        entries = suffixes[suffix]
        eids = [e['eid'] for e in entries]
        maps = [e['map'] for e in entries]
        print(f"  Suffix {suffix:>10s}: {len(entries)} instances, maps={maps}, EIDs={eids}")

    # ===================================================================
    # SECTION 4: Entity ID analysis
    # ===================================================================
    print("\n" + "=" * 80)
    print("SECTION 4: ENTITY ID ANALYSIS")
    print("=" * 80)

    eids = [pool['props'].get('EntityID', -1) for pool in all_pools]
    nonzero_eids = [e for e in eids if e > 0]
    zero_eids = [e for e in eids if e == 0]
    neg_eids = [e for e in eids if e < 0]

    print(f"\n  Total pools: {len(all_pools)}")
    print(f"  With EntityID > 0: {len(nonzero_eids)}")
    print(f"  With EntityID = 0: {len(zero_eids)}")
    print(f"  With EntityID < 0: {len(neg_eids)}")

    if nonzero_eids:
        print(f"\n  EntityID range: {min(nonzero_eids)} - {max(nonzero_eids)}")
        print(f"\n  All non-zero EntityIDs:")
        for pool in all_pools:
            eid = pool['props'].get('EntityID', -1)
            if eid > 0:
                print(f"    {pool['map']:25s} {pool['props'].get('Name', '?'):30s} EID={eid}")

    # Entity Groups analysis
    print(f"\n  Entity Groups analysis:")
    pools_with_groups = 0
    all_group_values = set()
    for pool in all_pools:
        egroups = pool['props'].get('EntityGroupIDs', [])
        nonzero = [g for g in (egroups or []) if isinstance(g, int) and g != 0]
        if nonzero:
            pools_with_groups += 1
            all_group_values.update(nonzero)
    print(f"  Pools with non-zero EntityGroups: {pools_with_groups}")
    if all_group_values:
        print(f"  Unique group values: {sorted(all_group_values)}")

    # ===================================================================
    # SECTION 5: Nearby regions and events
    # ===================================================================
    print("\n" + "=" * 80)
    print("SECTION 5: NEARBY REGIONS & EVENTS (within 5m of each pool)")
    print("=" * 80)

    pool_positions = {}
    for pool in all_pools:
        pos = pool['props'].get('Position', {})
        map_name = pool['map']
        if map_name not in pool_positions:
            pool_positions[map_name] = []
        pool_positions[map_name].append((
            pos.get('X', 0), pos.get('Y', 0), pos.get('Z', 0),
            pool['props'].get('Name', '?')
        ))

    nearby_count = 0
    RADIUS = 5.0

    for msb_path in msb_files:
        map_name = msb_path.name.replace('.msb.dcx', '')
        if map_name not in pool_positions:
            continue

        try:
            msb = read_msb(msb_path)
        except:
            continue

        positions = pool_positions[map_name]

        # Check Regions
        if hasattr(msb, 'Regions'):
            for rtype_name in dir(msb.Regions):
                if rtype_name.startswith('_') or rtype_name[0].islower():
                    continue
                try:
                    rtype = getattr(msb.Regions, rtype_name)
                    if not hasattr(rtype, '__iter__'):
                        continue
                    for r in rtype:
                        if not hasattr(r, 'Position'):
                            continue
                        rx = float(r.Position.X)
                        ry = float(r.Position.Y)
                        rz = float(r.Position.Z)
                        rname = str(r.Name) if hasattr(r, 'Name') else '?'
                        reid = int(r.EntityID) if hasattr(r, 'EntityID') else -1

                        for px, py, pz, pname in positions:
                            dist = ((rx-px)**2 + (ry-py)**2 + (rz-pz)**2)**0.5
                            if dist < RADIUS:
                                rprops = get_all_properties(r)
                                print(f"\n  Near {pname} in {map_name} (dist={dist:.1f}m):")
                                print(f"    Region type: {rtype_name}")
                                print(f"    Name: {rname}, EntityID: {reid}")
                                for k, v in sorted(rprops.items()):
                                    if k in ('Position', 'Name', 'EntityID'):
                                        continue
                                    if v is None or v == 0 or v == '' or v is False:
                                        continue
                                    if isinstance(v, list) and all(x == 0 for x in v):
                                        continue
                                    v_str = str(v)
                                    if len(v_str) > 120:
                                        v_str = v_str[:120] + "..."
                                    print(f"    {k}: {v}")
                                nearby_count += 1
                except:
                    pass

        # Check Events that reference pool parts
        if hasattr(msb, 'Events'):
            for etype_name in dir(msb.Events):
                if etype_name.startswith('_') or etype_name[0].islower():
                    continue
                try:
                    etype = getattr(msb.Events, etype_name)
                    if not hasattr(etype, '__iter__'):
                        continue
                    for ev in etype:
                        # Check if event references a pool part name
                        ev_props = get_all_properties(ev)
                        for pk, pv in ev_props.items():
                            if isinstance(pv, str):
                                for px, py, pz, pname in positions:
                                    if pv == pname:
                                        print(f"\n  Event referencing {pname} in {map_name}:")
                                        print(f"    Event type: {etype_name}")
                                        for k, v in sorted(ev_props.items()):
                                            if v is None or v == 0 or v == '':
                                                continue
                                            v_str = str(v)
                                            if len(v_str) > 120:
                                                v_str = v_str[:120] + "..."
                                            print(f"    {k}: {v}")
                                        nearby_count += 1
                except:
                    pass

    print(f"\n  Total nearby regions/events found: {nearby_count}")

    # ===================================================================
    # SECTION 6: Deduplication
    # ===================================================================
    print("\n" + "=" * 80)
    print("SECTION 6: DEDUPLICATION (_00 vs _10 sub-levels)")
    print("=" * 80)

    seen_positions = {}
    for pool in all_pools:
        pos = pool['props'].get('Position', {})
        map_name = pool['map']
        parts = map_name.split('_')
        area = int(parts[0][1:]) if len(parts) >= 4 else -1
        x = pos.get('X', 0)
        z = pos.get('Z', 0)
        key = (area, round(x, 0), round(z, 0))
        if key not in seen_positions:
            seen_positions[key] = []
        seen_positions[key].append({
            'map': map_name,
            'name': pool['props'].get('Name', '?'),
            'eid': pool['props'].get('EntityID', -1),
            'pos': pos,
        })

    unique_pools = len(seen_positions)
    duplicated = [(k, v) for k, v in seen_positions.items() if len(v) > 1]

    print(f"\n  Total raw entries: {len(all_pools)}")
    print(f"  Unique positions (deduplicated): {unique_pools}")
    print(f"  Positions in multiple sub-levels: {len(duplicated)}")

    if duplicated:
        print(f"\n  Duplicated pools:")
        for key, entries in sorted(duplicated):
            print(f"    Position key {key}:")
            for e in entries:
                print(f"      {e['map']:25s} {e['name']:30s} EID={e['eid']}")

    # ===================================================================
    # SECTION 7: Unk/metadata analysis
    # ===================================================================
    print("\n" + "=" * 80)
    print("SECTION 7: UnkStruct & METADATA ANALYSIS")
    print("=" * 80)

    if all_pools:
        unk_keys = [k for k in all_pools[0]['props'].keys() if 'Unk' in k or 'unk' in k]
        print(f"\n  Unk-prefixed properties: {unk_keys}")

        for key in sorted(unk_keys):
            values = set()
            for pool in all_pools:
                v = pool['props'].get(key)
                values.add(str(v))
            if len(values) == 1:
                print(f"  {key}: all same = {values.pop()}")
            else:
                print(f"  {key}: {len(values)} distinct values")
                for v in sorted(values):
                    if len(v) > 140:
                        v = v[:140] + "..."
                    print(f"    {v}")

    # Also check: what other properties vary across pools?
    print(f"\n  Properties that vary across pools:")
    if all_pools:
        all_keys = list(all_pools[0]['props'].keys())
        for key in sorted(all_keys):
            values = set()
            for pool in all_pools:
                v = pool['props'].get(key)
                values.add(str(v)[:200])
            if len(values) > 1 and key not in ('Name', 'Position', 'Rotation', 'EntityID',
                                                 'EntityGroups', 'DrawGroups', 'DispGroups',
                                                 'BackreadGroups'):
                print(f"    {key}: {len(values)} distinct values")
                # Show a few samples
                for v in sorted(list(values))[:5]:
                    if len(v) > 100:
                        v = v[:100] + "..."
                    print(f"      {v}")

    print("\n" + "=" * 80)
    print("RESEARCH COMPLETE")
    print("=" * 80)


if __name__ == '__main__':
    main()
