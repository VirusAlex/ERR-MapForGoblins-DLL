#include "goblin_inject.hpp"
#include "goblin_collected.hpp"
#include "goblin_config.hpp"
#include "generated/goblin_map_data.hpp"
#include "from/params.hpp"
#include "from/paramdef/WORLD_MAP_POINT_PARAM_ST.hpp"

#include <algorithm>
#include <spdlog/spdlog.h>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using ParamRowInfo = from::params::ParamRowInfo;
using ParamTable = from::params::ParamTable;
using ParamResCap = from::params::ParamResCap;
using Category = goblin::generated::Category;

static void *allocation = nullptr;

struct WrapperRowLocator
{
    int32_t row;
    int32_t index;
};

static ParamResCap *find_world_map_point_param_res_cap()
{
    auto param_list = *from::params::param_list_address;
    if (!param_list) return nullptr;
    for (int i = 0; i < 186; i++)
    {
        auto prc = param_list->entries[i].param_res_cap;
        if (!prc) continue;
        std::wstring_view name = from::params::dlw_c_str(&prc->param_name);
        if (name == L"WorldMapPointParam") return prc;
    }
    return nullptr;
}

static bool is_category_enabled(Category cat)
{
    switch (cat)
    {
    case Category::EquipArmaments:       return goblin::config::showArmaments;
    case Category::EquipArmour:          return goblin::config::showArmour;
    case Category::EquipAshesOfWar:      return goblin::config::showAshesOfWar;
    case Category::EquipSpirits:         return goblin::config::showSpirits;
    case Category::EquipTalismans:       return goblin::config::showTalismans;
    case Category::KeyCelestialDew:      return goblin::config::showCelestialDew;
    case Category::KeyCookbooks:         return goblin::config::showCookbooks;
    case Category::KeyCrystalTears:      return goblin::config::showCrystalTears;
    case Category::KeyImbuedSwordKeys:   return goblin::config::showImbuedSwordKeys;
    case Category::KeyLarvalTears:       return goblin::config::showLarvalTears;
    case Category::KeyLostAshes:         return goblin::config::showLostAshes;
    case Category::KeyPotsNPerfumes:     return goblin::config::showPotsNPerfumes;
    case Category::KeySeedsTears:        return goblin::config::showSeedsTears;
    case Category::KeyWhetblades:        return goblin::config::showWhetblades;
    case Category::LootAmmo:             return goblin::config::showAmmo;
    case Category::LootBellBearings:     return goblin::config::showBellBearings;
    case Category::LootConsumables:      return goblin::config::showConsumables;
    case Category::LootCraftingMaterials:return goblin::config::showCraftingMaterials;
    case Category::LootMPFingers:        return goblin::config::showMPFingers;
    case Category::LootMaterialNodes:    return goblin::config::showMaterialNodes;
    case Category::LootReusables:        return goblin::config::showReusables;
    case Category::LootSomberScarab:     return goblin::config::showSomberScarab;
    case Category::LootStoneswordKeys:   return goblin::config::showStoneswordKeys;
    case Category::LootUniqueDrops:      return goblin::config::showUniqueDrops;
    case Category::MagicIncantations:    return goblin::config::showIncantations;
    case Category::MagicMemoryStones:    return goblin::config::showMemoryStones;
    case Category::MagicSorceries:       return goblin::config::showSorceries;
    case Category::QuestDeathroot:       return goblin::config::showDeathroot;
    case Category::QuestProgression:     return goblin::config::showProgression;
    case Category::QuestSeedbedCurses:   return goblin::config::showSeedbedCurses;
    case Category::ReforgedCampContents: return goblin::config::showCampContents;
    case Category::ReforgedEmberPieces:  return goblin::config::showEmberPieces;
    case Category::ReforgedItemsAndChanges: return goblin::config::showItemsAndChanges;
    case Category::ReforgedRunePieces:   return goblin::config::showRunePieces;
    case Category::WorldGraces:          return goblin::config::showGraces;
    case Category::WorldHostileNPC:      return goblin::config::showHostileNPC;
    case Category::WorldImpStatues:      return goblin::config::showImpStatues;
    case Category::WorldPaintings:       return goblin::config::showPaintings;
    case Category::WorldSpiritSprings:   return goblin::config::showSpiritSprings;
    case Category::WorldSpiritspringHawks: return goblin::config::showSpiritspringHawks;
    case Category::WorldSummoningPools:  return goblin::config::showSummoningPools;
    default:                             return true;
    }
}

void goblin::inject_map_entries()
{
    struct InjectedEntry
    {
        int32_t row_id;
        const from::paramdef::WORLD_MAP_POINT_PARAM_ST *data;
        bool is_piece;     // Rune/Ember Piece — register for real-time hiding
        bool is_hidden;    // Category disabled — hide via areaNo=99
    };

    std::vector<InjectedEntry> entries;
    entries.reserve(generated::MAP_ENTRY_COUNT);

    size_t hidden_by_config = 0;
    for (size_t i = 0; i < generated::MAP_ENTRY_COUNT; i++)
    {
        const auto &e = generated::MAP_ENTRIES[i];
        bool enabled = is_category_enabled(e.category);
        if (!enabled)
            hidden_by_config++;
        entries.push_back({static_cast<int32_t>(e.row_id), &e.data,
                           e.category == Category::ReforgedRunePieces ||
                           e.category == Category::ReforgedEmberPieces,
                           !enabled});
    }

    spdlog::info("Injecting {} map entries ({} hidden by config)",
                 entries.size(), hidden_by_config);

    auto param_res_cap = find_world_map_point_param_res_cap();
    if (!param_res_cap)
    {
        spdlog::error("WorldMapPointParam not found");
        return;
    }

    auto *rescap = reinterpret_cast<uint8_t *>(param_res_cap->param_header);
    auto *&file_ptr_ref = *reinterpret_cast<uint8_t **>(rescap + 0x80);
    auto &file_size_ref = *reinterpret_cast<int64_t *>(rescap + 0x78);

    auto *old_param_file = file_ptr_ref;
    auto *old_table = reinterpret_cast<ParamTable *>(old_param_file);
    uint16_t orig_num_rows = old_table->num_rows;

    spdlog::debug("Original WorldMapPointParam: {} rows", orig_num_rows);

    uint32_t new_entry_count = static_cast<uint32_t>(entries.size());
    uint32_t total_rows = orig_num_rows + new_entry_count;

    spdlog::debug("Injecting {} entries ({} total)", new_entry_count, total_rows);

    constexpr size_t WRAPPER_HEADER = 0x10;
    constexpr size_t HEADER_SIZE = 0x40;
    constexpr size_t ROW_LOCATOR_SIZE = sizeof(ParamRowInfo);
    constexpr size_t PARAM_DATA_SIZE = sizeof(from::paramdef::WORLD_MAP_POINT_PARAM_ST);
    constexpr size_t WRAPPER_ROW_LOC_SIZE = sizeof(WrapperRowLocator);

    const char *type_str = reinterpret_cast<const char *>(old_param_file + old_table->param_type_offset);
    size_t type_str_len = strlen(type_str) + 1;

    size_t row_locators_start = HEADER_SIZE;
    size_t data_start = row_locators_start + total_rows * ROW_LOCATOR_SIZE;
    size_t data_end = data_start + total_rows * PARAM_DATA_SIZE;
    size_t type_str_start = data_end;
    size_t after_type_str = type_str_start + type_str_len;
    size_t wrapper_row_loc_start = (after_type_str + 3) & ~3;
    size_t wrapper_row_loc_end = wrapper_row_loc_start + total_rows * WRAPPER_ROW_LOC_SIZE;
    size_t param_file_size = wrapper_row_loc_end;
    size_t total_alloc = WRAPPER_HEADER + param_file_size;

    allocation = VirtualAlloc(nullptr, total_alloc, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!allocation)
    {
        spdlog::error("VirtualAlloc failed ({} bytes)", total_alloc);
        return;
    }
    memset(allocation, 0, total_alloc);

    auto *new_wrapper = reinterpret_cast<uint8_t *>(allocation);
    auto *new_param_file = new_wrapper + WRAPPER_HEADER;
    auto *new_table = reinterpret_cast<ParamTable *>(new_param_file);

    *reinterpret_cast<uint32_t *>(new_wrapper + 0x00) = static_cast<uint32_t>(wrapper_row_loc_start);
    *reinterpret_cast<int32_t *>(new_wrapper + 0x04) = static_cast<int32_t>(total_rows);

    memcpy(new_param_file, old_param_file, HEADER_SIZE);
    new_table->num_rows = static_cast<uint16_t>(total_rows);
    new_table->param_type_offset = type_str_start;
    *reinterpret_cast<uint32_t *>(new_param_file + 0x00) = static_cast<uint32_t>(type_str_start);
    *reinterpret_cast<uint16_t *>(new_param_file + 0x04) = static_cast<uint16_t>(data_start);  // shortDataOffset
    *reinterpret_cast<uint64_t *>(new_param_file + 0x30) = data_start;

    memcpy(new_param_file + type_str_start, type_str, type_str_len);

    struct RowSource
    {
        int32_t row_id;
        const uint8_t *data_ptr;
        bool is_piece;
        bool is_hidden;  // hide via areaNo=99 (category disabled)
    };

    std::vector<RowSource> all_rows;
    all_rows.reserve(total_rows);

    for (uint16_t i = 0; i < orig_num_rows; i++)
    {
        auto *data = old_param_file + old_table->rows[i].param_offset;
        all_rows.push_back({static_cast<int32_t>(old_table->rows[i].row_id), data, false, false});
    }
    for (auto &entry : entries)
    {
        all_rows.push_back({entry.row_id, reinterpret_cast<const uint8_t *>(entry.data),
                            entry.is_piece, entry.is_hidden});
    }

    std::sort(all_rows.begin(), all_rows.end(),
              [](const RowSource &a, const RowSource &b) { return a.row_id < b.row_id; });

    auto *new_locators = reinterpret_cast<ParamRowInfo *>(new_param_file + row_locators_start);
    auto *new_wrapper_locs = reinterpret_cast<WrapperRowLocator *>(new_param_file + wrapper_row_loc_start);
    size_t file_end_marker = type_str_start + type_str_len;

    int hidden_by_category = 0;
    for (size_t i = 0; i < all_rows.size(); i++)
    {
        size_t data_offset = data_start + i * PARAM_DATA_SIZE;
        new_locators[i].row_id = static_cast<uint64_t>(all_rows[i].row_id);
        new_locators[i].param_offset = data_offset;
        new_locators[i].param_end_offset = file_end_marker;
        memcpy(new_param_file + data_offset, all_rows[i].data_ptr, PARAM_DATA_SIZE);
        new_wrapper_locs[i].row = all_rows[i].row_id;
        new_wrapper_locs[i].index = static_cast<int32_t>(i);

        // Hide entries whose category is disabled (areaNo=99 → non-existent area)
        if (all_rows[i].is_hidden)
        {
            auto *param_ptr = new_param_file + data_offset;
            param_ptr[0x20] = 99;
            hidden_by_category++;
        }
    }

    int registered = 0, hidden_at_inject = 0;
    for (size_t i = 0; i < all_rows.size(); i++)
    {
        if (all_rows[i].is_piece)
        {
            size_t data_offset = data_start + i * PARAM_DATA_SIZE;
            auto *param_ptr = new_param_file + data_offset;
            uint64_t row_id = static_cast<uint64_t>(all_rows[i].row_id);
            collected::register_param_ptr(row_id, param_ptr);
            registered++;

            if (collected::is_row_collected(row_id))
            {
                param_ptr[0x20] = 99;  // areaNo = 99
                hidden_at_inject++;
            }
        }
    }

    spdlog::info("Registered {} piece pointers, {} hidden at inject time, {} hidden by category",
                 registered, hidden_at_inject, hidden_by_category);

    spdlog::debug("Swapping param_file pointer: {:p} -> {:p}", (void *)old_param_file, (void *)new_param_file);
    file_ptr_ref = new_param_file;
    file_size_ref = static_cast<int64_t>(param_file_size);

    spdlog::debug("Injection complete: {} total rows", total_rows);
}
