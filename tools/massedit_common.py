"""Shared utilities for MASSEDIT generator scripts."""

import json
from pathlib import Path

DATA_DIR = Path(__file__).parent.parent / 'data'
OUT_DIR = DATA_DIR / 'massedit_generated'

UNDERGROUND_AREAS = {12, 20, 21, 22, 25, 28, 40, 41, 42, 43}
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


def get_disp_mask(area):
    """Get display mask field name for a given area."""
    if area in UNDERGROUND_AREAS:
        return 'dispMask01'
    elif area in DLC_AREAS:
        return 'pad2_0'
    return 'dispMask00'
