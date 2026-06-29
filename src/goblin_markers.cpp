// Hotkey-driven dump of in-memory map markers (beacons + stamps).
//
// Format in memory (same as save file):
//   16 bytes per slot: {int32 idx, float x, float z, uint16 type, uint16 pad}
//   - Beacons: 10 slots at array base (type 0x0100 = base, 0x010A = DLC)
//   - Stamps: follow beacons (type 0x0600/0x080A/0x0900/0x0901/0x090A)
//   - Empty slot: idx=-1, x=z=0, type=0x0100, pad=0
//
// Strategy: AOB-scan all committed memory for the beacon-array signature.

#include "goblin_markers.hpp"
#include "goblin_collected.hpp"
#include "goblin_config.hpp"
#include "goblin_inject.hpp"
#include "goblin_messages.hpp"
#include "goblin_overlay.hpp"
#include "modutils.hpp"
#include "goblin_legacy_conv.hpp"
#include "goblin_map_data.hpp"
#include "from/params.hpp"
#include "from/paramdef/WORLD_MAP_POINT_PARAM_ST.hpp"

#include <windows.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace goblin::markers
{

#pragma pack(push, 1)
struct MarkerSlot
{
    int32_t idx;
    float x;
    float z;
    uint16_t type;
    uint16_t pad;
};
#pragma pack(pop)
static_assert(sizeof(MarkerSlot) == 16);

static std::filesystem::path g_output_path;

void set_output_path(std::filesystem::path path)
{
    g_output_path = std::move(path);
}


// ── Event flag query (for visibility state) ──
//
// AOB patterns from Erd-Tools. Resolved once lazily on first use.
//   IsEventFlag(void* event_man, uint32_t* flag_id) -> bool
//   EventMan pointer sits at a static RIP-relative address.

using IsEventFlagFn = bool (*)(void *, uint32_t *);
static IsEventFlagFn g_is_event_flag = nullptr;
static void **g_event_man_slot = nullptr;
static bool g_flag_resolve_tried = false;

static void resolve_flag_api()
{
    if (g_flag_resolve_tried) return;
    g_flag_resolve_tried = true;
    try
    {
        g_is_event_flag = modutils::scan<bool(void *, uint32_t *)>(
            { .aob = "48 83 EC 28 8B 12 85 D2" });
    }
    catch (...) { g_is_event_flag = nullptr; }

    try
    {
        // mov rdi, [rip+disp]; test rdi, rdi - resolve disp to pointer slot.
        g_event_man_slot = modutils::scan<void *>(
            { .aob = "48 8B 3D ?? ?? ?? ?? 48 85 FF ?? ?? 32 C0 E9",
              .relative_offsets = { {3, 7} } });
    }
    catch (...) { g_event_man_slot = nullptr; }
    spdlog::info("Flag API: IsEventFlag={} EventMan slot={}",
                 (void *)g_is_event_flag, (void *)g_event_man_slot);
}

static bool is_flag_set(uint32_t flag_id)
{
    if (!g_is_event_flag || !g_event_man_slot) return false;
    void *event_man = *g_event_man_slot;
    if (!event_man) return false;
    uint32_t id = flag_id;
    return g_is_event_flag(event_man, &id);
}


// ── Validation of a 16-byte slot ──

static bool slot_is_empty(const MarkerSlot &s)
{
    return s.idx == -1 && s.x == 0.0f && s.z == 0.0f
        && ((s.type >> 8) & 0xFF) == 0x01 && s.pad == 0;
}

// Map coords are always >100 magnitude in real markers. Rejects denormals /
// subnormals / garbage small-int memory that passes the 16-byte pattern.
static bool coord_plausible(float v)
{
    float a = v < 0 ? -v : v;
    return a > 100.0f && a < 25000.0f;
}

// (Beacon-shape predicates were removed when the array moved from a full
// memory scan to the static pointer chain in find_beacon_arrays - the chain
// gives the exact array, so no signature matching is needed. The slot format
// lives in docs / memory: map-marker-anchor.)

static bool slot_is_stamp(const MarkerSlot &s)
{
    if (s.pad != 0) return false;
    uint16_t hi = (s.type >> 8) & 0xFF;
    if (s.idx == -1)
        return s.x == 0.0f && s.z == 0.0f && (s.type == 0x0100 || hi == 0);
    if (s.idx < 0 || s.idx > 65535) return false;
    if (hi != 0x06 && hi != 0x08 && hi != 0x09) return false;
    return coord_plausible(s.x) && coord_plausible(s.z);
}


static bool seh_copy(const void *src, void *dst, size_t n);  // defined below

// ── Live marker array via a static pointer chain (no memory scan) ──
//
//   obj0 = *(eldenring.exe + 0x3D5DF38)   static slot (survives ASLR/restart)
//   obj1 = *(obj0 + 0x68)                  marker container (vtable RVA 0x2AC21D8)
//   beacons[10] @ obj1 + 0x118 ; stamps[] @ obj1 + 0x1B8 (== beacons + 10 slots).
//   The dumper reads up to N_STAMPS=200 stamp slots, stopping at the first
//   0xFFFF-type terminator / invalid slot (200 is a safe read bound, not a
//   confirmed array capacity).
//
// Found 2026-05-29 by matching real save-beacon coords in memory then chasing
// pointers back to the static slot. Replaces the old full-process scan, which
// produced ~2329 false candidates and raced the allocator (TOCTOU crash on the
// dump-read). The RVAs are build-specific (ERR 2.2.1.2 / app ~2.6.x); the
// +0x68/+0x118/+0x1B8 offsets are the stable struct layout. We validate the
// container's vtable before trusting it. (See memory: map-marker-anchor.)
static constexpr size_t    MARKER_OFF_OBJ1    = 0x68;
static constexpr size_t    MARKER_OFF_BEACONS = 0x118;

// Chain root slot + container vtable resolved by AOB (patch-resilient) instead
// of hardcoded RVAs (was 0x3D5DF38 / 0x2AC21D8 - both move on game updates).
// relative_offsets {{3,7}} extracts the target of the `mov/lea reg,[rip+X]`
// xref; AOBs wildcard the rip-disp. Resolved once, cached, 0 if not found.
static uintptr_t marker_resolve(const char *aob)
{
    return reinterpret_cast<uintptr_t>(modutils::scan<void>({
        .aob = aob, .relative_offsets = {{3, 7}}}));
}
static uintptr_t marker_chain_slot()
{
    static uintptr_t s = marker_resolve("48 8B 0D ?? ?? ?? ?? 48 8B 49 30 48 8D 55 5F");
    return s;
}
static uintptr_t marker_container_vtable()
{
    static uintptr_t s = marker_resolve(
        "48 8D 05 ?? ?? ?? ?? 48 89 07 48 8D 5F 10 48 8D 05 ?? ?? ?? ??");
    return s;
}

static std::vector<uintptr_t> find_beacon_arrays()
{
    std::vector<uintptr_t> out;
    uintptr_t chain = marker_chain_slot(), vtable = marker_container_vtable();
    if (!chain || !vtable)
    { spdlog::warn("Markers: chain/container lookup not resolved (game update?)"); return out; }

    uintptr_t obj0 = 0, obj1 = 0, vtab = 0;
    if (!seh_copy(reinterpret_cast<const void *>(chain), &obj0, sizeof(obj0)) || !obj0)
    { spdlog::warn("Markers: chain root slot empty (not in-world yet?)"); return out; }
    if (!seh_copy(reinterpret_cast<const void *>(obj0 + MARKER_OFF_OBJ1), &obj1, sizeof(obj1)) || !obj1)
    { spdlog::warn("Markers: container not available"); return out; }
    if (!seh_copy(reinterpret_cast<const void *>(obj1), &vtab, sizeof(vtab)))
    { spdlog::warn("Markers: container unreadable"); return out; }
    if (vtab != vtable)
    {
        spdlog::warn("Markers: container table 0x{:X} != resolved 0x{:X} - chain stale "
                     "(game update?) or not ready; skipping", vtab, vtable);
        return out;
    }
    out.push_back(obj1 + MARKER_OFF_BEACONS);
    spdlog::info("Markers: container @ 0x{:X} (beacons @ +0x118, stamps @ +0x1B8)", obj1);
    return out;
}


// ── Format helpers ──

static const char *type_name(uint16_t type)
{
    switch (type)
    {
        case 0x0100: return "beacon-empty";
        case 0x0101: return "beacon";
        case 0x010A: return "beacon-dlc";
        case 0x0600: return "stamp-icon";
        case 0x080A: return "stamp-dlc-alt";
        case 0x0900: return "stamp";
        case 0x0901: return "stamp-var";
        case 0x090A: return "stamp-dlc";
        default:
            if (((type >> 8) & 0xFF) == 0x01) return "beacon-other";
            return "?";
    }
}


static const char *category_name(generated::Category c)
{
    using C = generated::Category;
    switch (c)
    {
        case C::EquipArmaments: return "Equipment - Armaments";
        case C::EquipArmour: return "Equipment - Armour";
        case C::EquipAshesOfWar: return "Equipment - Ashes of War";
        case C::EquipSpirits: return "Equipment - Spirits";
        case C::EquipTalismans: return "Equipment - Talismans";
        case C::KeyCelestialDew: return "Key - Celestial Dew";
        case C::KeyCookbooks: return "Key - Cookbooks";
        case C::KeyCrystalTears: return "Key - Crystal Tears";
        case C::KeyImbuedSwordKeys: return "Key - Imbued Sword Keys";
        case C::KeyLarvalTears: return "Key - Larval Tears";
        case C::KeyScadutreeFragments: return "Key - Scadutree Fragments";
        case C::KeyGreatRunes: return "Key - Great Runes";
        case C::KeyLostAshes: return "Key - Lost Ashes";
        case C::KeyPotsNPerfumes: return "Key - Pots n Perfumes";
        case C::KeySeedsTears: return "Key - Seeds Tears Ashes";
        case C::KeyWhetblades: return "Key - Whetblades";
        case C::LootAmmo: return "Loot - Ammo";
        case C::LootBellBearings: return "Loot - Bell-Bearings";
        case C::LootMerchantBellBearings: return "Loot - Merchant Bell-Bearings";
        case C::LootConsumables: return "Loot - Consumables";
        case C::LootCraftingMaterials: return "Loot - Crafting Materials";
        case C::LootMPFingers: return "Loot - MP-Fingers";
        case C::LootMaterialNodes: return "Loot - Material Nodes";
        case C::LootReusables: return "Loot - Reusables";
        case C::LootSmithingStones: return "Loot - Smithing Stones";
        case C::LootSmithingStonesLow: return "Loot - Smithing Stones (Low)";
        case C::LootSmithingStonesRare: return "Loot - Smithing Stones (Rare)";
        case C::LootGoldenRunes: return "Loot - Golden Runes";
        case C::LootGoldenRunesLow: return "Loot - Golden Runes (Low)";
        case C::LootStoneswordKeys: return "Loot - Stonesword Keys";
        case C::LootThrowables: return "Loot - Throwables";
        case C::LootPrattlingPates: return "Loot - Prattling Pates";
        case C::LootRuneArcs: return "Loot - Rune Arcs";
        case C::LootDragonHearts: return "Loot - Dragon Hearts";
        case C::LootGloveworts: return "Loot - Gloveworts";
        case C::LootGreatGloveworts: return "Loot - Great Gloveworts";
        case C::LootGestures: return "Loot - Gestures";
        case C::LootGreases: return "Loot - Greases";
        case C::LootUtilities: return "Loot - Utilities";
        case C::LootStatBoosts: return "Loot - Stat Boosts";
        case C::ReforgedFortunes: return "Reforged - Fortunes";
        case C::MagicIncantations: return "Magic - Incantations";
        case C::MagicMemoryStones: return "Magic - Memory Stones";
        case C::MagicPrayerbooks: return "Magic - Prayerbooks";
        case C::MagicSorceries: return "Magic - Sorceries";
        case C::WorldBosses: return "World - Bosses";
        case C::QuestDeathroot: return "Quest - Deathroot";
        case C::QuestProgression: return "Quest - Progression";
        case C::QuestSeedbedCurses: return "Quest - Seedbed Curses";
        case C::ReforgedEmberPieces: return "Reforged - Ember Pieces";
        case C::ReforgedItemsAndChanges: return "Reforged - Items";
        case C::ReforgedRunePieces: return "Reforged - Rune Pieces";
        case C::WorldGraces: return "World - Graces";
        case C::WorldHostileNPC: return "World - Hostile NPC";
        case C::WorldImpStatues: return "World - Imp Statues";
        case C::WorldMaps: return "World - Maps";
        case C::WorldPaintings: return "World - Paintings";
        case C::WorldSpiritSprings: return "World - Spirit Springs";
        case C::WorldSpiritspringHawks: return "World - Spiritspring Hawks";
        case C::WorldStakesOfMarika: return "World - Stakes of Marika";
        case C::WorldSummoningPools: return "World - Summoning Pools";
        case C::WorldInteractables: return "World - Interactables";
    }
    return "?";
}


// ── Nearby MAP_ENTRY lookup ──
//
// Beacon coords in memory are "map-UI" space; convert to world via:
//   worldX = mapX + 7042
//   worldZ = -mapZ + 16511
// MAP_ENTRY (overworld m60/m61) world coords:
//   worldX = gridX * 256 + posX
//   worldZ = gridZ * 256 + posZ
// Dungeons need WorldMapLegacyConvParam mapping - skipped for now.

struct NearbyEntry
{
    uint64_t row_id;
    const from::paramdef::WORLD_MAP_POINT_PARAM_ST *data;  // baked row (all 8 textId slots)
    generated::Category category;
    int32_t item_tid;         // loot item-name textId  (0 = none)
    int32_t loc_tid;          // location (PlaceName) textId
    int32_t enemy_tid;        // drop-source enemy/npc textId (enemy drops, bosses)
    uint32_t disable_flag1;
    uint16_t icon_id;
    uint8_t area;
    uint8_t gx;
    uint8_t gz;
    float posX, posY, posZ;   // marker's own local map coords
    uint32_t lot_id;          // source ItemLotParam row (0 = none)
    const char *object_name;  // MSB object e.g. AEG099_610_9007, nullptr if none
    float entry_worldX;
    float entry_worldZ;
    float dist;
    bool is_ours;             // true = a marker this mod injected (vs vanilla/overhaul)
};

// Classify a marker's textId slots into loot / location / drop-source by their
// offset-encoding band. The slot ORDER is not fixed - plain loot is
// t1=item,t2=loc; enemy-drop loot is t1=item,t2=enemy,t3=loc; a boss marker is
// t1=enemy,t2=loc - so we go by id range, never by slot index:
//   loot item:    [50M, 600M)   (weapon 100M / protector 200M / … / goods 500M)
//   location:     (0, 50M)      raw PlaceName, minus logic sentinels
//   drop source:  >= 700M       (NpcName +700M, enemy +900M, BloodMsg +950M, +1.6B)
// (COMPOSE location ids 999M+ are a runtime-only hybrid override, never present
// in the baked MAP_ENTRIES this dump reads, so they don't collide here.)
static void classify_textids(const from::paramdef::WORLD_MAP_POINT_PARAM_ST &d,
                             int32_t &item_tid, int32_t &loc_tid, int32_t &enemy_tid)
{
    item_tid = loc_tid = enemy_tid = 0;
    const int32_t slots[8] = { d.textId1, d.textId2, d.textId3, d.textId4,
                               d.textId5, d.textId6, d.textId7, d.textId8 };
    for (int32_t v : slots)
    {
        if (v <= 0) continue;
        if (v >= 50000000 && v < 600000000) { if (!item_tid)  item_tid = v; }
        else if (v >= 700000000)            { if (!enemy_tid) enemy_tid = v; }
        else if (v < 50000000 && v != 5000 && v != 5100 && v != 5300 && v != 8800)
                                            { if (!loc_tid)   loc_tid = v; }
    }
}

// UTF-16 (game text) -> UTF-8 for the log file. Empty string on null/failure.
static std::string wide_to_utf8(const wchar_t *w)
{
    if (!w || !*w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}

// Resolve a marker textId to its in-game string (player's language), the way the
// game does: offset-encoded ids were relocated to fresh ids (remap_textid), and
// DLC-layer-only PlaceName ids aren't in our expanded base buffer (probe layers).
// "" if it resolves to nothing.
static std::string resolve_text(int32_t text_id)
{
    if (text_id <= 0)
        return {};
    int32_t mapped = goblin::remap_textid(text_id);
    if (const wchar_t *s = goblin::lookup_text(mapped))
        return wide_to_utf8(s);
    if (const wchar_t *s = goblin::lookup_text_dlc(text_id))
        return wide_to_utf8(s);
    return {};
}

// Map a marker's (area, grid, local pos) to overworld world coords. Overworld tiles
// (m60/m61) are direct; dungeon areas go through WorldMapLegacyConvParam (our baked
// LEGACY_CONV table, dumped from the same param the game uses).
static bool compute_world_coords(uint8_t area, uint8_t gx, uint8_t gz,
                                 float posX, float posZ, float &wx, float &wz)
{
    if (area == 60 || area == 61)
    {
        wx = static_cast<float>(gx) * 256.0f + posX;
        wz = static_cast<float>(gz) * 256.0f + posZ;
        return true;
    }
    for (size_t i = 0; i < generated::LEGACY_CONV_COUNT; ++i)
    {
        const auto &c = generated::LEGACY_CONV[i];
        if (c.src_area == area && c.src_gx == gx)
        {
            wx = static_cast<float>(c.dst_gx) * 256.0f + c.dst_pos_x + (posX - c.src_pos_x);
            wz = static_cast<float>(c.dst_gz) * 256.0f + c.dst_pos_z + (posZ - c.src_pos_z);
            return true;
        }
    }
    return false;  // unmappable (rare edge dungeons)
}


static std::vector<NearbyEntry> find_nearby_overworld(float mapX, float mapZ, float radius)
{
    float beacon_wx = mapX + 7042.0f;
    float beacon_wz = -mapZ + 16511.0f;
    float r2 = radius * radius;

    // Pointer identity of OUR injected rows - reliable even though the engine may
    // reassign their row ids at load (see row_id_registry), so match by data ptr.
    const auto &ours = goblin::injected_row_ptrs();
    std::unordered_set<const void *> ourset(ours.begin(), ours.end());

    std::vector<NearbyEntry> out;
    // Iterate the LIVE WorldMapPointParam so the dump lists EVERY marker on the map
    // (vanilla + overhaul + ours), not only the ones we injected.
    try
    {
        auto param = from::params::get_param<from::paramdef::WORLD_MAP_POINT_PARAM_ST>(
            L"WorldMapPointParam");
        for (auto entry : param)
        {
            uint64_t row_id = entry.first;
            auto &row = entry.second;
            float ewx, ewz;
            if (!compute_world_coords(row.areaNo, row.gridXNo, row.gridZNo,
                                      row.posX, row.posZ, ewx, ewz))
                continue;
            float dx = ewx - beacon_wx;
            float dz = ewz - beacon_wz;
            float d2 = dx * dx + dz * dz;
            if (d2 > r2) continue;
            int32_t item_tid, loc_tid, enemy_tid;
            classify_textids(row, item_tid, loc_tid, enemy_tid);
            out.push_back({
                row_id, &row, generated::Category{}, item_tid, loc_tid, enemy_tid,
                row.textDisableFlagId1,
                row.iconId, row.areaNo, row.gridXNo, row.gridZNo,
                row.posX, row.posY, row.posZ, 0u,
                nullptr,
                ewx, ewz, std::sqrt(d2),
                ourset.count(&row) != 0
            });
        }
    }
    catch (...)
    {
        spdlog::warn("Marker dump: WorldMapPointParam not available - live marker list skipped");
    }
    std::sort(out.begin(), out.end(),
              [](const NearbyEntry &a, const NearbyEntry &b) { return a.dist < b.dist; });
    return out;
}


// ── The actual dump ──

// Plain (POD-only) SEH-guarded memcpy: copies a found beacon array out of live
// game memory into a local buffer before we read it field-by-field. The array
// is located by a full process-memory scan, but the game's allocator can free
// or move the region between the scan and the read (a TOCTOU race) - reading it
// directly then access-violates and the whole dump aborts. Copying through this
// guard means a stale array is skipped, not fatal.
static bool seh_copy(const void *src, void *dst, size_t n)
{
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Returns the number of marker slots written (beacons + stamps), or -1 if the
// output path isn't set. 0 is a valid result (no beacon arrays live yet).
static int dump_impl(std::ostream &f, DumpSel sel)
{
    resolve_flag_api();
    auto arrays = find_beacon_arrays();
    spdlog::info("Marker dump: found {} beacon-array candidate(s)", arrays.size());

    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm_buf{};
    localtime_s(&tm_buf, &now);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);

    f << "\n===== " << ts << "  (" << arrays.size() << " beacon arrays found) =====\n";

    auto dump_nearby = [&](const MarkerSlot &s) {
        constexpr float RADIUS = 50.0f;
        auto nearby = find_nearby_overworld(s.x, s.z, RADIUS);
        if (nearby.empty())
        {
            f << "      (no overworld MAP_ENTRIES within 50u)\n";
            return;
        }
        f << "      " << nearby.size() << " markers within 50u (all map markers, not just ours):\n";
        char tile[16];
        for (const auto &n : nearby)
        {
            std::snprintf(tile, sizeof(tile), "m%02u_%02u_%02u", n.area, n.gx, n.gz);
            bool hidden_by_flag = n.disable_flag1 && is_flag_set(n.disable_flag1);
            bool hidden_by_mod = n.is_ours && goblin::collected::is_original_row_collected(n.row_id);
            const char *status = hidden_by_flag  ? "hidden(flag)"
                               : hidden_by_mod   ? "hidden(collected)"
                                                 : "visible";
            f << "        dist=" << std::fixed << std::setprecision(1) << n.dist
              << "  " << tile
              << "  icon=" << n.icon_id
              << "  row=" << n.row_id
              << (n.is_ours ? "  [ours]" : "  [live]")
              << "  " << status
              << "  pos=(" << std::fixed << std::setprecision(2)
              << n.posX << "," << n.posY << "," << n.posZ << ")";
            f << "\n";

            // Compact per-slot textId diagnostic, one line for the whole marker.
            // Per used slot (raw > 0): tN=raw[>remap]='resolved'  - remap shown only
            // when it differs, the string from our expanded base buffer or (~prefix)
            // the DLC PlaceName layers, EMPTY when nothing resolves. Lets us tell a
            // genuinely text-less marker (engine draws no icon) from a wrong string.
            if (n.data)
            {
                const int32_t tids[8] = {n.data->textId1, n.data->textId2, n.data->textId3,
                                         n.data->textId4, n.data->textId5, n.data->textId6,
                                         n.data->textId7, n.data->textId8};
                f << "            ids:";
                for (int i = 0; i < 8; ++i)
                {
                    int32_t raw = tids[i];
                    if (raw <= 0) continue;
                    int32_t rm = goblin::remap_textid(raw);
                    std::string s = wide_to_utf8(goblin::lookup_text(rm));
                    bool from_dlc = false;
                    if (s.empty()) { s = wide_to_utf8(goblin::lookup_text_dlc(raw)); from_dlc = !s.empty(); }
                    f << "  t" << (i + 1) << "=" << raw;
                    if (rm != raw) f << ">" << rm;
                    if (!s.empty())      f << "='" << (from_dlc ? "~" : "") << s << "'";
                    else                 f << "=EMPTY";
                }
                f << "\n";
            }
        }
    };

    constexpr int N_BEACONS = 10;        // physical slots before the stamp array
    constexpr int N_BEACONS_SHOWN = 5;   // the game only lets you place 5 (slots 0-4)
    constexpr int N_STAMPS  = 200;
    int total_markers = 0;

    for (size_t ai = 0; ai < arrays.size(); ++ai)
    {
        uintptr_t base = arrays[ai];

        // Copy the whole array (beacons + stamps) out of live memory in one
        // guarded shot, then read from the safe local copy. Eliminates the
        // TOCTOU AV that aborted the dump when the region moved/freed between
        // the scan and the read.
        MarkerSlot buf[N_BEACONS + N_STAMPS];
        if (!seh_copy(reinterpret_cast<const void *>(base), buf, sizeof(buf)))
        {
            f << "\n---- Array #" << (ai + 1) << " @ 0x" << std::hex << base << std::dec
              << " - SKIPPED (memory freed mid-read) ----\n";
            spdlog::warn("Marker dump: array #{} @ 0x{:X} vanished mid-read - skipped",
                         ai + 1, base);
            continue;
        }

        f << "\n---- Array #" << (ai + 1) << " @ 0x"
          << std::hex << base << std::dec << " ----\n";

        const MarkerSlot *beacons = buf;
        for (int i = 0; sel != DUMP_STAMPS && i < N_BEACONS_SHOWN; ++i)
        {
            const auto &s = beacons[i];
            if (slot_is_empty(s))
            {
                f << "  [beacon " << (i + 1) << "] empty\n";
                continue;
            }
            float wx = s.x + 7042.0f, wz = -s.z + 16511.0f;
            f << "  [beacon " << (i + 1) << "] idx=" << s.idx
              << " type=0x" << std::hex << s.type << std::dec
              << " (" << type_name(s.type) << ")"
              << " map=(" << std::fixed << std::setprecision(2) << s.x << "," << s.z << ")"
              << " world=(" << std::setprecision(1) << wx << "," << wz << ")\n";
            dump_nearby(s);
            ++total_markers;
        }

        // Stamps immediately follow. Scan until 0xFFFF type or invalid.
        const MarkerSlot *stamps = buf + N_BEACONS;
        int stamp_count = 0;
        for (int i = 0; sel != DUMP_BEACONS && i < N_STAMPS; ++i)
        {
            const auto &s = stamps[i];
            // Terminator: type high-byte 0xFF or all-zero
            if (s.type == 0xFFFF) break;
            if (!slot_is_stamp(s))
            {
                // unknown layout; stop to avoid garbage
                break;
            }
            if (s.idx == -1) continue;  // empty slot - skip
            float wx = s.x + 7042.0f, wz = -s.z + 16511.0f;
            f << "  [stamp  " << i << "] idx=" << s.idx
              << " type=0x" << std::hex << s.type << std::dec
              << " (" << type_name(s.type) << ")"
              << " map=(" << std::fixed << std::setprecision(2) << s.x << "," << s.z << ")"
              << " world=(" << std::setprecision(1) << wx << "," << wz << ")\n";
            dump_nearby(s);
            ++stamp_count;
            ++total_markers;
        }
        if (sel != DUMP_BEACONS)
            f << "  -- " << stamp_count << " stamps --\n";
    }

    return total_markers;
}

// SEH-guarded body invocation (only an ostream& in this frame, so __try is OK).
static int seh_dump_impl(std::ostream &f, DumpSel sel)
{
    __try { return dump_impl(f, sel); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -2; }
}

// File wrapper: append the dump to the configured log path.
static int dump_to_file()
{
    if (g_output_path.empty())
    {
        spdlog::warn("Marker dump: output path not set");
        return -1;
    }
    std::ofstream f(g_output_path, std::ios::app);
    if (!f)
    {
        spdlog::warn("Marker dump: cannot open {}", g_output_path.string());
        return -1;
    }
    int n = seh_dump_impl(f, DUMP_ALL); // log file always has both kinds together
    spdlog::info("Marker dump written to {} ({} markers)", g_output_path.string(), n);
    return n;
}

// In-memory dump for the in-game overlay's Tools tab (copyable text box).
std::string dump_to_string(DumpSel sel)
{
    std::ostringstream ss;
    int n = seh_dump_impl(ss, sel);
    if (n < 0)
        ss << "\n(no markers found / dump error code " << n
           << " - open the world map first, then dump)\n";
    return ss.str();
}


// ── Hotkey loop ──

// SEH-isolated invocation of dump_to_file. Returns the marker count (>=0) on
// success, or -2 if an access violation escaped the per-array guards (e.g. a
// fault in find_beacon_arrays itself). dump_to_file returns -1 if the output
// path isn't set.
static int seh_dump_to_file_invoke()
{
    __try
    {
        return dump_to_file();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -2;
    }
}

void hotkey_loop()
{
    bool prev_down = false;
    while (true)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        if (!config::enableMarkerDump) { prev_down = false; continue; }
        bool down = goblin::overlay::key_down(static_cast<int>(config::markerDumpKey));
        if (down && !prev_down)
        {
            int count = -2;
            try { count = seh_dump_to_file_invoke(); }
            catch (const std::exception &e) {
                spdlog::error("Marker dump failed: {}", e.what()); count = -2;
            }
            catch (...) { spdlog::error("Marker dump failed: unknown"); count = -2; }

            // On-screen feedback via the codex toast (static DUMP_OK/FAIL text).
            // The earlier F9 crash here was NOT override/map related - it was
            // the same broken trampoline RVA that also crashed F10 (the May-2026
            // game update shifted .text). With the trampoline now AOB-resolved,
            // this is safe again. Exact count goes to the log.
            if (count >= 0)
                spdlog::info("Marker dump OK: {} markers", count);
            else
                spdlog::error("Marker dump failed (code {})", count);
            goblin::show_codex_toast(goblin::g_toast_param_row_id[
                count >= 0 ? goblin::TOAST_DUMP_OK : goblin::TOAST_DUMP_FAIL]);
        }
        prev_down = down;
    }
}

}
