#include "goblin_inject.hpp"

#include "goblin_build_variants.hpp"
#include "goblin_collected.hpp"
#include "goblin_kindling.hpp"
#include "goblin_logic.hpp"
#include "goblin_map_api.hpp"
#include "goblin_config.hpp"
#include "goblin_messages.hpp"
#include "goblin_status_line.hpp" // the icons ON/OFF announcement
#include "goblin_i18n.hpp"
#include "modutils.hpp"
#include "goblin_map_data.hpp"
#include "goblin_item_icons.hpp"
#include "goblin_location_alt.hpp"
#include "goblin_gfx_probe.hpp"
#include <cstring>

#include "version.h" // BUILD_NAME: the starting anchor
#include "goblin_maphover.hpp"   // map_layer() for the native reticle-hover proxy
#include "goblin_mapproject.hpp" // read_view/to_map for the native reticle-hover proxy
#include "goblin_overlay.hpp"
#include "goblin_progress.hpp"   // region_place_id (tag each CategoryRow for focus)
#include "goblin_search.hpp"     // replace_picks/forget_picks: a restored or dropped pick focus
                                 // goes through the search's own set, never around it
#include "goblin/goblin_map_flags.hpp"
#include "from/params.hpp"
#include "from/paramdef/WORLD_MAP_POINT_PARAM_ST.hpp"

#include <cmath>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <optional>
#include <set>
#include <sstream>
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
using StateShow = goblin::generated::StateShow;

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
    int baked_g2[8];         // textEnableFlag2Id1..8 as baked (MapEntry::data). The post-event gate
                             // (goblin_logic.cpp) and a focus restore THESE, never the live row's.
    uint32_t state_flag;     // MapEntry::state_flag: world-state rule, 0 = none
    StateShow state_show;    // MapEntry::state_show
    int32_t region_id;       // progress region PlaceName id (goblin::progress::region_place_id) for focus
    int32_t baked_text1;     // textId1 as baked (restored when focus removes a fabricated label)
    bool baked_notext;       // isEnableNoText as baked (restored after focus force-show)
    bool focus_text;         // true while focus fabricated a label on a textless row
    bool picked;             // member of the search-pick focus set (set_focus_rows)
    uint64_t hide_key;       // stable manual-hide key (stable_hide_key of the baked entry)
    uint64_t hide_key_v2;    // the same marker's pre-2.1.4 key, to migrate a v2 file once
    uint8_t native_area;
    uint8_t native_layer;
    uint16_t native_gx;
    uint16_t native_gz;
    // Where this marker is DRAWN, and where it actually is. They differ when it shares a spot with
    // others: see the de-overlap below, which recomputes native_px/native_pz from anchor_px/anchor_pz for
    // the markers that are visible RIGHT NOW. Everything that positions anything - the icon factory's
    // snapshot, the hover pick, the highlight rings - reads native_px, so the spread reaches all of
    // them without any of them knowing about it.
    float native_px;
    float native_pz;
    // Pre-de-overlap display anchor (MapEntry::display_pos, or the row's own position for the
    // game's rows). Never the tracking position.
    float anchor_px;
    float anchor_pz;
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

// V3 rollout is deliberately category-by-category.  A migrated category keeps
// its WorldMapPointParam data for visibility/tooltip metadata, but the game must
// not build the heavyweight WorldMapItem for it; goblin_stall_probe creates the
// lightweight bitmap child instead.  Keep this debug-gated until parity is
// proven for every behaviour the stock widget supplied.
static bool native_category_migrated(Category cat)
{
    // Stage 4 final: EVERY injected category runs native, including the World
    // classes - defeat state renders via the CLEARED_ICON_ID twin child (the
    // native path's replacement for the stock completion overlay). Our grace
    // markers are ordinary icons too: the clickable/travel graces are the
    // GAME'S OWN pins, which we never touch.
    (void)cat;
    // Compile-time now, not an ini key: one marker mechanism per binary (see goblin_build_variants.hpp).
    return goblin::variants::kNativeMarkers;
}
// Progress-tab focus: g_focus_category = -1 (none) or a Category value; paired
// with g_focus_region (a region PlaceName id, or -1 for the "Other" bucket).
// When active, the live map shows ONLY that category's uncollected markers IN
// that region, swapped to the glow-highlight icon (see apply_category_visibility
// + apply_focus_highlight).
static int g_focus_category = -1;
static int32_t g_focus_region = -1;
// The second focus shape: an explicit pick set (the item search). While on, the per-row
// `picked` flag replaces the (category, region) test everywhere the focus is consulted.
static std::atomic<bool> g_focus_pick{false};
static std::atomic<size_t> g_focus_pick_count{0};
// Marker labels changed (live-loot relabel / settings re-apply): the search index rebuilds.
static std::atomic<uint32_t> g_label_epoch{1};
// The focus persists per character in MapForGoblins_focus_s<slot>.txt (same folder and slot
// tracking as the manual hides): a category focus as its config key + region, a pick focus as
// the stable hide keys of the picked rows. Written on every focus change, read on a slot
// switch and applied once the injected rows exist (restore_focus_pending).
static std::filesystem::path g_focus_file;
static bool g_focus_restore_pending = false;
static int g_pending_focus_cat = -1;
static int32_t g_pending_focus_region = -1;
static std::set<uint64_t> g_pending_focus_picks;
static void persist_focus();

static inline bool focus_active() { return g_focus_pick.load() || g_focus_category >= 0; }
// ONE definition of "this row is in the active focus", for both shapes. `focus` is the
// caller's snapshot of g_focus_category (kept so the loops read it once).
static inline bool row_in_focus(const CategoryRow &cr, int focus)
{
    if (g_focus_pick.load(std::memory_order_relaxed)) return cr.picked;
    return focus >= 0 && static_cast<int>(cr.cat) == focus && cr.region_id == g_focus_region;
}

// ---- Manual per-marker hide (hover + hotkey; managed in the overlay) --------
// A user can hide an individual marker by hovering it on the map and pressing the
// hide key. The hidden set persists across sessions keyed by a STABLE hash of what the
// marker IS, never by the dynamic row id: the MSB object it tracks, else the item lot it
// announces, else the flag that clears it, else (springs, stakes) its tile + position.
// apply_category_visibility() ANDs this in, so a hidden marker's icon disappears live and
// stays hidden on reload. Unhide/clear via the overlay.
// `seq` orders the set by WHEN each marker was hidden: the Hidden list shows the newest first,
// so the one just removed is at the top instead of somewhere in key order, and the file is
// written oldest-first so the order survives a restart (the loader numbers lines as they come).
struct HiddenMeta { int32_t textId; uint16_t iconId; int32_t region; uint8_t cat; uint32_t seq = 0; };
static uint32_t g_hidden_seq = 0;                 // last seq handed out; under g_manual_hidden_mtx
static uint32_t next_hidden_seq() { return ++g_hidden_seq; }
static std::map<uint64_t, HiddenMeta> g_manual_hidden;
static std::map<uint64_t, HiddenMeta> g_hidden_v1_pending;  // legacy keys read from a v1 file, migrated once rows exist
static std::map<uint64_t, HiddenMeta> g_hidden_v2_pending;  // same, for a v2 file (pre-2.1.4 key)
static std::mutex g_manual_hidden_mtx;
static std::filesystem::path g_hidden_dir;   // folder holding the per-slot hide files
static std::filesystem::path g_hidden_file;  // current slot's file (persist target)
static int g_hidden_slot = -2;               // slot the loaded set belongs to (-2 = none synced yet)

// The v1 key (files without a header). Baked position + textId1 + iconId of the LIVE row:
// every one of those moves between builds - textIds are remapped per FMG expansion, icon ids
// change with icon patches, and a marker's position changes whenever its source data does - so a
// hidden marker came back after an update and re-hiding it left the stale twin in the file. Kept
// only to migrate such files (migrate_hidden_v1).
static uint64_t marker_key_v1(const from::paramdef::WORLD_MAP_POINT_PARAM_ST *p)
{
    if (!p) return 0;
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

// The v2 key: kept ONLY to migrate files written by 2.1.3 and earlier (see hide_key_v3 below
// for why it had to change). Never write this one.
static uint64_t hide_key_v2(const goblin::generated::MapEntry &e)
{
    uint64_t h = 1469598103934665603ull;  // FNV-1a
    auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    auto mix_tile = [&]() {
        mix(static_cast<uint64_t>(e.data.areaNo));
        mix(static_cast<uint64_t>(e.data.gridXNo));
        mix(static_cast<uint64_t>(e.data.gridZNo));
    };
    auto mix_pos = [&](float scale) {
        mix(static_cast<uint64_t>(static_cast<int64_t>(std::llround(e.real_posX * scale))));
        mix(static_cast<uint64_t>(static_cast<int64_t>(std::llround(e.real_posZ * scale))));
    };
    mix(2);  // key version
    if (e.object_name && e.object_name[0])
    {
        mix('o');
        mix_tile();
        for (const char *c = e.object_name; *c; ++c) mix(static_cast<uint8_t>(*c));
    }
    else if (e.lotId != 0 && e.lotType != 0)
    {
        mix('l');
        mix(e.lotType);
        mix(e.lotId);
        if (e.lotType == 2) { mix_tile(); mix_pos(1.0f); }
    }
    else if (e.data.clearedEventFlagId != 0)
    {
        mix('c');
        mix(e.data.clearedEventFlagId);
    }
    else if (e.data.textDisableFlagId1 != 0)
    {
        mix('d');
        mix(e.data.textDisableFlagId1);
    }
    else
    {
        mix('p');
        mix_tile();
        mix(static_cast<uint64_t>(e.category));
        mix_pos(2.0f);
    }
    return h;
}

// The v3 key, from the BAKED entry (not the live row), by what identifies the marker across
// builds and data updates. An object or lot that ERR moves keeps its key, so it stays hidden.
//   1. MSB object (pieces, material nodes, kindling): tile + part name + category + position.
//      The position is here because a part name is NOT unique in a tile: ERR has two
//      AEG099_821_9000 rune pieces in 60/43/52 that are 258 units apart, and without it they
//      were one identity - picking one in the search and reloading selected both. Twins at the
//      SAME spot still share the key, which is right: the engine fuses them too (GEOF).
//   2. Item lot: type + lot id + category + tile + position at 1u.
//   3. The flag that hides it: clearedEventFlagId (bosses, hostile NPCs, hawks), else
//      textDisableFlagId1 (graces, pools, statues, maps, paintings, gestures, great runes),
//      again with category + tile + position.
//   4. Tile + category + real position at 0.5u (spirit springs, stakes of Marika only).
//
// WHY v3: v2 keyed a map lot on type + lot id alone, so every marker fed by one lot was one
// identity - hiding Loretta's War Sickle also hid Loretta's Mastery, and restoring a saved
// pick focus on one of them selected both. Same for the two flag branches, where a boss's
// several reward categories all carry its kill flag. Counted over the baked rows of all ten
// profiles, v2 left 32 to 80 colliding keys each (77 to 181 rows); v3 leaves 4 to 18 (8 to
// 36 rows), and what remains is genuinely co-located same-category rows of one source.
// Every branch takes the position (real_pos, the pre-de-overlap source position), so the key
// follows the object rather than where the icon ended up. It does mean a data update that
// MOVES a source moves its key and un-hides that one marker; that is the same trade the lot
// and flag branches already made, and it is the only thing that separates same-name twins.
static uint64_t stable_hide_key(const goblin::generated::MapEntry &e)
{
    uint64_t h = 1469598103934665603ull;  // FNV-1a
    auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    auto mix_tile = [&]() {
        mix(static_cast<uint64_t>(e.data.areaNo));
        mix(static_cast<uint64_t>(e.data.gridXNo));
        mix(static_cast<uint64_t>(e.data.gridZNo));
    };
    auto mix_pos = [&](float scale) {
        mix(static_cast<uint64_t>(static_cast<int64_t>(std::llround(e.real_posX * scale))));
        mix(static_cast<uint64_t>(static_cast<int64_t>(std::llround(e.real_posZ * scale))));
    };
    mix(3);  // key version
    mix(static_cast<uint64_t>(e.category));
    if (e.object_name && e.object_name[0])
    {
        mix('o');
        mix_tile();
        for (const char *c = e.object_name; *c; ++c) mix(static_cast<uint8_t>(*c));
        mix_pos(1.0f);
    }
    else if (e.lotId != 0 && e.lotType != 0)
    {
        mix('l');
        mix(e.lotType);
        mix(e.lotId);
        mix_tile();
        mix_pos(1.0f);
    }
    else if (e.data.clearedEventFlagId != 0)
    {
        mix('c');
        mix(e.data.clearedEventFlagId);
        mix_tile();
        mix_pos(1.0f);
    }
    else if (e.data.textDisableFlagId1 != 0)
    {
        mix('d');
        mix(e.data.textDisableFlagId1);
        mix_tile();
        mix_pos(1.0f);
    }
    else
    {
        mix('p');
        mix_tile();
        mix_pos(2.0f);
    }
    return h;
}

static bool is_manually_hidden(const CategoryRow &cr)
{
    if (!cr.p) return false;
    std::lock_guard<std::mutex> lk(g_manual_hidden_mtx);
    return g_manual_hidden.find(cr.hide_key) != g_manual_hidden.end();
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
// group-1 (both must hold). We set it for the switched-chest pair (baked, e.g. Patches'
// Glass Shard vs Cloth on flag 3691: the absent variant's gate is off) and, with
// require_map_fragments, for post-story-event areas (runtime, apply_map_logic, which puts its
// flag only in the slots the bake left free). ANY slot counts here, populated text line or not:
// for our markers this test is the renderer, so a row carrying both gates needs both flags.
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

// Does the world currently stand in the state this marker belongs to? (MapEntry::state_flag:
// Leyndell's capitals, flag 300 ON = Ashen / OFF = Royal; a spot found in one version of a
// world-state tile, flags 300 / 310 / 330 - goblin_world_variants.hpp.) Read live, so the right
// markers are there on the next map open after the switch, whatever else has been applied.
// DELIBERATELY not part of row_is_hidden: that one means "done" to the Progress page, the search
// and the focus prune, and a marker whose state is away is not done - it is waiting (or, for an
// OnlyWhile* rule, not in the world; the Progress page leaves those out itself).
static bool row_state_open(const CategoryRow &cr)
{
    if (cr.state_show == StateShow::Always || cr.state_flag == 0) return true;
    return goblin::generated::state_rule_open(cr.state_show, goblin::flag_is_set(cr.state_flag));
}

// Single "is this marker's icon currently hidden?" test, shared by the focus-highlight
// rings (skip hidden) and the region-progress count (a hidden marker counts as done):
// collected (GEOF), kindling-collected, manually hidden, or a live hide/cleared flag set.
static bool row_is_hidden(const CategoryRow &cr)
{
    return goblin::collected::is_row_collected(cr.row_id) ||
           goblin::kindling::is_row_collected(cr.row_id) ||
           is_manually_hidden(cr) ||
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
    // MapEntry::lotAggregate: this marker is one CATEGORY of a multi-item award, so the lot
    // does not address its item. Live labels and live hide-flags skip it (slot 1 of that lot
    // is some other category's item, and the award lands on the boss's kill flag, which is
    // what the baked flags already say). The spoiler-free icon and label still apply.
    bool aggregate;
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
        // find(), not operator[]: a missing lot is the expected answer under a randomizer (196
        // of them on one seed), and operator[] logged every miss as an error before throwing -
        // 530 error lines per launch that said nothing new.
        if (pref) return pref->find(lot_id);
        if (other) return other->find(lot_id);
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
        // Weapons AND ammo: +100M, the rule the baked textId uses. Keeping ids >= 50M raw as
        // "ammo" also caught every DLC weapon (60M-68M), so encode(live) never equalled the
        // baked textId for those and the live-loot path relabelled unchanged items; the icon
        // table (generate_loot_massedit._encode_item) now uses this same rule.
        case 2: return item_id + 100000000;                                       // weapon / ammo
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
    case Category::KeyReveredSpiritAshes: return goblin::config::showReveredSpiritAshes;
    case Category::KeySpectralSteedRegalia: return goblin::config::showSpectralSteedRegalia;
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
    case Category::WorldStrongEnemies:   return goblin::config::showStrongEnemies;
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
    case C::KeyReveredSpiritAshes: return "show_revered_spirit_ashes";
    case C::KeySpectralSteedRegalia: return "show_spectral_steed_regalia";
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
    case C::WorldStrongEnemies: return "show_strong_enemies";
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
    // A category focus and a pick focus are exclusive: setting (or clearing) one drops the other.
    if (g_focus_pick.exchange(false))
    {
        for (auto &cr : g_category_rows) cr.picked = false;
        g_focus_pick_count.store(0);
    }
    spdlog::info("[focus] set category={} region={}", g_focus_category, g_focus_region);
    persist_focus();
}

int goblin::focus_category() { return g_focus_category; }
int32_t goblin::focus_region() { return g_focus_region; }

void goblin::set_focus_rows(const std::vector<uint64_t> &original_row_ids)
{
    g_focus_category = -1;
    g_focus_region = -1;
    std::unordered_set<uint64_t> want(original_row_ids.begin(), original_row_ids.end());
    size_t n = 0;
    for (auto &cr : g_category_rows)
    {
        cr.picked = cr.original_row_id != 0 && want.count(cr.original_row_id) != 0;
        if (cr.picked) ++n;
    }
    g_focus_pick_count.store(n);
    g_focus_pick.store(n > 0);
    spdlog::info("[focus] set picks: {} requested, {} matched", original_row_ids.size(), n);
    persist_focus();
}

bool goblin::focus_rows_active() { return g_focus_pick.load(); }
size_t goblin::focus_rows_count() { return g_focus_pick_count.load(); }

// "#mfg-hidden v3" -> 3, for an EXACT header line. -1 = not a header of this kind, so the
// caller can tell "no header at all" (that is v1) from "a header this build does not know".
// Exact on purpose: a prefix test read "#mfg-hidden v30" as v3 and a future "v4" as no header
// at all, which fed a v4 file's keys to the v1 migration and then saved the wreckage back
// over it.
static int header_version(const std::string &line, const char *prefix)
{
    const size_t n = std::strlen(prefix);
    if (line.size() <= n || line.compare(0, n, prefix) != 0) return -1;
    size_t i = n;
    int v = 0;
    while (i < line.size() && line[i] >= '0' && line[i] <= '9')
    {
        if (v < 1000000) v = v * 10 + (line[i] - '0');  // saturate; still >> any real version
        ++i;
    }
    if (i == n) return -1;  // the prefix, then no digits
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
    return i == line.size() ? v : -1;  // trailing junk: not a header we wrote
}

// ---- Focus persistence -------------------------------------------------------
// File: `#mfg-focus v3`, then either one `category <config_key> <region>` line or one
// `pick <stable_hide_key>` line per picked row. Keys are the same stable identities the manual
// hides use, so a build or data update (or an object ERR moved) keeps the focus intact.
// A v2 file is read the same way but matched against the row's OLD key; the next save writes
// v3. Where an old key covered several markers the restore selects all of them - exactly what
// that file already meant, since the old key could not tell them apart.
static constexpr const char *kFocusHeaderPrefix = "#mfg-focus v";
static constexpr const char *kFocusHeaderV3 = "#mfg-focus v3";
static constexpr int kFocusVersion = 3;
static bool g_pending_focus_picks_are_v2 = false;
// A focus file written by a NEWER build: we neither read it nor write over it, so switching
// back to that build finds it intact.
static bool g_focus_file_unreadable = false;

static void persist_focus()
{
    if (g_focus_file.empty() || g_focus_file_unreadable) return;
    try
    {
        std::ofstream f(g_focus_file, std::ios::trunc);
        f << kFocusHeaderV3 << '\n';
        if (g_focus_pick.load())
        {
            for (const auto &cr : g_category_rows)
                if (cr.picked && cr.hide_key) f << "pick " << cr.hide_key << '\n';
        }
        else if (g_focus_category >= 0)
        {
            const char *key = goblin::category_config_key(static_cast<goblin::generated::Category>(g_focus_category));
            if (key) f << "category " << key << ' ' << g_focus_region << '\n';
        }
    }
    catch (...) {}
}

static void load_focus_file(const std::filesystem::path &path)
{
    g_focus_restore_pending = false;
    g_pending_focus_cat = -1;
    g_pending_focus_region = -1;
    g_pending_focus_picks.clear();
    g_pending_focus_picks_are_v2 = false;
    g_focus_file_unreadable = false;
    try
    {
        std::ifstream f(path);
        std::string line;
        if (!std::getline(f, line)) return;
        const int fv = header_version(line, kFocusHeaderPrefix);
        if (fv > kFocusVersion)
        {
            spdlog::warn("[focus] {} was written by a newer build (v{}); leaving it alone",
                         path.filename().string(), fv);
            g_focus_file_unreadable = true;
            return;
        }
        if (fv == 3) g_pending_focus_picks_are_v2 = false;
        else if (fv == 2) g_pending_focus_picks_are_v2 = true;
        else return;  // no header, or a version that never existed: not ours to read
        while (std::getline(f, line))
        {
            std::istringstream ss(line);
            std::string kind;
            if (!(ss >> kind)) continue;
            if (kind == "pick")
            {
                uint64_t k = 0;
                if (ss >> k) g_pending_focus_picks.insert(k);
            }
            else if (kind == "category")
            {
                std::string ckey;
                long region = -1;
                if (!(ss >> ckey >> region)) continue;
                for (int c = 0; c < goblin::progress::kCategoryCount; ++c)
                {
                    const char *k = goblin::category_config_key(static_cast<goblin::generated::Category>(c));
                    if (k && ckey == k) { g_pending_focus_cat = c; break; }
                }
                g_pending_focus_region = static_cast<int32_t>(region);
            }
        }
    }
    catch (...) {}
    g_focus_restore_pending = g_pending_focus_cat >= 0 || !g_pending_focus_picks.empty();
}

// Apply a loaded focus once the injected rows exist. Returns true when it applied (the
// caller reapplies visibility); the pick shape goes through the search module so its own pick
// set (the ticks in the search list) matches the map.
static bool restore_focus_pending()
{
    if (!g_focus_restore_pending) return false;
    if (g_category_rows.empty()) return false;  // rows not injected yet; next poll
    g_focus_restore_pending = false;
    if (!g_pending_focus_picks.empty())
    {
        std::vector<uint64_t> ids;
        for (const auto &cr : g_category_rows)
            if (cr.original_row_id &&
                g_pending_focus_picks.count(g_pending_focus_picks_are_v2 ? cr.hide_key_v2
                                                                        : cr.hide_key))
                ids.push_back(cr.original_row_id);
        spdlog::info("[focus] restored pick focus: {} saved key(s), {} matched a marker",
                     g_pending_focus_picks.size(), ids.size());
        g_pending_focus_picks.clear();
        g_pending_focus_picks_are_v2 = false;
        // Nothing matched: leave the FILE alone. This used to persist here, which rewrote the
        // character's focus as a bare header the moment its keys failed to resolve - a v2 file
        // on a v3 build, or a source that moved in a data update - destroying a selection a
        // later build could still have read. The next focus the player sets overwrites it anyway.
        if (ids.empty()) return false;
        goblin::search::replace_picks(ids);  // set_focus_rows + reapply + highlight
        return true;
    }
    spdlog::info("[focus] restored category focus: category={} region={}", g_pending_focus_cat,
                 g_pending_focus_region);
    goblin::set_focus_category(g_pending_focus_cat, g_pending_focus_region);
    goblin::reapply_live_settings();
    goblin::apply_focus_highlight();
    return true;
}

std::vector<goblin::SearchRow> goblin::search_row_snapshot()
{
    std::vector<SearchRow> out;
    out.reserve(g_category_rows.size());
    for (const auto &cr : g_category_rows)
    {
        if (!cr.p || cr.original_row_id == 0) continue;
        out.push_back(SearchRow{cr.original_row_id, cr.p->textId1, cr.region_id,
                                static_cast<uint8_t>(cr.cat)});
    }
    return out;
}

uint32_t goblin::label_epoch() { return g_label_epoch.load(std::memory_order_acquire); }

// Focus label pass. The on-map highlight itself is a NATIVE pooled child - a marker child
// carrying HIGHLIGHT_ICON_ID, emitted from native_marker_snapshot (NATIVE_RING_POOL, see the
// pool block further down this file). The overlay draws no ring and no map geometry at all
// since 2026-07-28; goblin::mapproject has no overlay consumer left either. This pass only
// keeps the focused markers' on-map ICONS present so a ring has something under it. For the focused
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
        const bool focused = row_in_focus(cr, focus);
        const bool shown = focused && !collected::is_row_collected(cr.row_id) &&
                           !kindling::is_row_collected(cr.row_id);

        if (shown)
        {
            ++n_shown;
            // Focus IGNORES require_map_fragments: force this marker visible so its
            // highlight appears (and focus doesn't self-cancel) even in an undiscovered or
            // post-event area, where apply_map_logic gated it - group-1 via eventFlagId, and
            // group-2 via a STORY flag (the post-event gate: Stone Platform, Shadow Keep,
            // Enir-Ilim, ...). We lift ONLY those discovery gates: group-2 goes back to the BAKED
            // values, so a switched-chest gate stays (a genuinely-absent variant isn't spuriously
            // highlighted). The world-state rule (Leyndell's two capitals) is lifted the same way,
            // but in native_row_visible - it is not in the row at all.
            // No explicit undo here: apply_map_logic re-derives every gate on each reapply, and
            // every path that ENDS a focus runs one (the menus, the search, the hide hotkey's
            // prune, and on the watcher the prune and a slot switch that drops a focus - see
            // safe_reapply_live_settings_seh in dllmain.cpp). A new path that ends a focus with only
            // apply_category_visibility would leave these rows forced visible.
            cr.p->eventFlagId = static_cast<decltype(cr.p->eventFlagId)>(goblin::flag::AlwaysOn);
            int *g2[8] = {&cr.p->textEnableFlag2Id1, &cr.p->textEnableFlag2Id2,
                          &cr.p->textEnableFlag2Id3, &cr.p->textEnableFlag2Id4,
                          &cr.p->textEnableFlag2Id5, &cr.p->textEnableFlag2Id6,
                          &cr.p->textEnableFlag2Id7, &cr.p->textEnableFlag2Id8};
            for (int k = 0; k < 8; ++k) *g2[k] = cr.baked_g2[k];
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
    if (focus_active())
        spdlog::info("[focus] apply: cat={} region={} picks={} shown={} forced={} (rows={})",
                     focus, g_focus_region, g_focus_pick_count.load(), n_shown, n_forced,
                     g_category_rows.size());
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

static void refresh_deoverlap(int layer);  // defined with the de-overlap, further down
static void build_state_flags();           // defined with the world-state poll, further down
static std::vector<uint8_t> g_vis;   // per row index; 1 = on screen right now
static int g_vis_layer = -1;         // which layer that answer was for (-1 = none yet)
// ONE lock over the layout state: g_vis, g_vis_layer, the de-overlap's occupancy grid and the
// native_px/native_pz the refresh writes. The refresh runs on the map thread (the marker manager's
// merges and the per-frame hover pick) AND on the manual-hide hotkey thread (Delete -> the overlay's
// native_hover_row -> native_reticle_row -> a snapshot, whenever its 200 ms copy is stale). The hover
// pick's own cache lock covers only that path; the merges never took it, so two refreshes could run
// at once - one reassigning g_vis while the other indexed it, both growing the same cell vectors.
// Held by every public reader below and across the whole snapshot; released before anything that
// could re-enter (nothing under it calls back into this file's public entry points).
static std::mutex g_layout_mtx;
static std::vector<goblin::HighlightPoint> focus_highlight_points_locked();

bool goblin::display_position(const void *rowptr, float &px, float &pz)
{
    std::lock_guard<std::mutex> lk(g_layout_mtx);
    for (const auto &cr : g_category_rows)
    {
        if (static_cast<const void *>(cr.p) != rowptr) continue;
        px = cr.native_px;
        pz = cr.native_pz;
        return true;
    }
    return false;
}

std::vector<goblin::HighlightPoint> goblin::focus_highlight_points()
{
    std::lock_guard<std::mutex> lk(g_layout_mtx);
    return focus_highlight_points_locked();
}

static std::vector<goblin::HighlightPoint> focus_highlight_points_locked()
{
    using goblin::HighlightPoint;
    std::vector<HighlightPoint> out;
    const int focus = g_focus_category;
    if (!focus_active()) return out;
    // A ring has to land ON its icon, so it reads the display positions the icons were drawn at. This
    // is also called from INSIDE the snapshot, which has just refreshed them for this layer - so it
    // refreshes only when nothing has answered for this layer yet, instead of repeating the pass.
    {
        const int layer = goblin::maphover::map_layer();
        if (layer >= 0 && (g_vis_layer != layer || g_vis.size() != g_category_rows.size()))
            refresh_deoverlap(layer);
    }
    for (size_t idx = 0; idx < g_category_rows.size(); ++idx)
    {
        auto &cr = g_category_rows[idx];
        if (!cr.p) continue;
        if (!row_in_focus(cr, focus)) continue;
        // A ring exists for exactly the icons being DRAWN, and reads the same answer they did: this is
        // the visibility the refresh above computed, not a second nearly-identical test. The old test
        // here missed the eventFlagId gate, so a marker the engine does not draw could still be ringed
        // - which looks the same as a ring that missed its marker.
        if (idx >= g_vis.size() || !g_vis[idx]) continue;
        // Coordinates/layer come from the CAPTURED native_* fields, NOT from the
        // live row: migrated rows run with dispMask zeroed (stock widget
        // suppression), so a live read derives layer "none" and the overlay's
        // per-layer filter drops every ring (the v2.0.6 focus-ring regression).
        HighlightPoint hp{};
        hp.area = cr.native_area;
        hp.layer = cr.native_layer;
        hp.gx = cr.native_gx;
        hp.gz = cr.native_gz;
        hp.px = cr.native_px;
        hp.pz = cr.native_pz;
        if (hp.area == 99) continue; // parked/hidden coordinate trick
        out.push_back(hp);
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

// A goblin::is_row_ptr_hidden(void*) stood here - true only if a row is MANUALLY hidden. Ten lines
// of rationale described the consumer in the present tense: the hover tooltip was to call it so a
// just-hidden pin, which the engine keeps reporting as hovered until the cursor moves, stops being
// described. That consumer does not exist - the live tooltip (maphover::drive_own_tip) asks
// hovered_marker() and makes no hidden test at all. The distinction the rationale drew is still
// TRUE and worth keeping if the check is ever wired up: it must NOT use the broader row_is_hidden,
// because a killed boss / done NPC / hawk sets clearedEventFlagId while its icon is still drawn
// (hideKilledBosses defaults off), and suppressing those tooltips was a real regression.

bool goblin::prune_focus_if_empty()
{
    if (!focus_active()) return false;
    // Layer-independent on purpose. This used to ask focus_highlight_points(), which answers
    // for the layer being LOOKED AT (the rings are drawn there): switching to a map layer that
    // holds none of the focused markers returned "empty" and the focus was dropped as if
    // everything had been collected. What "empty" means is that no focused marker is left
    // that could be shown anywhere - collected, kindling-collected, manually hidden, or hidden
    // by its live flag.
    const int focus = g_focus_category;
    for (const auto &cr : g_category_rows)
        if (cr.p && row_in_focus(cr, focus) && !row_is_hidden(cr))
            return false;  // still something to show (on some layer)
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
        bool lotAggregate; // the lot backs several markers; not this one's address
        uint64_t hide_key; // stable manual-hide key (stable_hide_key of the baked entry)
        uint64_t hide_key_v2; // the same marker's pre-2.1.4 key (migration only)
        uint32_t state_flag;  // world-state rule (MapEntry::state_flag / state_show)
        StateShow state_show;
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

    // EVERY category's rows are injected; per-category visibility is a live text-flag gate
    // applied later (goblin::apply_category_visibility), not a filter here. See the block at
    // the bottom of this loop. (Until 2026-07-30 this said the opposite and was paired with a
    // `skipped_by_config` counter that nothing ever incremented, so the startup log reported
    // "0 skipped by config" as though the filter had run and found nothing to drop.)
    std::vector<InjectedEntry> entries;
    entries.reserve(generated::MAP_ENTRY_COUNT);
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
        else if (goblin::config::liveLootIcons && is_lot && !e.lotAggregate && lot_reader.ok())
        {
            // Not for an aggregate: slot 1 of that lot is whichever category came first, so
            // re-gating this marker by it would move, say, the armour marker under Armaments.
            if (RawItemLotRow *r = lot_reader.row(lotId, lotType))
            {
                int32_t item_id = *reinterpret_cast<int32_t *>(r->b + 0x00);   // lotItemId01
                int32_t cat     = *reinterpret_cast<int32_t *>(r->b + 0x20);   // lotItemCategory01
                // Only a lot that now gives a DIFFERENT item (a randomizer) is re-gated. The icon
                // table holds one category per ITEM, so re-gating an unchanged item folded the
                // source-split categories back together: every merchant's Bell Bearing went under
                // show_bell_bearings with the plain bell icon, and show_merchant_bell_bearings did
                // nothing. The baked textId1 is that same item key for a lot-backed row.
                if (item_id > 0 && encode_live_item(item_id, cat) != e.data.textId1)
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
        entries.push_back({0, e.row_id, &e.data, is_piece, is_kindling, gate_cat, lotId, lotType,
                           e.lotAggregate != 0, stable_hide_key(e), hide_key_v2(e),
                           e.state_flag, e.state_show});
    }

    spdlog::info("Adding {} map entries ({} live-recategorized, live-loot table ready={})",
                 entries.size(), live_recat, lot_reader.ok());

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
    // exact bug once crashed a TutorialParam save-load in an earlier build.)
    size_t wrapper_row_loc_start = (after_type_str + 0xf) & ~(size_t)0xf;
    size_t wrapper_row_loc_end = wrapper_row_loc_start + total_rows * WRAPPER_ROW_LOC_SIZE;
    size_t param_file_size = wrapper_row_loc_end;
    size_t total_alloc = WRAPPER_HEADER + param_file_size;

    // The GAME's _aligned_malloc, not HeapAlloc. This buffer becomes FD4ParamResCap's +0x80 (the
    // param file bytes, stored as allocation + WRAPPER_HEADER), and ~FD4ParamResCap frees it:
    //   [obj+0x80] - 0x10 -> DL free -> the DL range table finds no owner -> the fallback
    //   DLKRD::HeapAllocator<Win32RuntimeHeapImpl>::Free -> _aligned_free -> _free_base.
    // _aligned_free does not free the pointer it is handed; it frees the back-pointer that
    // _aligned_malloc stored at (p & ~7) - 8. A HeapAlloc block has no such back-pointer, so the
    // engine read the encoded _HEAP_ENTRY sitting in front of our block and passed THAT to
    // RtlFreeHeap, which rejected it for not being 16-byte aligned and terminated the process.
    // That is the 0xC0000374 recorded at every shutdown once the map had been used.
    //
    // Found by scanning all 239 FD4ParamResCap instances in the full-memory dump: 237 carry a
    // DL-arena buffer, and exactly 2 carry a process-heap one - WorldMapPointParam and
    // TutorialParam, i.e. precisely the two params we expand. They are fingerprinted apart from the
    // engine's own by the wrapper header: an engine block satisfies u32[alloc] == [obj+0x78], ours
    // satisfies u32[alloc] + u32[alloc+4]*8 == [obj+0x78].
    //
    // Keep replacing the pointer and keep the +0x10 wrapper - the engine tolerates a foreign buffer
    // perfectly well, it just frees it its own way. Only the allocator was wrong.
    //
    // 2026-08-12 (reports 25/27/28/37): under a mod host the fallback free is SUBSTITUTED, and the
    // host's free faults on a pointer it never issued (me3_mod_host AV / ME2 0xC0000374). First
    // choice is therefore the arena that owns the ORIGINAL param buffer - then the DL range lookup
    // succeeds and the substituted fallback is never consulted. game_aligned_alloc stays as the
    // fallback (correct unhosted - the traced _aligned_free path).
    allocation = goblin::gfx_probe::dl_alloc_like(old_param_file, total_alloc);
    if (!allocation)
        allocation = goblin::gfx_probe::game_aligned_alloc(total_alloc);
    if (!allocation)
    {
        spdlog::error("alloc failed ({} bytes) - the game's aligned allocator is unavailable, so "
                      "the marker table is left untouched rather than handed over unfreeable",
                      total_alloc);
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
        bool lotAggregate;         // see InjectedEntry
        uint64_t hide_key;         // stable manual-hide key (0 for vanilla rows)
        uint64_t hide_key_v2;      // pre-2.1.4 key of the same marker (0 for vanilla rows)
        uint32_t state_flag;       // world-state rule (0 / Always for vanilla rows)
        StateShow state_show;
    };

    std::vector<RowSource> all_rows;
    all_rows.reserve(total_rows);

    for (uint16_t i = 0; i < orig_num_rows; i++)
    {
        auto *data = old_param_file + old_table->rows[i].param_offset;
        all_rows.push_back({static_cast<int32_t>(old_table->rows[i].row_id), data, false, false,
                            {}, 0, 0, 0, false, 0, 0, 0, StateShow::Always});
    }
    for (auto &entry : entries)
    {
        all_rows.push_back({entry.row_id, reinterpret_cast<const uint8_t *>(entry.data),
                            entry.is_piece, entry.is_kindling, entry.category, entry.original_row_id,
                            entry.lotId, entry.lotType, entry.lotAggregate,
                            entry.hide_key, entry.hide_key_v2, entry.state_flag, entry.state_show});
    }

    std::sort(all_rows.begin(), all_rows.end(),
              [](const RowSource &a, const RowSource &b) { return a.row_id < b.row_id; });

    auto *new_locators = reinterpret_cast<ParamRowInfo *>(new_param_file + row_locators_start);
    auto *new_wrapper_locs = reinterpret_cast<WrapperRowLocator *>(new_param_file + wrapper_row_loc_start);
    size_t file_end_marker = type_str_start + type_str_len;
    size_t native_suppressed[3]{};

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
            cr.native_area = wp->areaNo;
            cr.native_layer = wp->dispMask00 ? 0 : (wp->dispMask01 ? 1 :
                              (wp->dispMask02 ? 2 : 0xFF));
            cr.native_gx = wp->gridXNo;
            cr.native_gz = wp->gridZNo;
            // The row's own position IS the display anchor: nothing shifts it any more (the
            // generation-time de-overlap is gone), and the live de-overlap spreads from here.
            cr.anchor_px = wp->posX;
            cr.anchor_pz = wp->posZ;
            cr.native_px = cr.anchor_px;
            cr.native_pz = cr.anchor_pz;
            cr.hide_key = all_rows[i].hide_key;
            cr.hide_key_v2 = all_rows[i].hide_key_v2;
            cr.state_flag = all_rows[i].state_flag;
            cr.state_show = all_rows[i].state_show;
            // Group-2 straight from the MapEntry (data_ptr points INTO MAP_ENTRIES for our rows):
            // the baked values - the switched-chest gate where there is one - whatever the live row
            // is later made to carry.
            {
                const auto *baked =
                    reinterpret_cast<const from::paramdef::WORLD_MAP_POINT_PARAM_ST *>(all_rows[i].data_ptr);
                const int g2[8] = {baked->textEnableFlag2Id1, baked->textEnableFlag2Id2,
                                   baked->textEnableFlag2Id3, baked->textEnableFlag2Id4,
                                   baked->textEnableFlag2Id5, baked->textEnableFlag2Id6,
                                   baked->textEnableFlag2Id7, baked->textEnableFlag2Id8};
                for (int k = 0; k < 8; ++k) cr.baked_g2[k] = g2[k];
            }
            unsigned *en[8];
            enable_flag_ptrs(wp, en);
            for (int k = 0; k < 8; ++k) cr.baked_enable[k] = *en[k];
            g_category_rows.push_back(cr);
            if (native_category_migrated(cr.cat))
            {
                if (cr.native_layer < 3) ++native_suppressed[cr.native_layer];
                wp->dispMask00 = 0;
                wp->dispMask01 = 0;
                wp->dispMask02 = 0;
            }
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
            lb.aggregate = all_rows[i].lotAggregate;
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

        // Kill display mode (bosses / hawks / NPC invaders / strong enemies): green checkmark
        // vs hide killed. Without this, rows baked with BOTH clearedEventFlagId
        // and textDisableFlagId hide all their text on kill and the icon
        // vanishes before the checkmark can ever show.
        auto cat = all_rows[i].category;
        if (cat == Category::WorldBosses || cat == Category::WorldSpiritspringHawks ||
            cat == Category::WorldHostileNPC || cat == Category::WorldStrongEnemies)
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
    build_state_flags();  // the world-state rules' flags, now that every row is known
    if (goblin::config::debugLogging)
        spdlog::info("[v3native] item-category migration suppressed stock rows: "
                     "OW={} UG={} DLC={} total={}",
                     native_suppressed[0], native_suppressed[1], native_suppressed[2],
                     native_suppressed[0] + native_suppressed[1] + native_suppressed[2]);


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
    goblin::map_api::publish_rows();
}

// Native settings-menu text ids in GR_MenuText.fmg (see goblin_inject.hpp).
int goblin::g_menutext_tab_id = 0;
std::vector<int> goblin::g_menutext_row_ids;
int goblin::g_menutext_on_id = 0;
int goblin::g_menutext_off_id = 0;

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

// (A goblin::is_param_injection_active() accessor stood here. It had no callers -
//  menu_auto_toggle_loop reads g_param_injection_active directly, in this same file.)

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

// ---- World state: which flags the rules read, and what the watcher last saw them at ---------
// The distinct event flags the injected markers' world-state rules read (MapEntry::state_flag),
// with how many markers follow each and the value each had at the watcher's last poll
// (world_state_poll). Built once at injection (build_state_flags) and never resized after that;
// only the values change. The poll records a value the moment it REPORTS it, not once the re-apply
// it triggers has finished: a re-apply that faulted partway would otherwise leave the value behind
// and the poll reporting the same change - a re-apply, a merge and a log line - every tick for good.
static std::vector<uint32_t> g_state_flags;
static std::vector<size_t> g_state_counts;
static std::unique_ptr<std::atomic<uint8_t>[]> g_state_seen;  // 0 = never read, 1 = OFF, 2 = ON

static uint8_t state_now(uint32_t flag) { return goblin::flag_is_set(flag) ? 2 : 1; }

static void build_state_flags()
{
    std::map<uint32_t, size_t> seen;
    for (const auto &cr : g_category_rows)
        if (cr.state_show != StateShow::Always && cr.state_flag != 0) ++seen[cr.state_flag];
    g_state_flags.clear();
    g_state_counts.clear();
    for (const auto &[flag, n] : seen)
    {
        g_state_flags.push_back(flag);
        g_state_counts.push_back(n);
    }
    g_state_seen.reset(new std::atomic<uint8_t>[g_state_flags.size() ? g_state_flags.size() : 1]);
    for (size_t i = 0; i < g_state_flags.size(); ++i) g_state_seen[i].store(0);
}

size_t goblin::world_state_poll(WorldStateChange *out, size_t max_out)
{
    // No logging and nothing to destroy in here: the caller runs this under __try (it reads the
    // game's event flags, from the watcher, at the main menu and through loading screens too) and
    // says what changed outside it.
    size_t changed = 0;
    for (size_t i = 0; i < g_state_flags.size(); ++i)
    {
        const uint8_t now = state_now(g_state_flags[i]);
        const uint8_t was = g_state_seen[i].exchange(now, std::memory_order_relaxed);
        if (now == was) continue;
        if (out && changed < max_out)
            out[changed] = WorldStateChange{g_state_flags[i], now == 2, was != 0, g_state_counts[i]};
        ++changed;
    }
    return changed;
}

bool goblin::injected_row_baked_group2(int32_t row_id, int out[8])
{
    // g_category_rows is in row-id order: it is filled from all_rows after the sort by id.
    const auto it = std::lower_bound(
        g_category_rows.begin(), g_category_rows.end(), static_cast<uint64_t>(row_id),
        [](const CategoryRow &cr, uint64_t id) { return cr.row_id < id; });
    if (it == g_category_rows.end() || it->row_id != static_cast<uint64_t>(row_id)) return false;
    for (int k = 0; k < 8; ++k) out[k] = it->baked_g2[k];
    return true;
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
        const bool eligible = focus_active() ? row_in_focus(cr, focus)
                                             : is_category_enabled(cr.cat);
        bool show = eligible &&
                    !collected::is_row_collected(cr.row_id) &&
                    !kindling::is_row_collected(cr.row_id) &&
                    !is_manually_hidden(cr);  // user-hidden markers stay hidden
        // The engine-pin build draws from these flags, so the world state has to be written into
        // them there (native_row_visible decides it for the shipping build). Re-written when a rule's
        // flag flips: see world_state_poll. A focused marker is shown past it, as in native.
        if constexpr (goblin::variants::kLegacyPinMarkers)
            if (show && !focus_active() && !row_state_open(cr)) show = false;
        unsigned *en[8];
        enable_flag_ptrs(cr.p, en);
        for (int k = 0; k < 8; ++k)
            *en[k] = show ? cr.baked_enable[k]
                          : static_cast<unsigned>(goblin::flag::AlwaysOff);
    }
}

// Native bitmap markers do not render the stock green completion checkmark.
// A set clearedEventFlagId therefore does not hide the image when
// hideKilledBosses is off; apply_kill_display moves the same defeat condition
// into textDisableFlagId1 when the user explicitly wants killed markers hidden.
static bool native_row_hidden(const CategoryRow &cr)
{
    if (!cr.p || goblin::collected::is_row_collected(cr.row_id) ||
        goblin::kindling::is_row_collected(cr.row_id) || is_manually_hidden(cr))
        return true;
    const unsigned fl[8] = {cr.p->textDisableFlagId1, cr.p->textDisableFlagId2,
                            cr.p->textDisableFlagId3, cr.p->textDisableFlagId4,
                            cr.p->textDisableFlagId5, cr.p->textDisableFlagId6,
                            cr.p->textDisableFlagId7, cr.p->textDisableFlagId8};
    for (unsigned f : fl)
        if (f != 0 && goblin::flag_is_set(f)) return true;
    return false;
}

// ══ de-overlap, live ═════════════════════════════════════════════════════════════════════════════
// Markers that sit on top of each other are pulled apart onto a square spiral. That has always been
// done, but OFFLINE, once, for all ~9200 markers - so a collected pickup, a category the player turned
// off and a focus filter all kept reserving their spot and pushing the neighbour aside, and a marker
// left alone in its cluster still stood where the crowd had put it.
//
// The native render path made the live version possible: the icon is drawn from native_px/native_pz
// rather than from the param row, and this file already decides, per map open, which markers are
// visible. So the spread is recomputed from the visible set instead of being baked into the data.
//
// Spacing stays in WORLD units, as the offline pass had it (chosen so icons read apart at the map's
// closest zoom). Whether the engine rebuilds pins on a zoom step - which is what a screen-constant
// spacing would need - is not established, and this deliberately does not depend on it.
namespace
{
    // Spacing in WORLD units, as the offline pass had it: chosen so two icons read apart at the map's
    // closest zoom.
    constexpr float kMinDist = 8.0f;

    // The square spiral of candidate positions around a marker's real spot: (0,0), (0,-1), (1,-1),
    // (1,0), (1,1), (0,1), (-1,1), (-1,0), (-1,-1), (0,-2)...
    void spiral_offset(int n, float &ox, float &oz)
    {
        int x = 0, z = 0, d = 1, k = 0;
        auto step = [&](int dx, int dz) { x += dx; z += dz; ++k; };
        while (k < n)
        {
            for (int i = 0; i < d && k < n; ++i) step(0, -1);
            for (int i = 0; i < d && k < n; ++i) step(1, 1);
            for (int i = 0; i < d && k < n; ++i) step(0, 1);
            for (int i = 0; i < 2 * d && k < n; ++i) step(-1, 0);
            for (int i = 0; i < 2 * d && k < n; ++i) step(0, -1);
            for (int i = 0; i < d && k < n; ++i) step(1, 0);
            ++d;
        }
        ox = static_cast<float>(x) * kMinDist;
        oz = static_cast<float>(z) * kMinDist;
    }

    // Where the markers already placed on this map are, bucketed into cells of kMinDist so a candidate
    // only has to look at its own cell and the eight around it.
    //
    // This replaces the "cluster, then spiral inside it" approach the offline pass uses, because that
    // one only spaces a marker from the group it was ASSIGNED to: two nearby groups spiral outward
    // independently and their members can land on top of each other. A beacon dump at Fort Haight
    // showed exactly that - markers 2.8u and 5.3u apart with the spacing set to 8u. Asking "is anything
    // already within 8 units of here" instead makes the answer global, and it costs one hash lookup.
    //
    // Each placed marker also carries the ANCHOR of the lattice it sits on. A marker that finds its own
    // spot free is its own anchor; one that has to move adopts the anchor of whoever it collided with,
    // so everything pushed out of one crowded spot lands on ONE grid, a clean kMinDist apart. Anchoring
    // each marker's candidates at its own position instead - which is what this did first - leaves the
    // lattices of neighbours unaligned, and a crowd comes out ragged: 8u to one neighbour and 11.3u
    // diagonally to another.
    struct Placed
    {
        float x, z;    // where it ended up
        float ax, az;  // origin of the lattice it belongs to
    };

    // A cell is the WHOLE tile identity plus the cell indices, compared field by field. The first
    // version folded these into one 64-bit number with `tile << 26`, and the tile's area sits in bits
    // 40..47 - shifted clean off the top. Every legacy dungeon has gridX = gridZ = 0, so m10 and m15
    // (and every such pair) shared cells and compared their LOCAL coordinates as if they were one
    // map: 141 ERR / 93 vanilla marker pairs less than a spacing apart in different dungeons, each
    // pushing the other aside for a neighbour it does not have.
    struct CellKey
    {
        uint64_t tile;
        int32_t cx, cz;
        bool operator==(const CellKey &o) const { return tile == o.tile && cx == o.cx && cz == o.cz; }
    };
    struct CellKeyHash
    {
        size_t operator()(const CellKey &k) const
        {
            uint64_t h = k.tile * 0x9E3779B97F4A7C15ull;
            h ^= (static_cast<uint64_t>(static_cast<uint32_t>(k.cx)) << 32) |
                 static_cast<uint32_t>(k.cz);
            h ^= h >> 29;
            h *= 0xBF58476D1CE4E5B9ull;
            h ^= h >> 32;
            return static_cast<size_t>(h);
        }
    };

    struct Occupancy
    {
        std::unordered_map<CellKey, std::vector<Placed>, CellKeyHash> cells;

        static CellKey key(uint64_t tile, int32_t cx, int32_t cz) { return CellKey{tile, cx, cz}; }

        template <class Fn>
        void around(uint64_t tile, float x, float z, Fn &&fn) const
        {
            const int32_t cx = static_cast<int32_t>(std::floor(x / kMinDist));
            const int32_t cz = static_cast<int32_t>(std::floor(z / kMinDist));
            for (int dx = -1; dx <= 1; ++dx)
                for (int dz = -1; dz <= 1; ++dz)
                {
                    auto it = cells.find(key(tile, cx + dx, cz + dz));
                    if (it == cells.end()) continue;
                    for (const Placed &p : it->second) fn(p);
                }
        }

        bool free_at(uint64_t tile, float x, float z) const
        {
            bool ok = true;
            around(tile, x, z, [&](const Placed &p) {
                const float ddx = p.x - x, ddz = p.z - z;
                if (ddx * ddx + ddz * ddz < kMinDist * kMinDist) ok = false;
            });
            return ok;
        }

        // The lattice to join: the one belonging to the nearest marker already standing here.
        bool anchor_at(uint64_t tile, float x, float z, float &ax, float &az) const
        {
            float best = kMinDist * kMinDist;
            bool found = false;
            around(tile, x, z, [&](const Placed &p) {
                const float ddx = p.x - x, ddz = p.z - z;
                const float d2 = ddx * ddx + ddz * ddz;
                if (d2 < best) { best = d2; ax = p.ax; az = p.az; found = true; }
            });
            return found;
        }

        void take(uint64_t tile, float x, float z, float ax, float az)
        {
            const int32_t cx = static_cast<int32_t>(std::floor(x / kMinDist));
            const int32_t cz = static_cast<int32_t>(std::floor(z / kMinDist));
            cells[key(tile, cx, cz)].push_back({x, z, ax, az});
        }
    };
}  // namespace

// Is this marker on screen right now? ONE definition, used by the de-overlap, by the snapshot the
// icon factory and the hover pick read, and by the highlight rings - if they disagreed, an icon would
// be pulled aside by something the player cannot see, or a ring would sit next to its icon.
static bool native_row_visible(const CategoryRow &cr, int layer, int focus)
{
    if (!cr.p || cr.original_row_id == 0 || !native_category_migrated(cr.cat)) return false;
    if (cr.native_layer != layer) return false;
    const bool focused = focus_active() && row_in_focus(cr, focus);
    const bool eligible = focus_active() ? focused : is_category_enabled(cr.cat);
    if (!eligible) return false;
    if (cr.p->eventFlagId != 0 && !goblin::flag_is_set(cr.p->eventFlagId)) return false;
    // World state (Leyndell: the Royal Capital's markers while it stands, the Ashen Capital's
    // after it burns - and back again where ERR's Roundtable braziers switch it). A focused marker
    // is shown past it, as apply_focus_highlight lifts the post-event gates: the player asked
    // where it is.
    if (!focused && !row_state_open(cr)) return false;
    return !native_row_hidden(cr) && !row_group2_gate_off(cr.p);
}

// Visibility answered ONCE per refresh and then read by everyone. It is the expensive half: each row
// asks the engine's event-flag system about its gate and up to eight disable flags, so evaluating it
// twice - which the first version of this did, since the highlight builder runs INSIDE the snapshot -
// is the thing to avoid, not the spiral.
// Recompute display positions for `layer`. Everything invisible keeps its real coordinates and, more
// to the point, takes up no room - which is the whole difference from baking this offline.
//
// Placement is first-come on a stable order (row index), so the same visible set always produces the
// same layout: a marker keeps its spot while its neighbours come and go, rather than the map
// reshuffling itself every time something is collected.
static void refresh_deoverlap(int layer)
{
    const auto t0 = std::chrono::steady_clock::now();
    const int focus = g_focus_category;
    g_vis.assign(g_category_rows.size(), 0);
    size_t shown = 0;
    for (size_t i = 0; i < g_category_rows.size(); ++i)
    {
        CategoryRow &cr = g_category_rows[i];
        cr.native_px = cr.anchor_px;
        cr.native_pz = cr.anchor_pz;
        if (native_row_visible(cr, layer, focus)) { g_vis[i] = 1; ++shown; }
    }

    static Occupancy occ;  // kept across refreshes to hold its buckets; cleared here
    for (auto &kv : occ.cells) kv.second.clear();
    // Lit graces reserve their spot FIRST. Once a grace is discovered our own grace marker
    // hides (baked_dis1 = the grace's lit flag) and the game stands its NATIVE clickable pin
    // there - built from BonfireWarpParam, not from any WMP row we could move, and it wins
    // the hover, so a marker left under it is invisible AND unreachable. Non-clickable native
    // marks (cave/church/tunnel WMP rows) are deliberately NOT reserved: our icons draw above
    // those and should stay put.
    size_t native_seeded = 0;
    for (const CategoryRow &cr : g_category_rows)
    {
        if (cr.cat != Category::WorldGraces || cr.native_layer != layer) continue;
        if (cr.native_area == 99) continue;
        if (cr.baked_dis1 == 0 || !goblin::flag_is_set(cr.baked_dis1)) continue;
        const uint64_t tile = (static_cast<uint64_t>(cr.native_area) << 40) |
                              (static_cast<uint64_t>(cr.native_gx) << 20) | cr.native_gz;
        occ.take(tile, cr.anchor_px, cr.anchor_pz, cr.anchor_px, cr.anchor_pz);
        ++native_seeded;
    }
    size_t moved = 0, crowded = 0;
    for (size_t i = 0; i < g_category_rows.size(); ++i)
    {
        if (!g_vis[i]) continue;
        CategoryRow &cr = g_category_rows[i];
        if (cr.native_area == 99) continue;  // parked/hidden coordinate trick
        const uint64_t tile = (static_cast<uint64_t>(cr.native_area) << 40) |
                              (static_cast<uint64_t>(cr.native_gx) << 20) | cr.native_gz;
        if (occ.free_at(tile, cr.anchor_px, cr.anchor_pz))
        {
            // Nothing near it: it stands where it really is, and becomes the anchor of a lattice for
            // anything that arrives here later.
            occ.take(tile, cr.anchor_px, cr.anchor_pz, cr.anchor_px, cr.anchor_pz);
            continue;
        }
        ++crowded;
        // Join the lattice of whoever is already standing here, so a crowd comes out as one grid
        // rather than as overlapping grids of its own members.
        float ax = cr.anchor_px, az = cr.anchor_pz;
        occ.anchor_at(tile, cr.anchor_px, cr.anchor_pz, ax, az);
        constexpr int kMaxCandidates = 96;
        bool placed = false;
        for (int k = 1; k < kMaxCandidates && !placed; ++k)
        {
            float ox = 0.0f, oz = 0.0f;
            spiral_offset(k, ox, oz);
            const float x = ax + ox, z = az + oz;
            if (!occ.free_at(tile, x, z)) continue;
            cr.native_px = x;
            cr.native_pz = z;
            occ.take(tile, x, z, ax, az);
            placed = true;
            ++moved;
        }
        if (!placed) occ.take(tile, cr.anchor_px, cr.anchor_pz, ax, az);  // give up rather than search forever
    }
    g_vis_layer = layer;
    // Timed, not assumed: this runs on the map UI thread, so its cost is the thing to know. Said on
    // the first refresh and then rarely, because a per-refresh line would drown the log.
    static int s_said = 0;
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    if (s_said == 0 || (goblin::config::debugLogging && ++s_said % 200 == 0))
    {
        if (s_said == 0) s_said = 1;
        spdlog::info("[deoverlap] layer {}: {} of {} markers visible, {} of them had company, {} moved "
                     "aside, {} lit graces reserved, in {} us (spacing {}u)",
                     layer, shown, g_category_rows.size(), crowded, moved, native_seeded, us, kMinDist);
    }
}

namespace goblin
{
static std::vector<NativeMarkerPoint> collect_native_markers(int layer, bool include_hidden, NativeMarkerVisitor visitor, void *context)
{
    std::vector<NativeMarkerPoint> out;
    const auto emit = [&](const NativeMarkerPoint &point) {
        if (visitor) visitor(point, context);
        else out.push_back(point);
    };
    if (layer < 0 || layer > 2) return out;
    // Master switch off: the hover pick and the focus rings want NOTHING back - an empty snapshot
    // is what keeps tooltips and rings off icons the player just switched away. The marker manager
    // asks with include_hidden instead, because a row that never got CREATED cannot be switched
    // back on without reopening the map, and that is exactly the bug this argument exists for.
    // Those rows come back PARKED: `hidden` forces every point invisible here, so nothing shows
    // until the switch goes on again and the next merge re-reads the real visibility.
    const bool hidden = icons_hidden();
    if (hidden && !include_hidden) return out;
    // The layout lock is held from the refresh to the end of the loop: the loop reads g_vis and the
    // native_px/pz the refresh just wrote, and a second refresh on the other thread would move both.
    std::lock_guard<std::mutex> layout_lk(g_layout_mtx);
    if (!visitor) out.reserve(g_category_rows.size());
    const int focus = g_focus_category;
    // Pull crowded markers apart FIRST, from the set that is visible on this layer right now, so the
    // positions emitted below already carry the spread. Everything downstream - the icon factory, the
    // hover pick, the highlight rings - reads those positions and needs no idea this happened.
    refresh_deoverlap(layer);
    for (size_t idx = 0; idx < g_category_rows.size(); ++idx)
    {
        const CategoryRow &cr = g_category_rows[idx];
        // Rows of EVERY map layer are returned: the map hosts all layers'
        // markers in one shared parent, so a layer switch is pure show/hide
        // (the layer match is part of `visible`, not a row filter).
        if (!cr.p || cr.original_row_id == 0 ||
            !native_category_migrated(cr.cat)) continue;
        const bool visible = !hidden && g_vis[idx] != 0;  // answered once, by the refresh above
        const int source_icon = goblin::gfx_probe::source_iconid(cr.p->iconId);
        if (source_icon < 0) continue;
        emit({cr.original_row_id, source_icon, cr.native_area, cr.native_layer,
                       cr.native_gx, cr.native_gz, cr.native_px, cr.native_pz, visible,
                       cr.p});
        // Twin checkmark point for defeat-capable rows (bosses/NPC/hawks/...):
        // the native path has no stock completion overlay, so a second
        // lightweight child with the green check icon rides the same coords.
        // LAZY: emitted only once the defeat flag IS set - every attached
        // child costs driver-side teardown time (~8us) even when never drawn,
        // and most of the ~1900 defeat-capable rows are alive at any moment.
        // A row defeated mid-session gets its badge at the next map rebuild
        // (the factory cannot create children outside a build burst anyway;
        // the queue watchdog quietly drops the interim request).
        // (hide_killed_bosses=ON instead hides the base row through
        // textDisableFlagId - the twin follows it down via `visible`.)
        const unsigned cleared_flag = cr.p->clearedEventFlagId;
        if (cleared_flag != 0 && goblin::flag_is_set(cleared_flag))
            emit({cr.original_row_id | NATIVE_CLEARED_KEY_BIT,
                           static_cast<int>(goblin::generated::CLEARED_ICON_ID),
                           cr.native_area, cr.native_layer,
                           cr.native_gx, cr.native_gz, cr.native_px, cr.native_pz,
                           visible && goblin::flag_is_set(cleared_flag),
                           cr.p});
    }
    // Focus rings. The pool is emitted ALWAYS (even with no focus) so the children get built
    // during the seed burst; without a focus they simply report invisible. When a focus is on,
    // ring i rides the coordinates of the i-th marker in the set and the merge moves it there.
    // A pool rather than one ring per row: per-row would mean ~9500 extra children, and every
    // attached child costs driver-side teardown on map close.
    const uint32_t ring_frame =
        goblin::gfx_probe::injected_iconid(static_cast<int>(goblin::generated::HIGHLIGHT_ICON_ID));
    if (ring_frame && !g_category_rows.empty() && g_category_rows[0].p)
    {
        const std::vector<HighlightPoint> pts = focus_highlight_points_locked();  // layout lock is held
        // DEMAND vs POOL. The pool is fixed at NATIVE_RING_POOL and the point list is not capped,
        // so a focus set larger than the pool silently leaves its tail unringed. Reported when the
        // demand changes (the snapshot runs ~5x a second, so logging every pass would be noise):
        // this is the one number that separates "the pool is too small" from "the live update
        // missed some rings", and the user's report - rings absent after switching a progress
        // category with the map open, present after a reopen - fits either until it is measured.
        {
            static std::atomic<size_t> s_last_demand{SIZE_MAX};
            const size_t want = pts.size();
            if (s_last_demand.exchange(want) != want)
                spdlog::info("[v3ring] focus demand: {} icon(s) want a ring, pool is {} -> {} "
                             "ringed, {} without",
                             want, NATIVE_RING_POOL, want < NATIVE_RING_POOL ? want : NATIVE_RING_POOL,
                             want > NATIVE_RING_POOL ? want - NATIVE_RING_POOL : 0);
        }
        const auto &cr0 = g_category_rows[0];
        for (size_t k = 0; k < NATIVE_RING_POOL; ++k)
        {
            NativeMarkerPoint rp{};
            rp.original_row_id = NATIVE_HIGHLIGHT_KEY_BIT | static_cast<uint64_t>(k);
            rp.source_icon_id = static_cast<int>(goblin::generated::HIGHLIGHT_ICON_ID);
            rp.rowptr = nullptr; // not a real marker: no hover, no manual hide
            if (k < pts.size())
            {
                const auto &hp = pts[k];
                rp.area = hp.area;
                rp.layer = hp.layer;
                rp.gx = hp.gx;
                rp.gz = hp.gz;
                rp.px = hp.px;
                rp.pz = hp.pz;
                rp.visible = !hidden && hp.layer == layer;
            }
            else
            {
                // Unused rings still need coordinates that PROJECT, or they could not be built
                // at seed time at all. Borrow a real marker's and stay invisible.
                rp.area = cr0.native_area;
                rp.layer = cr0.native_layer;
                rp.gx = cr0.native_gx;
                rp.gz = cr0.native_gz;
                rp.px = cr0.native_px;
                rp.pz = cr0.native_pz;
                rp.visible = false;
            }
            emit(rp);
        }
    }
    return out;
}

std::vector<NativeMarkerPoint> native_marker_snapshot(int layer, bool include_hidden)
{
    return collect_native_markers(layer, include_hidden, nullptr, nullptr);
}

void visit_native_markers(int layer, NativeMarkerVisitor visitor, void *context)
{
    if (visitor) (void)collect_native_markers(layer, false, visitor, context);
}
} // namespace goblin

namespace
{
    // Shared reticle math for the native-tooltip proxy. Reticle distance in the
    // fixed 1920x1080 GFx canvas is client-independent:
    // screen - client/2 = (map - viewCentre) * zoom * (client/canvas), so the
    // canvas-space distance (map - viewCentre) * zoom needs no client size.
    bool reticle_view(float &cU, float &cV, float &zoom)
    {
        goblin::mapproject::MapView v{};
        if (!goblin::mapproject::read_view(v)) return false;
        cU = (v.panX + v.snapMidX) / v.zoom;
        cV = (v.panZ + v.snapMidZ) / v.zoom;
        zoom = v.zoom;
        return true;
    }

    // ── the anchor: where the reticle is, not where we assumed it is ──────────────
    // ONE definition, because two of them disagreed. native_reticle_row() picks the nearest marker
    // to this anchor, and row_reticle_dist2() measures the GAME's candidate for the same contest
    // (goblin_maphover.cpp: `use_ours = row_reticle_dist2(row, item_d2) && ours_d2 < item_d2`).
    // Until 2026-07-31 the second one measured from the view centre while the first had already
    // moved to the live reticle, so the arbitration compared two distances taken from DIFFERENT
    // points - and picked the wrong marker exactly where the anchors diverge: at full zoom-out,
    // where the map stops panning and the reticle leaves the centre of the view.
    //
    // (cU, cV) is the centre of the view, which IS the reticle on vanilla and ERR. On a build whose
    // map reticle follows the mouse (Convergence) it is not, and the popup then described the icon in
    // the middle of the screen while the player pointed at another one.
    //
    // Which of the two this build does is measured, not assumed: while the game hovers one of its own
    // pins it tells us where its reticle is (maphover::reticle_map, map space), and that either sits on
    // the view centre or on the cursor. One sample settles it for the session; with no sample the
    // centre stands, i.e. the old behaviour.
    enum class Anchor
    {
        Centre,
        Cursor,
    };
    // ATOMIC, not a plain static: this runs on the map dialog's frame AND on the overlay thread
    // (see the cache note in native_reticle_row), and since row_reticle_dist2 calls it too there is
    // no single lock covering both entries any more.
    // The STARTING value comes from the build, because a profile's DLL only ever runs on the mod it was
    // built from: Convergence is the one measured to follow the cursor. It is a starting value, not a
    // belief - the measurement below still decides, and this only fixes the first few seconds before a
    // sample arrives (until then the popup used to sit at the centre).
    std::atomic<int> g_anchor{static_cast<int>(std::strstr(BUILD_NAME, "convergence") != nullptr
                                                   ? Anchor::Cursor
                                                   : Anchor::Centre)};
    std::atomic<bool> g_anchor_measured{false};

    void reticle_anchor(float cU, float cV, float zoom, float &ax, float &ay)
    {
        ax = cU;
        ay = cV;
        // FIRST CHOICE: the position the GAME searches around this frame, read from its own dialog
        // (maphover::reticle_live). It needs no anchor guess at all, and it is the only one that is
        // right at full zoom-out, where the map stops panning and the reticle leaves the centre of
        // the view - the regime in which the two-anchor guess below quietly described the icon in
        // the middle of the screen.
        {
            float lmx = 0.0f, lmz = 0.0f;
            bool ptr_mode = false;
            if (goblin::maphover::reticle_live(&lmx, &lmz, &ptr_mode))
            {
                ax = lmx;
                ay = lmz;
                return;
            }
        }
        // Fallback: infer it. Kept because it is what shipped and it is still right in the common
        // case, but it is now only reached when that field could not be read at all.
        //
        // Cursor -> map space: the inverse of the projection the markers use.
        auto cursor_map = [&](float &out_mx, float &out_mz) -> bool {
            POINT pt{};
            if (!GetCursorPos(&pt))
                return false;
            HWND hw = GetForegroundWindow();
            RECT rc{};
            if (!hw || !ScreenToClient(hw, &pt) || !GetClientRect(hw, &rc))
                return false;
            const float cw = static_cast<float>(rc.right - rc.left);
            const float ch = static_cast<float>(rc.bottom - rc.top);
            if (cw < 100.0f || ch < 100.0f || zoom <= 0.0f)
                return false;
            if (pt.x < 0 || pt.y < 0 || pt.x > rc.right || pt.y > rc.bottom)
                return false;
            out_mx = (static_cast<float>(pt.x) - cw * 0.5f) / (zoom * (cw / 1920.0f)) + cU;
            out_mz = (static_cast<float>(pt.y) - ch * 0.5f) / (zoom * (ch / 1080.0f)) + cV;
            return true;
        };
        float rmx = 0.0f, rmz = 0.0f;
        const bool have_sample = goblin::maphover::reticle_map(&rmx, &rmz, 400);
        if (!g_anchor_measured.load(std::memory_order_acquire) && have_sample)
        {
            float kmx = 0.0f, kmz = 0.0f;
            const bool have_cursor = cursor_map(kmx, kmz);
            const float d_centre = (rmx - cU) * (rmx - cU) + (rmz - cV) * (rmz - cV);
            const float d_cursor = have_cursor ? (rmx - kmx) * (rmx - kmx) + (rmz - kmz) * (rmz - kmz)
                                               : 1e18f;
            const Anchor was = static_cast<Anchor>(g_anchor.load(std::memory_order_acquire));
            const Anchor now = d_cursor < d_centre ? Anchor::Cursor : Anchor::Centre;
            g_anchor.store(static_cast<int>(now), std::memory_order_release);
            g_anchor_measured.store(true, std::memory_order_release);
            if (was != now)
                spdlog::info("[hover] the build's starting anchor was wrong - corrected by measurement");
            spdlog::info("[hover] reticle anchor measured: {} (sample {:.0f},{:.0f}; centre "
                         "{:.0f},{:.0f} d2={:.0f}; cursor {:.0f},{:.0f} d2={:.0f})",
                         now == Anchor::Cursor ? "THE CURSOR" : "the view centre", rmx, rmz, cU,
                         cV, d_centre, kmx, kmz, have_cursor ? d_cursor : -1.0f);
        }
        if (static_cast<Anchor>(g_anchor.load(std::memory_order_acquire)) == Anchor::Cursor)
        {
            float kmx = 0.0f, kmz = 0.0f;
            if (cursor_map(kmx, kmz))
            {
                ax = kmx;
                ay = kmz;
            }
        }
        // Centred build: the sample and the centre agree, so nothing to do.
    }
}

void *goblin::native_reticle_row(float *out_dist2, float *out_map_x, float *out_map_z)
{
    const int layer = goblin::maphover::map_layer();
    if (layer < 0 || layer > 2) return nullptr;
    float cU = 0.0f, cV = 0.0f, zoom = 0.0f;
    if (!reticle_view(cU, cV, zoom)) return nullptr;
    // Called once per map frame; at stage-4 scale (~9k migrated rows) the full
    // visibility snapshot (per-row event-flag reads) is too heavy per frame -
    // refresh a cached copy at the same 200ms cadence the native manager uses.
    // Positions are static; 200ms-stale visibility on a hover test is invisible.
    // THIS CACHE IS SHARED BY TWO THREADS and must be locked. It is read from the map dialog's
    // per-frame update (our hover detour calls this) and from the manual-hide hotkey thread (Delete
    // asks which marker is under the reticle). Unlocked, the 200ms refresh below move-assigns the
    // vector - freeing the old buffer - while the other thread is iterating it or refreshing it as
    // well: the same block gets freed twice and the heap trips. That is a real crash, caught in the act:
    //   placename_detour -> native_reticle_row -> vector::operator=(&&) -> free_base -> ntdll
    //   reported 0xC0000374 (heap corruption), and map tiles rendered transparent alongside it.
    // A function-local static is thread-safe to INITIALIZE, never to use.
    // The lock covers the refresh AND the loop, because the loop reads the buffer the refresh frees.
    static std::mutex cache_mutex;
    static std::vector<NativeMarkerPoint> cache;
    static int cache_layer = -1;
    static std::chrono::steady_clock::time_point cache_at{};
    // Map-space position per row, kept across snapshots and re-resolved only when the row's local
    // position changes. Under the same lock as the snapshot, and outliving it on purpose: the
    // snapshot is thrown away and rebuilt every 200 ms, the positions in it almost never move.
    //
    // The loop below used to call mapproject::to_map for all ~7,100 rows on EVERY frame, and for
    // any row outside the overworld that is a call into the engine's world->map converter. Report
    // 19: twelve access violations a second down that path, because the converter is reached
    // through a cached CS::WorldMapViewModel that dies with the map generation. Positions are
    // nearly static, so re-asking the engine per frame bought nothing and paid in faults - and the
    // freshness gate that now guards the converter would otherwise drop every legacy-dungeon
    // marker out of the hover test whenever the game had not converted recently.
    //
    // "Nearly": the live de-overlap moves a marker when a neighbour appears or goes (a category
    // switch, a focus, a pickup), and the first version of this resolved each row ONCE for the
    // session, so after such a change the pick still measured from where the icon used to be -
    // the cursor sat on one marker and the tooltip (and the Delete hide) named the one beside it.
    // Each entry remembers the local position it was resolved from; a row whose position differs
    // is re-resolved. One that cannot be resolved right now keeps its previous answer - an
    // 8-unit-stale spot beats a marker that cannot be hovered at all - and its remembered
    // position stays the old one, so the next refresh asks again.
    struct Projected
    {
        float src_px, src_pz;  // the local position this answer was resolved from
        float mx, mz;          // map space
    };
    static std::unordered_map<uint64_t, Projected> projected;
    std::lock_guard<std::mutex> cache_lock(cache_mutex);
    const auto now = std::chrono::steady_clock::now();
    if (layer != cache_layer ||
        std::chrono::duration_cast<std::chrono::milliseconds>(now - cache_at).count() > 200)
    {
        cache = native_marker_snapshot(layer);
        cache_layer = layer;
        cache_at = now;
        for (const auto &p : cache)
        {
            if (!p.rowptr) continue;
            auto known = projected.find(p.original_row_id);
            if (known != projected.end() && known->second.src_px == p.px && known->second.src_pz == p.pz)
                continue;
            float mx = 0.0f, mz = 0.0f;
            if (goblin::mapproject::to_map(p.area, p.gx, p.gz, p.px, p.pz, mx, mz))
                projected[p.original_row_id] = Projected{p.px, p.pz, mx, mz};
        }
    }
    float ax = 0.0f, ay = 0.0f;
    reticle_anchor(cU, cV, zoom, ax, ay); // the SAME anchor row_reticle_dist2 measures from
    constexpr float PICK_CANVAS_PX = 40.0f; // ~engine pin focus radius
    float best = PICK_CANVAS_PX * PICK_CANVAS_PX;
    void *best_row = nullptr;
    float best_mx = 0.0f, best_mz = 0.0f;
    for (const auto &p : cache)
    {
        if (!p.visible || !p.rowptr) continue;
        const auto hit = projected.find(p.original_row_id);
        if (hit == projected.end()) continue; // not resolvable yet; retried on the next refresh
        const float mx = hit->second.mx;
        const float mz = hit->second.mz;
        const float dx = (mx - ax) * zoom;
        const float dy = (mz - ay) * zoom;
        const float d2 = dx * dx + dy * dy;
        if (d2 < best)
        {
            best = d2;
            best_row = p.rowptr;
            best_mx = mx;
            best_mz = mz;
        }
    }
    if (best_row)
    {
        if (out_dist2) *out_dist2 = best;
        if (out_map_x) *out_map_x = best_mx;
        if (out_map_z) *out_map_z = best_mz;
    }
    return best_row;
}

bool goblin::row_reticle_dist2(const void *rowptr, float &out_dist2)
{
    if (!rowptr) return false;
    float cU = 0.0f, cV = 0.0f, zoom = 0.0f;
    if (!reticle_view(cU, cV, zoom)) return false;
    const auto *wp = static_cast<const from::paramdef::WORLD_MAP_POINT_PARAM_ST *>(rowptr);
    // The RAW row is right here, and only here: the only caller passes the ENGINE's focused pin
    // (goblin_maphover.cpp), never one of ours - ours carry a zeroed dispMask, so the engine never
    // focuses them - and the engine's own rows are never moved by the live de-overlap. Asking
    // display_position() instead would take the layout lock on a per-frame path to learn nothing.
    float mx = 0.0f, mz = 0.0f;
    if (!goblin::mapproject::to_map(wp->areaNo, wp->gridXNo, wp->gridZNo,
                                    wp->posX, wp->posZ, mx, mz))
        return false;
    // From the SAME anchor native_reticle_row picks against - the caller compares the two numbers
    // directly, so measuring this one from the view centre made the comparison meaningless wherever
    // the reticle is not at the centre.
    float ax = 0.0f, ay = 0.0f;
    reticle_anchor(cU, cV, zoom, ax, ay);
    const float dx = (mx - ax) * zoom;
    const float dy = (mz - ay) * zoom;
    out_dist2 = dx * dx + dy * dy;
    return true;
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
        const uint64_t k = cr.hide_key;
        std::lock_guard<std::mutex> lk(g_manual_hidden_mtx);
        auto it = g_manual_hidden.find(k);
        if (it != g_manual_hidden.end()) { g_manual_hidden.erase(it); r.now_hidden = false; }
        else
        {
            g_manual_hidden[k] = HiddenMeta{cr.p->textId1, cr.p->iconId, cr.region_id,
                                            static_cast<uint8_t>(cr.cat), next_hidden_seq()};
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
    // Newest hide first: the marker the player just removed sits at the top of the Hidden
    // list, where it is looked for. Key order (the map's) told nothing.
    std::vector<const std::pair<const uint64_t, HiddenMeta> *> rows;
    rows.reserve(g_manual_hidden.size());
    for (const auto &kv : g_manual_hidden) rows.push_back(&kv);
    std::sort(rows.begin(), rows.end(),
              [](const auto *a, const auto *b) { return a->second.seq > b->second.seq; });
    for (const auto *kv : rows)
        out.push_back(HiddenMarkerInfo{kv->first, kv->second.textId, kv->second.iconId,
                                       kv->second.region, kv->second.cat});
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

// File format. v2 starts with the header line below; every other line is
// `<key> <textId> <iconId> <region> <category>`. A file WITHOUT the header is v1: its keys
// are marker_key_v1 values, which only mean something against the build that wrote them.
// They are parked in g_hidden_v1_pending and migrate_hidden_v1() maps each one to the row it
// still denotes in THIS build (same live position/text/icon), re-keys it, and drops the
// rest - those are exactly the entries whose icon had already come back and been hidden
// again under a fresh key, i.e. the duplicates the user saw in the list.
// v3 (2.1.4) changed the key itself, not the line format - see stable_hide_key. A v2 file is
// read into g_hidden_v2_pending and re-keyed by migrate_hidden_v2() against the rows of THIS
// build. That migration is exact: both keys are computed from the same baked entry, so a
// stored v2 key resolves to every marker it used to mean, which is what it hid.
static constexpr const char *kHiddenHeaderPrefix = "#mfg-hidden v";
static constexpr const char *kHiddenHeaderV3 = "#mfg-hidden v3";
static constexpr int kHiddenVersion = 3;
// A hide file written by a NEWER build: not read, and not written over either.
static bool g_hidden_file_unreadable = false;

void goblin::save_manual_hidden(const std::filesystem::path &path)
{
    std::lock_guard<std::mutex> lk(g_manual_hidden_mtx);
    try
    {
        std::ofstream f(path, std::ios::trunc);
        f << kHiddenHeaderV3 << '\n';
        // Oldest first: the loader numbers lines in order, so this is what keeps the Hidden
        // list's newest-first order across a restart. Same format as before, just ordered.
        std::vector<std::pair<uint64_t, HiddenMeta>> rows(g_manual_hidden.begin(), g_manual_hidden.end());
        std::sort(rows.begin(), rows.end(),
                  [](const auto &a, const auto &b) { return a.second.seq < b.second.seq; });
        for (const auto &[k, m] : rows)
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
        std::string line;
        int version = 1;  // no header at all = v1
        bool first = true;
        g_hidden_file_unreadable = false;
        while (std::getline(f, line))
        {
            if (first)
            {
                first = false;
                const int hv = header_version(line, kHiddenHeaderPrefix);
                if (hv > kHiddenVersion)
                {
                    // From a newer build. Load nothing (its keys mean nothing here) and block
                    // the persist target, so the next save cannot replace it with our view.
                    spdlog::warn("[hide] {} was written by a newer build (v{}); leaving it alone",
                                 path.filename().string(), hv);
                    g_hidden_file_unreadable = true;
                    return;
                }
                if (hv > 0) { version = hv; continue; }
                // No header: a v1 file, and THIS line is already data - fall through.
            }
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ss(line);
            uint64_t k; long tid, icon, region, cat;
            if (!(ss >> k >> tid >> icon >> region >> cat)) continue;
            auto &target = version == 3 ? g_manual_hidden
                         : version == 2 ? g_hidden_v2_pending
                                        : g_hidden_v1_pending;
            target[k] = HiddenMeta{static_cast<int32_t>(tid), static_cast<uint16_t>(icon),
                                   static_cast<int32_t>(region), static_cast<uint8_t>(cat),
                                   next_hidden_seq()};  // file order = hide order (oldest first)
        }
    }
    catch (...) {}
}

// v2 -> v3, once the injected rows exist. One stored v2 key can resolve to SEVERAL current
// markers - that is the collision v3 exists to end - and all of them are hidden, because all
// of them were hidden under the old key. Returns true when the set changed.
static bool migrate_hidden_v2()
{
    size_t matched_keys = 0, added = 0, dropped = 0;
    {
        std::lock_guard<std::mutex> lk(g_manual_hidden_mtx);
        if (g_hidden_v2_pending.empty()) return false;
        if (g_category_rows.empty()) return false;  // rows not injected yet; try again next poll
        std::set<uint64_t> seen;
        for (const auto &cr : g_category_rows)
        {
            if (!cr.p || !cr.hide_key_v2) continue;
            auto it = g_hidden_v2_pending.find(cr.hide_key_v2);
            if (it == g_hidden_v2_pending.end()) continue;
            seen.insert(it->first);
            if (g_manual_hidden.emplace(cr.hide_key,
                                        HiddenMeta{cr.p->textId1, cr.p->iconId, cr.region_id,
                                                   static_cast<uint8_t>(cr.cat), next_hidden_seq()}).second)
                ++added;
        }
        matched_keys = seen.size();
        dropped = g_hidden_v2_pending.size() - matched_keys;
        g_hidden_v2_pending.clear();
    }
    spdlog::info("[hide] hide file re-keyed to v3: {} stored key(s) matched a current marker "
                 "({} marker(s) hidden), {} matched none and were dropped",
                 matched_keys, added, dropped);
    return true;
}

// v1 -> v3, once the injected rows exist to resolve the legacy keys against. Returns true when
// the hidden set changed (caller persists + reapplies visibility).
static bool migrate_hidden_v1()
{
    size_t matched_keys = 0, added = 0, dropped = 0;
    {
        std::lock_guard<std::mutex> lk(g_manual_hidden_mtx);
        if (g_hidden_v1_pending.empty()) return false;
        if (g_category_rows.empty()) return false;  // rows not injected yet; try again next poll
        std::set<uint64_t> seen;
        for (const auto &cr : g_category_rows)
        {
            if (!cr.p) continue;
            auto it = g_hidden_v1_pending.find(marker_key_v1(cr.p));
            if (it == g_hidden_v1_pending.end()) continue;
            seen.insert(it->first);
            if (g_manual_hidden.emplace(cr.hide_key,
                                        HiddenMeta{cr.p->textId1, cr.p->iconId, cr.region_id,
                                                   static_cast<uint8_t>(cr.cat), next_hidden_seq()}).second)
                ++added;
        }
        matched_keys = seen.size();
        dropped = g_hidden_v1_pending.size() - matched_keys;
        g_hidden_v1_pending.clear();
    }
    spdlog::info("[hide] legacy hide file migrated: {} key(s) matched a current marker ({} re-keyed), "
                 "{} matched none and were dropped",
                 matched_keys, added, dropped);
    return true;
}

void goblin::set_hidden_dir(const std::filesystem::path &dir) { g_hidden_dir = dir; }
void goblin::persist_manual_hidden()
{
    if (!g_hidden_file.empty() && !g_hidden_file_unreadable) save_manual_hidden(g_hidden_file);
}

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
    if (slot == g_hidden_slot)
    {
        // Same character: the pending work is a legacy hide file whose keys could not be
        // resolved at load time, or a saved focus, both waiting for the injected rows.
        bool changed = false;
        // The manual-hide setting is a checkbox in both menus, so it can flip while a character
        // is loaded. The hide file is attached only on a character switch, so a flip to ON left
        // the hotkey working against an empty set with no persist target: the character's saved
        // hides never applied, and every new one was gone at exit. Attach the file on the flip
        // to ON; drop it (and the set - is_manually_hidden() stays honest) on the flip to OFF.
        if (slot >= 0 && !g_hidden_dir.empty())
        {
            if (goblin::config::enableManualHide && g_hidden_file.empty())
            {
                g_hidden_file = g_hidden_dir / ("MapForGoblins_hidden_s" + std::to_string(slot) + ".txt");
                load_manual_hidden(g_hidden_file);
                spdlog::info("[hide] manual hide switched on -> {} ({} hidden loaded)",
                             g_hidden_file.filename().string(), manual_hidden_count());
                changed = true;
            }
            else if (!goblin::config::enableManualHide && !g_hidden_file.empty())
            {
                g_hidden_file.clear();
                std::lock_guard<std::mutex> lk(g_manual_hidden_mtx);
                g_manual_hidden.clear();
                g_hidden_v1_pending.clear();
                g_hidden_v2_pending.clear();
                changed = true;
            }
        }
        const bool from_v1 = migrate_hidden_v1();
        const bool from_v2 = migrate_hidden_v2();  // both, never short-circuited
        if (from_v1 || from_v2)
        {
            persist_manual_hidden();
            changed = true;
        }
        if (restore_focus_pending())
            changed = true;
        return changed;  // false = no character-switch since last check
    }
    g_hidden_slot = slot;
    {
        std::lock_guard<std::mutex> lk(g_manual_hidden_mtx);
        g_manual_hidden.clear();
        g_hidden_v1_pending.clear();
        g_hidden_v2_pending.clear();
    }
    // Collected-geometry memory is per character too. Its sticky carry-forward keeps a row hidden
    // until the live object is seen alive, which after a switch meant everything A had picked up
    // stayed hidden for B until B walked onto the tile. The next poll rebuilds it from B's save.
    goblin::collected::forget_session_state();
    // The focus belongs to the character too: drop the old one WITHOUT writing it into the new
    // character's file (no persist target while it is cleared), then load the new one.
    g_focus_file.clear();
    g_focus_restore_pending = false;
    g_pending_focus_picks.clear();
    // The search module keeps its own copy of the tick set, and set_focus_category() below
    // only clears the MAP's side of it. Left behind, it would be unioned with the character
    // we are switching TO (restore_focus_pending -> replace_picks) and saved into their file.
    goblin::search::forget_picks();
    if (focus_active())
        set_focus_category(-1);
    if (slot >= 0 && !g_hidden_dir.empty())
    {
        if (goblin::config::enableManualHide)
        {
            g_hidden_file = g_hidden_dir / ("MapForGoblins_hidden_s" + std::to_string(slot) + ".txt");
            load_manual_hidden(g_hidden_file);  // per-character set (empty file -> empty set)
            size_t legacy = 0;
            {
                std::lock_guard<std::mutex> lk(g_manual_hidden_mtx);
                legacy = g_hidden_v1_pending.size() + g_hidden_v2_pending.size();
            }
            spdlog::info("[hide] active save slot {} -> {} ({} hidden loaded, {} legacy key(s) to migrate)",
                         slot, g_hidden_file.filename().string(), manual_hidden_count(), legacy);
            const bool from_v1 = migrate_hidden_v1();
            const bool from_v2 = migrate_hidden_v2();  // both, never short-circuited
            if (from_v1 || from_v2)
                persist_manual_hidden();
        }
        else
        {
            // Manual hide is off: no hide file is read or written, and the set stays empty so
            // is_manually_hidden() keeps returning false. The FOCUS below is a separate feature
            // and is per character regardless.
            g_hidden_file.clear();
        }
        g_focus_file = g_hidden_dir / ("MapForGoblins_focus_s" + std::to_string(slot) + ".txt");
        load_focus_file(g_focus_file);
        restore_focus_pending();  // applies now if the rows exist, else on a later poll
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

// Live re-apply of hide_killed_bosses (boss / spiritspring-hawk / hostile-NPC /
// strong-enemy rows). Re-derives from the baked fields so either mode is reversible.
void goblin::apply_kill_display()
{
    for (auto &cr : g_category_rows)
    {
        if (cr.cat != Category::WorldBosses &&
            cr.cat != Category::WorldSpiritspringHawks &&
            cr.cat != Category::WorldHostileNPC &&
            cr.cat != Category::WorldStrongEnemies)
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
        // An aggregate marker (one category of a boss's award) is not addressed by the lot:
        // reading slot 1 of it would relabel this marker with another category's item and
        // pin its hide flag to that item's pickup. Keep the baked label and the baked kill
        // flag; the spoiler-free icon and label below still apply, they say nothing specific.
        const bool live = !lr.aggregate;
        RawItemLotRow *row = have_lots && live ? lots.row(lr.lotId, lr.lotType) : nullptr;
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
            // Same rule as at injection: only an item the lot no longer gives is re-iconed, so a
            // source-split category (merchant Bell Bearings) keeps its own icon.
            if (do_icons && live && item > 0 && encode_live_item(item, cat) != lr.baked_text1)
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
            else if (do_labels && live && item > 0)
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
        if (do_flags && live && row)
        {
            flag = *reinterpret_cast<uint32_t *>(row->b + 0x80);
            if (flag == 0)
            {
                int32_t item2 = *reinterpret_cast<int32_t *>(row->b + 0x04);
                if (item2 == 0) flag = *reinterpret_cast<uint32_t *>(row->b + 0x60);
            }
        }
        for (int i = 0; i < 8; ++i)
            *fls[i] = (do_flags && live && flag && *tids[i] > 0 && lr.baked_dis[i] != 0)
                          ? flag
                          : lr.baked_dis[i]; // else restore baked
    }
}

// One call to re-apply every LIVE-capable setting after the overlay edits the
// config. Each step re-derives from baked state (idempotent). Takes effect on
// the next world-map (re)open.
// One counter for "what should be on screen has changed". Read by the native-marker tick;
// see visibility_epoch() in the header for why a toggle cannot ride the periodic refresh.
static std::atomic<uint32_t> g_visibility_epoch{0};

uint32_t goblin::visibility_epoch()
{
    return g_visibility_epoch.load(std::memory_order_acquire);
}

void goblin::note_visibility_changed()
{
    g_visibility_epoch.fetch_add(1, std::memory_order_release);
}

void goblin::reapply_live_settings()
{
    g_visibility_epoch.fetch_add(1, std::memory_order_release);
    g_label_epoch.fetch_add(1, std::memory_order_release); // apply_loot_settings may relabel
    apply_category_visibility();           // show_* categories (+ focus isolation)
    apply_kill_display();                  // hide_killed_bosses
    apply_loot_settings();                 // anonymous_loot + live_loot_icons/labels/flags
    goblin::apply_map_logic();             // require_map_fragments + ERR patch_* markers
    goblin::apply_worldmap_fragment_bypass(); // after map logic (it re-gates eventFlagId)
    apply_focus_highlight();               // LAST: focus glow-icon + force-visible must win over
                                           // apply_loot_settings (which rewrites loot iconId) and the
                                           // category/fragment enable-flag gating above.
}

void goblin::set_icons_hidden(bool hidden)
{
    const bool was = g_icons_user_disabled.exchange(hidden);
    g_visibility_epoch.fetch_add(1, std::memory_order_release);
    // SAY SO. This is the master switch - it decides whether the player sees any marker at all,
    // it is reachable from three places (the F10 hotkey, the overlay checkbox, the native menu's
    // first row), and until 2026-08-07 it flipped in complete silence. A log then read exactly
    // like a broken build: "CATEGORIES READY created=0", no icons on the map, and nothing
    // anywhere saying a human had switched them off. That cost a diagnosis round trip.
    if (was != hidden)
        spdlog::info("[icons] master switch -> {}", hidden ? "OFF (user)" : "ON (user)");
}
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
        const bool master_mode = !config::menuEnabled; // no menu on the key -> the key is the master switch
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

// The icons ON/OFF announcement. The status line copies the text and can be asked from this
// polling thread; there is no UI-thread hand-off any more (the codex toast needed one).
static void show_toggle_banner(bool icons_on)
{
    spdlog::info("[status] icons {}", icons_on ? "ON" : "OFF");
    goblin::status_line::show(goblin::i18n::wtr(
        icons_on ? goblin::i18n::ToastId::MapIconsOn : goblin::i18n::ToastId::MapIconsOff));
}


// The live rows we injected, for whoever needs to walk them. (A thirteen-line header describing the
// WorldMapPointParam STATE MACHINE - who owns the vanilla/expanded decision and what the desired
// state is per user toggle - stood over this accessor, which owns none of it. That machine is
// menu_auto_toggle_loop; the header moved there.)
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
// the running session. Pairs mirror inputs/quest_invader_overrides.json.
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
}

// ── Live-loot: hide loot markers on the LIVE item-lot pickup flag ──────
// Reads each lot-backed marker's source ItemLotParam row from memory and sets
// textDisableFlagId1 to the lot's current getItemFlagId. Because we read the
// LOADED regulation (vanilla, Randomizer, any file mod), the marker hides on
// the actual light-point pickup regardless of which item the lot now gives.
// (This header sat above remap_injected_icons() until 2026-07-30, separated from its own function
// by a missing blank line - and that function does not read ItemLotParam at all.)
//
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
        // See apply_loot_settings: an aggregate marker keeps its baked label and flags.
        const bool live = !lr.aggregate;
        RawItemLotRow *row = live ? read_row(lr.lotId, lr.lotType) : nullptr;
        if (!row && live) { not_found++; continue; }
        auto *p = reinterpret_cast<from::paramdef::WORLD_MAP_POINT_PARAM_ST *>(lr.ptr);

        if (do_flags && live)
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
        else if (do_labels && live)
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
    g_label_epoch.fetch_add(1, std::memory_order_release);
}

// WorldMapPointParam state owner. Since the 16-align fix in inject_map_entries
// (see docs/ersc_hosting_and_map_autohide.md), the expanded table is safe during ERSC hosting -
// the old "expand only while the map is open" auto-hide is no longer needed and has been removed.
// The table now stays EXPANDED always; the hotkey is a pure personal show/hide toggle.
//
// Desired table state:
//   userDisabled (F10/gamepad master-off) -> VANILLA  (user hid the icons)
//   else                                  -> EXPANDED  (icons everywhere)
//
// (The retired map-state auto-hide read CSMenuMan+0xCD with inverse logic; it is fully documented
// in docs/ersc_hosting_and_map_autohide.md should a future patch ever need it back.)
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
