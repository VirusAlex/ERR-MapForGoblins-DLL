#pragma once

#include <cstdint>
#include <filesystem>
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

    // Swap focused rows to the glow-highlight icon (and others back to normal).
    // Called by reapply_live_settings() and after remap_injected_icons().
    void apply_focus_highlight();

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
    bool is_param_injection_active();

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
    struct HoveredMarker { bool matched = false; int32_t textId = 0; float posY = 0.0f; };
    HoveredMarker hovered_marker(void *rowptr);
    // Snapshot of the hidden set for the overlay manager.
    struct HiddenMarkerInfo { uint64_t key; int32_t textId; uint16_t iconId; int32_t region; uint8_t cat; };
    std::vector<HiddenMarkerInfo> manual_hidden_snapshot();
    size_t manual_hidden_count();
    void unhide_marker(uint64_t key);   // remove one from the hidden set
    void clear_manual_hidden();         // remove all
    void load_manual_hidden(const std::filesystem::path &path);  // at startup
    void save_manual_hidden(const std::filesystem::path &path);  // low-level write
    void set_hidden_file(const std::filesystem::path &path);     // set persist target once
    void persist_manual_hidden();                                // save to the set target

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

    // Codex-toast ids, allocated DYNAMICALLY at runtime above the live max so they
    // never collide with an overhaul's / another mod's tutorial content (same
    // principle as the marker textId remap). Two independent id spaces:
    //   g_toast_fmg_id[]       = TutorialBody.fmg text ids (set by setup_messages,
    //                            above the live TutorialBody max). The TutorialParam
    //                            row's textId field points here, and the FMG text
    //                            lives here.
    //   g_toast_param_row_id[] = TutorialParam row ids (set by inject_tutorial_popup_rows,
    //                            above the live TutorialParam max). show_codex_toast
    //                            triggers a banner by one of THESE.
    // setup_messages MUST run before inject_tutorial_popup_rows (the popup rows
    // point at the fmg ids). 0 until allocated.
    enum ToastSlot { TOAST_ON = 0, TOAST_OFF, TOAST_DUMP_OK, TOAST_DUMP_FAIL, TOAST_COUNT };
    extern int g_toast_fmg_id[TOAST_COUNT];
    extern int g_toast_param_row_id[TOAST_COUNT];

    // Inject the codex-toast TutorialParam rows for the F10/F9 banners. Rows
    // get menuType=0 (upper-left codex caption widget) with textId pointing
    // at TutorialBody.fmg entries injected by goblin_messages.
    // Returns true on success.
    bool inject_tutorial_popup_rows();

    // Fire an upper-left codex-style toast for one of the injected TutorialParam
    // rows (pass a goblin::g_toast_param_row_id[...] value). Static text, no FMG
    // rewrite - same path as the F10 banner. Safe from any thread once init has run.
    void show_codex_toast(int tutorial_id);

    // Background thread polling the toggle hotkey.
    void toggle_hotkey_loop();

    // Background thread owning the WorldMapPointParam expand/revert state. It
    // applies the F10/gamepad personal show/hide and shows the toggle banner.
    void menu_auto_toggle_loop();
};
