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
#include <unordered_set>

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
// Coarse area -> its ONE zone, for legacy/story dungeons whose many interior sub-tiles
// (and per-grace sub-areas) would otherwise each surface as their own progress sub-zone.
// Applied to non-fold interiors NOT already resolved by the precise grace tile map, so
// distinct sub-zones the tile map DOES know (Roundtable Hold 11100, Nokron 12020, Leyndell
// Ashen 11050, ...) still win. PlaceName ids verified against data/PlaceName_engus.json.
struct AreaFallback { int area; int32_t place_name_id; const char *en; };
const AreaFallback AREA_FALLBACK[] = {
    {10, 10000, "Stormveil Castle"},
    {11, 11000, "Leyndell, Royal Capital"},
    {13, 13000, "Crumbling Farum Azula"},
    {14, 14000, "Academy of Raya Lucaria"},
    {15, 15000, "Elphael, Brace of the Haligtree"},
    {16, 16000, "Volcano Manor"},
    {18, 18000, "Stranded Graveyard"},
    {20, 20000, "Belurat, Tower Settlement"},
    {21, 21000, "Shadow Keep"},
    {22, 22000, "Stone Coffin Fissure"},
    {28, 28000, "Midra's Manse"},
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

// Minor dungeons (catacombs m30 / caves m31 / tunnels m32 / divine towers m34), the
// underground (m12), and DLC minor dungeons (m40-43) should group under their COARSE
// parent region - never a per-dungeon or per-grace sub-zone. For these the fragment
// grouping (which merges an interior tile into the overworld region that REVEALS it) is
// preferred over the marker's precise baked location name. Big named interiors (Stormveil,
// Leyndell, Nokron, Shadow Keep, ...) are NOT fold-areas and keep their own zone.
static bool is_fold_area(int area)
{
    switch (area)
    {
    case 12: case 30: case 31: case 32: case 34:
    case 40: case 41: case 42: case 43:
        return true;
    default:
        return false;
    }
}

// Resolve a marker's region: PlaceName id (or -1 for "Other") + English name.
// NOTE: no lookup_text() here - resolve_region also runs at inject time (for the focus
// region_id) before the message bank exists, and inject/rebuild MUST agree or focus-by-
// region breaks. rebuild() localizes the returned id via lookup_text.
int32_t resolve_region(const from::paramdef::WORLD_MAP_POINT_PARAM_ST &d, const char **en_out)
{
    // Both the Lands Between (60) and the Land of Shadow (61) are "overworld": grouped by
    // the coarse map-fragment partition, never per-grace.
    const bool overworld = (d.areaNo == 60 || d.areaNo == 61);

    // 1. Precise game-zone from the grace-derived tile map: major interiors (Stormveil,
    //    Leyndell, Nokron, Roundtable Hold, ...) keep their own zone; grace-mapped minor
    //    dungeons already fold to their overworld region here (see generate_region_map.py).
    if (!overworld)
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
    // 2. Coarse fragment region - for the overworld AND every fold-area, so an un-mapped
    //    cave folds into e.g. "Limgrave" instead of its own "Murkwater Cave" sub-zone, and
    //    the underground collapses to "Ainsel River" etc. instead of per-grace sub-areas.
    if (def && (overworld || is_fold_area(d.areaNo)))
    {
        if (en_out) *en_out = def->en;
        return def->place_name_id;
    }
    // 3. Legacy/story dungeons (non-fold interiors): fold ALL their sub-tiles into the ONE
    //    dungeon zone (AREA_FALLBACK), so Leyndell/Stormveil/Farum Azula don't splinter into
    //    a sub-zone per Site of Grace. Runs BEFORE the baked name so it wins over the grace.
    if (!overworld && !is_fold_area(d.areaNo))
    {
        for (const auto &af : AREA_FALLBACK)
            if (af.area == d.areaNo)
            {
                if (en_out) *en_out = af.en;
                return af.place_name_id;
            }
        // 4. Not a known legacy area - use the marker's OWN baked location PlaceName
        //    (slot 2..8; slot 1 is the item name, but a raw slot-1 id < 1e6 is accepted).
        //    Offset-encoded item/npc ids are >= 1e8 so the range filter skips them.
        const int loc[8] = {d.textId1, d.textId2, d.textId3, d.textId4,
                            d.textId5, d.textId6, d.textId7, d.textId8};
        for (int id : loc)
            if (id > 0 && id < 1000000)
            {
                if (en_out) *en_out = "";  // localized from the id in rebuild()
                return id;
            }
    }
    // 5. Any remaining fragment match, else "Other".
    if (def)
    {
        if (en_out) *en_out = def->en;
        return def->place_name_id;
    }
    if (en_out) *en_out = "Other";
    return -1;
}

// Baked-data fallback for markers that are NOT injected as live rows (so the live
// hidden-set below can't cover them): GEOF pickup, any baked "done" flag
// (textDisableFlagId1..8 = loot pickup) or clearedEventFlagId (boss/hawk kill). Same
// flags the game uses to hide the marker. The live hidden-set is preferred when present
// because it also reflects live-loot flag rewrites and manual hide.
bool row_is_collected_baked(const goblin::generated::MapEntry &e)
{
    if (goblin::collected::is_original_row_collected(e.row_id)) return true;
    const unsigned dis[8] = {e.data.textDisableFlagId1, e.data.textDisableFlagId2,
                             e.data.textDisableFlagId3, e.data.textDisableFlagId4,
                             e.data.textDisableFlagId5, e.data.textDisableFlagId6,
                             e.data.textDisableFlagId7, e.data.textDisableFlagId8};
    for (unsigned f : dis)
        if (goblin::flag_is_set(f)) return true;
    return goblin::flag_is_set(e.data.clearedEventFlagId);
}

// A marker is TRACKABLE (belongs in a collection-progress count) only if it can
// ever register as done: GEOF-capable (geom_slot>=0) or it carries a done flag
// (any textDisableFlagId slot, or clearedEventFlagId). Permanent world features with
// none (Stakes of Marika, Spirit Springs) are excluded so they don't show as forever-0/N.
bool row_is_trackable(const goblin::generated::MapEntry &e)
{
    if (e.geom_slot >= 0) return true;
    const unsigned dis[8] = {e.data.textDisableFlagId1, e.data.textDisableFlagId2,
                             e.data.textDisableFlagId3, e.data.textDisableFlagId4,
                             e.data.textDisableFlagId5, e.data.textDisableFlagId6,
                             e.data.textDisableFlagId7, e.data.textDisableFlagId8};
    for (unsigned f : dis)
        if (f > 0) return true;
    return e.data.clearedEventFlagId > 0;
}

// A switched-chest marker (Patches' Glass Shard vs Cloth chest, toggled on flag 3691) is
// ABSENT in the current world-state when its baked group-2 ENABLE flag is off - the OTHER
// variant is the one present. Exclude it from the count entirely (it isn't a real, present
// collectible right now). Only the switched-chest gate is BAKED into group-2; post-event
// story gates are applied at runtime (not in MAP_ENTRIES), so this matches exactly that
// case and never touches story-gated markers.
bool row_switch_gate_off(const goblin::generated::MapEntry &e)
{
    const int g2[8] = {e.data.textEnableFlag2Id1, e.data.textEnableFlag2Id2, e.data.textEnableFlag2Id3,
                       e.data.textEnableFlag2Id4, e.data.textEnableFlag2Id5, e.data.textEnableFlag2Id6,
                       e.data.textEnableFlag2Id7, e.data.textEnableFlag2Id8};
    for (int f : g2)
        if (f > 0 && !goblin::flag_is_set(static_cast<uint32_t>(f))) return true;
    return false;
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

    // Original row ids whose LIVE marker icon is currently hidden (collected / kindling /
    // manually hidden / a live disable or cleared flag set). A hidden marker counts as
    // done - this is the same "is it shown?" test the highlight rings use, so the two
    // always agree. Built once per rebuild (the live-loot rewrite + manual hide only
    // show up here, not in the baked data).
    const std::unordered_set<uint64_t> hidden = goblin::hidden_marker_original_ids();

    auto get_region = [&](int32_t place_id, const char *en, Mega mega) -> RegionProgress & {
        auto it = index.find(place_id);
        if (it != index.end()) return regions[it->second];
        index[place_id] = regions.size();
        regions.emplace_back();
        RegionProgress &rp = regions.back();
        rp.place_name_id = place_id;
        rp.name = en ? en : "";
        rp.mega = mega;
        return rp;
    };

    for (size_t i = 0; i < MAP_ENTRY_COUNT; ++i)
    {
        const auto &e = MAP_ENTRIES[i];
        // Stone Platform (m19) is the endgame Fractured Marika / Elden Beast arena - not a
        // navigable map plane, so its handful of markers can't be projected or highlighted;
        // they'd show as a broken "Stone Platform" region whose focus instantly resets.
        // Skip it (the arena isn't somewhere you plan a route to on the map).
        if (e.data.areaNo == 19)
            continue;
        if (!row_is_trackable(e))
            continue;  // permanent world feature (stake/spring) - nothing to collect
        if (row_switch_gate_off(e))
            continue;  // switched-chest variant absent in this world-state (the twin is present)

        const char *en = "Other";
        int32_t place_id = resolve_region(e.data, &en);

        RegionProgress &rp = get_region(place_id, en, region_mega(e.data));
        const int ci = static_cast<int>(e.category);
        const bool collected = hidden.count(e.row_id) != 0 || row_is_collected_baked(e);

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

    // Sort by mega-section (Lands Between, Dungeons, Shadow), then name; the "Other"
    // bucket always sorts last (after every mega-section).
    std::sort(regions.begin(), regions.end(),
              [](const RegionProgress &a, const RegionProgress &b) {
                  bool ao = a.place_name_id < 0, bo = b.place_name_id < 0;
                  if (ao != bo) return !ao;  // non-Other before Other
                  if (!ao && a.mega != b.mega) return a.mega < b.mega;
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

// Coarse mega-section for a marker, split by the GAME'S OWN map PLANE (same 3 canvases the
// world map switches between), NOT by areaNo:
//   dispMask02 / area 61  = the DLC (Land of Shadow) plane      -> ShadowLands
//   dispMask01 / area 12  = the Underground plane (Siofra, Ainsel, Nokron, Deeproot, ...)
//                                                                -> Dungeons ("Подземелья")
//   dispMask00 / else     = the overworld plane                 -> LandsBetween
// Caves/catacombs/tunnels/legacy dungeons live on the OVERWORLD plane (their entrances are
// on the surface map), so they belong to The Lands Between - only the true subterranean
// world is the Underground section. This mirrors goblin::maphover::map_layer().
goblin::progress::Mega goblin::progress::region_mega(const from::paramdef::WORLD_MAP_POINT_PARAM_ST &d)
{
    if (d.dispMask02 || d.areaNo == 61) return Mega::ShadowLands;
    if (d.dispMask01 || d.areaNo == 12) return Mega::Dungeons;
    return Mega::LandsBetween;
}
