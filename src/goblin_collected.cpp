#include "goblin_collected.hpp"
#include "goblin_config.hpp"
#include "generated/goblin_map_data.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <spdlog/spdlog.h>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using Category = goblin::generated::Category;

static constexpr uint16_t GEOM_IDX_MIN = 0x1194;
// Model hash for AEG099_821 (bytes 4-7 of GEOF entry)
static constexpr uint32_t MODEL_HASH_821 = 0x009A1C6D;

static std::set<uint64_t> g_collected_rows;
static int g_collected_count = 0;
static int g_unmatched_count = 0;

struct ParamRef {
    uint8_t *ptr;
    uint8_t original_areaNo;
};
static std::map<uint64_t, ParamRef> g_param_ptrs;

static std::map<uint32_t, std::vector<uint64_t>> g_tile_to_rows;        // tile → ordered row_ids
static std::map<uint32_t, std::map<int, uint64_t>> g_tile_name_to_row;  // tile → name_suffix → row_id
static std::map<uint32_t, std::map<int, uint64_t>> g_tile_slot_to_row;  // tile → geom_slot → row_id
static bool g_initialized = false;
static bool g_dumped_geom_ins = false;

// Cached GEOF state for tiles that transitioned out of GEOF
static std::map<uint32_t, std::set<uint64_t>> g_geof_cache;

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

            if (model_hash == MODEL_HASH_821 && (entry_flags == 0x00 || entry_flags == 0x80))
                out.push_back({tile_id, entry_flags, geom_idx, model_hash});
        }
    }

}

// ─── Read ALIVE pieces from CSWorldGeomMan (loaded tiles) ───────────

// Returns tile_id → set of alive MSB part names (e.g. "AEG099_821_9000")
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

                    if (strncmp(narrow, "AEG099_821", 10) == 0)
                    {
                        if (result.find(block_id) == result.end())
                            result[block_id] = {};

                        // +0x263 bit1: persistent alive flag (slow to update)
                        // +0x269 & 0x60: immediate pickup flag (lost on restart)
                        uint8_t f263 = 0, f269 = 0;
                        safe_read((char *)geom_ins + 0x263, &f263, 1);
                        safe_read((char *)geom_ins + 0x269, &f269, 1);

                        bool alive = (f263 & 0x02) && !(f269 & 0x60);
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

static void add_collected_from_loaded_tiles(
    const std::map<uint32_t, std::set<std::string>> &alive,
    std::set<uint64_t> &collected_rows)
{
    int hidden_from_wgm = 0;

    int tiles_matched = 0, tiles_unmatched = 0;
    for (auto &[tile_id, alive_names] : alive)
    {
        auto it = g_tile_to_rows.find(tile_id);
        if (it == g_tile_to_rows.end())
        {
            tiles_unmatched++;
            continue;
        }
        tiles_matched++;

        auto &rows = it->second;

        for (int slot = 0; slot < (int)rows.size(); slot++)
        {
            char expected_name[32];
            snprintf(expected_name, sizeof(expected_name), "AEG099_821_%d", 9000 + slot);

            bool is_alive = alive_names.find(expected_name) != alive_names.end();
            if (!is_alive)
            {
                if (collected_rows.insert(rows[slot]).second)
                    hidden_from_wgm++;
            }
        }
    }

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

    for (size_t i = 0; i < generated::MAP_ENTRY_COUNT; i++)
    {
        const auto &e = generated::MAP_ENTRIES[i];
        if (e.category != Category::ReforgedRunePieces &&
            e.category != Category::ReforgedEmberPieces)
            continue;

        uint32_t tile = encode_tile(e.data.areaNo, e.data.gridXNo, e.data.gridZNo);
        g_tile_to_rows[tile].push_back(e.row_id);
        if (e.geom_slot >= 0)
            g_tile_slot_to_row[tile][e.geom_slot] = e.row_id;
        if (e.name_suffix >= 0)
            g_tile_name_to_row[tile][e.name_suffix] = e.row_id;
    }

    int total_piece_entries = 0;
    for (auto &[t, rows] : g_tile_to_rows)
        total_piece_entries += (int)rows.size();

    spdlog::info("[COLLECTED] {} piece entries across {} tiles in map data",
                 total_piece_entries, g_tile_to_rows.size());

    g_initialized = true;
    spdlog::info("[COLLECTED] Initialized, awaiting refresh()");
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

    std::map<uint32_t, std::vector<int>> geof_tile_slots;
    for (auto &e : geof)
    {
        if (g_tile_to_rows.find(e.tile_id) == g_tile_to_rows.end())
            continue;
        int slot = aeg099_index_from_geof(e.geom_idx, e.flags);
        geof_tile_slots[e.tile_id].push_back(slot);
    }

    // WGM is fresher than GEOF for loaded tiles
    auto alive = read_alive_from_world_geom_man();
    std::set<uint32_t> wgm_tiles;

    for (auto &[tile_id, alive_names] : alive)
    {
        wgm_tiles.insert(tile_id);

        auto name_it = g_tile_name_to_row.find(tile_id);
        if (name_it == g_tile_name_to_row.end())
            continue;

        for (auto &[suffix, row_id] : name_it->second)
        {
            char piece_name[32];
            snprintf(piece_name, sizeof(piece_name), "AEG099_821_%d", suffix);
            bool is_alive = alive_names.count(piece_name) > 0;
            if (!is_alive)
                new_collected.insert(row_id);
        }
    }

    // GEOF for unloaded tiles only
    for (auto &[tid, slots] : geof_tile_slots)
    {
        if (wgm_tiles.count(tid))
            continue;

        auto slot_map_it = g_tile_slot_to_row.find(tid);
        if (slot_map_it == g_tile_slot_to_row.end()) continue;
        auto &slot_map = slot_map_it->second;

        for (int slot : slots)
        {
            auto row_it = slot_map.find(slot);
            if (row_it != slot_map.end())
                new_collected.insert(row_it->second);
        }
    }

    int new_cache_entries = 0;
    for (auto &[tid, slots] : geof_tile_slots)
    {
        if (g_geof_cache.find(tid) == g_geof_cache.end())
            new_cache_entries++;
        std::set<uint64_t> cached;
        auto slot_map_it = g_tile_slot_to_row.find(tid);
        if (slot_map_it != g_tile_slot_to_row.end())
        {
            for (int s : slots)
            {
                auto it = slot_map_it->second.find(s);
                if (it != slot_map_it->second.end())
                    cached.insert(it->second);
            }
        }
        g_geof_cache[tid] = cached;
    }
    if (new_cache_entries > 0)
        spdlog::info("[CACHE] Added {} new tiles to cache (total: {})", new_cache_entries, g_geof_cache.size());

    if (new_collected == g_collected_rows)
        return 0;

    // Restore all to visible, then re-hide collected
    for (auto &[row_id, ref] : g_param_ptrs)
        ref.ptr[0x20] = ref.original_areaNo;

    for (uint64_t row_id : new_collected)
    {
        auto pit = g_param_ptrs.find(row_id);
        if (pit != g_param_ptrs.end())
            pit->second.ptr[0x20] = 99;
    }

    int delta = (int)new_collected.size() - (int)g_collected_rows.size();
    g_collected_rows = std::move(new_collected);
    g_collected_count = (int)g_collected_rows.size();

    spdlog::info("[COLLECTED] Refresh: {} hidden (delta {:+d}), {} GEOF entries",
                 g_collected_count, delta, geof.size());

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
