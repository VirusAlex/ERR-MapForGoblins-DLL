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
DLC_AREAS = {20, 21, 22, 25, 28, 40, 41, 42, 43, 61}
DLC_OVERWORLD_AREAS = {61}
OVERWORLD_AREAS = {60, 61}

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


def get_disp_mask(area):
    """Get display mask field name for a given area.

    Returns the paramdef field that, when set to 1, makes the marker show
    on the matching map plane. Mirrors the vanilla WorldMapPointParam
    convention (see comments above the *_AREAS sets)."""
    if area in UNDERGROUND_AREAS:
        return 'dispMask01'      # base-game underground (m12)
    if area in DLC_AREAS:
        return 'pad2_0'           # bit 2 = dispMask02 in updated paramdefs
                                  # (DLC overworld + DLC/base legacy dungeons)
    return 'dispMask00'           # base-game overworld + small dungeons
