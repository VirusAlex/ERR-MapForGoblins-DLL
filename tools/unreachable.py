"""Manual exclude list for icons whose position in ERR differs from vanilla
in a way that leaves the pickup inside terrain (under-map, under-cliff,
etc.), so it cannot be reached in-game and its marker would mislead.

Conditional check: an entry only fires if the ERR position is actually
below the vanilla position by more than `MIN_DROP` units. If a future
ERR update moves the entity back to the vanilla height, the entry
becomes a no-op automatically - no need to clean the list manually.

Used by:
  - extract_all_items.py (Parts.Assets bound to Events.Treasures)
  - generate_imp_statues.py (Parts.Assets, AEG027_078/079)
  - generate_spirit_springs.py (Regions.MountJumps)
"""
import os, tempfile, json

# (map, name) entries. `name` is either an MSB Part.Name (assets) or a
# Region.Name (mount jumps).
UNREACHABLE = {
    # Golden Seed twins clipped under the Capital outer wall (Altus).
    # They sit ~5-7 units lower than in vanilla, under the wall geometry.
    ('m60_42_51_00', 'AEG099_090_9001'),
    ('m60_43_52_00', 'AEG099_090_9002'),
    # Imp Statue in Mt. Gelmir tile m60_38_52 dy=-13.86 vs vanilla
    # (Y 818.43 -> 804.57) - sits under the terrain; reported in-game (2026-06-11).
    ('m60_38_52_00', 'AEG027_079_9000'),
    # Imp Statue south of Caelid Highway dy=-15.8 vs vanilla - clips
    # below the cliff in ERR, the seal is not interactable.
    ('m60_47_40_00', 'AEG027_079_9000'),
    # Caravan chest "Giant-Crusher" in Altus tile m60_42_50_00 dy=-11.4 -
    # the caravan sits below the road surface. The same weapon drops from a
    # second chest (AEG099_630_9000) on the surface at the same tile, so
    # the underground caravan instance is the redundant unreachable one.
    # NOTE: physical part lives in supertile m60_10_12_02 with a cross-
    # tile prefix name (m60_42_50_00-AEG100_101_1000).
    ('m60_10_12_02', 'm60_42_50_00-AEG100_101_1000'),
    # Spirit Spring "to Rune Wolf Forest" dy=-110.9 vs vanilla - the
    # launch point sits ~110 units lower, no jump triggers in-game.
    ('m60_39_43_00', '騎乗大ジャンプポイント ルーンウルフの森へ'),
    # Midra's Library grace (m28_00_00_00 DLC) - the bonfire sits
    # +9.38 units above its vanilla position, out of player reach. BonfireWarpParam
    # position unchanged so the marker still points at vanilla pos.
    ('m28_00_00_00', 'AEG099_060_9002'),
    # Second Floor Chamber grace (m28_00_00_00 DLC) - the bonfire sits
    # -14.87 units below its vanilla position, out of player reach. BonfireWarpParam
    # position unchanged.
    ('m28_00_00_00', 'AEG099_060_9003'),
    # Fissure Cross grace (m22_00_00_00 DLC) - the bonfire sits
    # -19.58 units below its vanilla position, inside terrain; also iconId rebound
    # to 44 (forbidden look). BonfireWarpParam position unchanged.
    ('m22_00_00_00', 'AEG099_060_9004'),
    # Fort of Reprimand grace (Scadu Altus DLC) - the bonfire sits
    # -73.1 units below its vanilla position (Y 399.24 -> 326.14, X/Z
    # unchanged), inside terrain out of reach. BonfireWarpParam pos unchanged
    # so the marker would otherwise point at the vanilla surface spot.
    # Physical part is in supertile m61_12_10_02 with a cross-tile name.
    # (The sibling "Behind the Fort of Reprimand" 9002 is NOT displaced.)
    ('m61_12_10_02', 'm61_49_43_00-AEG099_060_9001'),
    # Crafting-material gather asset in Altus tile m60_49_40 - sits
    # -3.59 units below its vanilla position (Y 180.70 -> 177.10), inside
    # terrain; confirmed unreachable in-game.
    ('m60_49_40_00', 'AEG099_610_9001'),
    # Crafting-material gather asset (Turtle Neck Meat) in Mt. Gelmir tile
    # m60_39_51 - sits -10.0 below its vanilla position (Y 761.97 -> 751.97),
    # under the terrain; confirmed unreachable in-game (2026-06).
    ('m60_39_51_00', 'AEG099_610_9002'),
    # Immunizing Cured Meat gather asset in Seethewater Cave (Mt. Gelmir,
    # m31_07) - ERR raises it +9.49 ABOVE vanilla (Y 770.52 -> 780.01, X/Z
    # unchanged), leaving it floating above the room out of reach; confirmed
    # in-game and in Smithbox (2026-06). Displaced upward → 'displaced' kind.
    ('m31_07_00_00', 'AEG099_610_9007'),
}

# Unconditional excludes: out of reach in ERR, but the vertical delta vs
# vanilla is small (so the dY heuristic won't catch them) - e.g. the
# surrounding terrain differs rather than the asset position. Confirmed in-game.
# These fire regardless of vanilla comparison.
UNCONDITIONAL = {
    # Material-node gather asset in Altus tile m60_51_39 - only -1.81 vs
    # vanilla, but sits UNDER the ground in ERR (confirmed in-game).
    # NOTE: this confirmation PREDATES the generate_material_nodes slot-shift
    # fix - the in-game icon labeled 9000 was actually 9001's position. Kept
    # until re-verified at 9000's real spot (-26.4, 245.2, 80.6).
    ('m60_51_39_00', 'AEG099_653_9000'),
    # Material-node gather asset in Altus tile m60_51_39 at (-89.6, 246.7,
    # 34.3) - Y identical to vanilla, but sits under the terrain in ERR
    # (reshaped ground); confirmed in-game (2026-06).
    ('m60_51_39_00', 'AEG099_653_9001'),
}

# Entries whose displacement is checked in any direction (abs dy > threshold),
# not just downward. Used for graces displaced out of reach in either
# direction (Midra higher, Second Floor / Fissure lower).
ENTRY_KIND_DISPLACED = {
    ('m28_00_00_00', 'AEG099_060_9002'),
    ('m28_00_00_00', 'AEG099_060_9003'),
    ('m22_00_00_00', 'AEG099_060_9004'),
    # Seethewater Cave meat - displaced UPWARD (+9.49) out of reach in ERR.
    ('m31_07_00_00', 'AEG099_610_9007'),
}

MIN_DROP = 3.0  # units the ERR pos must be below vanilla to qualify ('down' kind)

# Lazy cache: (map, name) -> vanilla_y. Populated on first call.
_vanilla_y_cache = None


def _load_vanilla_cache():
    """Scan vanilla MSBs only for the maps in UNREACHABLE - fast (4 MSBs)."""
    global _vanilla_y_cache
    if _vanilla_y_cache is not None:
        return _vanilla_y_cache
    _vanilla_y_cache = {}
    import config
    if not getattr(config, 'GAME_DIR', None) or not config.GAME_DIR.exists():
        # Vanilla not configured; conservative fallback - no exclusion fires
        return _vanilla_y_cache

    needed_maps = {m for (m, _) in UNREACHABLE}
    needed_keys = set(UNREACHABLE)
    van_dir = config.GAME_DIR / 'map' / 'MapStudio'

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

    for map_name in needed_maps:
        p = van_dir / (map_name + '.msb.dcx')
        if not p.exists():
            continue
        data = SoulsFormats.DCX.Decompress(str(p)).ToArray()
        tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_unreach.msb')
        SysFile.WriteAllBytes(tmp, data)
        msb = _msbe.Invoke(None, Array[Object]([tmp]))
        # Index all Parts and Regions by name → y
        def add(name, y):
            key = (map_name, str(name))
            if key in needed_keys and key not in _vanilla_y_cache:
                _vanilla_y_cache[key] = float(y)
        for cat_name in ('Assets', 'DummyAssets', 'Enemies'):
            cat = getattr(msb.Parts, cat_name, None) or []
            for x in cat: add(x.Name, x.Position.Y)
        for reg_name in dir(msb.Regions):
            if reg_name.startswith('_'):
                continue
            try: cat = getattr(msb.Regions, reg_name)
            except: continue
            if not hasattr(cat, '__iter__'): continue
            try:
                for r in cat:
                    try: add(r.Name, r.Position.Y)
                    except: pass
            except: pass
    return _vanilla_y_cache


def is_unreachable_in_err(map_name, name, err_y):
    """True iff (map, name) is excluded. UNCONDITIONAL entries fire only in
    the ERR profile - they encode ERR terrain reshapes confirmed in-game
    (the asset Y matches vanilla), so other profiles must keep those icons.
    For UNREACHABLE entries, the active source's Y must diverge from vanilla
    beyond threshold: 'displaced' kind = ANY direction (`abs(dy) > MIN_DROP`),
    default 'down' kind = only below vanilla (`err_y < vanilla_y - MIN_DROP`).
    Returns False if vanilla data unavailable (conservative - keep the icon)."""
    import config
    key = (map_name, str(name))
    if key in UNCONDITIONAL and config.PROFILE == 'err':
        return True
    if key not in UNREACHABLE:
        return False
    cache = _load_vanilla_cache()
    vy = cache.get(key)
    if vy is None:
        return False
    dy = float(err_y) - vy
    if key in ENTRY_KIND_DISPLACED:
        return abs(dy) > MIN_DROP
    return dy < -MIN_DROP


# ── Per-entity helper for graces ──
_err_entity_cache = None

def _load_err_entity_cache():
    """Scan ERR MSBs for graces in UNREACHABLE: bonfireEntityId → (map, part_name, err_y).
    Only loads MSBs whose tile matches a registered grace entry."""
    global _err_entity_cache
    if _err_entity_cache is not None:
        return _err_entity_cache
    _err_entity_cache = {}
    import config
    if not getattr(config, 'ERR_MOD_DIR', None) or not config.ERR_MOD_DIR.exists():
        return _err_entity_cache

    needed_maps = {m for (m, _) in UNREACHABLE}
    err_dir = config.ERR_MOD_DIR / 'map' / 'MapStudio'

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

    for map_name in needed_maps:
        p = err_dir / (map_name + '.msb.dcx')
        if not p.exists(): continue
        data = SoulsFormats.DCX.Decompress(str(p)).ToArray()
        tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_unreach_err.msb')
        SysFile.WriteAllBytes(tmp, data)
        msb = _msbe.Invoke(None, Array[Object]([tmp]))
        for cat_name in ('Assets', 'DummyAssets', 'Enemies'):
            cat = getattr(msb.Parts, cat_name, None) or []
            for x in cat:
                try:
                    eid = int(getattr(x, 'EntityID', 0) or 0)
                    if eid > 0:
                        _err_entity_cache[eid] = (map_name, str(x.Name), float(x.Position.Y))
                except Exception: pass
    return _err_entity_cache


def is_unreachable_grace(map_name, bonfire_entity_id):
    """For BonfireWarpParam rows: looks up the corresponding MSB asset by
    bonfireEntityId, then checks displacement against vanilla. Returns
    True iff the asset is registered in UNREACHABLE and the displacement
    exceeds threshold (per its kind: 'down' or 'displaced')."""
    if not bonfire_entity_id or int(bonfire_entity_id) <= 0:
        return False
    cache = _load_err_entity_cache()
    entry = cache.get(int(bonfire_entity_id))
    if not entry:
        return False
    err_map, part_name, err_y = entry
    # The grace's BWP row tells us the logical tile; but the MSB asset
    # might live in a different supertile. Use the asset's actual map
    # for the vanilla lookup - that's what UNREACHABLE was registered with.
    return is_unreachable_in_err(err_map, part_name, err_y)
