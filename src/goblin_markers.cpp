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
#include "modutils.hpp"
#include "generated/goblin_legacy_conv.hpp"
#include "generated/goblin_map_data.hpp"

#include <windows.h>
#include <psapi.h>
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
        // mov rdi, [rip+disp]; test rdi, rdi — resolve disp to pointer slot.
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

// Beacon types share high byte 0x01, low byte varies by placement context:
//   0x0100 — empty/uninitialized slot
//   0x0101 — base game world placement
//   0x010A — DLC / Shadow Realm placement
// (Other low-byte values may appear for events / unique world states; we
// accept any high-byte 0x01 here. Coord/idx/pad checks below filter out
// random memory hits that happen to have a 0x01 byte at the type offset.)
static bool slot_is_empty_beacon(const MarkerSlot &s)
{
    return s.idx == -1 && s.x == 0.0f && s.z == 0.0f
        && ((s.type >> 8) & 0xFF) == 0x01 && s.pad == 0;
}

static bool slot_is_filled_beacon(const MarkerSlot &s)
{
    if (s.pad != 0) return false;
    if (((s.type >> 8) & 0xFF) != 0x01) return false;
    if (s.idx < 0 || s.idx > 65535) return false;
    return coord_plausible(s.x) && coord_plausible(s.z);
}

static bool slot_is_beacon(const MarkerSlot &s)
{
    return slot_is_empty_beacon(s) || slot_is_filled_beacon(s);
}

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


// ── AOB scan for the 10-slot beacon array ──
//
// Signature: 10 consecutive valid beacon slots at 16-byte alignment.
// At least one must be non-empty to avoid zero-filled memory.
//
// Note: SEH (__try) cannot coexist with C++ unwinding in the same function,
// so the per-region scan is isolated in a C-style helper.

constexpr size_t MAX_HITS = 64;

// Plain function: no C++ objects, can use __try.
static size_t scan_region_for_beacons(const uint8_t *begin, size_t size,
                                      uintptr_t *out_hits, size_t max_hits,
                                      size_t cur_count)
{
    if (size < 160) return cur_count;
    __try
    {
        // Step by 4 bytes — the array is 16-aligned internally but its
        // absolute placement inside a region is NOT guaranteed to align
        // with the region start. Seen in practice at page+0xCBC2.
        for (size_t off = 0; off + 160 <= size; off += 4)
        {
            const MarkerSlot *slots = reinterpret_cast<const MarkerSlot *>(begin + off);
            bool ok = true, any_filled = false;
            for (int i = 0; i < 10; ++i)
            {
                if (!slot_is_beacon(slots[i])) { ok = false; break; }
                if (slots[i].idx >= 0) any_filled = true;
            }
            if (ok && any_filled)
            {
                if (cur_count < max_hits)
                    out_hits[cur_count++] = reinterpret_cast<uintptr_t>(slots);
                else
                    return cur_count;  // table full
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
    return cur_count;
}

static std::vector<uintptr_t> find_beacon_arrays()
{
    uintptr_t hits_arr[MAX_HITS];
    size_t hit_count = 0;

    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = 0;
    HANDLE self = GetCurrentProcess();
    size_t regions_scanned = 0;
    uint64_t bytes_scanned = 0;

    while (VirtualQueryEx(self, reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)))
    {
        if (mbi.State == MEM_COMMIT
            && (mbi.Protect == PAGE_READWRITE
                || mbi.Protect == PAGE_READONLY
                || mbi.Protect == PAGE_EXECUTE_READ
                || mbi.Protect == PAGE_EXECUTE_READWRITE
                || mbi.Protect == PAGE_WRITECOPY)
            && mbi.RegionSize <= 512 * 1024 * 1024)
        {
            ++regions_scanned;
            bytes_scanned += mbi.RegionSize;
            hit_count = scan_region_for_beacons(
                reinterpret_cast<const uint8_t *>(mbi.BaseAddress),
                mbi.RegionSize, hits_arr, MAX_HITS, hit_count);
            if (hit_count >= MAX_HITS) break;
        }
        addr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (mbi.RegionSize == 0) break;
    }

    spdlog::info("Marker scan: {} regions, {} MB, {} candidates",
                 regions_scanned, bytes_scanned / (1024 * 1024), hit_count);
    return std::vector<uintptr_t>(hits_arr, hits_arr + hit_count);
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
        case C::LootRadaFruit: return "Loot - Rada Fruit";
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
// Dungeons need WorldMapLegacyConvParam mapping — skipped for now.

struct NearbyEntry
{
    uint64_t row_id;
    generated::Category category;
    int32_t text_id1;
    uint32_t disable_flag1;
    uint16_t icon_id;
    uint8_t area;
    uint8_t gx;
    uint8_t gz;
    const char *object_name;  // nullptr if none
    float entry_worldX;
    float entry_worldZ;
    float dist;
};

// For dungeon entries, use LegacyConvParam to map onto overworld (m60/m61).
static bool entry_world_coords(const generated::MapEntry &e, float &wx, float &wz)
{
    uint8_t area = e.data.areaNo;
    if (area == 60 || area == 61)
    {
        wx = static_cast<float>(e.data.gridXNo) * 256.0f + e.data.posX;
        wz = static_cast<float>(e.data.gridZNo) * 256.0f + e.data.posZ;
        return true;
    }
    // Dungeon: lookup (srcArea, srcGridX) in conv table (takes first match).
    for (size_t i = 0; i < generated::LEGACY_CONV_COUNT; ++i)
    {
        const auto &c = generated::LEGACY_CONV[i];
        if (c.src_area == area && c.src_gx == e.data.gridXNo)
        {
            wx = static_cast<float>(c.dst_gx) * 256.0f + c.dst_pos_x
               + (e.data.posX - c.src_pos_x);
            wz = static_cast<float>(c.dst_gz) * 256.0f + c.dst_pos_z
               + (e.data.posZ - c.src_pos_z);
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

    std::vector<NearbyEntry> out;
    for (size_t i = 0; i < generated::MAP_ENTRY_COUNT; ++i)
    {
        const auto &e = generated::MAP_ENTRIES[i];
        float ewx, ewz;
        if (!entry_world_coords(e, ewx, ewz)) continue;
        float dx = ewx - beacon_wx;
        float dz = ewz - beacon_wz;
        float d2 = dx * dx + dz * dz;
        if (d2 > r2) continue;
        out.push_back({
            e.row_id, e.category, e.data.textId1, e.data.textDisableFlagId1,
            e.data.iconId, e.data.areaNo, e.data.gridXNo, e.data.gridZNo,
            e.object_name,
            ewx, ewz, std::sqrt(d2)
        });
    }
    std::sort(out.begin(), out.end(),
              [](const NearbyEntry &a, const NearbyEntry &b) { return a.dist < b.dist; });
    return out;
}


// ── The actual dump ──

static void dump_to_file()
{
    if (g_output_path.empty())
    {
        spdlog::warn("Marker dump: output path not set");
        return;
    }

    resolve_flag_api();
    auto arrays = find_beacon_arrays();
    spdlog::info("Marker dump: found {} beacon-array candidate(s)", arrays.size());

    std::ofstream f(g_output_path, std::ios::app);
    if (!f)
    {
        spdlog::warn("Marker dump: cannot open {}", g_output_path.string());
        return;
    }

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
        f << "      " << nearby.size() << " MAP_ENTRIES within 50u:\n";
        char tile[16];
        for (const auto &n : nearby)
        {
            std::snprintf(tile, sizeof(tile), "m%02u_%02u_%02u", n.area, n.gx, n.gz);
            bool hidden_by_flag = n.disable_flag1 && is_flag_set(n.disable_flag1);
            bool hidden_by_mod = goblin::collected::is_original_row_collected(n.row_id);
            const char *status = hidden_by_flag  ? "hidden(flag)"
                               : hidden_by_mod   ? "hidden(collected)"
                                                 : "visible";
            f << "        dist=" << std::fixed << std::setprecision(1) << n.dist
              << "  " << tile
              << "  icon=" << n.icon_id
              << "  row=" << n.row_id
              << "  [" << category_name(n.category) << "]"
              << "  " << status;
            if (n.object_name) f << "  " << n.object_name;
            f << "\n";
        }
    };

    for (size_t ai = 0; ai < arrays.size(); ++ai)
    {
        uintptr_t base = arrays[ai];
        f << "\n---- Array #" << (ai + 1) << " @ 0x"
          << std::hex << base << std::dec << " ----\n";

        // 10 beacon slots
        const MarkerSlot *beacons = reinterpret_cast<const MarkerSlot *>(base);
        for (int i = 0; i < 10; ++i)
        {
            const auto &s = beacons[i];
            if (slot_is_empty(s))
            {
                f << "  [beacon " << i << "] empty\n";
                continue;
            }
            float wx = s.x + 7042.0f, wz = -s.z + 16511.0f;
            f << "  [beacon " << i << "] idx=" << s.idx
              << " type=0x" << std::hex << s.type << std::dec
              << " (" << type_name(s.type) << ")"
              << " map=(" << std::fixed << std::setprecision(2) << s.x << "," << s.z << ")"
              << " world=(" << std::setprecision(1) << wx << "," << wz << ")\n";
            dump_nearby(s);
        }

        // Stamps immediately follow (100+ slots). Scan until 0xFFFF type or invalid.
        const MarkerSlot *stamps = beacons + 10;
        int stamp_count = 0;
        for (int i = 0; i < 200; ++i)
        {
            const auto &s = stamps[i];
            // Terminator: type high-byte 0xFF or all-zero
            if (s.type == 0xFFFF) break;
            if (!slot_is_stamp(s))
            {
                // unknown layout; stop to avoid garbage
                break;
            }
            if (s.idx == -1) continue;  // empty slot — skip
            float wx = s.x + 7042.0f, wz = -s.z + 16511.0f;
            f << "  [stamp  " << i << "] idx=" << s.idx
              << " type=0x" << std::hex << s.type << std::dec
              << " (" << type_name(s.type) << ")"
              << " map=(" << std::fixed << std::setprecision(2) << s.x << "," << s.z << ")"
              << " world=(" << std::setprecision(1) << wx << "," << wz << ")\n";
            dump_nearby(s);
            ++stamp_count;
        }
        f << "  -- " << stamp_count << " stamps --\n";
    }

    spdlog::info("Marker dump written to {}", g_output_path.string());
}


// ── Hotkey loop ──

// SEH-isolated invocation of dump_to_file. The outer try/catch only
// catches C++ exceptions; this additionally traps access violations from
// reading stale game memory during multiplayer transitions or param
// reloads.
static bool seh_dump_to_file_invoke()
{
    __try
    {
        dump_to_file();
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void hotkey_loop()
{
    bool prev_down = false;
    while (true)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if (!config::enableMarkerDump) { prev_down = false; continue; }
        SHORT state = GetAsyncKeyState(static_cast<int>(config::markerDumpKey));
        bool down = (state & 0x8000) != 0;
        if (down && !prev_down)
        {
            bool ok = false;
            try { ok = seh_dump_to_file_invoke(); }
            catch (const std::exception &e) {
                spdlog::error("Marker dump failed: {}", e.what());
            }
            catch (...) { spdlog::error("Marker dump failed: unknown"); }
            if (!ok)
                spdlog::error("Marker dump failed: access violation (stale memory)");
        }
        prev_down = down;
    }
}

}
