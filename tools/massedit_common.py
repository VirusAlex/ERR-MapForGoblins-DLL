"""Shared utilities for MASSEDIT generator scripts."""

import json
from pathlib import Path

DATA_DIR = Path(__file__).parent.parent / 'data'
OUT_DIR = DATA_DIR / 'massedit_generated'

# Vanilla WorldMapPointParam dispMask conventions (verified against the 740
# rows shipped in regulation.bin):
#   dispMask00 (bit 0) = base game overworld + small/legacy dungeons
#                        (m10-19 except m12, m30-39, m60)
#   dispMask01 (bit 1) = m12 only — the base-game underground map plane
#   dispMask02 (bit 2) = base legacy dungeons that share the DLC plane
#                        (m20-28), DLC legacy dungeons (m40-43), and the
#                        DLC overworld (m61). In our paramdef this bit
#                        is exposed as `pad2_0` (legacy field name).
#
# IMPORTANT: UNDERGROUND_AREAS must be JUST {12}. Earlier versions lumped
# m20-43 in here too, which sent every DLC-dungeon marker to dispMask01,
# i.e. the base-underground plane — exactly where the user reported them
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
