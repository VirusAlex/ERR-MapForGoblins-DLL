#include "goblin_inject.hpp"
#include "goblin_collected.hpp"
#include "goblin_config.hpp"
#include "generated/goblin_map_data.hpp"
#include "from/params.hpp"
#include "from/paramdef/WORLD_MAP_POINT_PARAM_ST.hpp"

#include <algorithm>
#include <set>
#include <unordered_map>
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
    case Category::KeyScadutreeFragments: return goblin::config::showScadutreeFragments;
    case Category::KeyGreatRunes:        return goblin::config::showGreatRunes;
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
    case Category::LootSmithingStones:       return goblin::config::showSmithingStones;
    case Category::LootSmithingStonesLow:   return goblin::config::showSmithingStonesLow;
    case Category::LootSmithingStonesRare:  return goblin::config::showSmithingStonesRare;
    case Category::LootGoldenRunes:         return goblin::config::showGoldenRunes;
    case Category::LootGoldenRunesLow:      return goblin::config::showGoldenRunesLow;
    case Category::LootStoneswordKeys:   return goblin::config::showStoneswordKeys;
    case Category::LootThrowables:       return goblin::config::showThrowables;
    case Category::LootPrattlingPates:   return goblin::config::showPrattlingPates;
    case Category::LootRuneArcs:         return goblin::config::showRuneArcs;
    case Category::LootDragonHearts:     return goblin::config::showDragonHearts;
    case Category::LootGloveworts:       return goblin::config::showGloveworts;
    case Category::LootGreatGloveworts:  return goblin::config::showGreatGloveworts;
    case Category::LootRadaFruit:        return goblin::config::showRadaFruit;
    case Category::LootGestures:         return goblin::config::showGestures;
    case Category::LootGreases:          return goblin::config::showGreases;
    case Category::LootUtilities:        return goblin::config::showUtilities;
    case Category::LootStatBoosts:       return goblin::config::showStatBoosts;
    case Category::ReforgedFortunes:     return goblin::config::showFortunes;
    case Category::WorldHostileNPC:      return goblin::config::showHostileNPC;
    case Category::MagicIncantations:    return goblin::config::showIncantations;
    case Category::MagicMemoryStones:    return goblin::config::showMemoryStones;
    case Category::MagicPrayerbooks:     return goblin::config::showPrayerbooks;
    case Category::MagicSorceries:       return goblin::config::showSorceries;
    case Category::WorldBosses:          return goblin::config::showBosses;
    case Category::QuestDeathroot:       return goblin::config::showDeathroot;
    case Category::QuestProgression:     return goblin::config::showProgression;
    case Category::QuestSeedbedCurses:   return goblin::config::showSeedbedCurses;
    case Category::ReforgedEmberPieces:  return goblin::config::showEmberPieces;
    case Category::ReforgedItemsAndChanges: return goblin::config::showItemsAndChanges;
    case Category::ReforgedRunePieces:   return goblin::config::showRunePieces;
    case Category::WorldGraces:          return goblin::config::showGraces;
    case Category::WorldImpStatues:      return goblin::config::showImpStatues;
    case Category::WorldMaps:            return goblin::config::showWorldMaps;
    case Category::WorldPaintings:       return goblin::config::showPaintings;
    case Category::WorldSpiritSprings:   return goblin::config::showSpiritSprings;
    case Category::WorldSpiritspringHawks: return goblin::config::showSpiritspringHawks;
    case Category::WorldStakesOfMarika:  return goblin::config::showStakesOfMarika;
    case Category::WorldSummoningPools:  return goblin::config::showSummoningPools;
    default:                             return true;
    }
}

void goblin::inject_map_entries()
{
    struct InjectedEntry
    {
        int32_t row_id;
        uint64_t original_row_id;
        const from::paramdef::WORLD_MAP_POINT_PARAM_ST *data;
        bool is_piece;
        Category category;
    };

    // Filter: only include enabled categories (disabled ones are simply not injected)
    std::vector<InjectedEntry> entries;
    entries.reserve(generated::MAP_ENTRY_COUNT);

    size_t skipped_by_config = 0;
    for (size_t i = 0; i < generated::MAP_ENTRY_COUNT; i++)
    {
        const auto &e = generated::MAP_ENTRIES[i];
        if (!is_category_enabled(e.category))
        {
            skipped_by_config++;
            continue;
        }
        entries.push_back({0, e.row_id, &e.data,
                           e.category == Category::ReforgedRunePieces ||
                           e.category == Category::ReforgedEmberPieces ||
                           e.category == Category::LootMaterialNodes,
                           e.category});
    }

    spdlog::info("Injecting {} map entries ({} skipped by config)",
                 entries.size(), skipped_by_config);

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

    // Collect vanilla row IDs to avoid collisions
    std::set<int32_t> vanilla_ids;
    for (uint16_t i = 0; i < orig_num_rows; i++)
        vanilla_ids.insert(static_cast<int32_t>(old_table->rows[i].row_id));

    // Assign sequential IDs starting from 1, skipping vanilla IDs
    std::unordered_map<uint64_t, uint64_t> id_remap;  // original -> dynamic
    int32_t next_id = 1;
    for (auto &entry : entries)
    {
        while (vanilla_ids.count(next_id))
            next_id++;
        id_remap[entry.original_row_id] = static_cast<uint64_t>(next_id);
        entry.row_id = next_id++;
    }

    // Update collected system with new dynamic IDs
    collected::remap_row_ids(id_remap);

    spdlog::debug("Assigned IDs: {} entries, range {}-{}, remapped {} piece IDs",
                  entries.size(),
                  entries.empty() ? 0 : entries.front().row_id,
                  entries.empty() ? 0 : entries.back().row_id,
                  id_remap.size());

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
    *reinterpret_cast<uint16_t *>(new_param_file + 0x04) = static_cast<uint16_t>(data_start);
    *reinterpret_cast<uint64_t *>(new_param_file + 0x30) = data_start;

    memcpy(new_param_file + type_str_start, type_str, type_str_len);

    struct RowSource
    {
        int32_t row_id;
        const uint8_t *data_ptr;
        bool is_piece;
        Category category;
    };

    std::vector<RowSource> all_rows;
    all_rows.reserve(total_rows);

    for (uint16_t i = 0; i < orig_num_rows; i++)
    {
        auto *data = old_param_file + old_table->rows[i].param_offset;
        all_rows.push_back({static_cast<int32_t>(old_table->rows[i].row_id), data, false, {}});
    }
    for (auto &entry : entries)
    {
        all_rows.push_back({entry.row_id, reinterpret_cast<const uint8_t *>(entry.data),
                            entry.is_piece, entry.category});
    }

    std::sort(all_rows.begin(), all_rows.end(),
              [](const RowSource &a, const RowSource &b) { return a.row_id < b.row_id; });

    auto *new_locators = reinterpret_cast<ParamRowInfo *>(new_param_file + row_locators_start);
    auto *new_wrapper_locs = reinterpret_cast<WrapperRowLocator *>(new_param_file + wrapper_row_loc_start);
    size_t file_end_marker = type_str_start + type_str_len;

    for (size_t i = 0; i < all_rows.size(); i++)
    {
        size_t data_offset = data_start + i * PARAM_DATA_SIZE;
        new_locators[i].row_id = static_cast<uint64_t>(all_rows[i].row_id);
        new_locators[i].param_offset = data_offset;
        new_locators[i].param_end_offset = file_end_marker;
        memcpy(new_param_file + data_offset, all_rows[i].data_ptr, PARAM_DATA_SIZE);
        new_wrapper_locs[i].row = all_rows[i].row_id;
        new_wrapper_locs[i].index = static_cast<int32_t>(i);

        // Boss/hawk kill display mode: green checkmark vs hide killed
        auto cat = all_rows[i].category;
        if (cat == Category::WorldBosses || cat == Category::WorldSpiritspringHawks)
        {
            auto *p = reinterpret_cast<from::paramdef::WORLD_MAP_POINT_PARAM_ST *>(
                new_param_file + data_offset);
            if (goblin::config::hideKilledBosses)
            {
                p->clearedEventFlagId = 0;  // no green checkmark, text hides → icon hides
            }
            else
            {
                p->textDisableFlagId1 = 0;  // keep green checkmark, don't hide text
                p->textDisableFlagId2 = 0;  // keep location text visible too
            }
        }
    }

    // Register Rune/Ember piece pointers for real-time tracking
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

    spdlog::info("Registered {} piece pointers, {} hidden at inject time",
                 registered, hidden_at_inject);

    spdlog::debug("Swapping param_file pointer: {:p} -> {:p}", (void *)old_param_file, (void *)new_param_file);
    file_ptr_ref = new_param_file;
    file_size_ref = static_cast<int64_t>(param_file_size);

    spdlog::debug("Injection complete: {} total rows", total_rows);
}
