#pragma once

#include <cstdint>
#include <filesystem>
#include <unordered_set>
#include <vector>

// Forward-declare the generated Category enum instead of pulling the full
// (profile-scoped) goblin_map_data.hpp here: some TUs (goblin_gfx_probe.cpp)
// also include the err-path "generated/..." header explicitly, and having both
// the bare and the generated/ map-data header in one TU is a redefinition.
namespace goblin::generated { enum class Category : uint8_t; }

namespace goblin
{
    void inject_map_entries();

    // Public accessor: is this marker category currently shown (its show_*
    // config toggle on)? Used by the overlay's Progress tab. Named distinctly
    // from the internal is_category_enabled() to avoid a lookup clash.
    bool category_enabled(generated::Category cat);

    // Is a game event flag currently set? Reads the live EventFlagMan (resolves
    // the flag API by AOB on first use). Used by the Progress tab to count
    // flag-based pickups/kills (loot, bosses) that GEOF tracking doesn't cover.
    // Returns false if the flag id is 0 or the flag API isn't resolved yet.
    bool flag_is_set(uint32_t flag_id);

    // Progress-tab "focus" mode: when set, the live map shows ONLY the focused
    // category's UNCOLLECTED markers IN the focused region (everything else
    // hidden), swapped to the glow-highlight icon, so the player sees where its
    // remaining items are. Pass a generated::Category cast to int + the region's
    // PlaceName id (goblin::progress::region_place_id), or category = -1 to clear.
    // Stores the state; call reapply_live_settings() to apply.
    void set_focus_category(int category_or_negative, int32_t region_place_id = -1);
    int focus_category();
    int32_t focus_region();

    // The other focus shape: an explicit SET of markers (the item search's picks), keyed by
    // original_row_id (MAP_ENTRIES.row_id). While active, the live map shows ONLY those
    // markers (uncollected, same rules as the category focus) with the highlight ring on each.
    // Replaces any category focus; an empty set clears. set_focus_category(-1) clears it too.
    // Stores the state; call reapply_live_settings() to apply.
    void set_focus_rows(const std::vector<uint64_t> &original_row_ids);
    bool focus_rows_active();
    size_t focus_rows_count();

    // Search-index feed: one entry per injected marker, with the row's CURRENT label textId
    // (live-loot relabels and the anonymous-loot label included, so a spoiler-free player
    // cannot search past the "something" label), its progress region and category.
    struct SearchRow { uint64_t key; int32_t textId; int32_t region; uint8_t cat; };
    std::vector<SearchRow> search_row_snapshot();
    // Bumped whenever marker labels may have changed (live-loot refresh, every live-settings
    // re-apply). The search index rebuilds when it sees a new value.
    uint32_t label_epoch();

    // Swap focused rows to the glow-highlight icon (and others back to normal).
    // Called by reapply_live_settings() and after remap_injected_icons().
    void apply_focus_highlight();

    // World position of a marker to highlight on the open map, for the overlay's
    // projected ring (replaces the baked glow-icon swap: no reopen needed). area is
    // WorldMapPointParam.areaNo; world_x/z = gridNo*256 + pos. The overlay projects
    // these via goblin::mapproject. Thread-safe snapshot (row set is stable between
    // rebuilds; positions are read once here).
    // layer = WorldMapPointParam dispMask bit (0=overworld/M00, 1=underground/M01,
    // 2=DLC/M02, 0xFF=none) - the map layer this marker belongs to. The overlay draws it
    // only when the displayed layer (goblin::maphover::map_layer) matches. gx/gz/px/pz are
    // the RAW area-local grid + pos (the projector folds them per the marker's area).
    struct HighlightPoint { uint8_t area; uint8_t layer; uint16_t gx; uint16_t gz; float px; float pz; };
    // The current focus set (focus category+region) - only markers whose icon is
    // currently SHOWN (not collected, not kindling-collected, not manually hidden, and
    // no live hide/cleared flag set). Empty when no focus is active.
    std::vector<HighlightPoint> focus_highlight_points();

    /// Where one of OUR markers is DRAWN, by its live param row pointer, which is not where its row
    /// says it is: the live de-overlap moves markers that would land on each other. false = not one of
    /// ours. The beacon dump reports both, because reading the row alone answers a question nobody
    /// asked once the drawing stopped coming from it.
    bool display_position(const void *rowptr, float &px, float &pz);

    // Lightweight native-marker input. During the category-by-category debug
    // rollout, only rows whose stock WorldMapItem has been suppressed are
    // returned. The native manager consumes this snapshot on the map thread.
    struct NativeMarkerPoint
    {
        uint64_t original_row_id;
        int source_icon_id;
        uint8_t area;
        uint8_t layer;
        uint16_t gx;
        uint16_t gz;
        float px;
        float pz;
        bool visible;
        void *rowptr; // live WorldMapPointParam row (hover tooltip / manual hide)
    };
    // Key-space bit for the twin "cleared" checkmark points: defeat-capable
    // rows emit a second NativeMarkerPoint whose original_row_id carries this
    // bit and whose icon is the green check badge; its visibility is
    // base-visible AND defeat-flag-set. Real row ids never reach bit 63.
    constexpr uint64_t NATIVE_CLEARED_KEY_BIT = 1ull << 63;

    // Focus-ring points: a small FIXED pool of extra native children, keyed by this bit plus
    // the pool index. They exist from the seed build (the icon factory only pulses during a
    // build burst, so anything not created then can never appear) and are then MOVED onto the
    // focused markers live - which is why the pool is a pool and not one ring per row: one ring
    // per row would mean ~9500 extra children, and every attached child costs driver-side
    // teardown on map close.
    constexpr uint64_t NATIVE_HIGHLIGHT_KEY_BIT = 1ull << 62;
    constexpr size_t NATIVE_RING_POOL = 128;

    // include_hidden: still return every row when the master switch is off, each one forced
    // invisible. The marker manager needs that (a row it never created cannot be switched back
    // on without reopening the map); the hover pick and the rings want the default empty answer.
    std::vector<NativeMarkerPoint> native_marker_snapshot(int layer, bool include_hidden = false);

    // Synchronous visitor under the layout lock; does not allocate an intermediate snapshot.
    // The callback must not reenter layout functions or retain the point reference.
    using NativeMarkerVisitor = void (*)(const NativeMarkerPoint &, void *);
    void visit_native_markers(int layer, NativeMarkerVisitor visitor, void *context);

    // Live row of the VISIBLE native marker nearest the map reticle, within a
    // pin-sized radius - or nullptr. The reticle is NOT assumed to be the screen
    // centre: it is read from the map dialog when the game exposes it, and inferred
    // (centre vs cursor, measured once per session) only when it cannot be. The
    // native-tooltip proxy: called once per frame from the map hover hook (our raw
    // display objects are invisible to the engine hit test). out_dist2 (optional)
    // receives the canvas-space squared distance for nearest-wins arbitration vs
    // engine pins; out_map_x/out_map_z receive the marker's map-space position (the
    // units of CS::WorldMapPointPinData+0x10, which anchors the engine name panel).
    void *native_reticle_row(float *out_dist2 = nullptr,
                             float *out_map_x = nullptr, float *out_map_z = nullptr);

    // Same canvas-space squared reticle distance for ANY live WorldMapPointParam
    // row (engine pin rows included) - the shared metric of that arbitration.
    // It resolves the anchor through the SAME function native_reticle_row uses,
    // which is the property that matters: until 2026-07-31 this one measured from
    // the view centre while that one had already moved to the live reticle, so the
    // caller compared distances taken from different points and picked the wrong
    // marker at full zoom-out.
    // Precisely: same anchor FUNCTION, evaluated a second time - not the same
    // stored value. On the normal path both evaluations read the same published
    // reticle and agree exactly. They can differ by a pixel or two only in the
    // fallback regime, where the anchor is the OS cursor and the two calls sample
    // it separately; that is well inside the pin-sized pick radius, so it cannot
    // flip the arbitration. Call it once per frame - it is not loop-cheap.
    bool row_reticle_dist2(const void *rowptr, float &out_dist2);

    // Original (pre-remap) row ids of injected markers whose icon is currently HIDDEN
    // for any reason (collected / kindling / manually hidden / a live disable or cleared
    // flag is set). The region-progress tab counts these as done. Reads the LIVE rows so
    // live-loot flag rewrites and manual hide (both keyed on the live param) are seen.
    // A marker off the map only because of its world-state rule is NOT in here: it is not done
    // (the Royal Capital's loot while the Ashen Capital stands is still uncollected).
    std::unordered_set<uint64_t> hidden_marker_original_ids();

    // (is_row_ptr_hidden(void*) was declared here for a hover-tooltip consumer that was never
    //  written. See the note at its former definition in goblin_inject.cpp.)

    // If a category focus is active but NO shown marker remains in it (all collected /
    // hidden), clear the focus. Returns true if it cleared (caller reapplies visibility).
    bool prune_focus_if_empty();

    // The ini config key ("show_*") for a category, or nullptr. Lets the Progress tab
    // reuse the Settings-tab icon + localized entry label for each category.
    const char *category_config_key(generated::Category cat);

    // Data pointers of MFG-injected WorldMapPointParam rows in the expanded
    // table. Populated by inject_map_entries(); consumed by
    // sanitize_injected_textids() after the FMG bank is built.
    const std::vector<uint8_t *> &injected_row_ptrs();

    // Remap every marker whose iconId is a custom icon we injected to that icon's runtime-injected frame
    // (gfx_probe::injected_iconid). Called from the worldmap-load hook after the icon frames are appended,
    // before pins are built. Lets our embedded bitmaps render without shipping a custom gfx, on any base.
    void remap_injected_icons();

    // Rewrites rows baked with a primary completion flag to its alternative
    // once the alternative flag turns on (quest fights with two mutually-
    // exclusive outcome flags, e.g. the Sellen/Jerren academy battle).
    // Called periodically from the refresh loop.
    void apply_flag_or_pairs();

    // Live-loot (config::liveLootFlags): read each loot marker's source
    // ItemLotParam row from live memory and set textDisableFlagId1 to the
    // lot's current getItemFlagId, so markers hide on the actual light-point
    // pickup for the loaded regulation (Item/Enemy Randomizer compatible).
    // One-shot, called once after inject_map_entries().
    void refresh_loot_from_itemlot();

    // Toggle the WorldMapPointParam swap between vanilla and expanded states.
    // Used as an ERSC-hosting workaround: revert before host, re-apply after.
    void set_param_injection_active(bool active);
    // (is_param_injection_active() was declared here and had no callers: menu_auto_toggle_loop
    //  reads the static behind it directly. Its twin setter IS live and stays.)

    // World-state rules (MapEntry::state_flag: Leyndell's capital, flag 300). Reads each flag the
    // rules use and returns how many differ from the watcher's previous poll, recording the new
    // values as it reports them; up to max_out of the changes are described in `out`. Non-zero
    // means apply_category_visibility() has something to do: the shipping build reads the state
    // live in its visibility test anyway, but this is what lets the engine-pin build, which can
    // only be told through param flags, follow a switch too - and what makes an open map merge at
    // once. Plain data and no logging, so the watcher can run it under __try and log outside.
    struct WorldStateChange
    {
        uint32_t flag;
        bool now_on;
        bool was_known;  // false on the first read after the rows were injected: not a change anybody made
        size_t markers;  // how many markers follow this flag
    };
    size_t world_state_poll(WorldStateChange *out, size_t max_out);

    // The BAKED textEnableFlag2Id1..8 of one of our injected rows, by its live (remapped) row id -
    // i.e. what the bake put there (MapEntry::data), not what the live row carries now. false = not
    // one of ours. goblin_logic's post-event gate restores these when it lets go of a row.
    bool injected_row_baked_group2(int32_t row_id, int out[8]);

    // Live marker visibility: sets each injected row's textEnableFlagId1 so the
    // primary line/icon shows only when its category is enabled AND the row is
    // not collected. The engine re-evaluates this flag every frame, so the
    // effect is INSTANT on the open map (no reopen). Call after the overlay
    // changes a toggle, and from the refresh thread when the collected set
    // changes. Idempotent.
    void apply_category_visibility();

    // ---- Manual per-marker hide (hover + hotkey; overlay-managed) ----------
    // Toggle the hidden state of the marker whose live WorldMapPointParam row is
    // `rowptr` (the pin under the cursor, from goblin::maphover::hovered_row()).
    struct ManualHideResult { bool matched = false; bool now_hidden = false; int32_t textId = 0; };
    ManualHideResult toggle_hovered_marker(void *rowptr);
    // Read-only lookup of the hovered marker (for the passive hover-info overlay):
    // matches the live row ptr to one of our injected rows and returns its name
    // textId + source world Y (WorldMapPointParam.posY carries the MSB part Y).
    struct HoveredMarker { bool matched = false; int32_t textId = 0; float posY = 0.0f;
                           uint8_t area = 0; float world_x = 0.0f; float world_z = 0.0f; };
    HoveredMarker hovered_marker(void *rowptr);
    // Snapshot of the hidden set for the overlay manager.
    struct HiddenMarkerInfo { uint64_t key; int32_t textId; uint16_t iconId; int32_t region; uint8_t cat; };
    std::vector<HiddenMarkerInfo> manual_hidden_snapshot();
    size_t manual_hidden_count();
    void unhide_marker(uint64_t key);   // remove one from the hidden set
    void clear_manual_hidden();         // remove all
    void load_manual_hidden(const std::filesystem::path &path);  // low-level read
    void save_manual_hidden(const std::filesystem::path &path);  // low-level write
    void set_hidden_dir(const std::filesystem::path &dir);       // folder for per-slot hide files
    void persist_manual_hidden();                                // save to the current slot's file

    // Active save-slot / profile index (0..9) of the loaded character, or -1 when none is
    // loaded. Read live from GameMan (AOB-resolved slot, "game_man_slot") + 0xAC0
    // ("Save Slot (Profile Index)"). SEH-guarded. Manual hides are scoped per slot.
    int active_save_slot();
    // If the active slot changed since the last call, switch the persist target to that
    // slot's file (MapForGoblins_hidden_s<N>.txt) and reload the hidden set (empty when no
    // character is loaded). Returns true if the set changed (caller should reapply
    // visibility). Call from the watcher loop.
    bool sync_hidden_slot();

    // Force World Map fragment markers visible regardless of require_map_fragments
    // (config::worldMapsIgnoreFragments). Call AFTER apply_map_logic. Idempotent.
    void apply_worldmap_fragment_bypass();

    // Live re-apply of hide_killed_bosses across boss/hawk/hostile-NPC rows.
    void apply_kill_display();

    // Re-apply ALL live-capable settings at once (show_* + hide_killed_bosses +
    // require_map_fragments + ERR patch markers). Call from the overlay on a
    // settings change; effective on next world-map open. (anonymous_loot /
    // live_loot_* are not live - they rewrite markers from ItemLotParam.)
    void reapply_live_settings();

    // Master show/hide of ALL icons (the former F10 behavior), now driven from
    // the in-game overlay. The watcher (menu_auto_toggle_loop) applies it.
    void set_icons_hidden(bool hidden);
    bool icons_hidden();

    // Bumped by anything that changes which markers SHOULD be visible: the master switch and
    // every live-settings re-apply. The native-marker manager watches it so a toggle reaches
    // children that ALREADY exist without waiting for the build queue - see the tick, where
    // the periodic refresh is deliberately gated on a spent queue and this is not.
    uint32_t visibility_epoch();

    // Same bump, for a caller that has ALREADY rewritten the rows and only needs the tick to
    // notice. The background watcher is the one: it compares the collected/kindling counts every
    // two seconds and re-applies category visibility on a change, but it used to do that
    // silently, so the only thing that ever picked the change up was the tick's periodic
    // re-snapshot. That poll cost 4.3 ms of one map frame EVERY second (measured on
    // convergence3, 8425 rows, 2026-08-07) to look for something it usually could not find -
    // in single-player the world is paused while the map is up, so nothing can be collected at
    // all. With this the change announces itself, the tick merges on the next frame instead of
    // up to a second later, and the poll drops to a rare safety net.
    void note_visibility_changed();

    // Native settings-menu texts: GR_MenuText.fmg ids allocated dynamically
    // above the live max by setup_messages (same principle as the marker textId remap).
    // The native menu (goblin_stall_probe) builds its tab category label and its
    // page row labels via the game's MenuTextCtor against these ids. 0/empty
    // until allocated / if the merge failed.
    extern int g_menutext_tab_id;
    extern std::vector<int> g_menutext_row_ids; // parallel to native_menu_rows()
    // Value texts for rows rendered on the native keybinding form (its rows show a
    // name plus a VALUE column): localized "On"/"Off". 0 if the merge failed.
    extern int g_menutext_on_id;
    extern int g_menutext_off_id;

    // Rows of our native settings page: each a native checkbox bound directly to
    // a config bool. ini_key doubles as the i18n entry_labels lookup key (the
    // localized label text injected into GR_MenuText). Defined in
    // goblin_stall_probe.cpp (the native-menu owner).
    struct NativeMenuRowDef
    {
        const char *ini_key;
        bool *value;
        bool def; // schema default (drives the row's "modified" indicator)
    };
    const NativeMenuRowDef *native_menu_rows(size_t *count);

    // Background thread polling the toggle hotkey.
    void toggle_hotkey_loop();

    // Background thread owning the WorldMapPointParam expand/revert state. It
    // applies the F10/gamepad personal show/hide and shows the toggle banner.
    void menu_auto_toggle_loop();
};
