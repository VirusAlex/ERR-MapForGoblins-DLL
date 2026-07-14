#include "goblin_inject.hpp"
#include "goblin_collected.hpp"
#include "goblin_kindling.hpp"
#include "goblin_logic.hpp"
#include "goblin_config.hpp"
#include "goblin_messages.hpp"
#include "modutils.hpp"
#include "goblin_map_data.hpp"
#include "goblin_item_icons.hpp"
#include "goblin_location_alt.hpp"
#include "goblin_gfx_probe.hpp"
#include "goblin_overlay.hpp"
#include "goblin_progress.hpp"   // region_place_id (tag each CategoryRow for focus)
#include "goblin_diag.hpp"
#include "goblin/goblin_map_flags.hpp"
#include "from/params.hpp"
#include "from/paramdef/WORLD_MAP_POINT_PARAM_ST.hpp"

#include <cmath>
#include <fstream>
#include <map>
#include <mutex>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <optional>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <spdlog/spdlog.h>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <Xinput.h>
#pragma comment(lib, "Xinput.lib")

using ParamRowInfo = from::params::ParamRowInfo;
using ParamTable = from::params::ParamTable;
using ParamResCap = from::params::ParamResCap;
using Category = goblin::generated::Category;

static void *allocation = nullptr;

// State for runtime toggle (ERSC-hosting workaround). On hotkey press the
// pointers stored on the param-res-cap are swapped between vanilla and
// expanded values. set_param_injection_active() is a no-op until
// inject_map_entries() has populated these.
static uint8_t **g_file_ptr_ref = nullptr;
static int64_t *g_file_size_ref = nullptr;
static uint8_t *g_vanilla_param_file = nullptr;
static int64_t g_vanilla_param_size = 0;
static uint8_t *g_expanded_param_file = nullptr;
static int64_t g_expanded_param_size = 0;
static bool g_param_injection_active = false;

// Data pointers of MFG-injected WorldMapPointParam rows in the expanded table.
// Used by sanitize_injected_textids() (run after the FMG is built) to strip
// textIds that don't resolve to a real string.
static std::vector<uint8_t *> g_injected_row_ptrs;

// Per-injected-row visibility control. Every category is injected; a marker's
// primary line (and thus its icon) shows only when its category is enabled AND
// the row is not collected. Gated LIVE via textEnableFlagId1 - the engine
// re-evaluates the text enable/disable flags every frame (ce390), so overlay
// category toggles AND collection both take effect on the OPEN map with no
// reopen. dispMask00 is left baked (the engine reads it only at map build, so
// it can't drive a live toggle); the live lever is the enable flag.
struct CategoryRow
{
    from::paramdef::WORLD_MAP_POINT_PARAM_ST *p;
    Category cat;
    uint64_t row_id;         // dynamic row id (matches collected/kindling is_row_collected)
    uint64_t original_row_id; // pre-remap id (matches MAP_ENTRIES.row_id / progress); 0 = vanilla
    unsigned baked_enable[8]; // textEnableFlagId1..8 as baked; restored when shown. ALL
                              // lines are gated, since the engine hides the icon only when
                              // EVERY text line (item / enemy / location) is hidden.
    unsigned baked_cleared;  // clearedEventFlagId as baked (for live hide_killed_bosses)
    unsigned baked_dis1;     // textDisableFlagId1 as baked
    unsigned baked_dis2;     // textDisableFlagId2 as baked
    int32_t region_id;       // progress region PlaceName id (goblin::progress::region_place_id) for focus
    int32_t baked_text1;     // textId1 as baked (restored when focus removes a fabricated label)
    bool baked_notext;       // isEnableNoText as baked (restored after focus force-show)
    bool focus_text;         // true while focus fabricated a label on a textless row
};

// textEnableFlagId1..8 of a row, as a pointer array (the paramdef has them as
// separate fields). Used to gate/restore every text line for live visibility.
static inline void enable_flag_ptrs(from::paramdef::WORLD_MAP_POINT_PARAM_ST *p,
                                    unsigned *out[8])
{
    out[0] = &p->textEnableFlagId1; out[1] = &p->textEnableFlagId2;
    out[2] = &p->textEnableFlagId3; out[3] = &p->textEnableFlagId4;
    out[4] = &p->textEnableFlagId5; out[5] = &p->textEnableFlagId6;
    out[6] = &p->textEnableFlagId7; out[7] = &p->textEnableFlagId8;
}
static std::vector<CategoryRow> g_category_rows;
// Progress-tab focus: g_focus_category = -1 (none) or a Category value; paired
// with g_focus_region (a region PlaceName id, or -1 for the "Other" bucket).
// When active, the live map shows ONLY that category's uncollected markers IN
// that region, swapped to the glow-highlight icon (see apply_category_visibility
// + apply_focus_highlight).
static int g_focus_category = -1;
static int32_t g_focus_region = -1;

// ---- Manual per-marker hide (hover + hotkey; managed in the overlay) --------
// A user can hide an individual marker by hovering it on the map and pressing the
// hide key. The hidden set persists across sessions keyed by a STABLE hash of the
// marker's deterministic fields (position + area/grid + textId + iconId), NOT the
// dynamic row id. apply_category_visibility() ANDs this in, so a hidden marker's
// icon disappears live and stays hidden on reload. Unhide/clear via the overlay.
struct HiddenMeta { int32_t textId; uint16_t iconId; int32_t region; uint8_t cat; };
static std::map<uint64_t, HiddenMeta> g_manual_hidden;
static std::mutex g_manual_hidden_mtx;
static std::filesystem::path g_hidden_dir;   // folder holding the per-slot hide files
static std::filesystem::path g_hidden_file;  // current slot's file (persist target)
static int g_hidden_slot = -2;               // slot the loaded set belongs to (-2 = none synced yet)

static uint64_t marker_key(const from::paramdef::WORLD_MAP_POINT_PARAM_ST *p)
{
    if (!p) return 0;
    // quantise position to 1/8u so tiny float noise can't change the key
    auto q = [](float f) { return static_cast<int64_t>(std::llround(f * 8.0f)); };
    uint64_t h = 1469598103934665603ull;  // FNV-1a
    auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    mix(static_cast<uint64_t>(p->areaNo));
    mix(static_cast<uint64_t>(p->gridXNo));
    mix(static_cast<uint64_t>(p->gridZNo));
    mix(static_cast<uint64_t>(q(p->posX)));
    mix(static_cast<uint64_t>(q(p->posZ)));
    mix(static_cast<uint64_t>(static_cast<uint32_t>(p->textId1)));
    mix(static_cast<uint64_t>(p->iconId));
    return h;
}

static bool is_manually_hidden(const from::paramdef::WORLD_MAP_POINT_PARAM_ST *p)
{
    if (!p) return false;
    std::lock_guard<std::mutex> lk(g_manual_hidden_mtx);
    return g_manual_hidden.find(marker_key(p)) != g_manual_hidden.end();
}

// True if any of the row's live "hide when set" flags is currently set - i.e. the engine
// is hiding this icon: textDisableFlagId1..8 (loot pickup; live-loot rewrites slot 1 to
// the real getItemFlagId) or clearedEventFlagId (boss/hawk kill). Reads the LIVE param.
static bool row_hidden_by_flag(const from::paramdef::WORLD_MAP_POINT_PARAM_ST *p)
{
    if (!p) return false;
    const unsigned fl[9] = {p->textDisableFlagId1, p->textDisableFlagId2, p->textDisableFlagId3,
                            p->textDisableFlagId4, p->textDisableFlagId5, p->textDisableFlagId6,
                            p->textDisableFlagId7, p->textDisableFlagId8, p->clearedEventFlagId};
    for (unsigned f : fl)
        if (f != 0 && goblin::flag_is_set(f)) return true;
    return false;
}

// True if a group-2 ENABLE gate is currently blocking this marker's icon: any live
// textEnableFlag2IdN is a real flag that is NOT set. Group-2 gates a slot IN ADDITION to
// group-1 (both must hold), applied uniformly across a row's populated slots. We set it
// for the switched-chest pair (baked, e.g. Patches' Glass Shard vs Cloth on flag 3691:
// the absent variant's gate is off) and post-story-event areas (runtime, apply_map_logic).
// Either way an off gate means the game isn't drawing this icon.
static bool row_group2_gate_off(const from::paramdef::WORLD_MAP_POINT_PARAM_ST *p)
{
    if (!p) return false;
    const int g2[8] = {p->textEnableFlag2Id1, p->textEnableFlag2Id2, p->textEnableFlag2Id3,
                       p->textEnableFlag2Id4, p->textEnableFlag2Id5, p->textEnableFlag2Id6,
                       p->textEnableFlag2Id7, p->textEnableFlag2Id8};
    for (int f : g2)
        if (f > 0 && !goblin::flag_is_set(static_cast<uint32_t>(f))) return true;
    return false;
}

// Single "is this marker's icon currently hidden?" test, shared by the focus-highlight
// rings (skip hidden) and the region-progress count (a hidden marker counts as done):
// collected (GEOF), kindling-collected, manually hidden, or a live hide/cleared flag set.
static bool row_is_hidden(const CategoryRow &cr)
{
    return goblin::collected::is_row_collected(cr.row_id) ||
           goblin::kindling::is_row_collected(cr.row_id) ||
           is_manually_hidden(cr.p) ||
           row_hidden_by_flag(cr.p);
}

// Live-loot: lot-backed injected rows. refresh_loot_from_itemlot() reads the
// LIVE ItemLotParam getItemFlagId for each and rewrites textDisableFlagId1 so
// the marker hides on the actual light-point pickup for the loaded regulation
// (Randomizer-compatible). g_lot_backed_set lets apply_flag_or_pairs skip them.
struct LotBackedRow
{
    uint8_t *ptr;
    uint32_t lotId;
    uint8_t lotType;
    int baked_icon;        // iconId as baked (restored when live-loot/anon off)
    int32_t baked_text1;   // textId1 as baked (the item-name label)
    unsigned baked_dis[8]; // textDisableFlagId1..8 as baked
};
static std::vector<LotBackedRow> g_lot_backed_rows;
static std::set<uint8_t *> g_lot_backed_set;

namespace
{
// One ItemLotParam row, read by raw offset (ITEMLOT_PARAM_ST = 152 bytes,
// shared layout for _map and _enemy).
struct RawItemLotRow { uint8_t b[0x98]; };

// Reads ItemLotParam_map / _enemy from live memory once, then resolves rows by
// id. Shared by inject_map_entries (live icon/category) and
// refresh_loot_from_itemlot (live hide-flags / labels).
struct LotReader
{
    std::optional<from::params::ParamTableSequence<RawItemLotRow>> map_lots, enemy_lots;
    void init()
    {
        // ParamTableSequence has a const member (not copy-assignable) → emplace.
        try { map_lots.emplace(from::params::get_param<RawItemLotRow>(L"ItemLotParam_map")); } catch (...) {}
        try { enemy_lots.emplace(from::params::get_param<RawItemLotRow>(L"ItemLotParam_enemy")); } catch (...) {}
    }
    bool ok() const { return map_lots.has_value() || enemy_lots.has_value(); }
    RawItemLotRow *row(uint32_t lot_id, uint8_t lot_type)
    {
        auto &pref  = (lot_type == 2) ? enemy_lots : map_lots;
        auto &other = (lot_type == 2) ? map_lots : enemy_lots;
        // Resolve ONLY in the param that matches lotType. Do NOT cross-fall-back
        // to the other param when a row is missing: ItemLotParam_map and _enemy
        // share one numeric id space, so a randomizer that renumbers/removes enemy
        // lots makes a baked enemy id collide with an UNRELATED map lot (and vice
        // versa) - that returned the wrong item and shuffled the live-loot icons.
        // On a miss we return nullptr (marker keeps its baked icon). The other
        // param is consulted only when the intended one failed to load entirely.
        if (pref) { try { return &(*pref)[lot_id]; } catch (...) { return nullptr; } }
        if (other) { try { return &(*other)[lot_id]; } catch (...) {} }
        return nullptr;
    }
};

// Encode a live item (id + ItemLotParam category 1-5) into the offset-encoded
// key used by both marker textIds and the generated ITEM_ICONS table.
inline int32_t encode_live_item(int32_t item_id, int32_t cat)
{
    switch (cat)
    {
        case 1: return item_id + 500000000;                                       // goods
        case 2: return (item_id >= 50000000) ? item_id : item_id + 100000000;     // ammo / weapon
        case 3: return item_id + 200000000;                                       // protector
        case 4: return item_id + 300000000;                                       // accessory
        case 5: return item_id + 400000000;                                       // gem (ash of war)
        default: return 0;
    }
}

// Spoiler-free (config::anonymousLoot) constants. The generic label reuses the
// localized BloodMsg word "something" (id 32004) at the +950M encoding (copied
// into PlaceName by setup_messages). The icon is our gray "?" frame added to
// sprite 171 of the worldmap gfx (next free frame after the tinted variants).
constexpr int32_t ANON_LABEL_TEXTID = 950000000 + 32004;  // "something"
// gray "?" frame - generated per profile (goblin::generated::ANON_ICON_ID),
// 440 on a vanilla-base gfx, shifted by the icon-frame offset on Convergence.

// Binary-search the baked item-icon table (sorted by key).
const goblin::generated::ItemIcon *lookup_item_icon(int32_t key)
{
    const auto *begin = goblin::generated::ITEM_ICONS;
    const auto *end   = begin + goblin::generated::ITEM_ICON_COUNT;
    const auto *it = std::lower_bound(begin, end, key,
        [](const goblin::generated::ItemIcon &a, int32_t k) { return a.key < k; });
    return (it != end && it->key == key) ? it : nullptr;
}
} // namespace

// Master-off intent set by the toggle hotkey. When true the user has
// explicitly hidden the icons, so the auto-toggle must keep the table vanilla
// even while the world map is open. Shared between the hotkey and watcher
// threads; a lone bool flag is fine, but use atomic for correctness.
static std::atomic<bool> g_icons_user_disabled{false};

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
    case Category::LootMerchantBellBearings: return goblin::config::showMerchantBellBearings;
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
    case Category::WorldKindlingSpirits: return goblin::config::showKindlingSpirits;
    case Category::WorldInteractables:   return goblin::config::showInteractables;
    default:                             return true;
    }
}

// Public wrapper over the file-static is_category_enabled (declared in the
// header for the overlay's Progress tab).
bool goblin::category_enabled(generated::Category cat)
{
    return is_category_enabled(cat);
}

// The ini config key ("show_*") for a category, so the Progress tab can reuse the exact
// Settings-tab icon (draw_row_icon(key)) and localized label (entry_label(key)) instead
// of the raw enum name. Keep in sync with is_category_enabled + the config schema.
const char *goblin::category_config_key(generated::Category cat)
{
    using C = generated::Category;
    switch (cat)
    {
    case C::EquipArmaments: return "show_armaments";
    case C::EquipArmour: return "show_armour";
    case C::EquipAshesOfWar: return "show_ashes_of_war";
    case C::EquipSpirits: return "show_spirits";
    case C::EquipTalismans: return "show_talismans";
    case C::KeyCelestialDew: return "show_celestial_dew";
    case C::KeyCookbooks: return "show_cookbooks";
    case C::KeyCrystalTears: return "show_crystal_tears";
    case C::KeyImbuedSwordKeys: return "show_imbued_sword_keys";
    case C::KeyLarvalTears: return "show_larval_tears";
    case C::KeyScadutreeFragments: return "show_scadutree_fragments";
    case C::KeyGreatRunes: return "show_great_runes";
    case C::KeyLostAshes: return "show_lost_ashes";
    case C::KeyPotsNPerfumes: return "show_pots_n_perfumes";
    case C::KeySeedsTears: return "show_seeds_tears";
    case C::KeyWhetblades: return "show_whetblades";
    case C::LootAmmo: return "show_ammo";
    case C::LootBellBearings: return "show_bell_bearings";
    case C::LootMerchantBellBearings: return "show_merchant_bell_bearings";
    case C::LootConsumables: return "show_consumables";
    case C::LootCraftingMaterials: return "show_crafting_materials";
    case C::LootMPFingers: return "show_mp_fingers";
    case C::LootMaterialNodes: return "show_material_nodes";
    case C::LootReusables: return "show_reusables";
    case C::LootSmithingStones: return "show_smithing_stones";
    case C::LootSmithingStonesLow: return "show_smithing_stones_low";
    case C::LootSmithingStonesRare: return "show_smithing_stones_rare";
    case C::LootGoldenRunes: return "show_golden_runes";
    case C::LootGoldenRunesLow: return "show_golden_runes_low";
    case C::LootStoneswordKeys: return "show_stonesword_keys";
    case C::LootThrowables: return "show_throwables";
    case C::LootPrattlingPates: return "show_prattling_pates";
    case C::LootRuneArcs: return "show_rune_arcs";
    case C::LootDragonHearts: return "show_dragon_hearts";
    case C::LootGloveworts: return "show_gloveworts";
    case C::LootGreatGloveworts: return "show_great_gloveworts";
    case C::LootGestures: return "show_gestures";
    case C::LootGreases: return "show_greases";
    case C::LootUtilities: return "show_utilities";
    case C::LootStatBoosts: return "show_stat_boosts";
    case C::ReforgedFortunes: return "show_fortunes";
    case C::WorldHostileNPC: return "show_hostile_npc";
    case C::MagicIncantations: return "show_incantations";
    case C::MagicMemoryStones: return "show_memory_stones";
    case C::MagicPrayerbooks: return "show_prayerbooks";
    case C::MagicSorceries: return "show_sorceries";
    case C::WorldBosses: return "show_bosses";
    case C::QuestDeathroot: return "show_deathroot";
    case C::QuestProgression: return "show_progression";
    case C::QuestSeedbedCurses: return "show_seedbed_curses";
    case C::ReforgedEmberPieces: return "show_ember_pieces";
    case C::ReforgedItemsAndChanges: return "show_items_and_changes";
    case C::ReforgedRunePieces: return "show_rune_pieces";
    case C::WorldGraces: return "show_graces";
    case C::WorldImpStatues: return "show_imp_statues";
    case C::WorldMaps: return "show_world_maps";
    case C::WorldPaintings: return "show_paintings";
    case C::WorldSpiritSprings: return "show_spirit_springs";
    case C::WorldSpiritspringHawks: return "show_spiritspring_hawks";
    case C::WorldStakesOfMarika: return "show_stakes_of_marika";
    case C::WorldSummoningPools: return "show_summoning_pools";
    case C::WorldKindlingSpirits: return "show_kindling_spirits";
    case C::WorldInteractables: return "show_interactables";
    default: return nullptr;
    }
}

void goblin::set_focus_category(int category_or_negative, int32_t region_place_id)
{
    g_focus_category = category_or_negative;
    g_focus_region = (category_or_negative < 0) ? -1 : region_place_id;
    spdlog::info("[focus] set category={} region={}", g_focus_category, g_focus_region);
}

int goblin::focus_category() { return g_focus_category; }
int32_t goblin::focus_region() { return g_focus_region; }

// Focus label pass. The on-map highlight itself is drawn by the overlay as a projected
// ring (goblin::mapproject / focus_highlight_points); this pass only keeps the focused
// markers' on-map ICONS present so a ring has something under it. For the focused
// (category, region) UNCOLLECTED rows it gives TEXTLESS markers a "?" label +
// isEnableNoText and forces the line on (a point with no text is dropped by the game),
// then restores the baked text when the row leaves focus. Only touches rows it labelled,
// so it can run after apply_loot_settings without clobbering it.
void goblin::apply_focus_highlight()
{
    const int focus = g_focus_category;
    int n_shown = 0, n_forced = 0;
    for (auto &cr : g_category_rows)
    {
        if (!cr.p) continue;
        const bool focused = focus >= 0 && static_cast<int>(cr.cat) == focus &&
                             cr.region_id == g_focus_region;
        const bool shown = focused && !collected::is_row_collected(cr.row_id) &&
                           !kindling::is_row_collected(cr.row_id);

        if (shown)
        {
            ++n_shown;
            // Focus IGNORES require_map_fragments: force this marker visible so its
            // highlight appears (and focus doesn't self-cancel) even in an undiscovered or
            // post-event area, where apply_map_logic gated it - group-1 via eventFlagId, and
            // group-2 via a STORY flag (SetSecondaryFlags, e.g. Leyndell Ashen Capital). We
            // clear ONLY those discovery gates: a switched-chest group-2 gate is a different
            // flag and stays (so a genuinely-absent variant isn't spuriously highlighted).
            // apply_map_logic re-derives every gate on each reapply, so leaving focus
            // restores them automatically - no explicit undo needed.
            cr.p->eventFlagId = static_cast<decltype(cr.p->eventFlagId)>(goblin::flag::AlwaysOn);
            auto unstory = [](auto &slot) {
                if (slot == goblin::flag::StoryErdtreeOnFire ||
                    slot == goblin::flag::StoryCharmBroken ||
                    slot == goblin::flag::StorySealingTreeBurnt)
                    slot = goblin::flag::AlwaysOn;
            };
            unstory(cr.p->textEnableFlag2Id1); unstory(cr.p->textEnableFlag2Id2);
            unstory(cr.p->textEnableFlag2Id3); unstory(cr.p->textEnableFlag2Id4);
            unstory(cr.p->textEnableFlag2Id5); unstory(cr.p->textEnableFlag2Id6);
            unstory(cr.p->textEnableFlag2Id7); unstory(cr.p->textEnableFlag2Id8);
            // Textless rows: fabricate a "?" label + force the line on (textEnableFlagId
            // 0 = treated as On; NOT flag::AlwaysOn=6001, which is a real flag that must
            // be set, so it would HIDE the line). Text-having rows keep their baked gating.
            if (cr.p->textId1 <= 0 && cr.p->textId2 <= 0 && cr.p->textId3 <= 0)
            {
                ++n_forced;
                cr.p->textId1 = goblin::remap_textid(ANON_LABEL_TEXTID);
                cr.p->isEnableNoText = true;
                cr.p->textEnableFlagId1 = 0;
                cr.focus_text = true;
            }
        }
        else if (cr.focus_text)
        {
            cr.p->textId1 = cr.baked_text1;
            cr.p->isEnableNoText = cr.baked_notext;
            cr.focus_text = false;
        }
    }
    if (focus >= 0)
        spdlog::info("[focus] apply: cat={} region={} shown={} forced={} (rows={})",
                     focus, g_focus_region, n_shown, n_forced, g_category_rows.size());
}

// World position of a row, from its live param (grid tile + local offset). The
// affine pages (60/61/12) store gridNo*256 + pos in world units.
static bool row_marker_info(const from::paramdef::WORLD_MAP_POINT_PARAM_ST *p,
                            goblin::HighlightPoint &hp)
{
    __try
    {
        hp.area = p->areaNo;
        hp.layer = p->dispMask00 ? 0 : (p->dispMask01 ? 1 : (p->dispMask02 ? 2 : 0xFF));
        hp.gx = p->gridXNo;
        hp.gz = p->gridZNo;
        hp.px = p->posX;
        hp.pz = p->posZ;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

std::vector<goblin::HighlightPoint> goblin::focus_highlight_points()
{
    std::vector<HighlightPoint> out;
    const int focus = g_focus_category;
    if (focus < 0) return out;
    for (auto &cr : g_category_rows)
    {
        if (!cr.p) continue;
        if (static_cast<int>(cr.cat) != focus || cr.region_id != g_focus_region) continue;
        // Only ring markers whose icon is actually shown: not collected/hidden, and not
        // gated off by a group-2 ENABLE flag (switched-chest absent variant / pre-event area).
        if (row_is_hidden(cr) || row_group2_gate_off(cr.p)) continue;
        HighlightPoint hp{};
        if (row_marker_info(cr.p, hp)) out.push_back(hp);
    }
    return out;
}

std::unordered_set<uint64_t> goblin::hidden_marker_original_ids()
{
    std::unordered_set<uint64_t> out;
    for (const auto &cr : g_category_rows)
        if (cr.original_row_id != 0 && row_is_hidden(cr))
            out.insert(cr.original_row_id);
    return out;
}

// True only if this row is MANUALLY hidden. Used to suppress our hover tooltip: after a
// manual hide the engine keeps reporting the (now icon-less) pin as hovered until the
// cursor moves, so the tooltip would linger. We must NOT use the broader row_is_hidden
// here - a killed boss/done NPC/hawk sets clearedEventFlagId yet its icon is still drawn
// (hideKilledBosses defaults off), and suppressing those tooltips is the regression we hit.
// Collected/flag-hidden icons aren't drawn at all, so the engine never reports them hovered.
bool goblin::is_row_ptr_hidden(void *rowptr)
{
    if (!rowptr) return false;
    for (const auto &cr : g_category_rows)
        if (cr.p == rowptr) return is_manually_hidden(cr.p);
    return false;  // not one of ours
}

bool goblin::prune_focus_if_empty()
{
    if (g_focus_category < 0) return false;
    if (!focus_highlight_points().empty()) return false;  // still something to show
    set_focus_category(-1);
    return true;
}

void goblin::inject_map_entries()
{
    // (The CSFreeListMemorySystem int3-assert NOP patch that used to run here
    // was removed 2026-05-29: it was an artifact of the old hosting-crash
    // theory. The real cause was the 16-align bug in the wrapper_row_locator
    // layout; with that fixed, hosting works with no assert patching -
    // verified live. See docs/ersc_hosting_and_map_autohide.md.)

    struct InjectedEntry
    {
        int32_t row_id;
        uint64_t original_row_id;
        const from::paramdef::WORLD_MAP_POINT_PARAM_ST *data;
        bool is_piece;     // collected::register_param_ptr (CSWorldGeomMan-tracked)
        bool is_kindling;  // kindling::register_param_ptr  (SFX-region-tracked)
        Category category;
        uint32_t lotId;    // live-loot: source ItemLotParam row (0 = none)
        uint8_t lotType;   // 0=none, 1=ItemLotParam_map, 2=ItemLotParam_enemy
    };

    // Live-loot icons (config::liveLootIcons): a randomized lot may now hold an
    // item of a different category than the one baked at this marker. Read the
    // live item, look up the icon + category it would get as a normal marker,
    // and gate / re-icon by THAT instead of the baked category. Resolved icons
    // are keyed by original_row_id and applied when the row is copied below.
    LotReader lot_reader;
    if (goblin::config::liveLootIcons)
        lot_reader.init();
    std::unordered_map<uint64_t, uint16_t> live_icon_override;
    size_t live_recat = 0;
    size_t ll_dbg_lot = 0, ll_dbg_rowfound = 0, ll_dbg_item_gt0 = 0, ll_dbg_item_le0 = 0,
           ll_dbg_hit = 0, ll_dbg_sample = 0;

    // Filter: only include enabled categories (disabled ones are simply not injected)
    std::vector<InjectedEntry> entries;
    entries.reserve(generated::MAP_ENTRY_COUNT);

    size_t skipped_by_config = 0;
    for (size_t i = 0; i < generated::MAP_ENTRY_COUNT; i++)
    {
        const auto &e = generated::MAP_ENTRIES[i];
        bool is_piece = e.category == Category::ReforgedRunePieces ||
                        e.category == Category::ReforgedEmberPieces ||
                        e.category == Category::LootMaterialNodes;
        bool is_kindling = e.category == Category::WorldKindlingSpirits;
        // Live-loot linkage: only for lot-backed loot rows, and never for
        // piece/kindling rows (those are geom/SFX-tracked via collected::).
        uint32_t lotId = (is_piece || is_kindling) ? 0 : e.lotId;
        uint8_t lotType = (is_piece || is_kindling) ? 0 : e.lotType;

        // Resolve the gate/icon from the LIVE item when live-loot icons is on.
        // Spoiler-free mode takes precedence: keep the BAKED category gate (so
        // visibility doesn't leak the hidden item's type) and force the "?" icon
        // on every lot-backed marker.
        Category gate_cat = e.category;
        const bool is_lot = (lotType != 0 && lotId != 0);
        if (goblin::config::anonymousLoot && is_lot)
        {
            live_icon_override[e.row_id] = goblin::generated::ANON_ICON_ID;
        }
        else if (goblin::config::liveLootIcons && is_lot && lot_reader.ok())
        {
            if (RawItemLotRow *r = lot_reader.row(lotId, lotType))
            {
                int32_t item_id = *reinterpret_cast<int32_t *>(r->b + 0x00);   // lotItemId01
                int32_t cat     = *reinterpret_cast<int32_t *>(r->b + 0x20);   // lotItemCategory01
                if (item_id > 0)
                {
                    const auto *ic = lookup_item_icon(encode_live_item(item_id, cat));
                    if (ic)
                    {
                        if (ic->category != gate_cat) live_recat++;
                        gate_cat = ic->category;
                        live_icon_override[e.row_id] = ic->iconId;
                    }
                }
            }
        }

        // Inject EVERY category's rows. Per-category visibility is applied as a
        // live text-flag gate (goblin::apply_category_visibility) and stays
        // toggleable from the in-game config overlay. Carry gate_cat, NOT the
        // baked e.category: when live-loot icons re-icon a randomized drop,
        // gate_cat is the LIVE item's category, so the marker is gated under the
        // SAME category its icon shows. Using the baked category here desynced
        // the two - hiding e.g. Armaments missed randomized weapons and hid
        // unrelated markers whose baked category happened to be Armaments.
        // (Spoiler-free and non-lot rows leave gate_cat == e.category.)
        entries.push_back({0, e.row_id, &e.data, is_piece, is_kindling, gate_cat, lotId, lotType});
    }

    spdlog::info("Adding {} map entries ({} skipped by config, {} live-recategorized)",
                 entries.size(), skipped_by_config, live_recat);
    spdlog::info("[ll-diag] lot-backed processed={} rowFound={} item>0={} item<=0={} tableHit={} (lot_reader.ok={})",
                 ll_dbg_lot, ll_dbg_rowfound, ll_dbg_item_gt0, ll_dbg_item_le0, ll_dbg_hit, lot_reader.ok());

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

    // Update collected + kindling systems with new dynamic IDs
    collected::remap_row_ids(id_remap);
    kindling::remap_row_ids(id_remap);


    uint32_t new_entry_count = static_cast<uint32_t>(entries.size());
    uint32_t total_rows = orig_num_rows + new_entry_count;

    spdlog::debug("Adding {} entries ({} total)", new_entry_count, total_rows);

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
    // Align wrapper_row_loc to 16: the param lookup-by-id engine reads this
    // offset from the wrapper header and rounds it UP to 16 (`(x+0xf)&~0xf`)
    // before using it as the binary-search base. 4-align worked for WMP only
    // because it's iterated, never id-looked-up - but keep it correct so an
    // id lookup (or a future engine path) can't read past the array. (This
    // exact bug crashed TutorialParam save-load; see inject_tutorial_popup_rows.)
    size_t wrapper_row_loc_start = (after_type_str + 0xf) & ~(size_t)0xf;
    size_t wrapper_row_loc_end = wrapper_row_loc_start + total_rows * WRAPPER_ROW_LOC_SIZE;
    size_t param_file_size = wrapper_row_loc_end;
    size_t total_alloc = WRAPPER_HEADER + param_file_size;

    // Allocate the expanded ParamTable from the process heap.
    // HEAP_ZERO_MEMORY zero-inits.
    allocation = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total_alloc);
    if (!allocation)
    {
        spdlog::error("alloc failed ({} bytes)", total_alloc);
        return;
    }

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
        bool is_kindling;
        Category category;
        uint64_t original_row_id;  // pre-remap id (matches locationOverrides keys); 0 for vanilla rows
        uint32_t lotId;            // live-loot: source ItemLotParam row (0 = none)
        uint8_t lotType;           // 0=none, 1=ItemLotParam_map, 2=ItemLotParam_enemy
    };

    std::vector<RowSource> all_rows;
    all_rows.reserve(total_rows);

    for (uint16_t i = 0; i < orig_num_rows; i++)
    {
        auto *data = old_param_file + old_table->rows[i].param_offset;
        all_rows.push_back({static_cast<int32_t>(old_table->rows[i].row_id), data, false, false, {}, 0, 0, 0});
    }
    for (auto &entry : entries)
    {
        all_rows.push_back({entry.row_id, reinterpret_cast<const uint8_t *>(entry.data),
                            entry.is_piece, entry.is_kindling, entry.category, entry.original_row_id,
                            entry.lotId, entry.lotType});
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

        // Record MFG-injected rows (vanilla rows have original_row_id 0) so
        // sanitize_injected_textids() can later strip any textId that the
        // expanded PlaceName FMG didn't end up containing.
        if (all_rows[i].original_row_id)
        {
            g_injected_row_ptrs.push_back(new_param_file + data_offset);
            auto *wp = reinterpret_cast<from::paramdef::WORLD_MAP_POINT_PARAM_ST *>(
                new_param_file + data_offset);
            // Capture baked fields BEFORE the kill-display mutation below, so
            // live re-apply (apply_kill_display) can restore either mode.
            CategoryRow cr{};
            cr.p = wp;
            cr.cat = all_rows[i].category;
            cr.row_id = static_cast<uint64_t>(all_rows[i].row_id);
            cr.original_row_id = all_rows[i].original_row_id;
            cr.baked_cleared = wp->clearedEventFlagId;
            cr.baked_dis1 = wp->textDisableFlagId1;
            cr.baked_dis2 = wp->textDisableFlagId2;
            cr.region_id = goblin::progress::region_place_id(*wp);  // for region-scoped focus
            cr.baked_text1 = wp->textId1;
            cr.baked_notext = wp->isEnableNoText;
            cr.focus_text = false;
            unsigned *en[8];
            enable_flag_ptrs(wp, en);
            for (int k = 0; k < 8; ++k) cr.baked_enable[k] = *en[k];
            g_category_rows.push_back(cr);
            if (!is_category_enabled(all_rows[i].category))
                // Gate EVERY text line behind a never-set flag -> icon hidden
                // (the engine hides the icon only once all text lines - item,
                // enemy, location - are hidden). Unlike dispMask00 this is
                // re-evaluated live, so the overlay can toggle it on the open map.
                for (int k = 0; k < 8; ++k)
                    *en[k] = static_cast<unsigned>(goblin::flag::AlwaysOff);
        }

        // Live-loot: remember lot-backed rows for refresh_loot_from_itemlot().
        if (all_rows[i].lotType != 0 && all_rows[i].lotId != 0)
        {
            uint8_t *rp = new_param_file + data_offset;
            // Capture baked icon/label/flags BEFORE the icon override + live-loot
            // pass, so apply_loot_settings() can revert when those options are off.
            auto *wp = reinterpret_cast<from::paramdef::WORLD_MAP_POINT_PARAM_ST *>(rp);
            LotBackedRow lb{};
            lb.ptr = rp;
            lb.lotId = all_rows[i].lotId;
            lb.lotType = all_rows[i].lotType;
            lb.baked_icon = wp->iconId;
            lb.baked_text1 = wp->textId1;
            unsigned *bfls[8] = {&wp->textDisableFlagId1, &wp->textDisableFlagId2,
                                 &wp->textDisableFlagId3, &wp->textDisableFlagId4,
                                 &wp->textDisableFlagId5, &wp->textDisableFlagId6,
                                 &wp->textDisableFlagId7, &wp->textDisableFlagId8};
            for (int k = 0; k < 8; ++k) lb.baked_dis[k] = *bfls[k];
            g_lot_backed_rows.push_back(lb);
            g_lot_backed_set.insert(rp);

            // Live-loot icons: re-icon the marker to match the live item's
            // category (resolved in the filter loop, keyed by original id).
            auto ico = live_icon_override.find(all_rows[i].original_row_id);
            if (ico != live_icon_override.end())
                reinterpret_cast<from::paramdef::WORLD_MAP_POINT_PARAM_ST *>(rp)->iconId =
                    ico->second;
        }

        // Hybrid sub-area location naming (PRIMARY): overwrite the marker's location
        // line (textId2) with the height-aware sub-area name from generated::LOCATION_ALT
        // (MSB MapPoint/MapNameOverride volume containment, else nearest authored anchor in
        // 3D). The table only holds rows where the hybrid name differs from the baked one;
        // rows absent from it keep their baked textId2 = the FALLBACK (tile/nearest-grace
        // via resolve_location_id_at) for overworld / no-volume / no-anchor spots.
        // The value may be a synthetic compose id (generated::LOCATION_COMPOSE) for
        // duplicate-named sub-zones - goblin_messages builds its FMG string.
        if (all_rows[i].original_row_id)
        {
            auto *alt_end = generated::LOCATION_ALT + generated::LOCATION_ALT_COUNT;
            auto *alt = std::lower_bound(
                generated::LOCATION_ALT, alt_end, all_rows[i].original_row_id,
                [](const generated::LocationAlt &a, uint64_t id) { return a.row_id < id; });
            if (alt != alt_end && alt->row_id == all_rows[i].original_row_id)
            {
                auto *p = reinterpret_cast<from::paramdef::WORLD_MAP_POINT_PARAM_ST *>(
                    new_param_file + data_offset);
                // Overwrite the marker's LOCATION slot (slot picked at generation time:
                // textId2 for plain loot, textId3 for enemy-drops). slot 0 = no baseline
                // location → add one in the first free of textId2/textId3.
                int32_t *tid[9]  = {nullptr, &p->textId1, &p->textId2, &p->textId3, &p->textId4,
                                    &p->textId5, &p->textId6, &p->textId7, &p->textId8};
                unsigned int *fl[9] = {nullptr, &p->textDisableFlagId1, &p->textDisableFlagId2,
                                       &p->textDisableFlagId3, &p->textDisableFlagId4,
                                       &p->textDisableFlagId5, &p->textDisableFlagId6,
                                       &p->textDisableFlagId7, &p->textDisableFlagId8};
                uint8_t s = alt->slot;
                if (s >= 2 && s <= 8)
                {
                    *tid[s] = alt->textId2;   // hide-flag already set on this slot by the generator
                }
                else  // s == 0: add a location line where none existed (e.g. gestures)
                {
                    int add = (p->textId2 == -1) ? 2 : (p->textId3 == -1 ? 3 : 0);
                    if (add)
                    {
                        *tid[add] = alt->textId2;
                        *fl[add] = p->textDisableFlagId1;  // hide with the marker on pickup
                    }
                }
            }
        }

        // Kill display mode (bosses / hawks / NPC invaders): green checkmark
        // vs hide killed. Without this, rows baked with BOTH clearedEventFlagId
        // and textDisableFlagId hide all their text on kill and the icon
        // vanishes before the checkmark can ever show.
        auto cat = all_rows[i].category;
        if (cat == Category::WorldBosses || cat == Category::WorldSpiritspringHawks ||
            cat == Category::WorldHostileNPC)
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

    // Register Rune/Ember piece + kindling-spirit pointers for real-time tracking.
    // Pieces are CSWorldGeomMan-driven (collected::); kindling spirits are
    // SFX-region-driven (kindling::). Same hide-trick (areaNo = 99).
    int registered_pieces = 0, hidden_pieces = 0;
    int registered_kindling = 0, hidden_kindling = 0;
    for (size_t i = 0; i < all_rows.size(); i++)
    {
        size_t data_offset = data_start + i * PARAM_DATA_SIZE;
        auto *param_ptr = new_param_file + data_offset;
        uint64_t row_id = static_cast<uint64_t>(all_rows[i].row_id);

        if (all_rows[i].is_piece)
        {
            collected::register_param_ptr(row_id, param_ptr);
            registered_pieces++;
            if (collected::is_row_collected(row_id))
            {
                param_ptr[0x20] = 99;  // areaNo = 99
                hidden_pieces++;
            }
        }
        else if (all_rows[i].is_kindling)
        {
            kindling::register_param_ptr(row_id, param_ptr);
            registered_kindling++;
            if (kindling::is_row_collected(row_id))
            {
                param_ptr[0x20] = 99;
                hidden_kindling++;
            }
        }
    }

    spdlog::info("Registered {} piece + {} kindling entries ({} + {} hidden at load)",
                 registered_pieces, registered_kindling, hidden_pieces, hidden_kindling);


    // Capture state for runtime toggle. Save original size before overwriting.
    g_file_ptr_ref = &file_ptr_ref;
    g_file_size_ref = &file_size_ref;
    g_vanilla_param_file = old_param_file;
    g_vanilla_param_size = file_size_ref;
    g_expanded_param_file = new_param_file;
    g_expanded_param_size = static_cast<int64_t>(param_file_size);

    file_ptr_ref = new_param_file;
    file_size_ref = static_cast<int64_t>(param_file_size);
    g_param_injection_active = true;

    spdlog::debug("Map entries complete: {} total rows", total_rows);
}

// ─── TutorialParam row injection ─────────────────────────────────────
//
// Adds two new rows for the F10 banner: one displays "Map icons: ON", the
// other "Map icons: OFF". Each row is copied from an existing codex row
// (4167000 - guaranteed to exist with menuType=0 / triggerType=0 / repeatType=1
// from ERR's codex data) and then patched so its textId points at our newly
// injected TutorialBody.fmg entries.
//
// Per ERR TutorialParam.xml paramdef (TUTORIAL_PARAM_ST):
//   offset 4  u8 menuType                (0 = upper-left toast widget)
//   offset 5  u8 triggerType
//   offset 6  u8 repeatType
//   offset 16 (0x10) s32 textId          ← FMG id we point at our entries
//   offset 12 u32 unlockEventFlagId      ← cleared, no gate
//   offset 20 (0x14) f32 dispMinTime
//   offset 24 (0x18) f32 dispTime

// TutorialParam template row we clone.
static constexpr int TUTORIAL_TEMPLATE_ROW_ID = 4167000;

// Dynamically-allocated codex-toast ids (see goblin_inject.hpp). 0 until init.
int goblin::g_toast_fmg_id[goblin::TOAST_COUNT]       = {0, 0, 0, 0};
int goblin::g_toast_param_row_id[goblin::TOAST_COUNT] = {0, 0, 0, 0};

static ParamResCap *find_param_res_cap_by_name(const wchar_t *target)
{
    auto param_list = *from::params::param_list_address;
    if (!param_list) return nullptr;
    for (int i = 0; i < 186; i++)
    {
        auto prc = param_list->entries[i].param_res_cap;
        if (!prc) continue;
        std::wstring_view name = from::params::dlw_c_str(&prc->param_name);
        if (name == target) return prc;
    }
    return nullptr;
}

bool goblin::inject_tutorial_popup_rows()
{
    auto prc = find_param_res_cap_by_name(L"TutorialParam");
    if (!prc)
    {
        spdlog::warn("[TOAST] TutorialParam not found - F10 banner falls back to Summon");
        return false;
    }
    auto *rescap = reinterpret_cast<uint8_t *>(prc->param_header);
    auto *&file_ptr = *reinterpret_cast<uint8_t **>(rescap + 0x80);
    auto &file_size = *reinterpret_cast<int64_t *>(rescap + 0x78);

    auto *old_file = file_ptr;
    auto *old_table = reinterpret_cast<ParamTable *>(old_file);
    uint16_t orig_rows = old_table->num_rows;
    if (orig_rows < 2)
    {
        spdlog::warn("[TOAST] TutorialParam has only {} rows", orig_rows);
        return false;
    }

    // Row data size from TUTORIAL_PARAM_ST paramdef: 1+3 reserve, menuType,
    // triggerType, repeatType, pad1, imageId(u16), pad2(2), unlockEventFlagId
    // (u32), textId(s32), displayMinTime(f32), displayTime(f32), pad3(4) = 32B.
    constexpr int64_t TUTORIAL_ROW_DATA_SIZE = 32;
    int64_t row_data_size = TUTORIAL_ROW_DATA_SIZE;

    // Sanity: the in-memory stride between rows must match the paramdef size.
    int64_t derived_stride = (int64_t)old_table->rows[1].param_offset -
                             (int64_t)old_table->rows[0].param_offset;
    if (derived_stride != row_data_size)
    {
        spdlog::warn("[TOAST] TutorialParam stride {} != paramdef {} - re-laying contiguously",
                     derived_stride, row_data_size);
    }

    // Find a template row. Preferred: ERR codex row 4167000 (menuType=0,
    // repeatType=1). Vanilla has no such row, so fall back to any row with
    // menuType==0 (vanilla ships 13 of those - the toast widget is a vanilla
    // mechanism), and as a last resort synthesize the 32-byte row locally.
    // Every field we depend on is patched explicitly below anyway.
    uint8_t synth_row[TUTORIAL_ROW_DATA_SIZE] = {};
    const uint8_t *template_data = nullptr;
    for (uint16_t i = 0; i < orig_rows; i++)
    {
        if ((int)old_table->rows[i].row_id == TUTORIAL_TEMPLATE_ROW_ID)
        {
            template_data = old_file + old_table->rows[i].param_offset;
            break;
        }
    }
    if (!template_data)
    {
        for (uint16_t i = 0; i < orig_rows; i++)
        {
            const uint8_t *row = old_file + old_table->rows[i].param_offset;
            if (row[4] == 0)  // menuType == 0 (toast)
            {
                template_data = row;
                spdlog::info("[TOAST] template row {} absent (vanilla?) - using row {} (menuType=0)",
                             TUTORIAL_TEMPLATE_ROW_ID, (int)old_table->rows[i].row_id);
                break;
            }
        }
    }
    if (!template_data)
    {
        // Synthesized toast row: menuType=0, triggerType=0, repeatType set
        // below, no image, dispMinTime=1s, dispTime=3s (vanilla toast values).
        *reinterpret_cast<float *>(synth_row + 0x14) = 1.0f;
        *reinterpret_cast<float *>(synth_row + 0x18) = 3.0f;
        template_data = synth_row;
        spdlog::info("[TOAST] no menuType=0 row found - synthesizing toast template");
    }

    constexpr size_t WRAPPER_HEADER = 0x10;
    constexpr size_t HEADER_SIZE = 0x40;
    constexpr size_t ROW_LOCATOR_SIZE = sizeof(ParamRowInfo);
    constexpr size_t WRAPPER_ROW_LOC_SIZE = sizeof(WrapperRowLocator);

    const char *type_str = reinterpret_cast<const char *>(old_file + old_table->param_type_offset);
    size_t type_str_len = strlen(type_str) + 1;

    uint32_t new_row_count = 4;  // ON, OFF, DUMP_OK, DUMP_FAIL
    uint32_t total_rows = orig_rows + new_row_count;

    size_t row_locators_start = HEADER_SIZE;
    size_t data_start = row_locators_start + total_rows * ROW_LOCATOR_SIZE;
    size_t data_end = data_start + total_rows * (size_t)row_data_size;
    size_t type_str_start = data_end;
    size_t after_type_str = type_str_start + type_str_len;
    // CRITICAL: align wrapper_row_loc to 16, NOT 4. The lookup-by-id engine
    // (LookupTutorialParam @ eldenring.exe+0xD51BA0, pre-2026-05-29 RVA) reads this offset from the
    // wrapper header and rounds it UP to 16 via `(x + 0xf) & ~0xf` before using
    // it as the wrapper_row_locator base for its binary search. If our actual
    // array sits at a merely-4-aligned offset, the engine reads 4-12 bytes
    // past it → garbage row ids → out-of-range index → OOB row-data read →
    // crash on save-load (which does an id lookup). WMP got away with 4-align
    // because it's only ever iterated, never id-looked-up.
    size_t wrapper_row_loc_start = (after_type_str + 0xf) & ~(size_t)0xf;
    size_t wrapper_row_loc_end = wrapper_row_loc_start + total_rows * WRAPPER_ROW_LOC_SIZE;
    size_t param_file_size = wrapper_row_loc_end;
    size_t total_alloc = WRAPPER_HEADER + param_file_size;

    auto *allocation = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total_alloc);
    if (!allocation)
    {
        spdlog::error("[TOAST] alloc failed ({} bytes) for TutorialParam expansion", total_alloc);
        return false;
    }

    auto *new_wrapper = reinterpret_cast<uint8_t *>(allocation);
    auto *new_file = new_wrapper + WRAPPER_HEADER;
    auto *new_table = reinterpret_cast<ParamTable *>(new_file);

    *reinterpret_cast<uint32_t *>(new_wrapper + 0x00) = (uint32_t)wrapper_row_loc_start;
    *reinterpret_cast<int32_t *>(new_wrapper + 0x04) = (int32_t)total_rows;

    memcpy(new_file, old_file, HEADER_SIZE);
    new_table->num_rows = (uint16_t)total_rows;
    new_table->param_type_offset = type_str_start;
    *reinterpret_cast<uint32_t *>(new_file + 0x00) = (uint32_t)type_str_start;
    // Offset 0x04 (ushortDataOffset) left as memcpy'd from original (0): the
    // new ER param format uses the u64 dataOffset @0x30 as canonical source.
    *reinterpret_cast<uint64_t *>(new_file + 0x30) = data_start;

    memcpy(new_file + type_str_start, type_str, type_str_len);

    struct RowSource
    {
        int32_t row_id;
        const uint8_t *data_ptr;
    };
    std::vector<RowSource> all_rows;
    all_rows.reserve(total_rows);
    for (uint16_t i = 0; i < orig_rows; i++)
    {
        auto *data = old_file + old_table->rows[i].param_offset;
        all_rows.push_back({(int32_t)old_table->rows[i].row_id, data});
    }
    // Allocate our 4 TutorialParam row ids ABOVE the live max (dynamic - never
    // collides with an overhaul's/another mod's tutorial rows).
    int32_t tp_max = 0;
    for (uint16_t i = 0; i < orig_rows; i++)
        tp_max = std::max(tp_max, (int32_t)old_table->rows[i].row_id);
    for (int s = 0; s < goblin::TOAST_COUNT; ++s)
        goblin::g_toast_param_row_id[s] = tp_max + 1 + s;
    all_rows.push_back({goblin::g_toast_param_row_id[goblin::TOAST_ON],        template_data});
    all_rows.push_back({goblin::g_toast_param_row_id[goblin::TOAST_OFF],       template_data});
    all_rows.push_back({goblin::g_toast_param_row_id[goblin::TOAST_DUMP_OK],   template_data});
    all_rows.push_back({goblin::g_toast_param_row_id[goblin::TOAST_DUMP_FAIL], template_data});

    std::sort(all_rows.begin(), all_rows.end(),
              [](const RowSource &a, const RowSource &b) { return a.row_id < b.row_id; });

    auto *new_locators = reinterpret_cast<ParamRowInfo *>(new_file + row_locators_start);
    auto *new_wrapper_locs = reinterpret_cast<WrapperRowLocator *>(new_file + wrapper_row_loc_start);
    size_t file_end_marker = type_str_start + type_str_len;

    for (size_t i = 0; i < all_rows.size(); i++)
    {
        size_t data_offset = data_start + i * (size_t)row_data_size;
        new_locators[i].row_id = (uint64_t)all_rows[i].row_id;
        new_locators[i].param_offset = data_offset;
        new_locators[i].param_end_offset = file_end_marker;
        memcpy(new_file + data_offset, all_rows[i].data_ptr, (size_t)row_data_size);
        new_wrapper_locs[i].row = all_rows[i].row_id;
        new_wrapper_locs[i].index = (int32_t)i;

        // Patch our new rows: textId -> our dynamically-allocated TutorialBody fmg
        // id (set by setup_messages, which runs first), clear unlockEventFlagId so
        // no gate prevents display. repeatType is set to 1 explicitly: ERR's
        // template carries 1, but vanilla menuType=0 rows ship repeatType=0
        // (show-once) - the toast must repeat.
        int32_t rid = all_rows[i].row_id;
        int slot = -1;
        for (int s = 0; s < goblin::TOAST_COUNT; ++s)
            if (rid == goblin::g_toast_param_row_id[s]) { slot = s; break; }
        if (slot >= 0)
        {
            auto *p = new_file + data_offset;
            *reinterpret_cast<uint8_t *>(p + 4)  = 0;      // menuType = 0 (toast)
            *reinterpret_cast<uint8_t *>(p + 6)  = 1;      // repeatType = 1 (repeatable)
            *reinterpret_cast<uint32_t *>(p + 12) = 0;     // unlockEventFlagId = 0
            *reinterpret_cast<int32_t *>(p + 16)  = goblin::g_toast_fmg_id[slot]; // textId -> our TutorialBody fmg id
        }
    }

    file_ptr = new_file;
    file_size = (int64_t)param_file_size;

    spdlog::info("[TOAST] TutorialParam expanded: {} -> {} rows (row ids ON={}, OFF={}, DUMP_OK={}, DUMP_FAIL={} -> fmg {}/{}/{}/{})",
                 orig_rows, total_rows,
                 goblin::g_toast_param_row_id[goblin::TOAST_ON], goblin::g_toast_param_row_id[goblin::TOAST_OFF],
                 goblin::g_toast_param_row_id[goblin::TOAST_DUMP_OK], goblin::g_toast_param_row_id[goblin::TOAST_DUMP_FAIL],
                 goblin::g_toast_fmg_id[goblin::TOAST_ON], goblin::g_toast_fmg_id[goblin::TOAST_OFF],
                 goblin::g_toast_fmg_id[goblin::TOAST_DUMP_OK], goblin::g_toast_fmg_id[goblin::TOAST_DUMP_FAIL]);
    return true;
}


// ─── Runtime param toggle (drives the F10 personal show/hide) ────────

void goblin::set_param_injection_active(bool active)
{
    if (!g_file_ptr_ref)
    {
        spdlog::warn("[TOGGLE] not ready - map entries not added yet");
        return;
    }
    if (active == g_param_injection_active)
        return;
    if (active)
    {
        *g_file_ptr_ref = g_expanded_param_file;
        *g_file_size_ref = g_expanded_param_size;
    }
    else
    {
        *g_file_ptr_ref = g_vanilla_param_file;
        *g_file_size_ref = g_vanilla_param_size;
    }
    g_param_injection_active = active;
    spdlog::info("[TOGGLE] WorldMapPointParam -> {}", active ? "EXPANDED" : "VANILLA");
}

bool goblin::is_param_injection_active()
{
    return g_param_injection_active;
}

// Combo is configurable via toggle_gamepad_combo in the ini. Default is
// Y + R3 (right stick click), which is uncommon during normal play. Polled
// on all 4 XInput slots so the order of pad-plug doesn't matter.
static bool gamepad_combo_held()
{
    WORD mask = goblin::config::toggleGamepadMask;
    if (!mask) return false;
    for (DWORD i = 0; i < XUSER_MAX_COUNT; i++)
    {
        XINPUT_STATE st{};
        if (XInputGetState(i, &st) != ERROR_SUCCESS) continue;
        if ((st.Gamepad.wButtons & mask) == mask)
            return true;
    }
    return false;
}

// Single source of truth for live marker visibility. A row's primary line (and
// thus its icon) is shown only when its category is enabled AND it is not
// collected (pieces/nodes via collected::, kindling spirits via kindling::).
// Writes textEnableFlagId1, which the engine re-evaluates every frame, so the
// effect is instant on the open map. Called from the overlay on a toggle and
// from the refresh thread when the collected set changes. Idempotent.
void goblin::apply_category_visibility()
{
    const int focus = g_focus_category;
    for (auto &cr : g_category_rows)
    {
        // In focus mode only the focused category IN the focused region is
        // eligible (ignoring its show_* toggle); otherwise the normal per-category
        // toggle applies. Both paths still hide collected rows, so what remains
        // visible is the uncollected markers of that category in that region.
        const bool eligible =
            (focus >= 0) ? (static_cast<int>(cr.cat) == focus && cr.region_id == g_focus_region)
                         : is_category_enabled(cr.cat);
        bool show = eligible &&
                    !collected::is_row_collected(cr.row_id) &&
                    !kindling::is_row_collected(cr.row_id) &&
                    !is_manually_hidden(cr.p);  // user-hidden markers stay hidden
        unsigned *en[8];
        enable_flag_ptrs(cr.p, en);
        for (int k = 0; k < 8; ++k)
            *en[k] = show ? cr.baked_enable[k]
                          : static_cast<unsigned>(goblin::flag::AlwaysOff);
    }
}

// ---- Manual per-marker hide: public API ------------------------------------
goblin::ManualHideResult goblin::toggle_hovered_marker(void *rowptr)
{
    ManualHideResult r{};
    if (!rowptr) return r;
    for (auto &cr : g_category_rows)
    {
        if (cr.p != rowptr) continue;
        r.matched = true;
        r.textId = cr.p->textId1;
        uint64_t k = marker_key(cr.p);
        std::lock_guard<std::mutex> lk(g_manual_hidden_mtx);
        auto it = g_manual_hidden.find(k);
        if (it != g_manual_hidden.end()) { g_manual_hidden.erase(it); r.now_hidden = false; }
        else
        {
            g_manual_hidden[k] = HiddenMeta{cr.p->textId1, cr.p->iconId, cr.region_id,
                                            static_cast<uint8_t>(cr.cat)};
            r.now_hidden = true;
        }
        return r;
    }
    return r;  // hovered pin is not one of our injected markers (a vanilla point)
}

goblin::HoveredMarker goblin::hovered_marker(void *rowptr)
{
    HoveredMarker r{};
    if (!rowptr) return r;
    for (const auto &cr : g_category_rows)
        if (cr.p == rowptr)
        {
            r.matched = true;
            r.textId = cr.p->textId1;
            r.posY = cr.p->posY;
            HighlightPoint hp{};
            if (row_marker_info(cr.p, hp))
            {
                r.area = hp.area;
                r.world_x = static_cast<float>(hp.gx) * 256.0f + hp.px;
                r.world_z = static_cast<float>(hp.gz) * 256.0f + hp.pz;
            }
            return r;
        }
    return r;
}

size_t goblin::manual_hidden_count()
{
    std::lock_guard<std::mutex> lk(g_manual_hidden_mtx);
    return g_manual_hidden.size();
}

std::vector<goblin::HiddenMarkerInfo> goblin::manual_hidden_snapshot()
{
    std::vector<HiddenMarkerInfo> out;
    std::lock_guard<std::mutex> lk(g_manual_hidden_mtx);
    out.reserve(g_manual_hidden.size());
    for (const auto &[k, m] : g_manual_hidden)
        out.push_back(HiddenMarkerInfo{k, m.textId, m.iconId, m.region, m.cat});
    return out;
}

void goblin::unhide_marker(uint64_t key)
{
    std::lock_guard<std::mutex> lk(g_manual_hidden_mtx);
    g_manual_hidden.erase(key);
}

void goblin::clear_manual_hidden()
{
    std::lock_guard<std::mutex> lk(g_manual_hidden_mtx);
    g_manual_hidden.clear();
}

void goblin::save_manual_hidden(const std::filesystem::path &path)
{
    std::lock_guard<std::mutex> lk(g_manual_hidden_mtx);
    try
    {
        std::ofstream f(path, std::ios::trunc);
        for (const auto &[k, m] : g_manual_hidden)
            f << k << ' ' << m.textId << ' ' << m.iconId << ' ' << m.region << ' '
              << static_cast<int>(m.cat) << '\n';
    }
    catch (...) {}
}

void goblin::load_manual_hidden(const std::filesystem::path &path)
{
    std::lock_guard<std::mutex> lk(g_manual_hidden_mtx);
    try
    {
        std::ifstream f(path);
        uint64_t k; long tid, icon, region, cat;
        while (f >> k >> tid >> icon >> region >> cat)
            g_manual_hidden[k] = HiddenMeta{static_cast<int32_t>(tid), static_cast<uint16_t>(icon),
                                            static_cast<int32_t>(region), static_cast<uint8_t>(cat)};
    }
    catch (...) {}
}

void goblin::set_hidden_dir(const std::filesystem::path &dir) { g_hidden_dir = dir; }
void goblin::persist_manual_hidden() { if (!g_hidden_file.empty()) save_manual_hidden(g_hidden_file); }

// GameMan .data slot, resolved by AOB (patch-resilient; NOT a hardcoded RVA - the static
// slot moves on every game update). Pinned by the getter idiom
// `mov rax,[rip+GameMan]; cmp byte[rax+imm],0x0D; setz al; ret`; {{3,7}} extracts the
// slot address from the rip-relative mov. Resolved once, cached; 0 if not found.
// (Source: Hexinton CE table AOB. Was hardcoded RVA 0x3D69918; live-verified.)
static uintptr_t game_man_slot()
{
    static uintptr_t s = []() -> uintptr_t {
        try
        {
            return reinterpret_cast<uintptr_t>(modutils::scan<void>(
                {.aob = "48 8B 05 ?? ?? ?? ?? 80 B8 ?? ?? ?? ?? 0D 0F 94 C0 C3",
                 .relative_offsets = {{3, 7}}}));
        }
        catch (...) { return 0; }
    }();
    return s;
}

int goblin::active_save_slot()
{
    // slot = *(int*)(GameMan + 0xAC0). -1 = no character loaded. GameMan+0xAC0 =
    // "Save Slot (Profile Index)" per the Hexinton CE table. Live-verified 2026-07-13
    // (1st character -> 0). A missing AOB (game update) degrades to -1 = no per-slot set.
    const uintptr_t slotaddr = game_man_slot();
    if (!slotaddr) return -1;
    int slot = -1;
    __try
    {
        void *gm = *reinterpret_cast<void **>(slotaddr);
        if (gm) slot = *reinterpret_cast<int *>(reinterpret_cast<uint8_t *>(gm) + 0xAC0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    return (slot >= 0 && slot <= 9) ? slot : -1;  // 0..9 valid; anything else = none
}

bool goblin::sync_hidden_slot()
{
    const int slot = active_save_slot();
    if (slot == g_hidden_slot) return false;  // no character-switch since last check
    g_hidden_slot = slot;
    {
        std::lock_guard<std::mutex> lk(g_manual_hidden_mtx);
        g_manual_hidden.clear();
    }
    if (slot >= 0 && !g_hidden_dir.empty())
    {
        g_hidden_file = g_hidden_dir / ("MapForGoblins_hidden_s" + std::to_string(slot) + ".txt");
        load_manual_hidden(g_hidden_file);  // per-character set (empty file -> empty set)
        spdlog::info("[hide] active save slot {} -> {} ({} hidden loaded)", slot,
                     g_hidden_file.filename().string(), manual_hidden_count());
    }
    else
    {
        g_hidden_file.clear();  // no character loaded: no persist target, empty set
    }
    return true;  // set changed -> caller reapplies visibility
}

// World Map fragment markers are only useful BEFORE you own that fragment, but the
// require_map_fragments gate (apply_map_logic sets eventFlagId = the area's fragment
// flag) would hide them until you do - i.e. never usefully. When the option is on,
// clear that gate (eventFlagId = AlwaysOn) so they always show. Independent of
// show_world_maps (the category on/off). Runs AFTER apply_map_logic, which re-derives
// eventFlagId every call, so turning the option off restores the gate on next reapply.
void goblin::apply_worldmap_fragment_bypass()
{
    if (!goblin::config::worldMapsIgnoreFragments)
        return;
    for (auto &cr : g_category_rows)
        if (cr.cat == Category::WorldMaps && cr.p)
            cr.p->eventFlagId =
                static_cast<decltype(cr.p->eventFlagId)>(goblin::flag::AlwaysOn);
}

// Live re-apply of hide_killed_bosses (boss / spiritspring-hawk / hostile-NPC
// rows). Re-derives from the baked fields so either mode is reversible.
void goblin::apply_kill_display()
{
    for (auto &cr : g_category_rows)
    {
        if (cr.cat != Category::WorldBosses &&
            cr.cat != Category::WorldSpiritspringHawks &&
            cr.cat != Category::WorldHostileNPC)
            continue;
        if (goblin::config::hideKilledBosses)
        {
            cr.p->clearedEventFlagId = 0; // text hides on kill -> icon hides
            // Hide on the defeat flag. Bosses bake it into textDisableFlagId1;
            // Spiritspring Hawks only carry it in clearedEventFlagId, so fall back
            // to that (else the hawk icon never hides, only loses its checkmark).
            cr.p->textDisableFlagId1 = cr.baked_dis1 ? cr.baked_dis1 : cr.baked_cleared;
            cr.p->textDisableFlagId2 = cr.baked_dis2;
        }
        else
        {
            cr.p->clearedEventFlagId = cr.baked_cleared; // keep green checkmark
            cr.p->textDisableFlagId1 = 0;
            cr.p->textDisableFlagId2 = 0;
        }
    }
}

// Live re-apply of the loot icon/label/flag options (anonymous_loot,
// live_loot_icons/labels/flags) across every lot-backed marker. Re-reads the
// live ItemLotParam and re-derives each marker's icon, item-name label, and
// hide-on-pickup flag - reverting to the baked values when an option is off.
static void apply_loot_settings()
{
    if (g_lot_backed_rows.empty())
        return;
    const bool anon = goblin::config::anonymousLoot;
    const bool do_icons = goblin::config::liveLootIcons;
    const bool do_labels = goblin::config::liveLootLabels;
    const bool do_flags = goblin::config::liveLootFlags;

    LotReader lots;
    lots.init();
    const bool have_lots = lots.ok();

    for (auto &lr : g_lot_backed_rows)
    {
        auto *p = reinterpret_cast<from::paramdef::WORLD_MAP_POINT_PARAM_ST *>(lr.ptr);
        RawItemLotRow *row = have_lots ? lots.row(lr.lotId, lr.lotType) : nullptr;
        int32_t item = 0, cat = 0;
        if (row)
        {
            item = *reinterpret_cast<int32_t *>(row->b + 0x00); // lotItemId01
            cat = *reinterpret_cast<int32_t *>(row->b + 0x20);  // lotItemCategory01
        }

        // Icon. The result MUST be an INJECTED iconId (a runtime-appended frame), not a baked one:
        // without our gfx the baked iconIds are our gfx's frame numbers, which are out of range in the
        // stock sprite and clamp to garbage/"?". The anon path already yields an injected frame
        // (anon_dynamic_iconid); the live-loot / baked paths give a baked srcIconId that we convert via
        // injected_iconid(). This also fixes "anon stays stuck after disabling it": the revert now lands
        // on the real injected icon instead of an out-of-range baked one.
        int icon;
        if (anon)
        {
            uint32_t dyn = goblin::gfx_probe::anon_dynamic_iconid(); // already an injected frame
            icon = dyn ? static_cast<int>(dyn) : goblin::generated::ANON_ICON_ID;
        }
        else
        {
            int baked = lr.baked_icon;
            if (do_icons && item > 0)
                if (const auto *ic = lookup_item_icon(encode_live_item(item, cat)))
                    baked = ic->iconId;
            uint32_t inj = goblin::gfx_probe::injected_iconid(baked); // baked srcIconId -> injected frame
            icon = inj ? static_cast<int>(inj) : baked;
        }
        p->iconId = static_cast<decltype(p->iconId)>(icon);

        // Item-name label (only touch an item-name slot). Classify by the ORIGINAL
        // encoded baked_text1, write the collision-proof remapped id (the string
        // lives at the fresh id setup_messages allocated; remap_textid is identity
        // for an unmapped key).
        if (lr.baked_text1 >= 50000000 && lr.baked_text1 < 600000000)
        {
            int32_t label = lr.baked_text1;
            if (anon)
                label = ANON_LABEL_TEXTID;
            else if (do_labels && item > 0)
            {
                int32_t enc = encode_live_item(item, cat);
                if (enc > 0) label = enc;
            }
            p->textId1 = goblin::remap_textid(label);
        }

        // Hide-on-pickup flag (all populated lines that had a baked flag)
        int *tids[8] = {&p->textId1, &p->textId2, &p->textId3, &p->textId4,
                        &p->textId5, &p->textId6, &p->textId7, &p->textId8};
        unsigned *fls[8] = {&p->textDisableFlagId1, &p->textDisableFlagId2,
                            &p->textDisableFlagId3, &p->textDisableFlagId4,
                            &p->textDisableFlagId5, &p->textDisableFlagId6,
                            &p->textDisableFlagId7, &p->textDisableFlagId8};
        uint32_t flag = 0;
        if (do_flags && row)
        {
            flag = *reinterpret_cast<uint32_t *>(row->b + 0x80);
            if (flag == 0)
            {
                int32_t item2 = *reinterpret_cast<int32_t *>(row->b + 0x04);
                if (item2 == 0) flag = *reinterpret_cast<uint32_t *>(row->b + 0x60);
            }
        }
        for (int i = 0; i < 8; ++i)
            *fls[i] = (do_flags && flag && *tids[i] > 0 && lr.baked_dis[i] != 0)
                          ? flag
                          : lr.baked_dis[i]; // else restore baked
    }
}

// One call to re-apply every LIVE-capable setting after the overlay edits the
// config. Each step re-derives from baked state (idempotent). Takes effect on
// the next world-map (re)open.
void goblin::reapply_live_settings()
{
    apply_category_visibility();           // show_* categories (+ focus isolation)
    apply_kill_display();                  // hide_killed_bosses
    apply_loot_settings();                 // anonymous_loot + live_loot_icons/labels/flags
    goblin::apply_map_logic();             // require_map_fragments + ERR patch_* markers
    goblin::apply_worldmap_fragment_bypass(); // after map logic (it re-gates eventFlagId)
    apply_focus_highlight();               // LAST: focus glow-icon + force-visible must win over
                                           // apply_loot_settings (which rewrites loot iconId) and the
                                           // category/fragment enable-flag gating above.
}

void goblin::set_icons_hidden(bool hidden) { g_icons_user_disabled.store(hidden); }
bool goblin::icons_hidden() { return g_icons_user_disabled.load(); }

void goblin::toggle_hotkey_loop()
{
    bool prev_kbd = false, prev_pad = false;
    while (true)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!config::enableToggleHotkey) { prev_kbd = false; prev_pad = false; continue; }

        // The toggle key/combo OPEN the overlay when it's enabled (handled in the
        // overlay's hkPresent). This master show/hide path only fires when the
        // overlay is DISABLED, so the same press never both opens the menu AND
        // toggles icons.
        const bool master_mode = !config::enableOverlay;
        bool kbd = master_mode &&
                   goblin::overlay::key_down(static_cast<int>(config::toggleInjectionKey));
        bool pad = master_mode && gamepad_combo_held();

        // Rising-edge on either input source independently.
        bool fired = (kbd && !prev_kbd) || (pad && !prev_pad);
        prev_kbd = kbd;
        prev_pad = pad;

        if (fired)
        {
            // The hotkey is a master-off intent, not a direct param swap. The
            // watcher thread (menu_auto_toggle_loop) is the single owner of the
            // param state; it honours this flag. When disabled the user has
            // explicitly hidden the icons, so they stay hidden even on the map.
            bool disabled = !g_icons_user_disabled.load();
            g_icons_user_disabled.store(disabled);
            spdlog::info("[TOGGLE] icons {} by user (source: {})",
                         disabled ? "HIDDEN" : "SHOWN",
                         (kbd && !pad) ? "keyboard" : (pad && !kbd) ? "gamepad" : "both");
        }
    }
}

// (The old Summon-message path (post_summon) was removed: it depended on five
// hardcoded RVAs (0x763360/0x11A3E0/0x843860/0x844060/0x843910) that a game
// update invalidates, and the codex trampoline below is the toast style we
// actually ship. The F10/F9 banner uses the AOB-resolved trampoline only.)

// ShowTutorialPopup callers - codex/medal upper-left toast.
// Three entries pinned by static analysis (agent run, May 2026):
//   - inner   0x7EF5B0  `void(CSPopupMenu*, int id, bool, bool)` (286-byte fn)
//   - outer   0x7EE630  `void(CSPopupMenu*, int id, bool)` (4 direct call sites)
//   - tramp   0x80DA50  `void(int id)` - resolves singleton internally
// CSPopupMenu singleton ptr lives in .data at `CSFeMan_slot + 0x80`.
// AOB anchor for outer (24 bytes, unique across image):
//   48 8B C4 44 88 40 18 89 50 10 55 56 57 41 56 41 57 48 8D 68 A1 48 81 EC
// Patch-resilient anchor: LEA xref in real-.text to string
//   "CS::CSPopupMenu::_CanOpenTutorialParam" in .rdata.
//
// Note: eldenring.exe has TWO `.text` sections (VMProtect adds one). When
// pinning via pefile, scan the original MSVC `.text` at RVA 0x1000..0x29A3000,
// NOT the VMP-added one at 0x4C0E000+ - different content, will miss real fns.
// Resolve the trampoline by AOB (NOT a hardcoded RVA): a game update shifts
// every function's RVA (the May-2026 patch moved this one from 0x80DA50 to
// 0x80D960), so we pin it by a stable surrounding-byte signature that survives
// patches. modutils::scan returns the address of the AOB's first byte = the
// function entry. Resolved once and cached.
static void show_tutorial_popup_trampoline(uintptr_t /*er*/, int tutorial_id)
{
    static void (*fn)(int) = nullptr;
    static bool tried = false;
    if (!tried)
    {
        tried = true;
        fn = reinterpret_cast<void (*)(int)>(modutils::scan<void>({
            .aob = "48 8B 05 ?? ?? ?? ?? 8B D1 48 85 C0 74 17 48 8B 88 80 00 00 00 48 85 C9",
        }));
        spdlog::info("[TOAST] resolved ShowTutorialPopup @ {:p}", (void *)fn);
    }
    if (fn) fn(tutorial_id);
}

// SEH-guarded codex-toast fire (POD-only locals, no C++ unwinding).
static void seh_dispatch_toast(uintptr_t er, bool icons_on)
{
    int tutorial_id = goblin::g_toast_param_row_id[icons_on ? goblin::TOAST_ON : goblin::TOAST_OFF];
    __try
    {
        show_tutorial_popup_trampoline(er, tutorial_id);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// Fire the upper-left codex toast for the icons ON/OFF toggle. Resolves the
// module base once.
static void show_toggle_banner(bool icons_on)
{
    static uintptr_t er = 0;
    if (!er) er = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr) /* main module = the game exe */);
    if (!er) return;
    spdlog::info("[TOAST] fire (icons {})", icons_on ? "ON" : "OFF");
    seh_dispatch_toast(er, icons_on);
}

// SEH-guarded trampoline fire (POD-only locals - no C++ unwinding).
static void seh_fire_trampoline(uintptr_t er, int tutorial_id)
{
    __try { show_tutorial_popup_trampoline(er, tutorial_id); }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}

// Fire an upper-left codex toast for one of the injected TutorialParam rows
// (a goblin::g_toast_param_row_id[...] value). Static text via the same trampoline
// path as the F10 banner - no FMG rewrite. Used by the F9 marker-dump banner.
void goblin::show_codex_toast(int tutorial_id)
{
    static uintptr_t er = 0;
    if (!er) er = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr) /* main module = the game exe */);
    if (!er) return;
    seh_fire_trampoline(er, tutorial_id);
}


// WorldMapPointParam state owner. Since the 16-align fix in inject_map_entries
// (see docs/ersc_hosting_and_map_autohide.md), the expanded table is safe during
// ERSC hosting - the old "expand only while the map is open" auto-hide is no
// longer needed and has been removed. The table now stays EXPANDED always; the
// hotkey is a pure personal show/hide toggle.
//
// Desired table state:
//   userDisabled (F10/gamepad master-off) -> VANILLA  (user hid the icons)
//   else                                  -> EXPANDED  (icons everywhere)
//
// (The retired map-state auto-hide read CSMenuMan+0xCD with inverse logic;
// it's fully documented in docs/ersc_hosting_and_map_autohide.md should a
// future patch ever need it back.)
const std::vector<uint8_t *> &goblin::injected_row_ptrs()
{
    return g_injected_row_ptrs;
}

// ── Either-flag (OR) kill indicators ─────────────────────────────────
// Some quest fights have two mutually-exclusive completion flags (one per
// story branch) and no single "battle over" flag. Example: the academy
// battle - 7608 = Sellen's battle body defeated (sided with Jerren),
// 7609 = Jerren defeated (sided with Sellen); after either one, BOTH NPCs
// stop being attackable, so both markers should show the checkmark.
// Such rows are baked with the PRIMARY flag; once the ALT flag turns on
// this rewrites the matching fields so the checkmark/hide reacts within
// the running session. Pairs mirror data/quest_invader_overrides.json.
//
// Event-flag query - same AOBs as goblin_markers.cpp / goblin_kindling.cpp
// (each keeps its own local copy by established convention there).
using OrPairIsFlagFn = bool (*)(void *, uint32_t *);
static OrPairIsFlagFn g_orp_is_flag = nullptr;
static void **g_orp_event_man_slot = nullptr;
static bool g_orp_resolve_tried = false;

static bool orp_flag_set(uint32_t flag_id)
{
    if (!g_orp_resolve_tried)
    {
        g_orp_resolve_tried = true;
        try
        {
            g_orp_is_flag = modutils::scan<bool(void *, uint32_t *)>(
                { .aob = "48 83 EC 28 8B 12 85 D2" });
            g_orp_event_man_slot = reinterpret_cast<void **>(modutils::scan<void *>(
                { .aob = "48 8B 3D ?? ?? ?? ?? 48 85 FF ?? ?? 32 C0 E9",
                  .relative_offsets = { {3, 7} } }));
        }
        catch (...) { g_orp_is_flag = nullptr; g_orp_event_man_slot = nullptr; }
    }
    if (!g_orp_is_flag || !g_orp_event_man_slot) return false;
    void *event_man = *g_orp_event_man_slot;
    if (!event_man) return false;
    uint32_t id = flag_id;
    return g_orp_is_flag(event_man, &id);
}

// Public event-flag query (Progress tab). Guards flag 0, which the game treats
// as always-on - a marker with no pickup flag must NOT count as collected.
bool goblin::flag_is_set(uint32_t flag_id)
{
    if (flag_id == 0) return false;
    return orp_flag_set(flag_id);
}

struct FlagOrPair { uint32_t primary; uint32_t alt; };
static constexpr FlagOrPair FLAG_OR_PAIRS[] = {
    {7608, 7609},  // Sellen/Jerren academy battle
};

void goblin::apply_flag_or_pairs()
{
    for (const auto &pr : FLAG_OR_PAIRS)
    {
        if (!orp_flag_set(pr.alt))
            continue;
        for (uint8_t *ptr : g_injected_row_ptrs)
        {
            // Skip live-loot rows: their textDisableFlagId1 holds a lot pickup
            // flag (set by refresh_loot_from_itemlot), not a boss/quest flag -
            // don't let a value-collision rewrite it.
            if (g_lot_backed_set.count(ptr)) continue;
            auto *p = reinterpret_cast<from::paramdef::WORLD_MAP_POINT_PARAM_ST *>(ptr);
            if (p->clearedEventFlagId == pr.primary) p->clearedEventFlagId = pr.alt;
            if (p->textDisableFlagId1 == pr.primary) p->textDisableFlagId1 = pr.alt;
            if (p->textDisableFlagId2 == pr.primary) p->textDisableFlagId2 = pr.alt;
        }
    }
}

// ── Live-loot: hide loot markers on the LIVE item-lot pickup flag ──────
// Reads each lot-backed marker's source ItemLotParam row from memory and sets
// textDisableFlagId1 to the lot's current getItemFlagId. Because we read the
// LOADED regulation (vanilla, Randomizer, any file mod), the marker hides on
// the actual light-point pickup regardless of which item the lot now gives.
// Point every marker whose (baked/live) iconId is a custom icon we injected at the runtime-injected
// frame for it. Called from the worldmap-load hook after the icon frames are appended, before pins are
// built. gfx_probe::injected_iconid(src) returns the appended frame's iconId for source icon `src`, or
// 0 if that icon wasn't injected (e.g. vanilla icons 1-348 - left as-is, the base gfx provides them).
void goblin::remap_injected_icons()
{
    uint32_t iid_lo = 0, iid_hi = 0;
    goblin::gfx_probe::injected_iid_range(iid_lo, iid_hi);
    int n = 0, already = 0;
    for (auto &cr : g_category_rows)
    {
        if (!cr.p)
            continue;
        uint32_t cur = static_cast<uint32_t>(cr.p->iconId);
        // A marker already inside the injected frame-id range is being remapped a SECOND time:
        // injected_iconid() reads it as a srcIconId and (when the ranges overlap) resolves to the
        // wrong frame -> categories shuffle. Expected 0; nonzero = two DLL instances injected.
        if (iid_lo && cur >= iid_lo && cur <= iid_hi) ++already;
        uint32_t inj = goblin::gfx_probe::injected_iconid(static_cast<int>(cur));
        if (inj)
        {
            cr.p->iconId = static_cast<decltype(cr.p->iconId)>(inj);
            ++n;
        }
    }
    if (already)
        spdlog::warn("[icons] {} markers already in the new frame range [{},{}] (duplicate pass = "
                     "possibly two copies active).", already, iid_lo, iid_hi);
    spdlog::info("[icons] mapped {} markers to new frame ids.", n);
    apply_focus_highlight();  // re-apply any active focus glow after the frames are (re)mapped
    goblin::diag::set_remap(n);
}

// One-shot at init: the flag VALUE in a row is static post-load; the engine
// then evaluates textDisableFlagId1 live every frame. See reference_cleared_badge
// / the randomizer-compat research. Gated by config::liveLootFlags/Labels.
void goblin::refresh_loot_from_itemlot()
{
    const bool do_flags  = goblin::config::liveLootFlags;
    const bool do_labels = goblin::config::liveLootLabels;
    const bool do_anon   = goblin::config::anonymousLoot;
    if ((!do_flags && !do_labels && !do_anon) || g_lot_backed_rows.empty())
        return;

    LotReader lots;
    lots.init();
    if (!lots.ok())
    {
        spdlog::warn("[LIVE-LOOT] ItemLotParam not available - skipped");
        return;
    }
    auto read_row = [&](uint32_t lot_id, uint8_t lot_type) { return lots.row(lot_id, lot_type); };

    int updated = 0, relabeled = 0, not_found = 0, no_flag = 0;
    for (auto &lr : g_lot_backed_rows)
    {
        RawItemLotRow *row = read_row(lr.lotId, lr.lotType);
        if (!row) { not_found++; continue; }
        auto *p = reinterpret_cast<from::paramdef::WORLD_MAP_POINT_PARAM_ST *>(lr.ptr);

        if (do_flags)
        {
            uint32_t flag = *reinterpret_cast<uint32_t *>(row->b + 0x80);  // lot-wide getItemFlagId
            if (flag == 0)
            {
                // Fall back to the per-slot flag only for single-item lots
                // (lotItemId02 @0x04 == 0), else a slot-1 award would hide the
                // marker while other loot remains.
                int32_t item2 = *reinterpret_cast<int32_t *>(row->b + 0x04);
                if (item2 == 0)
                    flag = *reinterpret_cast<uint32_t *>(row->b + 0x60);  // getItemFlagId01
            }
            if (flag)
            {
                // Hide the WHOLE marker on the live pickup flag. A loot marker
                // carries the same disable flag on every populated text line
                // (item line + location line; verified uniform across all
                // lot-backed rows) - the engine only drops the icon once ALL
                // its lines are disabled. Rewriting just slot 1 left the
                // location line (slot 2) pinned to the stale baked flag, which
                // never fires under a regulation that reassigns flags (the
                // randomizer), so the marker never disappeared. Update every
                // line that had a (non-zero) disable flag baked.
                int *tids[8] = {&p->textId1, &p->textId2, &p->textId3, &p->textId4,
                                &p->textId5, &p->textId6, &p->textId7, &p->textId8};
                unsigned int *fls[8] = {&p->textDisableFlagId1, &p->textDisableFlagId2,
                                        &p->textDisableFlagId3, &p->textDisableFlagId4,
                                        &p->textDisableFlagId5, &p->textDisableFlagId6,
                                        &p->textDisableFlagId7, &p->textDisableFlagId8};
                for (int i = 0; i < 8; ++i)
                    if (*tids[i] > 0 && *fls[i] != 0)
                        *fls[i] = flag;
                updated++;
            }
            else no_flag++;
        }

        // Classify the item-name slot by the ORIGINAL encoded baked_text1 (the
        // live textId1 now holds a remapped fresh id, which no longer carries the
        // 50M..600M item band), and write the collision-proof remapped id.
        const bool item_slot = (lr.baked_text1 >= 50000000 && lr.baked_text1 < 600000000);
        if (do_anon)
        {
            if (item_slot)
            {
                int32_t anon = goblin::remap_textid(ANON_LABEL_TEXTID);
                if (p->textId1 != anon) { p->textId1 = anon; relabeled++; }
            }
        }
        else if (do_labels)
        {
            // Relabel the item-name slot (textId1) to whatever the lot now gives.
            if (item_slot)
            {
                int32_t item_id = *reinterpret_cast<int32_t *>(row->b + 0x00);  // lotItemId01
                int32_t cat     = *reinterpret_cast<int32_t *>(row->b + 0x20);  // lotItemCategory01
                int32_t enc = encode_live_item(item_id, cat);
                int32_t fresh = goblin::remap_textid(enc);
                if (item_id > 0 && enc > 0 && fresh != p->textId1)
                {
                    p->textId1 = fresh;
                    relabeled++;
                }
            }
        }
    }
    spdlog::info("[LIVE-LOOT] {} hide-flags, {} relabels set from live ItemLotParam "
                 "({} lots not found, {} no flag, {} lot-backed total)",
                 updated, relabeled, not_found, no_flag, g_lot_backed_rows.size());
}

void goblin::menu_auto_toggle_loop()
{
    bool prev_user_disabled = g_icons_user_disabled.load();

    while (true)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        // Show the native banner when the user flips the master-off via hotkey.
        // Driven from this thread (the param-state owner) so all game-state
        // mutation happens in one place.
        bool user_disabled_now = g_icons_user_disabled.load();
        if (user_disabled_now != prev_user_disabled)
        {
            show_toggle_banner(!user_disabled_now);
            prev_user_disabled = user_disabled_now;
        }

        bool want_expanded = !user_disabled_now;

        if (want_expanded && !g_param_injection_active)
        {
            set_param_injection_active(true);
            spdlog::info("[TOGGLE] -> EXPANDED (icons on)");
        }
        else if (!want_expanded && g_param_injection_active)
        {
            set_param_injection_active(false);
            spdlog::info("[TOGGLE] -> VANILLA (icons off)");
        }
    }
}
