#include "goblin_progress.hpp"

#include "goblin_collected.hpp"
#include "goblin_inject.hpp"   // goblin::flag_is_set (event-flag pickups)
#include "goblin_messages.hpp"
#include "goblin_region_map.hpp"  // baked tile -> game-zone PlaceName id (interiors)
#include "goblin/goblin_map_tiles.hpp"  // MapList (tile->fragment) + goblin::flag + MapTile

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <unordered_map>

using goblin::generated::Category;
using goblin::generated::MAP_ENTRIES;
using goblin::generated::MAP_ENTRY_COUNT;
using goblin::mapPoint::MapTile;

namespace
{

// Map-fragment region flag -> the PlaceName id used for the localized region
// name + an English fallback. Several fragment flags intentionally share a
// PlaceName id (e.g. West/East Limgrave -> "Limgrave", the three Liurnia
// pieces -> "Liurnia of the Lakes"), so they merge into one region group.
// PlaceName ids verified against data/PlaceName_engus.json (vanilla).
struct RegionDef
{
    int flag;
    int32_t place_name_id;
    const char *en;
};

const RegionDef REGION_DEFS[] = {
    {goblin::flag::WestLimgrave, 61000, "Limgrave"},
    {goblin::flag::EastLimgrave, 61000, "Limgrave"},
    {goblin::flag::WeepingPeninsula, 61002, "Weeping Peninsula"},
    {goblin::flag::NorthLiurnia, 62000, "Liurnia of the Lakes"},
    {goblin::flag::EastLiurnia, 62000, "Liurnia of the Lakes"},
    {goblin::flag::WestLiurnia, 62000, "Liurnia of the Lakes"},
    {goblin::flag::Altus, 63000, "Altus Plateau"},
    {goblin::flag::Leyndell, 11000, "Leyndell, Royal Capital"},
    {goblin::flag::Gelmir, 63001, "Mt. Gelmir"},
    {goblin::flag::Caelid, 64000, "Caelid"},
    {goblin::flag::Dragonbarrow, 64001, "Greyoll's Dragonbarrow"},
    {goblin::flag::MountaintopsWest, 65000, "Mountaintops of the Giants"},
    {goblin::flag::MountaintopsEast, 65000, "Mountaintops of the Giants"},
    {goblin::flag::Snowfields, 65002, "Consecrated Snowfield"},
    {goblin::flag::Ainsel, 12012, "Ainsel River"},
    {goblin::flag::LakeOfRot, 12011, "Lake of Rot"},
    {goblin::flag::Mohgwyn, 12050, "Mohgwyn Palace"},
    {goblin::flag::Siofra, 12070, "Siofra River"},
    {goblin::flag::Deeproot, 12030, "Deeproot Depths"},
    {goblin::flag::FarumAzula, 13000, "Crumbling Farum Azula"},
    {goblin::flag::Haligtree, 15001, "Miquella's Haligtree"},
    {goblin::flag::GravesitePlain, 68000, "Gravesite Plain"},
    {goblin::flag::ScaduAltus, 69000, "Scadu Altus"},
    {goblin::flag::SouthernShore, 68300, "Cerulean Coast"},
    {goblin::flag::RauhRuins, 69001, "Rauh Base"},
    {goblin::flag::Abyss, 68600, "Abyssal Woods"},
};

std::string wide_to_utf8(const wchar_t *w)
{
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}

// Resolve the map-fragment region flag for a marker's baked tile. 0 = the tile
// is not attached to any known region (falls into the "Other" bucket).
int tile_flag(const from::paramdef::WORLD_MAP_POINT_PARAM_ST &d)
{
    MapTile chunk(d.areaNo, d.gridXNo, d.gridZNo);
    for (const auto &frag : MapList)
        for (const auto &t : frag.mapFragmentTile)
            if (t == chunk)
            {
                // Lake of Rot shares fine tile m12_01 with Ainsel River but is
                // its own region; split by the baked Lake-of-Rot PlaceName (12011).
                if (chunk == MapTile(12, 1))
                {
                    const int loc[8] = {d.textId1, d.textId2, d.textId3, d.textId4,
                                        d.textId5, d.textId6, d.textId7, d.textId8};
                    for (int x : loc)
                        if (x == 12011) return goblin::flag::LakeOfRot;
                }
                return frag.mapFragmentId;
            }
    return 0;
}

const RegionDef *region_for_flag(int flag)
{
    if (flag == 0) return nullptr;
    for (const auto &r : REGION_DEFS)
        if (r.flag == flag) return &r;
    return nullptr;
}

// Coarse areaNo -> region fallback for tiles the map-fragment MapList doesn't
// cover (e.g. m45 Subterranean Shunning-Grounds, beneath Leyndell). Progress-
// only; does NOT touch the visibility gating in goblin_logic. Keeps stray
// legacy-dungeon markers out of the generic "Other" bucket.
struct AreaFallback { int area; int32_t place_name_id; const char *en; };
const AreaFallback AREA_FALLBACK[] = {
    {45, 11000, "Leyndell, Royal Capital"},  // Subterranean Shunning-Grounds
};

// Look up a baked interior/underground tile in the game-zone map (subCategoryId).
// Returns the PlaceName id (>0) + English fallback, or 0 if the tile isn't mapped.
int32_t zone_for_tile(const from::paramdef::WORLD_MAP_POINT_PARAM_ST &d, const char **en_out)
{
    const uint32_t key = (static_cast<uint32_t>(d.areaNo) << 16) |
                         (static_cast<uint32_t>(d.gridXNo) << 8) |
                         static_cast<uint32_t>(d.gridZNo);
    // REGION_TILES is sorted by key (generator) -> binary search.
    size_t lo = 0, hi = goblin::generated::REGION_TILE_COUNT;
    while (lo < hi)
    {
        size_t mid = (lo + hi) / 2;
        uint32_t k = goblin::generated::REGION_TILES[mid].key;
        if (k < key) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= goblin::generated::REGION_TILE_COUNT ||
        goblin::generated::REGION_TILES[lo].key != key)
        return 0;
    const int32_t pid = goblin::generated::REGION_TILES[lo].place_name_id;
    if (en_out)
    {
        *en_out = "";
        for (size_t i = 0; i < goblin::generated::REGION_NAME_COUNT; ++i)
            if (goblin::generated::REGION_NAMES[i].place_name_id == pid)
            {
                *en_out = goblin::generated::REGION_NAMES[i].en;
                break;
            }
    }
    return pid;
}

// Resolve a marker's region: PlaceName id (or -1 for "Other") + English name.
// Overworld (area 60) uses the coarse map-fragment grouping (Limgrave, Caelid,
// ...). Interior/underground tiles (area != 60) prefer the baked game-zone map so
// distinct zones don't collapse into the fragment that reveals them (Roundtable
// Hold, Stormveil, Leyndell, ... vs "Limgrave"). Falls back to the fragment
// grouping when a tile isn't in the zone map (e.g. profiles without the data).
int32_t resolve_region(const from::paramdef::WORLD_MAP_POINT_PARAM_ST &d, const char **en_out)
{
    if (d.areaNo != 60)
    {
        const char *zen = "";
        int32_t zpid = zone_for_tile(d, &zen);
        if (zpid > 0)
        {
            if (en_out) *en_out = zen;
            return zpid;
        }
    }
    const RegionDef *def = region_for_flag(tile_flag(d));
    if (def)
    {
        if (en_out) *en_out = def->en;
        return def->place_name_id;
    }
    for (const auto &af : AREA_FALLBACK)
        if (af.area == d.areaNo)
        {
            if (en_out) *en_out = af.en;
            return af.place_name_id;
        }
    if (en_out) *en_out = "Other";
    return -1;
}

// A marker is COLLECTED if GEOF/WGM marks it picked up (material nodes, rune
// pieces, gathering, seals...) OR its baked "done" flag is set: textDisableFlagId1
// (loot pickup) or clearedEventFlagId (boss/hawk kill). Same flags the game uses
// to hide the marker.
bool row_is_collected(const goblin::generated::MapEntry &e)
{
    if (goblin::collected::is_original_row_collected(e.row_id)) return true;
    if (goblin::flag_is_set(e.data.textDisableFlagId1)) return true;
    return goblin::flag_is_set(e.data.clearedEventFlagId);
}

// A marker is TRACKABLE (belongs in a collection-progress count) only if it can
// ever register as done: GEOF-capable (geom_slot>=0) or it carries a done flag.
// Permanent world features with neither (Stakes of Marika, Spirit Springs) are
// excluded so they don't show as forever-0/N bars.
bool row_is_trackable(const goblin::generated::MapEntry &e)
{
    return e.geom_slot >= 0 || e.data.textDisableFlagId1 > 0 ||
           e.data.clearedEventFlagId > 0;
}

std::vector<goblin::progress::RegionProgress> g_regions;
double g_last_build_time = -1.0e9;
bool g_built = false;

}  // namespace

void goblin::progress::rebuild()
{
    // Group by PlaceName id so fragment flags that share a region merge.
    // Key -1 is reserved for the "Other" (unmapped) bucket.
    std::unordered_map<int32_t, size_t> index;  // place_name_id -> g_regions index
    std::vector<RegionProgress> regions;
    regions.reserve(32);

    auto get_region = [&](int32_t place_id, const char *en) -> RegionProgress & {
        auto it = index.find(place_id);
        if (it != index.end()) return regions[it->second];
        index[place_id] = regions.size();
        regions.emplace_back();
        RegionProgress &rp = regions.back();
        rp.place_name_id = place_id;
        rp.name = en ? en : "";
        return rp;
    };

    for (size_t i = 0; i < MAP_ENTRY_COUNT; ++i)
    {
        const auto &e = MAP_ENTRIES[i];
        if (!row_is_trackable(e))
            continue;  // permanent world feature (stake/spring) - nothing to collect

        const char *en = "Other";
        int32_t place_id = resolve_region(e.data, &en);

        RegionProgress &rp = get_region(place_id, en);
        const int ci = static_cast<int>(e.category);
        const bool collected = row_is_collected(e);

        rp.total++;
        if (ci >= 0 && ci < kCategoryCount)
        {
            rp.cats[ci].total++;
            if (collected) rp.cats[ci].collected++;
        }
        if (collected) rp.collected++;
    }

    // Localize region names (English fallback already stored). "Other" (id -1)
    // keeps its English label.
    for (auto &rp : regions)
        if (rp.place_name_id > 0)
        {
            const wchar_t *w = goblin::lookup_text(rp.place_name_id);
            std::string loc = wide_to_utf8(w);
            if (!loc.empty()) rp.name = std::move(loc);
        }

    // Sort by name, but always push the "Other" bucket last.
    std::sort(regions.begin(), regions.end(),
              [](const RegionProgress &a, const RegionProgress &b) {
                  bool ao = a.place_name_id < 0, bo = b.place_name_id < 0;
                  if (ao != bo) return !ao;  // non-Other before Other
                  return a.name < b.name;
              });

    g_regions = std::move(regions);
    g_built = true;
}

void goblin::progress::rebuild_if_stale(double now_seconds)
{
    if (!g_built || now_seconds - g_last_build_time >= 0.5)
    {
        rebuild();
        g_last_build_time = now_seconds;
    }
}

const std::vector<goblin::progress::RegionProgress> &goblin::progress::snapshot()
{
    return g_regions;
}

int32_t goblin::progress::region_place_id(const from::paramdef::WORLD_MAP_POINT_PARAM_ST &data)
{
    return resolve_region(data, nullptr);
}
