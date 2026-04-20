#include "goblin_collected.hpp"
#include "goblin_config.hpp"
#include "generated/goblin_map_data.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <spdlog/spdlog.h>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using Category = goblin::generated::Category;

// Model IDs are: 10,000,000 + AEG_group*1000 + model_number
// e.g. AEG099_821 = 10,099,821 = 0x009A1C6D
static constexpr uint16_t GEOM_IDX_MIN = 0x1194;

// ─── data-driven model tracking ─────────────────────────────────────
// Built from MAP_ENTRIES at init time — no hardcoded model lists.

static std::set<std::string> g_tracked_prefixes;   // "AEG099_821", "AEG099_651", etc.
static std::set<uint32_t>    g_tracked_model_ids;   // 10099821, 10099651, etc.

static std::string prefix_from_object_name(const char *name)
{
    // "AEG099_821_9000" → "AEG099_821"
    const char *last_us = strrchr(name, '_');
    if (!last_us || last_us == name) return {};
    return std::string(name, last_us - name);
}

static std::string prefix_from_model_id(uint32_t model_id)
{
    if (model_id < 10000000) return {};
    uint32_t raw = model_id - 10000000;
    uint32_t group = raw / 1000;
    uint32_t number = raw % 1000;
    char buf[20];
    snprintf(buf, sizeof(buf), "AEG%03u_%03u", group, number);
    return buf;
}

static uint32_t model_id_from_prefix(const std::string &prefix)
{
    // "AEG099_821" → 10099821
    if (prefix.size() < 10 || prefix.substr(0, 3) != "AEG" || prefix[6] != '_')
        return 0;
    unsigned group = 0, number = 0;
    if (sscanf(prefix.c_str(), "AEG%u_%u", &group, &number) != 2) return 0;
    return 10000000u + group * 1000u + number;
}

static std::set<uint64_t> g_collected_rows;
static int g_collected_count = 0;
static int g_unmatched_count = 0;

struct ParamRef {
    uint8_t *ptr;
    uint8_t original_areaNo;
};
static std::map<uint64_t, ParamRef> g_param_ptrs;

static std::map<uint32_t, std::vector<uint64_t>> g_tile_to_rows;                // tile → ordered row_ids
static std::map<uint32_t, std::map<std::string, uint64_t>> g_tile_name_to_row;  // tile → object_name → row_id
// 3D slot map: tile → prefix → geom_slot → row_id  (no cross-model collisions)
static std::map<uint32_t, std::map<std::string, std::map<int, uint64_t>>> g_tile_slot_to_row;
static bool g_initialized = false;
static bool g_dumped_geom_ins = false;


// ─── save file discovery ───────────────────────────────────────────────

static std::filesystem::path find_save_file()
{
    char *appdata = nullptr;
    size_t len = 0;
    _dupenv_s(&appdata, &len, "APPDATA");
    if (!appdata)
        return {};

    std::filesystem::path er_dir = std::filesystem::path(appdata) / "EldenRing";
    free(appdata);

    if (!std::filesystem::exists(er_dir))
        return {};

    for (auto &entry : std::filesystem::directory_iterator(er_dir))
    {
        if (!entry.is_directory())
            continue;
        for (auto ext : {"ER0000.err", "ER0000.sl2"})
        {
            auto candidate = entry.path() / ext;
            if (std::filesystem::exists(candidate))
                return candidate;
        }
    }
    return {};
}

struct GEOFEntry
{
    uint32_t tile_id;
    uint8_t flags;
    uint16_t geom_idx;
    uint32_t model_hash;  // bytes 4-7 of GEOF entry, identifies model type
};

struct GEOFSection
{
    size_t offset;
    int piece_count;
    std::vector<GEOFEntry> pieces;
};

// Each geom_idx holds two slots: flags=0x00 → even, flags=0x80 → odd
static int aeg099_index_from_geof(uint16_t geom_idx, uint8_t flags)
{
    return (geom_idx - GEOM_IDX_MIN) * 2 + ((flags & 0x80) ? 1 : 0);
}

// ─── GEOF parsing from save file ──────────────────────────────────────

static bool is_aeg099_geom(uint16_t geom_idx, uint8_t flags)
{
    return geom_idx >= GEOM_IDX_MIN &&
           (flags == 0x00 || flags == 0x80);
}

static std::vector<GEOFSection> parse_all_geof_sections(const std::vector<uint8_t> &data)
{
    std::vector<GEOFSection> sections;
    const uint8_t magic[] = {'F', 'O', 'E', 'G'};

    for (size_t pos = 4; pos + 4 < data.size(); pos++)
    {
        if (memcmp(data.data() + pos, magic, 4) != 0)
            continue;

        int32_t total_size = 0;
        memcpy(&total_size, data.data() + pos - 4, 4);
        if (total_size <= 12 || total_size > 0x100000)
            continue;

        size_t section_start = pos - 4;
        size_t chunk_pos = section_start + 12;
        size_t section_end = section_start + total_size;

        GEOFSection sec;
        sec.offset = section_start;
        sec.piece_count = 0;

        while (chunk_pos + 16 <= section_end && chunk_pos + 16 <= data.size())
        {
            if (data[chunk_pos] == 0xFF && data[chunk_pos + 1] == 0xFF &&
                data[chunk_pos + 2] == 0xFF && data[chunk_pos + 3] == 0xFF)
                break;

            int32_t entry_size = 0;
            memcpy(&entry_size, data.data() + chunk_pos + 4, 4);
            if (entry_size <= 0 || entry_size > 0x100000)
                break;

            uint32_t tile_id = 0, count = 0;
            memcpy(&tile_id, data.data() + chunk_pos, 4);
            memcpy(&count, data.data() + chunk_pos + 8, 4);

            for (uint32_t ei = 0; ei < count; ei++)
            {
                size_t eoff = chunk_pos + 16 + ei * 8;
                if (eoff + 8 > data.size())
                    break;

                uint8_t entry_flags = data[eoff + 1];
                uint16_t geom_idx = data[eoff + 2] | (data[eoff + 3] << 8);

                if (is_aeg099_geom(geom_idx, entry_flags))
                {
                    sec.pieces.push_back({tile_id, entry_flags, geom_idx});
                    sec.piece_count++;
                }
            }
            chunk_pos += entry_size;
        }
        sections.push_back(std::move(sec));
    }
    return sections;
}

// ─── GEOF from memory (GeomFlagSaveDataManager) ──────────────────────

static constexpr uintptr_t RVA_GEOM_FLAG     = 0x3D69D18;  // GeomFlagSaveDataManager (unloaded tiles)
static constexpr uintptr_t RVA_GEOM_NONACTIVE = 0x3D69D98;  // GeomNonActiveBlockManager
static constexpr uintptr_t RVA_WORLD_GEOM_MAN = 0x3D69BA8;  // CSWorldGeomMan (loaded tiles)

static bool safe_read(void *addr, void *out, size_t count)
{
    __try
    {
        memcpy(out, addr, count);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static void read_singleton_entries(uintptr_t game_base, uintptr_t rva,
                                    std::vector<GEOFEntry> &out)
{
    void *gf_ptr = nullptr;
    if (!safe_read((void *)(game_base + rva), &gf_ptr, 8) || !gf_ptr)
        return;

    int tiles_found = 0, tiles_skipped = 0, consecutive_empty = 0;
    for (int off = 0x08; off < 0x20000; off += 16)
    {
        uint64_t id_val = 0, ptr_val = 0;
        if (!safe_read((char *)gf_ptr + off, &id_val, 8))
            break;
        if (!safe_read((char *)gf_ptr + off + 8, &ptr_val, 8))
            break;

        if (id_val == 0 && ptr_val == 0)
        {
            if (++consecutive_empty > 256)
                break;
            continue;
        }
        consecutive_empty = 0;

        uint32_t tile_id = (uint32_t)id_val;
        uint8_t area = (tile_id >> 24) & 0xFF;
        if (area < 0x0A || area > 0x3D)
        {
            tiles_skipped++;
            continue;
        }
        if (ptr_val < 0x10000 || ptr_val > 0x7FFFFFFFFFFF)
        {
            tiles_skipped++;
            continue;
        }
        tiles_found++;

        // Layout A: count @+8, entries @+16 | Layout B: count @+0, entries @+8
        uint8_t header[16] = {};
        if (!safe_read((void *)ptr_val, header, 16))
            continue;

        uint32_t count = 0;
        uintptr_t entries_start = 0;

        uint32_t countA = 0;
        memcpy(&countA, header + 8, 4);
        uint32_t countB = 0;
        memcpy(&countB, header + 0, 4);

        if (countA > 0 && countA < 100000)
        {
            count = countA;
            entries_start = ptr_val + 16;
        }
        else if (countB > 0 && countB < 100000)
        {
            count = countB;
            entries_start = ptr_val + 8;
        }
        else
            continue;

        for (uint32_t ei = 0; ei < count; ei++)
        {
            uint8_t entry[8] = {};
            if (!safe_read((void *)(entries_start + ei * 8), entry, 8))
                break;

            uint8_t entry_flags = entry[1];
            uint16_t geom_idx = entry[2] | (entry[3] << 8);
            uint32_t model_hash = entry[4] | (entry[5] << 8) | (entry[6] << 16) | (entry[7] << 24);

            if (g_tracked_model_ids.count(model_hash) && (entry_flags == 0x00 || entry_flags == 0x80))
                out.push_back({tile_id, entry_flags, geom_idx, model_hash});
        }
    }

}

// ─── Read ALIVE pieces from CSWorldGeomMan (loaded tiles) ───────────

// Returns tile_id → set of alive MSB part names for ALL tracked model prefixes
static std::map<uint32_t, std::set<std::string>> read_alive_from_world_geom_man()
{
    std::map<uint32_t, std::set<std::string>> result;

    uintptr_t game_base = (uintptr_t)GetModuleHandleA("eldenring.exe");
    if (!game_base) return result;

    void *wgm = nullptr;
    if (!safe_read((void *)(game_base + RVA_WORLD_GEOM_MAN), &wgm, 8) || !wgm)
        return result;

    // Tree at WGM+0x18: +0x08 head_ptr, +0x10 size
    void *tree_head = nullptr;
    uint64_t tree_size = 0;
    if (!safe_read((char *)wgm + 0x18 + 0x08, &tree_head, 8) || !tree_head)
        return result;
    safe_read((char *)wgm + 0x18 + 0x10, &tree_size, 8);

    if (tree_size == 0 || tree_size > 1000)
        return result;

    // RB tree node: +0x00 left, +0x08 parent, +0x10 right, +0x19 is_nil, +0x20 value
    // Value (BlocksEntry): +0x00 block_id(u32), +0x08 data_ptr
    void *root = nullptr;
    safe_read((char *)tree_head + 0x08, &root, 8); // parent of head = root

    auto get_is_nil = [](void *node) -> bool {
        uint8_t val = 1;
        safe_read((char *)node + 0x19, &val, 1);
        return val != 0;
    };

    auto get_left = [](void *node) -> void * {
        void *p = nullptr;
        safe_read((char *)node + 0x00, &p, 8);
        return p;
    };

    auto get_right = [](void *node) -> void * {
        void *p = nullptr;
        safe_read((char *)node + 0x10, &p, 8);
        return p;
    };

    auto get_parent = [](void *node) -> void * {
        void *p = nullptr;
        safe_read((char *)node + 0x08, &p, 8);
        return p;
    };

    auto min_node = [&](void *node) -> void * {
        while (node && !get_is_nil(node)) {
            void *left = get_left(node);
            if (!left || get_is_nil(left)) break;
            node = left;
        }
        return node;
    };

    void *current = min_node(root);
    int nodes_visited = 0;

    while (current && current != tree_head && !get_is_nil(current) && nodes_visited < 500)
    {
        nodes_visited++;

        uint32_t block_id = 0;
        void *block_data = nullptr;
        safe_read((char *)current + 0x20, &block_id, 4);
        safe_read((char *)current + 0x28, &block_data, 8);

        if (block_data)
        {
            // geom_ins vector at BlockData+0x288
            void *vec_begin = nullptr, *vec_end = nullptr;
            safe_read((char *)block_data + 0x288 + 0x08, &vec_begin, 8);
            safe_read((char *)block_data + 0x288 + 0x10, &vec_end, 8);

            if (vec_begin && vec_end && vec_end > vec_begin)
            {
                size_t count = ((uintptr_t)vec_end - (uintptr_t)vec_begin) / 8;
                if (count > 10000) count = 10000;

                for (size_t i = 0; i < count; i++)
                {
                    void *geom_ins = nullptr;
                    safe_read((char *)vec_begin + i * 8, &geom_ins, 8);
                    if (!geom_ins) continue;

                    void *msb_part_ptr = nullptr;
                    safe_read((char *)geom_ins + 0x18 + 0x18 + 0x18, &msb_part_ptr, 8);
                    if (!msb_part_ptr) continue;

                    void *name_ptr = nullptr;
                    safe_read(msb_part_ptr, &name_ptr, 8);
                    if (!name_ptr) continue;

                    wchar_t name_buf[64] = {};
                    safe_read(name_ptr, name_buf, sizeof(name_buf) - 2);

                    char narrow[64] = {};
                    for (int c = 0; c < 63 && name_buf[c]; c++)
                        narrow[c] = (char)(name_buf[c] & 0xFF);

                    // Check if this object belongs to any tracked model prefix
                    std::string prefix = prefix_from_object_name(narrow);
                    if (!prefix.empty() && g_tracked_prefixes.count(prefix))
                    {
                        if (result.find(block_id) == result.end())
                            result[block_id] = {};

                        // +0x263 bit1: persistent alive flag (survives restart)
                        // +0x26B bit4: immediate collection flag (all model types, lost on restart)
                        uint8_t f263 = 0, f26B = 0;
                        safe_read((char *)geom_ins + 0x263, &f263, 1);
                        safe_read((char *)geom_ins + 0x26B, &f26B, 1);

                        bool alive = (f263 & 0x02) && !(f26B & 0x10);

                        if (alive)
                            result[block_id].insert(std::string(narrow));
                    }
                }
            }
        }

        // In-order successor
        void *right = get_right(current);
        if (right && !get_is_nil(right))
        {
            current = min_node(right);
        }
        else
        {
            void *parent = get_parent(current);
            int walk_up = 0;
            while (parent && parent != tree_head && ++walk_up < 500)
            {
                void *parent_right = get_right(parent);
                if (current != parent_right) break;
                current = parent;
                parent = get_parent(current);
            }
            if (walk_up >= 500) break;
            current = parent;
        }
    }

    if (!g_dumped_geom_ins)
        g_dumped_geom_ins = true;

    return result;
}


static void dump_singleton_raw(uintptr_t game_base, uintptr_t rva, const char *name)
{
    void *ptr = nullptr;
    if (!safe_read((void *)(game_base + rva), &ptr, 8) || !ptr)
    {
        spdlog::info("[DUMP] {} @ base+0x{:X}: NULL", name, rva);
        return;
    }

    spdlog::info("[DUMP] {} @ base+0x{:X} -> {:016X}", name, rva, (uintptr_t)ptr);

    uint8_t raw[512] = {};
    if (!safe_read(ptr, raw, 512))
    {
        spdlog::info("[DUMP]   Cannot read memory");
        return;
    }

    for (int i = 0; i < 512; i += 16)
    {
        char hex[80];
        int h = 0;
        for (int j = 0; j < 16; j++)
            h += snprintf(hex + h, sizeof(hex) - h, "%02X ", raw[i + j]);
        spdlog::info("[DUMP]   +{:02X}: {}", i, hex);
    }

    for (int i = 0; i < 512; i += 8)
    {
        uint64_t val = 0;
        memcpy(&val, raw + i, 8);
        if (val > 0x10000 && val < 0x7FFFFFFFFFFF && (val & 0x7) == 0)
        {
            uint8_t probe[16] = {};
            if (safe_read((void *)val, probe, 16))
            {
                char hex2[60];
                int h2 = 0;
                for (int j = 0; j < 16; j++)
                    h2 += snprintf(hex2 + h2, sizeof(hex2) - h2, "%02X ", probe[j]);
                spdlog::info("[DUMP]   +{:02X} PTR -> {:016X}: {}", i, val, hex2);
            }
        }
    }
}

static std::vector<GEOFEntry> read_geof_from_memory()
{
    std::vector<GEOFEntry> result;

    uintptr_t game_base = (uintptr_t)GetModuleHandleA("eldenring.exe");
    if (!game_base)
        return result;

    read_singleton_entries(game_base, RVA_GEOM_FLAG, result);
    size_t active_count = result.size();

    read_singleton_entries(game_base, RVA_GEOM_NONACTIVE, result);
    size_t nonactive_count = result.size() - active_count;

    if (!result.empty())
        spdlog::debug("[GEOF] Memory: {} active + {} non-active = {} total entries",
                      active_count, nonactive_count, result.size());

    return result;
}

// ─── tile ID helper ──────────────────────────────────────────────────

static uint32_t encode_tile(uint8_t area, uint8_t gridX, uint8_t gridZ)
{
    return ((uint32_t)area << 24) | ((uint32_t)gridX << 16) | ((uint32_t)gridZ << 8);
}

// ─── main initialization ─────────────────────────────────────────────

void goblin::collected::initialize()
{
    g_collected_rows.clear();
    g_collected_count = 0;
    g_unmatched_count = 0;

    g_tile_to_rows.clear();
    g_tile_slot_to_row.clear();
    g_tile_name_to_row.clear();
    g_tracked_prefixes.clear();
    g_tracked_model_ids.clear();

    // Build tracking tables from MAP_ENTRIES — any entry with object_name is tracked.
    // Adding a new model type only requires adding entries + _slots.json.
    for (size_t i = 0; i < generated::MAP_ENTRY_COUNT; i++)
    {
        const auto &e = generated::MAP_ENTRIES[i];
        if (!e.object_name || !e.object_name[0])
            continue;

        std::string prefix = prefix_from_object_name(e.object_name);
        if (prefix.empty())
            continue;

        g_tracked_prefixes.insert(prefix);

        uint32_t tile = encode_tile(e.data.areaNo, e.data.gridXNo, e.data.gridZNo);
        g_tile_to_rows[tile].push_back(e.row_id);

        // 3D slot map: tile → prefix → geom_slot → row_id
        if (e.geom_slot >= 0)
            g_tile_slot_to_row[tile][prefix][e.geom_slot] = e.row_id;

        // WGM name tracking: full object name for accurate alive matching
        g_tile_name_to_row[tile][e.object_name] = e.row_id;
    }

    // Build model_id set for GEOF filtering
    for (auto &prefix : g_tracked_prefixes)
    {
        uint32_t mid = model_id_from_prefix(prefix);
        if (mid)
            g_tracked_model_ids.insert(mid);
    }

    int total_piece_entries = 0;
    for (auto &[t, rows] : g_tile_to_rows)
        total_piece_entries += (int)rows.size();

    spdlog::info("[COLLECTED] {} entries across {} tiles, {} tracked prefixes ({} model IDs)",
                 total_piece_entries, g_tile_to_rows.size(),
                 g_tracked_prefixes.size(), g_tracked_model_ids.size());

    for (auto &prefix : g_tracked_prefixes)
        spdlog::debug("[COLLECTED]   prefix: {} -> model_id {}", prefix, model_id_from_prefix(prefix));

    g_initialized = true;
    spdlog::info("[COLLECTED] Initialized, awaiting refresh()");
}

// ─── remap row IDs after dynamic assignment ────────────────────────

void goblin::collected::remap_row_ids(const std::unordered_map<uint64_t, uint64_t> &old_to_new)
{
    // Remap g_tile_to_rows
    for (auto &[tile, rows] : g_tile_to_rows)
    {
        for (auto &rid : rows)
        {
            auto it = old_to_new.find(rid);
            if (it != old_to_new.end())
                rid = it->second;
        }
    }

    // Remap g_tile_slot_to_row (3D: tile → prefix → slot → row_id)
    for (auto &[tile, prefix_map] : g_tile_slot_to_row)
    {
        for (auto &[prefix, slot_map] : prefix_map)
        {
            for (auto &[slot, rid] : slot_map)
            {
                auto it = old_to_new.find(rid);
                if (it != old_to_new.end())
                    rid = it->second;
            }
        }
    }

    // Remap g_tile_name_to_row
    for (auto &[tile, name_map] : g_tile_name_to_row)
    {
        for (auto &[obj_name, rid] : name_map)
        {
            auto it = old_to_new.find(rid);
            if (it != old_to_new.end())
                rid = it->second;
        }
    }

    spdlog::debug("[COLLECTED] Remapped {} row IDs", old_to_new.size());
}

// ─── hide icon in-place ─────────────────────────────────────────────

static void hide_icon(void *param_data)
{
    if (!param_data) return;
    auto *p = reinterpret_cast<uint8_t *>(param_data);
    // Set areaNo (offset 0x20, uint8) to 99 → non-existent area, icon won't display
    p[0x20] = 99;
}

// ─── register param pointer for real-time hiding ────────────────────

void goblin::collected::register_param_ptr(uint64_t row_id, void *param_data)
{
    auto *p = reinterpret_cast<uint8_t *>(param_data);
    g_param_ptrs[row_id] = {p, p[0x20]};  // save original areaNo
}

// ─── refresh from memory (real-time update) ─────────────────────────

int goblin::collected::refresh()
{
    uintptr_t game_base = (uintptr_t)GetModuleHandleA("eldenring.exe");
    if (!game_base)
        return 0;
    void *wgm_check = nullptr;
    safe_read((void *)(game_base + RVA_WORLD_GEOM_MAN), &wgm_check, 8);
    void *geof_check = nullptr;
    safe_read((void *)(game_base + RVA_GEOM_FLAG), &geof_check, 8);
    if (!wgm_check && !geof_check)
        return 0;

    if (!g_initialized || g_tile_to_rows.empty())
        return 0;

    auto geof = read_geof_from_memory();

    std::set<uint64_t> new_collected;

    // Group GEOF entries by tile + prefix
    // key: tile_id → prefix → list of GEOF-computed slots
    std::map<uint32_t, std::map<std::string, std::vector<int>>> geof_tile_prefix_slots;
    for (auto &e : geof)
    {
        if (g_tile_to_rows.find(e.tile_id) == g_tile_to_rows.end())
            continue;
        std::string prefix = prefix_from_model_id(e.model_hash);
        if (prefix.empty())
            continue;
        int slot = aeg099_index_from_geof(e.geom_idx, e.flags);
        geof_tile_prefix_slots[e.tile_id][prefix].push_back(slot);
    }

    // ── WGM: accurate tracking for loaded tiles ──
    auto alive = read_alive_from_world_geom_man();
    std::set<uint32_t> wgm_tiles;

    for (auto &[tile_id, alive_names] : alive)
    {
        wgm_tiles.insert(tile_id);

        auto name_it = g_tile_name_to_row.find(tile_id);
        if (name_it == g_tile_name_to_row.end())
        {
            // WGM has tracked objects on this tile, but we have no map data for it
            if (!alive_names.empty())
                spdlog::debug("[COLLECTED] WGM tile 0x{:08X} NOT in map data ({} alive objects)",
                              tile_id, alive_names.size());
            continue;
        }

        for (auto &[object_name, row_id] : name_it->second)
        {
            if (alive_names.count(object_name) == 0)
                new_collected.insert(row_id);
        }
    }

    // ── GEOF: for unloaded tiles ──
    for (auto &[tid, prefix_slots] : geof_tile_prefix_slots)
    {
        if (wgm_tiles.count(tid))
            continue;

        auto tile_it = g_tile_slot_to_row.find(tid);
        if (tile_it == g_tile_slot_to_row.end()) continue;

        for (auto &[prefix, slots] : prefix_slots)
        {
            auto prefix_it = tile_it->second.find(prefix);
            if (prefix_it == tile_it->second.end()) continue;

            for (int slot : slots)
            {
                auto row_it = prefix_it->second.find(slot);
                if (row_it != prefix_it->second.end())
                    new_collected.insert(row_it->second);
            }
        }
    }

    if (new_collected == g_collected_rows)
        return 0;
    // Log which rows were added/removed (for small deltas only)
    {
        std::vector<uint64_t> added, removed;
        for (auto r : new_collected)
            if (!g_collected_rows.count(r)) added.push_back(r);
        for (auto r : g_collected_rows)
            if (!new_collected.count(r)) removed.push_back(r);
        spdlog::info("[COLLECTED] Set changed: {} -> {} (added {}, removed {})",
                     g_collected_rows.size(), new_collected.size(), added.size(), removed.size());
        if (added.size() <= 10)
            for (auto r : added) spdlog::info("[COLLECTED]   +row {}", r);
        if (removed.size() <= 10)
            for (auto r : removed) spdlog::info("[COLLECTED]   -row {}", r);
    }

    // Restore all to visible, then re-hide collected
    for (auto &[row_id, ref] : g_param_ptrs)
        ref.ptr[0x20] = ref.original_areaNo;

    int applied = 0, missed = 0;
    for (uint64_t row_id : new_collected)
    {
        auto pit = g_param_ptrs.find(row_id);
        if (pit != g_param_ptrs.end())
        {
            pit->second.ptr[0x20] = 99;
            applied++;
        }
        else
            missed++;
    }
    if (missed > 0)
        spdlog::warn("[COLLECTED] {} row IDs NOT in param_ptrs (out of {} collected)", missed, new_collected.size());

    int delta = (int)new_collected.size() - (int)g_collected_rows.size();
    g_collected_rows = std::move(new_collected);
    g_collected_count = (int)g_collected_rows.size();

    spdlog::info("[COLLECTED] Refresh: {} hidden (delta {:+d}), {} GEOF entries, {} WGM tiles",
                 g_collected_count, delta, geof.size(), wgm_tiles.size());

    return delta;
}

// ─── queries ────────────────────────────────────────────────────────

bool goblin::collected::is_row_collected(uint64_t row_id)
{
    return g_collected_rows.count(row_id) > 0;
}

int goblin::collected::collected_count()
{
    return g_collected_count;
}

int goblin::collected::skipped_count()
{
    return g_unmatched_count;
}
