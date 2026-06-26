"""Shared utilities for MASSEDIT generator scripts."""

import json
from pathlib import Path

import config

# Profile-scoped (config selects data/ or data/vanilla/ via MFG_PROFILE).
DATA_DIR = config.DATA_DIR
OUT_DIR = DATA_DIR / 'massedit_generated'

# Vanilla WorldMapPointParam dispMask conventions (verified against the 740
# rows shipped in regulation.bin):
#   dispMask00 (bit 0) = base game overworld + small/legacy dungeons
#                        (m10-19 except m12, m30-39, m60)
#   dispMask01 (bit 1) = m12 only - the base-game underground map plane
#   dispMask02 (bit 2) = base legacy dungeons that share the DLC plane
#                        (m20-28), DLC legacy dungeons (m40-43), and the
#                        DLC overworld (m61). In our paramdef this bit
#                        is exposed as `pad2_0` (legacy field name).
#
# IMPORTANT: UNDERGROUND_AREAS must be JUST {12}. Earlier versions lumped
# m20-43 in here too, which sent every DLC-dungeon marker to dispMask01,
# i.e. the base-underground plane - exactly where the user reported them
# wrongly appearing instead of on the DLC map.
UNDERGROUND_AREAS = {12}
DLC_OVERWORLD_AREAS = {61}
OVERWORLD_AREAS = {60, 61}
# DLC_AREAS (legacy areas whose markers belong on the DLC map plane) is NOT hardcoded - it is
# derived below from THIS mod's WorldMapLegacyConvParam at build time, so it adapts to whatever
# legacy areas the loaded overhaul defines/relocates. See is_dlc_plane().

# Valid PlaceName location IDs (for dungeon name fallback)
_loc_path = DATA_DIR / 'valid_location_ids.json'
VALID_LOCATION_IDS = set()
if _loc_path.exists():
    with open(_loc_path) as _f:
        VALID_LOCATION_IDS = set(json.load(_f))


# Legacy dungeon coordinate conversion (WorldMapLegacyConvParam)
_conv_path = DATA_DIR / 'WorldMapLegacyConvParam.json'
_LEGACY_CONV = {}  # (srcArea, srcGx) -> (dstArea, dstGx, dstGz, offsetX, offsetZ)
if _conv_path.exists():
    with open(_conv_path) as _f:
        for _e in json.load(_f):
            _sa = int(_e.get('srcAreaNo', 0))
            _sg = int(_e.get('srcGridXNo', 0))
            if _sa == 0 or (_sa, _sg) in _LEGACY_CONV:
                continue
            _da = int(_e.get('dstAreaNo', 0))
            _dg = int(_e.get('dstGridXNo', 0))
            _dz = int(_e.get('dstGridZNo', 0))
            _ox = float(_e.get('dstPosX', 0)) - float(_e.get('srcPosX', 0))
            _oz = float(_e.get('dstPosZ', 0)) - float(_e.get('srcPosZ', 0))
            _LEGACY_CONV[(_sa, _sg)] = (_da, _dg, _dz, _ox, _oz)

# (area, gridX) pairs this profile's WorldMapLegacyConvParam projects onto the DLC overworld
# (dstAreaNo=61). Keyed by (area, gridX) - NOT area alone - because an area can be mixed:
# e.g. The Convergence 3.x maps m17 wholly to the DLC overworld, but m31 has most sub-grids
# on the base overworld (60) and only a couple (gx 81/83) on the DLC overworld (61), with
# real markers on BOTH. A per-area DLC flag would wrongly send the base m31 caves to the
# DLC plane. is_dlc_plane() below decides each marker's map plane from this set.
_DLC_CONV_KEYS = {(_k[0], _k[1]) for _k, _v in _LEGACY_CONV.items() if _v[0] == 61}

# Data-driven replacement for the old hardcoded DLC dungeon list: the DLC overworld (61) plus
# every legacy area whose conv sub-grids ALL project onto the DLC overworld. Built from the
# loaded mod's WorldMapLegacyConvParam, so an overhaul that adds a wholly-DLC area (e.g. The
# Convergence 3.x's m17) is picked up automatically. MIXED areas (some sub-grids base, some DLC
# - e.g. Convergence m31) are deliberately EXCLUDED so the coarse `area in DLC_AREAS` test (used
# by some generators and by grid-emission) never sends a base sub-grid to the DLC plane; those
# areas are resolved per-sub-grid by is_dlc_plane(). For vanilla/SOTE this reproduces the former
# hardcode exactly ({20,21,22,25,28,40,41,42,43,61}).
_conv_dst_by_area = {}
for (_a, _g), _v in _LEGACY_CONV.items():
    _conv_dst_by_area.setdefault(_a, set()).add(_v[0])
DLC_AREAS = DLC_OVERWORLD_AREAS | {_a for _a, _dsts in _conv_dst_by_area.items() if _dsts == {61}}


def is_dlc_plane(area, gx=0):
    """True if a marker at (area, gridX) belongs on the DLC map plane (dispMask02 / pad2_0).

    Precise, per sub-grid: the DLC overworld (61), a wholly-DLC area (DLC_AREAS), or any legacy
    (area, gridX) this mod's conv param projects onto the DLC overworld. The loot/stake/node
    generators call this so a mixed area (Convergence m31: base caves + a couple DLC sub-grids)
    routes each marker correctly. (Graces read dispMask02 straight from the source
    WorldMapPointParam, so they were already correct; generators that derive the plane from area
    membership are why overhaul-added DLC areas like m17 leaked onto the base map.)"""
    return area in DLC_AREAS or (area, gx) in _DLC_CONV_KEYS


def convert_legacy_coords(area, gx, gz, x, z):
    """Convert legacy dungeon coordinates to overworld. Returns (area, gx, gz, x, z)."""
    key = (area, gx)
    if key in _LEGACY_CONV:
        da, dgx, dgz, ox, oz = _LEGACY_CONV[key]
        return da, dgx, dgz, round(x + ox, 3), round(z + oz, 3)
    return area, gx, gz, x, z



def resolve_location_id(map_name):
    """Compute PlaceName FMG text ID from map code (e.g. 'm21_02_00_00').

    Tries sub-area scheme (area*1000+sub*10), then detail scheme
    (area*10000+sub*100+1), then area-level fallback (area*1000).
    Returns 0 for overworld areas (no subtitle needed).
    """
    parts = map_name.replace('.msb', '').split('_')
    if len(parts) < 4:
        return 0
    area = int(parts[0][1:])
    sub = int(parts[1])
    if area in OVERWORLD_AREAS:
        return 0
    loc_id = area * 1000 + sub * 10
    if loc_id not in VALID_LOCATION_IDS:
        loc_id = area * 10000 + sub * 100 + 1
    if loc_id not in VALID_LOCATION_IDS:
        loc_id = area * 1000
    if loc_id not in VALID_LOCATION_IDS:
        loc_id = 0
    return loc_id


# Lazy grace position index for per-marker location resolution
_GRACE_INDEX = None


def _load_grace_index():
    global _GRACE_INDEX
    if _GRACE_INDEX is not None:
        return _GRACE_INDEX
    p = DATA_DIR / 'grace_position_index.json'
    if p.exists():
        with open(p, encoding='utf-8') as f:
            _GRACE_INDEX = json.load(f)
    else:
        _GRACE_INDEX = []
    return _GRACE_INDEX


# Tiles where multiple PlaceName regions are physically stacked in 3D inside
# the same MSB. For these, the tile-level resolver picks one dominant region
# for the whole tile and mislabels everything on the other vertical layer -
# so we fall back to per-marker nearest-grace lookup. Everywhere else the
# tile-level scheme is correct AND safer: regular caves have a single
# canonical PlaceName per tile, and nearest-grace picks up unrelated
# sub-regions that disagree with the tile's name.
#
# Known stacked case: m12_02 and m12_07 - Nokron, Eternal City sits above
# Siofra River in both tiles.
STACKED_REGION_TILES = {(12, 2), (12, 7)}


def resolve_location_id_at(map_name, x, y, z):
    """Per-marker location resolution - the FALLBACK layer (since 2026-06).

    NO LONGER the primary location name. The primary is the HYBRID sub-area
    resolver in tools/generate_location_overrides.py (MSB MapPoint/MapNameOverride
    volume containment + nearest authored anchor in 3D), which overwrites textId2
    at runtime via generated::LOCATION_ALT. This function bakes the BASELINE
    textId2 that the hybrid overrides where it has an opinion; the baseline
    REMAINS for overworld / no-volume / no-anchor spots where the hybrid is
    silent. Keep as the coarse fallback (do not delete).

    For tiles in STACKED_REGION_TILES, finds the nearest grace in the SAME
    MSB tile by 3D Euclidean distance and returns its subCategoryId - a
    valid PlaceName FMG entry (e.g. 12020 = "Nokron, Eternal City",
    12070 = "Siofra River").

    Everywhere else, defers to the tile-based `resolve_location_id` to keep
    each tile's canonical PlaceName.
    """
    parts = map_name.replace('.msb', '').split('_')
    if len(parts) < 4:
        return resolve_location_id(map_name)
    area = int(parts[0][1:])
    if area in OVERWORLD_AREAS:
        return 0
    try:
        gx = int(parts[1])
        gz = int(parts[2])
    except ValueError:
        return resolve_location_id(map_name)
    if (area, gx) not in STACKED_REGION_TILES:
        return resolve_location_id(map_name)
    graces = _load_grace_index()
    if not graces:
        return resolve_location_id(map_name)
    best = None
    best_d2 = float('inf')
    for g in graces:
        if g.get('areaNo') != area:
            continue
        if g.get('gridX') != gx or g.get('gridZ') != gz:
            continue
        dx = x - g['x']
        dy = y - g['y']
        dz = z - g['z']
        d2 = dx*dx + dy*dy + dz*dz
        if d2 < best_d2:
            best_d2 = d2
            best = g
    if best and best.get('subCategoryId', 0) > 0:
        return int(best['subCategoryId'])
    return resolve_location_id(map_name)


def get_disp_mask(area, gx=0):
    """Get display mask field name for a marker at (area, gridX).

    Returns the paramdef field that, when set to 1, makes the marker show
    on the matching map plane. Mirrors the vanilla WorldMapPointParam
    convention (see comments above the *_AREAS sets). gridX matters for
    mixed legacy areas (see is_dlc_plane); pass it when known."""
    if area in UNDERGROUND_AREAS:
        return 'dispMask01'      # base-game underground (m12)
    if is_dlc_plane(area, gx):
        return 'pad2_0'           # bit 2 = dispMask02 in updated paramdefs
                                  # (DLC overworld + DLC/base legacy dungeons)
    return 'dispMask00'           # base-game overworld + small dungeons
