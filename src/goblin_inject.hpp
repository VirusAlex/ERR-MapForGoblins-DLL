#pragma once

#include <cstdint>
#include <vector>

namespace goblin
{
    void inject_map_entries();

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
