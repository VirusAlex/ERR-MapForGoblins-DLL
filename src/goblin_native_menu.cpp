#include "goblin_native_menu.hpp"

#include "version.h" // PROJECT_VERSION (generated) for the About page

#include "goblin_own_movie.hpp"

#include "goblin_config.hpp"
#include "goblin_config_schema.hpp"
#include "goblin_float_ranges.hpp" // slider ranges shared with the overlay
#include "goblin_i18n.hpp"
#include "goblin_inject.hpp"
#include "goblin_build_variants.hpp" // MFG_MENU_ADDON_HOST - do not rely on an undefined macro
#if MFG_MENU_ADDON_HOST
#include "goblin_menu_addons.hpp"
#endif
#include "goblin_gfx_probe.hpp"
#include "goblin_markers.hpp"
#include "goblin_messages.hpp" // lookup_text() names the hidden markers
#include "goblin_progress.hpp"
#include "goblin_search.hpp"   // the item-search page
#include "goblin_overlay.hpp"  // text_layout_tag: the layout the search page types with
#include "generated_shared/goblin_overlay_icons.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include <windows.h>

namespace
{
    namespace tr = goblin::i18n;
    // ui_language covers this screen too (it used to follow the game's language alone). Every
    // lookup below still passes it explicitly rather than leaning on the i18n default, so the
    // whole menu is built from ONE reading of the setting even if it changes mid-build.
    inline tr::Language mlang() { return tr::current_language(); }
    using goblin::nmenu::Row;
    using goblin::nmenu::RowKind;

    // ── model state ──────────────────────────────────────────────────────────────────
    std::vector<Row> g_rows;
    std::deque<std::wstring> g_arena; // stable backing for every label/value pointer
    std::wstring g_title;
    int32_t g_page = goblin::nmenu::kPageRoot;
    std::vector<int32_t> g_stack; // pages we came from, for Back
    // Which page is being PREVIEWED in the right-hand column (-1 = none). Confirming a
    // sub-page row previews it here first; confirming the same row again enters it. The form
    // is 11 rows x 2 columns and the right clips only carry a value field, so a preview row
    // is one line of text.
    int32_t g_preview_page = -1;
    std::vector<Row> g_preview_rows;
    std::deque<std::wstring> g_preview_arena;
    bool g_dirty = false;

    // The entry a value page / rebind page is editing. Pointers into the ini schema, which
    // outlives every page, so holding them across a rebuild is safe.
    const goblin::IniEntry *g_edit_entry = nullptr;

    // ── screen-per-page mode ─────────────────────────────────────────────────────────
    // On: every page is a real native screen owned by the host, so the model must not move
    // g_page by itself - it only reports what the host should open or close.
    bool g_nested = false;
    int32_t g_child_page = -1; // page to open as a child screen
    bool g_want_close = false; // this screen is finished

    const wchar_t *hold(std::wstring s)
    {
        g_arena.push_back(std::move(s));
        return g_arena.back().c_str();
    }

    std::wstring wide(const char *u8)
    {
        std::wstring w;
        if (!u8 || !*u8)
            return w;
        const int wl = MultiByteToWideChar(CP_UTF8, 0, u8, -1, nullptr, 0);
        if (wl > 1)
        {
            w.resize(static_cast<size_t>(wl) - 1);
            MultiByteToWideChar(CP_UTF8, 0, u8, -1, w.data(), wl);
        }
        return w;
    }

    const wchar_t *text(tr::TextId id) { return hold(wide(tr::tr(id, mlang()))); }

    // ── numeric row metadata ─────────────────────────────────────────────────────────
    // The ini schema stores defaults as text and has no range info, so ranges for the
    // rows a player can step live here, keyed by ini key. Anything not listed is shown
    // read-only (the value is still visible, it just cannot be changed on this screen).
    // Ranges come from the table the overlay's sliders read too (goblin_float_ranges.hpp): two menus
    // disagreeing about the limits of one setting is a bug report waiting to happen. A Float entry
    // without a range (the overlay WINDOW geometry, overlay_window_x/y/w/h) stays a read-only Info row.
    using NumRange = goblin::FloatRange;
    const NumRange *range_for(const char *key) { return goblin::float_range(key); }

    // The two ini values with a fixed option list (both mirror the overlay's combos).
    const char *const kLanguages[] = {"auto",    "english", "schinese", "tchinese", "korean",
                                      "russian", "german",  "french",   "spanish", "vietnamese"};

    // ── markup ───────────────────────────────────────────────────────────────────────
    // The row's text fields are html=1 EditText, and the setter the mod already uses
    // (RVA 0x74A000 -> FUN_140D842A0) passes isHtml=1 unconditionally - so <font>, <b>
    // and friends work with no extra plumbing (recon_draw_primitives.md section 2).
    // Row colour is UNCONDITIONAL - there is no switch. The toggle that used to gate it existed so
    // one in-game run could compare markup on vs off; the run happened, colour won, and the toggle
    // went. The fields are html=1, so a <font> tag is the only way to paint text here.
    std::wstring colored(const std::wstring &text, uint32_t rgb)
    {
        wchar_t open[32];
        _snwprintf_s(open, _TRUNCATE, L"<font color=\"#%06X\">", rgb & 0xFFFFFF);
        return std::wstring(open) + text + L"</font>";
    }

    constexpr uint32_t kColOn = 0x7FD97F;    // green - enabled
    constexpr uint32_t kColOff = 0x9A9A9A;   // grey - disabled
    constexpr uint32_t kColValue = 0xE8D9A0; // parchment - neutral value
    // (a kColBarOn sat here, identical to kColOn and never used - bar_render draws with
    // kColBarDone / kColBarPart)
    constexpr uint32_t kColBarOff = 0x50504A;
    // The mod's name as it should read on screen, in one place.
    const wchar_t *const kProductName = L"Map for Goblins";

    constexpr uint32_t kColFocus = 0xE86A6A;  // the category isolated on the map right now
    constexpr uint32_t kColHeader = 0x9ED1FF; // mega-section captions, as in the overlay

    // ── per-section marker glyph ─────────────────────────────────────────────────────
    // Real per-category bitmaps need an image clip the 02_160 row does not have (icons_*
    // recon is running). Until that lands, each section gets a coloured glyph so
    // categories stay distinguishable. Only code points VERIFIED present in the menu font
    // (font/eu_std/font.gfx, "Agmena W1G", 910 glyphs) are used: U+25A0 SQUARE,
    // U+25CF CIRCLE, U+25C6 DIAMOND, U+2605 STAR, U+25B2 TRIANGLE, U+2022 BULLET.
    // ONE declaration of the glyph, used everywhere it is needed. A hand-repeated escape is
    // how the bullet once shipped as a bare " 22" - the backslash was lost in an edit and every
    // icon-less hidden-marker row drew " 22" in front of its name. An escape that exists in
    // exactly one place cannot lose its backslash in nine others.
    constexpr wchar_t kBulletCh = L'\x2022';
    constexpr const wchar_t *kBullet = L"\x2022";

    struct SectionMark
    {
        const char *section;
        wchar_t glyph;
        uint32_t rgb;
    };
    // In-game truth (screenshots 2026-07-25): this client's menu font draws U+2022
    // BULLET but NOT the geometric shapes (U+25A0/25C6/25CF/25B2/2605 all rendered as
    // tofu boxes). So every section uses the bullet and is distinguished by COLOUR.
    constexpr SectionMark kMarks[] = {
        {"Goblin", kBulletCh, 0xE8D9A0},    {"Equipment", kBulletCh, 0x9FC6E8},
        {"Key Items", kBulletCh, 0xE8C86A}, {"Loot", kBulletCh, 0xC8E89A},
        {"Magic", kBulletCh, 0xC9A0E8},     {"Quest", kBulletCh, 0xE8A0A0},
        {"Reforged", kBulletCh, 0xE8B080},  {"World", kBulletCh, 0x9AD8C0},
        {"ERR Markers", kBulletCh, 0xD0A0E8},
    };
    // ini key -> source icon id, via the same atlas mapping the overlay draws from
    // (generated_shared/goblin_overlay_icons: ICON_CELLS gives the cell, CELL_SRC_ICON the
    // icon id). -1 when the key has no icon.
    int32_t icon_for_key(const char *key)
    {
        if (!key)
            return -1;
        namespace oi = goblin::overlay_icons;
        for (int i = 0; i < oi::ICON_CELL_COUNT; ++i)
        {
            if (std::strcmp(oi::ICON_CELLS[i].key, key) != 0)
                continue;
            const int cell = oi::ICON_CELLS[i].row * (oi::ATLAS_W / oi::CELL) + oi::ICON_CELLS[i].col;
            if (cell >= 0 && cell < oi::ATLAS_CELL_COUNT)
                return oi::CELL_SRC_ICON[cell];
            return -1;
        }
        return -1;
    }

    // section_icon_entry() lived here: it picked a representative icon for a whole section (the
    // first entry that had one). It lost its caller when the page rows stopped borrowing an icon
    // from their first entry - a loot icon standing for dozens of settings said the wrong thing.

    // The glyph is a STAND-IN for a picture. Where a real icon draws, showing both put a coloured
    // dot right next to the image that replaced it; where there is no icon, the row still needs its
    // marker. That decision is the CALLER's - mark_for only maps a section to its glyph and colour;
    // the "does this row have an icon" test lives at the call site (push_entry_row, r.icon_id < 0).
    const SectionMark *mark_for(const char *section)
    {
        if (!section)
            return nullptr;
        for (const auto &m : kMarks)
            if (std::strcmp(m.section, section) == 0)
                return &m;
        return nullptr;
    }

    // ── value formatting ─────────────────────────────────────────────────────────────
    // The value field (Text_1, cid 185) is 260 px wide and the row font is 22 px (kRowFontTwips in
    // goblin_own_movie.cpp - THE definition of that number; this comment carried a stale 21 that was
    // already corrected at the other copy). Roughly a quarter more characters fit than when the
    // eight-cell bar was chosen. How many EXACTLY is a question for the eye, not for arithmetic - the
    // font is proportional, so a digit, a '#' and a '.' are all different widths. Hence the styles
    // below; pick what reads best in game, then set kBarStyle to it. (The comparison rulers this note
    // used to send you to were removed from the Debug page on 2026-07-29.)
    constexpr uint32_t kColBarDone = 0x8CE68C; // a finished zone reads green, numbers and all
    // Anything short of finished is yellow. Green used to mean "some of it is done", which is the one
    // thing a colour should not say when green also means finished two rows below.
    constexpr uint32_t kColBarPart = 0xE8D96A;
    constexpr int kBarStyle = 9;

    // Cells filled, rounded to NEAREST rather than up: with ceiling, one collected marker out of a
    // hundred already lit a cell, which read as progress that was not there. Nothing but a complete
    // zone may show a full bar, and nothing but an empty one may show none.
    int bar_filled(int collected, int total, int width)
    {
        if (total <= 0 || collected <= 0)
            return 0;
        if (collected >= total)
            return width;
        int n = static_cast<int>((static_cast<double>(collected) / total) * width + 0.5);
        return std::clamp(n, 1, width - 1);
    }

    std::wstring bar_render(int collected, int total, int style)
    {
        const bool done = total > 0 && collected >= total;
        wchar_t head[40];
        _snwprintf_s(head, _TRUNCATE, L"%d/%d", collected, total);
        const int pct = total > 0 ? static_cast<int>((100.0 * collected) / total + 0.5) : 0;
        auto cells = [&](int width, wchar_t on, wchar_t off, uint32_t con, uint32_t coff) {
            const int f = bar_filled(collected, total, width);
            std::wstring b = colored(std::wstring(static_cast<size_t>(f), on), con);
            if (f < width)
                b += colored(std::wstring(static_cast<size_t>(width - f), off), coff);
            return b;
        };
        const uint32_t on_col = done ? kColBarDone : kColBarPart;
        switch (style)
        {
        case 0: // as it was: numbers, then eight cells
            return std::wstring(head) + L" " + cells(8, L'#', L'-', on_col, kColBarOff);
        case 1: // numbers, then a bracketed sixteen-cell bar - the wider bar the field can now hold
            return colored(std::wstring(head), done ? kColBarDone : kColValue) + L" " +
                   colored(L"[", kColBarOff) + cells(16, L'=', L'-', on_col, kColBarOff) +
                   colored(L"]", kColBarOff);
        case 2: // percent first, for comparing zones rather than counting markers
            {
                wchar_t pc[16];
                _snwprintf_s(pc, _TRUNCATE, L"%3d%% ", pct);
                return colored(std::wstring(pc), done ? kColBarDone : kColValue) +
                       cells(14, L'=', L'.', on_col, kColBarOff);
            }
        case 3: // numbers only, as large as they get - the "how much room is there" case
            return colored(std::wstring(head), done ? kColBarDone : kColValue);
        case 4: // numbers, percent, and a short bar: everything, if it fits
            {
                wchar_t all[64];
                _snwprintf_s(all, _TRUNCATE, L"%d/%d %d%%", collected, total, pct);
                return colored(std::wstring(all), done ? kColBarDone : kColValue) + L" " +
                       cells(6, L'#', L'-', on_col, kColBarOff);
            }
        case 5: // pipes and dots - a lighter texture than '=' at the same cell count
            return colored(std::wstring(head), done ? kColBarDone : kColValue) + L" " +
                   cells(16, L'|', L'.', on_col, kColBarOff);
        case 6: // no numbers at all: the widest bar the column can take
            return cells(24, L'=', L'-', on_col, kColBarOff);
        // 7..12 exist to be COMPARED, not chosen blind: the field clips on pixel width, and these
        // glyphs are 1.5x apart in width, so the same cell count reads very differently. Same numbers
        // in front for all of them, so only the bar differs.
        case 7:
            return colored(std::wstring(head), done ? kColBarDone : kColValue) + L" " +
                   cells(20, L'#', L'-', on_col, kColBarOff);
        case 8:
            return colored(std::wstring(head), done ? kColBarDone : kColValue) + L" " +
                   cells(24, L'|', L'-', on_col, kColBarOff);
        case 9: // CHOSEN. The empty cell is U+00B7 MIDDLE DOT, not a full stop: the dot sits on the
                // same line as the colons instead of on the baseline. 26 cells measured to fit the
                // 260 px column with the counts still in front of them.
            return colored(std::wstring(head), done ? kColBarDone : kColValue) + L" " +
                   cells(26, L':', L'·', on_col, kColBarOff);
        case 10:
            return colored(std::wstring(head), done ? kColBarDone : kColValue) + L" " +
                   cells(20, L'+', L'-', on_col, kColBarOff);
        case 11: // Latin-1: the base font carries 0x20-0xFF, so these draw
            return colored(std::wstring(head), done ? kColBarDone : kColValue) + L" " +
                   cells(24, L'¦', L'·', on_col, kColBarOff);
        case 12:
            return colored(std::wstring(head), done ? kColBarDone : kColValue) + L" " +
                   cells(20, L'°', L'·', on_col, kColBarOff);
        default:
            return std::wstring(head);
        }
    }

    // The help block under the list is 1670 px wide and reads at 18 px, so it can carry a bar with a
    // hundred cells - fine detail the 260 px value column cannot show. Progress rows put one there.
    std::wstring long_bar(int collected, int total)
    {
        const bool done = total > 0 && collected >= total;
        const int width = 100;
        const int f = bar_filled(collected, total, width);
        const int pct = total > 0 ? static_cast<int>((100.0 * collected) / total + 0.5) : 0;
        wchar_t head[48];
        _snwprintf_s(head, _TRUNCATE, L"%d/%d  ", collected, total);
        std::wstring out = colored(std::wstring(head), done ? kColBarDone : kColValue);
        // Same glyph pair as the rows, so the two readings of the same number look like each other.
        out += colored(std::wstring(static_cast<size_t>(f), L':'), done ? kColBarDone : kColBarPart);
        if (f < width)
            out += colored(std::wstring(static_cast<size_t>(width - f), L'·'), kColBarOff);
        wchar_t tail[16];
        _snwprintf_s(tail, _TRUNCATE, L"  %d%%", pct); // after the bar: the eye lands on the bar first
        out += colored(std::wstring(tail), done ? kColBarDone : kColValue);
        return out;
    }

    std::wstring bar_text(int collected, int total) { return bar_render(collected, total, kBarStyle); }

    // Is this zone finished? Its label goes green as well, so a completed zone reads as done from the
    // label column without looking at the numbers.
    bool zone_done(int collected, int total) { return total > 0 && collected >= total; }

    std::wstring value_of(const goblin::IniEntry &e)
    {
        wchar_t buf[96] = {};
        switch (e.type)
        {
        case goblin::IniType::Bool:
        {
            const bool on = e.target && *static_cast<bool *>(e.target);
            return colored(wide(tr::tr(on ? tr::TextId::ValueOn : tr::TextId::ValueOff, mlang())),
                           on ? kColOn : kColOff);
        }
        case goblin::IniType::Float:
            if (e.target)
                _snwprintf_s(buf, _TRUNCATE, L"%.2f", *static_cast<float *>(e.target));
            break;
        case goblin::IniType::U8:
            if (e.target)
                _snwprintf_s(buf, _TRUNCATE, L"%u",
                             static_cast<unsigned>(*static_cast<uint8_t *>(e.target)));
            break;
        case goblin::IniType::VkKey:
            // The same spelling the ini uses ("F10", "Home", ...) rather than a raw code -
            // config.cpp already owns that formatting for saving, so reuse it.
            if (e.target)
                return wide(goblin::format_vk_code(*static_cast<uint32_t *>(e.target)).c_str());
            break;
        case goblin::IniType::GamepadMask:
            if (e.target)
                return wide(goblin::format_gamepad_combo(*static_cast<uint16_t *>(e.target))
                                .c_str());
            break;
        case goblin::IniType::Language:
            if (e.target)
            {
                const auto *s = static_cast<std::string *>(e.target);
                return wide(tr::language_option_label(*s));
            }
            break;
        case goblin::IniType::Text:
            if (e.target)
                return wide(static_cast<std::string *>(e.target)->c_str());
            break;
        }
        return buf;
    }

    // The schema entry behind a row. Rows carry only the key (they are rebuilt constantly),
    // and the schema is a stable global, so this is the safe way back to the definition.
    const goblin::IniEntry *entry_for_key(const char *key)
    {
        if (!key)
            return nullptr;
        for (const auto &sec : goblin::ini_schema())
            for (const auto &e : sec.entries)
                if (std::strcmp(e.key, key) == 0)
                    return &e;
        return nullptr;
    }

    RowKind kind_of(const goblin::IniEntry &e)
    {
        if (e.type == goblin::IniType::Bool)
            return RowKind::Toggle;
        if (e.type == goblin::IniType::Language)
            return RowKind::Enum;
        // Both binding kinds open the same "press it" page; build_rebind and the poller branch on
        // the entry's type. Until 2026-08-01 a GamepadMask fell through to Info, so the menu showed
        // the button and refused to change it - the reporter hit hide_marker_gamepad (default RB,
        // which is also the map's tab-switch button) all session with no way out.
        if (e.type == goblin::IniType::VkKey || e.type == goblin::IniType::GamepadMask)
            return RowKind::Rebind;
        if (range_for(e.key))
            return RowKind::Slider;
        return RowKind::Info;
    }

    // ── slider row: what its value column shows ──────────────────────────────────────
    // The current value of a slider row, both as text and as a 0..1 fraction. The BAR is
    // not text any more: the host draws the game's own slider look from the fraction (the
    // "MfgSlider" strip spliced into the row clip - see generate_menu_icon_tags.py), so
    // the value column carries only the number, right-aligned clear of the bar. A first
    // text-bar round (':' / '·' glyphs) read as a progress bar, not a slider - reported in
    // game 2026-08-04 and replaced the same day. The number drops its decimals when the
    // whole range walks on integers ("100%"), and keeps them when it does not ("1.20").
    std::wstring slider_text(const goblin::IniEntry &e, const NumRange &r, float *out_frac)
    {
        const float v = !e.target ? r.min
                        : e.type == goblin::IniType::Float
                            ? *static_cast<float *>(e.target)
                            : static_cast<float>(*static_cast<uint8_t *>(e.target));
        float f = r.max > r.min ? (v - r.min) / (r.max - r.min) : 0.f;
        if (f < 0.f)
            f = 0.f;
        if (f > 1.f)
            f = 1.f;
        if (out_frac)
            *out_frac = f;
        const bool whole = r.step == static_cast<float>(static_cast<int>(r.step)) &&
                           r.min == static_cast<float>(static_cast<int>(r.min));
        wchar_t num[32];
        _snwprintf_s(num, _TRUNCATE, whole ? L"%.0f%s" : L"%.2f%s", v, r.suffix);
        return colored(num, kColValue);
    }

    // ── the choices behind one entry ─────────────────────────────────────────────────
    // Both value kinds boil down to "a list of options with one of them current", which is
    // what the value page renders. Numbers get their list from the range, enums from their
    // fixed table - so the page itself needs no per-type code.
    // Since 2026-08-04 the NUMBER half of these helpers is unreachable: range keys are
    // Slider rows stepped in place, and only an Enum still opens the value page (itself a
    // page short of reachable - see the RowKind note in the header). The range branches
    // stay with the Enum machinery they are interleaved with.
    size_t option_count(const goblin::IniEntry &e)
    {
        if (e.type == goblin::IniType::Language)
            return sizeof(kLanguages) / sizeof(kLanguages[0]);
        const NumRange *r = range_for(e.key);
        if (!r || r->step <= 0.f)
            return 0;
        return static_cast<size_t>((r->max - r->min) / r->step + 0.5f) + 1;
    }

    std::wstring option_label(const goblin::IniEntry &e, size_t index)
    {
        if (e.type == goblin::IniType::Language)
            return wide(tr::language_option_label(kLanguages[index]));
        const NumRange *r = range_for(e.key);
        if (!r)
            return L"";
        wchar_t buf[32];
        const float v = r->min + r->step * static_cast<float>(index);
        if (e.type == goblin::IniType::Float)
            _snwprintf_s(buf, _TRUNCATE, L"%.2f", v);
        else
            _snwprintf_s(buf, _TRUNCATE, L"%d", static_cast<int>(v + 0.5f));
        return buf;
    }

    // Which option is live right now (-1 when the value sits between steps).
    int current_option(const goblin::IniEntry &e)
    {
        if (!e.target)
            return -1;
        if (e.type == goblin::IniType::Language)
        {
            const std::string cur = tr::normalize_language_config(*static_cast<std::string *>(e.target));
            for (size_t i = 0; i < sizeof(kLanguages) / sizeof(kLanguages[0]); ++i)
                if (cur == kLanguages[i])
                    return static_cast<int>(i);
            return -1;
        }
        const NumRange *r = range_for(e.key);
        if (!r || r->step <= 0.f)
            return -1;
        const float v = e.type == goblin::IniType::Float
                            ? *static_cast<float *>(e.target)
                            : static_cast<float>(*static_cast<uint8_t *>(e.target));
        const int ix = static_cast<int>((v - r->min) / r->step + 0.5f);
        return ix >= 0 && static_cast<size_t>(ix) < option_count(e) ? ix : -1;
    }

    void apply_option(const goblin::IniEntry &e, size_t index)
    {
        if (!e.target || index >= option_count(e))
            return;
        if (e.type == goblin::IniType::Language)
            *static_cast<std::string *>(e.target) = kLanguages[index];
        else if (const NumRange *r = range_for(e.key))
        {
            const float v = r->min + r->step * static_cast<float>(index);
            if (e.type == goblin::IniType::Float)
                *static_cast<float *>(e.target) = v;
            else
                *static_cast<uint8_t *>(e.target) = static_cast<uint8_t>(v + 0.5f);
        }
        g_dirty = true;
        goblin::reapply_live_settings();
    }

    bool entry_visible(const goblin::IniEntry &e)
    {
        if (e.ini_only) // decided in the ini alone - see IniEntry::ini_only
            return false;
        return !(e.err_only && goblin::profile_is_vanilla());
    }
    bool section_visible(const goblin::IniSection &s)
    {
        return !(s.err_only && goblin::profile_is_vanilla());
    }

    // ── actions ──────────────────────────────────────────────────────────────────────
    // A set_section_bools(bool) with all-on / all-off wrappers lived here and had no callers and
    // no Action rows pointing at it. Its index arithmetic had also gone stale: it indexed
    // ini_schema() by (g_page - kPageSectionBase), which was true when a page WAS a schema
    // section, but pages index kLayout now (4 entries against 12 schema sections), so a bulk
    // toggle would have applied to whichever section happened to sit at that index.
    void action_unhide_all()
    {
        const size_t n = goblin::manual_hidden_count();
        goblin::clear_manual_hidden();
        // Written down like the other two unhide paths (the overlay's and the per-row one). This
        // one never was: the list emptied, the file kept every entry, and the next launch
        // brought all of them back hidden (2026-09-15, five markers on slot 1).
        goblin::persist_manual_hidden();
        goblin::reapply_live_settings();
        spdlog::info("[nmenu] unhid {} manually hidden markers", n);
    }

    // ── page builders ────────────────────────────────────────────────────────────────
    void push(Row row) { g_rows.push_back(row); }

    // ── menu layout ─────────────────────────────────────────────────────────────────
    // Pages, in order. A page is either a CONCATENATION of ini sections (each preceded by a
    // valueless Info row, which is what makes the row clip draw it on the PadCategory frame) or an
    // explicit ordered KEY list, which is how a page shows only some of a section's entries.
    // Page id = kPageSectionBase + index into this table, so all the existing id arithmetic and the
    // host's set_page arithmetic keeps working unchanged.
    //
    // Keys deliberately absent from every page: `native_menu` (it gates the menu itself - switching
    // it off from inside would be a trap) and the overlay-only settings (window geometry, opacity,
    // font scale, render mode), which say nothing about the native menu.
    struct LayoutPage
    {
        const char *label;             // section name used for the title/label lookup
        const char *const *sections;   // sections to concatenate, each with a separator
        size_t section_count;
        const char *const *keys;       // ...or an explicit ordered key list
        size_t key_count;
        bool toggle_all;               // add the aggregate "all icon categories" row
        bool dump_rows;                // add the dump / copy rows (Debug)
    };

    const char *const kCategorySections[] = {"Equipment", "Key Items", "Loot",     "Magic",
                                             "Quest",     "World",     "Reforged", "ERR Markers"};
    const char *const kCompatSections[] = {"Compatibility"};
    const char *const kDebugSections[] = {"Debug"};
    // The menu-settings page: only what actually concerns the menu and the hotkeys.
    // ui_language leads: this screen now speaks it too (see mlang()), so the row that changes it
    // has to be reachable from the screen it changes.
    const char *const kMenuSettingKeys[] = {
        "ui_language",
        "enable_toggle_hotkey", "toggle_key",          "toggle_gamepad_combo",
        "enable_manual_hide",   "hide_marker_key",     "hide_marker_gamepad", "hover_info",
        // Right after hover_info on purpose: it positions the very panels that setting turns on,
        // and a player who has just found the tooltip in the wrong place looks for the fix here.
        // A key in kRanges only gets a numeric range - it appears on a page ONLY if it is listed
        // here, which is why adding the range alone showed nothing in game.
        "map_panel_offset_percent",
    };

    const LayoutPage kLayout[] = {
        {"Categories", kCategorySections, 8, nullptr, 0, true, false},
        {"Compatibility", kCompatSections, 1, nullptr, 0, false, false},
        {"Menu settings", nullptr, 0, kMenuSettingKeys,
         sizeof(kMenuSettingKeys) / sizeof(kMenuSettingKeys[0]), false, false},
        {"Debug", kDebugSections, 1, nullptr, 0, false, true},
    };
    constexpr size_t kLayoutCount = sizeof(kLayout) / sizeof(kLayout[0]);
    // Named so the root page and the dispatch cannot drift apart.
    constexpr int32_t kPageCategories = goblin::nmenu::kPageSectionBase + 0;

    // Rows the player must see but must not change from here: shown with their value, as an Info
    // row, which the host draws on the Grayout frame - so "disabled" needs no new row kind.
    bool key_is_readonly(const char *key)
    {
        // enable_toggle_hotkey does NOT gate the key that opens this menu - that path reads
        // toggleInjectionKey / toggleGamepadMask directly (goblin_stall_probe, the native-mode
        // poll). The flag is the master switch of the ICON toggle hotkey, read by dllmain and by
        // toggle_hotkey_loop in goblin_inject. It is read-only here for the same reason the
        // overlay greys it (goblin_overlay.cpp, the `locked` pair): a master switch that turns
        // off the very input surface you would need to turn it back on does not belong on a row
        // the player can flip in passing.
        return std::strcmp(key, "enable_toggle_hotkey") == 0;
    }

    // (entry_for_key() stood here with a body identical to entry_for_key() above, minus the null
    //  guard. Two wrappers over one double loop, with the call sites split between them purely by
    //  which one happened to be in scope when each was written. Folded into entry_for_key.)

    // One schema entry -> one row. Shared by every page so the pages cannot disagree about how an
    // entry looks; `section` only decides the colour of the fallback bullet.
    void push_entry_row(const goblin::IniEntry &e, const char *section)
    {
        std::wstring label = wide(tr::entry_label(e.key, mlang()));
        if (label.empty())
            label = wide(e.key);
        const int32_t row_icon = icon_for_key(e.key);
        if (const SectionMark *mk = section ? mark_for(section) : nullptr)
            if (std::strncmp(e.key, "show_", 5) == 0 && row_icon < 0)
                label = colored(std::wstring(1, mk->glyph), mk->rgb) + L"  " + label;
        Row r;
        r.kind = key_is_readonly(e.key) ? RowKind::Info : kind_of(e);
        {
            std::wstring tip = wide(tr::entry_comment(e.key, e.comment ? e.comment : "", mlang()));
            if (!tip.empty())
                r.help = hold(std::move(tip));
        }
        r.label = hold(std::move(label));
        // A slider row shows its number and carries the bar fraction for the host's
        // native-look strip; everything else keeps the schema formatting.
        // (key_is_readonly rows are Info by the line above, so they take the plain branch.)
        const NumRange *nr = r.kind == RowKind::Slider ? range_for(e.key) : nullptr;
        r.value = hold(nr ? slider_text(e, *nr, &r.slider_frac) : value_of(e));
        r.ini_key = e.key;
        r.icon_id = row_icon;
        r.target = e.target;
        r.type_tag = static_cast<uint8_t>(e.type);
        push(r);
    }

    void push_separator(const char *section)
    {
        std::wstring name = wide(tr::section_label(section, mlang()));
        if (name.empty())
            name = wide(section);
        Row h;
        h.kind = RowKind::Info; // valueless -> PadCategory frame, one wide caption
        h.label = hold(colored(std::move(name), kColHeader));
        push(h);
    }

    // A "show_" PREFIX is not the same thing as an icon category, and treating it as one was a bug
    // with two symptoms. show_world_maps_ignore_fragments is a MODIFIER - it clears the map-fragment
    // gate on the World Maps rows - yet it matched the prefix, so the aggregate toggle at the top of
    // the Categories page switched it along with everything else while turning the very same
    // categories off by hand never touched it. That is exactly how the two paths came to disagree:
    // "disable all" also silently dropped the fragment bypass, and re-enabling categories one by one
    // never brought it back. It also made the aggregate miscount its own state (57 keys where there
    // are 56 categories), so "are they all on?" could answer wrongly.
    //
    // Answer from the REAL category list instead. category_config_key() is the one place that maps a
    // Category to its ini key and returns nullptr for everything else, so a future non-category
    // show_* key cannot reintroduce this. Category is a uint8_t enum, so 0..255 covers it whole.
    bool is_category_key(const char *key)
    {
        if (!key)
            return false;
        static const std::vector<std::string> keys = [] {
            std::vector<std::string> out;
            for (int i = 0; i < 256; ++i)
                if (const char *k = goblin::category_config_key(
                        static_cast<goblin::generated::Category>(i)))
                    out.emplace_back(k);
            return out;
        }();
        for (const auto &k : keys)
            if (k == key)
                return true;
        return false;
    }

    // Every icon category across the whole schema, not just one section - the same thing the
    // overlay's "show all / hide all" pair does. Membership comes from is_category_key(), NOT from
    // the "show_" prefix: see the note there for what the prefix swept in and why the aggregate and
    // the per-row toggles behaved differently because of it.
    void set_all_categories(bool on)
    {
        for (const auto &sec : goblin::ini_schema())
            for (const auto &e : sec.entries)
                if (e.type == goblin::IniType::Bool && e.target && entry_visible(e) &&
                    is_category_key(e.key))
                    *static_cast<bool *>(e.target) = on;
        g_dirty = true;
        goblin::reapply_live_settings();
    }
    // Drop the map filter set from a progress row. Kept next to the pages that offer it so the
    // enabled/disabled rule and the action cannot drift apart.
    void action_clear_focus()
    {
        if (goblin::focus_category() < 0 && !goblin::focus_rows_active())
            return; // nothing isolated - the row is shown greyed out in that state
        goblin::set_focus_category(-1); // clears the search-pick focus as well
        goblin::reapply_live_settings();
    }

    // First row of the progress and region pages. With no category focused it is an Info row
    // carrying its own label as the value, which the host draws on the Grayout frame - so
    // "disabled" costs no new row kind and confirming it does nothing.
    void push_clear_focus_row()
    {
        const bool active = goblin::focus_category() >= 0 || goblin::focus_rows_active();
        Row r;
        if (active)
        {
            r.kind = RowKind::Action;
            r.action = &action_clear_focus;
            r.label = hold(colored(wide(tr::tr(tr::TextId::ProgressFocusClear, mlang())), kColFocus));
        }
        else
        {
            r.kind = RowKind::Info;
            r.label = hold(wide(tr::tr(tr::TextId::ProgressFocusClear, mlang())));
            r.value = hold(L"-"); // valued Info -> Grayout frame
        }
        push(r);
    }

    // How many icon categories are on, out of how many exist. Drives both the row value and what
    // confirming it does, so the two can never disagree.
    void count_categories(int *on, int *total)
    {
        *on = 0;
        *total = 0;
        for (const auto &sec : goblin::ini_schema())
            for (const auto &e : sec.entries)
                if (e.type == goblin::IniType::Bool && e.target && entry_visible(e) &&
                    is_category_key(e.key))
                {
                    ++*total;
                    if (*static_cast<bool *>(e.target))
                        ++*on;
                }
    }

    // The dump the player can copy out. Held here so the "copy" row has something to copy and can
    // report its size, exactly like the overlay's byte counter.
    std::string g_dump_text;

    // Plain Win32: put UTF-8 text on the clipboard as UTF-16 (CF_UNICODETEXT), which is what every
    // paste target expects. Returns false if another process owns the clipboard right now.
    bool copy_to_clipboard(const std::string &utf8)
    {
        if (utf8.empty())
            return false;
        const int wide_len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
        if (wide_len <= 0)
            return false;
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, static_cast<size_t>(wide_len) * sizeof(wchar_t));
        if (!mem)
            return false;
        if (void *dst = GlobalLock(mem))
        {
            MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, static_cast<wchar_t *>(dst), wide_len);
            GlobalUnlock(mem);
        }
        if (!OpenClipboard(nullptr))
        {
            GlobalFree(mem);
            return false;
        }
        EmptyClipboard();
        const bool ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
        CloseClipboard();
        if (!ok)
            GlobalFree(mem); // ownership only passes to the clipboard on success
        return ok;
    }

    void take_dump(goblin::markers::DumpSel sel)
    {
        g_dump_text = goblin::markers::dump_to_string(sel);
        spdlog::info("[nmenu] dump taken: {} bytes", g_dump_text.size());
    }
    void action_dump_beacons() { take_dump(goblin::markers::DUMP_BEACONS); }
    void action_dump_stamps() { take_dump(goblin::markers::DUMP_STAMPS); }
    void action_copy_dump()
    {
        spdlog::info("[nmenu] copy dump ({} bytes): {}", g_dump_text.size(),
                     copy_to_clipboard(g_dump_text) ? "ok" : "refused");
    }

    // ── About ────────────────────────────────────────────────────────────────────────
    // Host and path are SEPARATE literals, joined only at runtime: a full URL sitting in an unsigned
    // DLL that installs hooks reads to an AV engine like "fetch a payload", and that verdict has cost
    // us a release round before. Same split as the overlay's About tab.
    struct AboutLink
    {
        tr::TextId label;
        const char *host;
        const char *path;
    };
    const AboutLink kAboutLinks[] = {
        {tr::TextId::LinkNexus, "www.nexusmods.com", "/eldenring/mods/10062"},
        {tr::TextId::LinkGithub, "github.com", "/VirusAlex/ERR-MapForGoblins-DLL"},
        {tr::TextId::LinkDiscord, "discord.gg", "/JvTMwPCygB"},
    };
    constexpr size_t kAboutLinkCount = sizeof(kAboutLinks) / sizeof(kAboutLinks[0]);

    std::string about_url(size_t i)
    {
        if (i >= kAboutLinkCount)
            return {};
        return std::string("https://") + kAboutLinks[i].host + kAboutLinks[i].path;
    }

    // One action per link: a row's action takes no argument, so the index is baked into three tiny
    // functions rather than smuggled through a global that a rebuild could desynchronise.
    void copy_link(size_t i)
    {
        const std::string url = about_url(i);
        spdlog::info("[nmenu] copy link {} ({}): {}", i, url.size(),
                     copy_to_clipboard(url) ? "ok" : "refused");
    }
    void action_copy_link_0() { copy_link(0); }
    void action_copy_link_1() { copy_link(1); }
    void action_copy_link_2() { copy_link(2); }

    void action_toggle_all_categories()
    {
        int on = 0, total = 0;
        count_categories(&on, &total);
        set_all_categories(on < total);
    }

    void build_about()
    {
        g_title = hold(wide(tr::tr(tr::TextId::TabAbout, mlang())));
        // Version first: it is the one line a bug report always needs.
        Row ver;
        ver.kind = RowKind::Info;
        ver.label = hold(wide(tr::tr(tr::TextId::Version, mlang())));
        ver.value = hold(wide(PROJECT_VERSION));
        ver.help = hold(wide(tr::tr(tr::TextId::AboutDescription, mlang())));
        push(ver);

        void (*const copiers[])() = {&action_copy_link_0, &action_copy_link_1, &action_copy_link_2};
        for (size_t i = 0; i < kAboutLinkCount; ++i)
        {
            Row r;
            r.kind = RowKind::Action;
            r.label = text(kAboutLinks[i].label);
            // The row shows the address and confirming copies it - there is no browser to open from
            // inside the game, so copy IS the action rather than a secondary button.
            r.value = hold(wide(about_url(i).c_str()));
            r.help = hold(wide(tr::tr(tr::TextId::Copy, mlang())));
            r.action = copiers[i];
            push(r);
        }
    }

    void build_layout_page(size_t ix)
    {
        if (ix >= kLayoutCount)
            return;
        const LayoutPage &lp = kLayout[ix];
        std::wstring title = wide(tr::section_label(lp.label, mlang()));
        if (title.empty())
            title = wide(lp.label);
        g_title = title;
        if (lp.toggle_all)
        {
            int on = 0, total = 0;
            count_categories(&on, &total);
            Row all;
            all.kind = RowKind::Action;
            all.label = text(tr::TextId::AllIconCategories);
            all.action = &action_toggle_all_categories;
            wchar_t v[32];
            _snwprintf_s(v, _TRUNCATE, L"%d/%d", on, total);
            all.value = hold(colored(v, on == total ? kColOn : on == 0 ? kColOff : kColHeader));
            push(all);
        }
        for (size_t si = 0; si < lp.section_count; ++si)
        {
            const char *name = lp.sections[si];
            const goblin::IniSection *sec = nullptr;
            for (const auto &cand : goblin::ini_schema())
                if (std::strcmp(cand.name, name) == 0)
                    sec = &cand;
            if (!sec || !section_visible(*sec))
                continue;
            size_t shown = 0;
            for (const auto &e : sec->entries)
                if (entry_visible(e))
                    ++shown;
            if (!shown)
                continue;
            if (lp.section_count > 1) // a single-section page needs no separator
                push_separator(name);
            for (const auto &e : sec->entries)
                if (entry_visible(e))
                    push_entry_row(e, name);
        }
        for (size_t ki = 0; ki < lp.key_count; ++ki)
            if (const goblin::IniEntry *e = entry_for_key(lp.keys[ki]))
                if (entry_visible(*e))
                    push_entry_row(*e, nullptr);
        if (lp.dump_rows)
        {
            // The progress-bar workshop lived here until 2026-07-29: thirteen styles at a
            // part-done value plus character rulers, used to pick the look and to measure how
            // much the 260 px value column actually holds (25 digits, 20 '=' - it clips on
            // pixel width, not on count). Style 9 won: colons filled, middle dots empty, 26
            // cells. bar_render() still has every style, so bringing the samples back is a
            // loop over them again.
            struct DumpRow { tr::TextId id; void (*fn)(); };
            const DumpRow rows[] = {{tr::TextId::DumpBeacons, &action_dump_beacons},
                                    {tr::TextId::DumpStamps, &action_dump_stamps}};
            // The same paragraph the overlay prints above its dump buttons, as the row's help -
            // which is where a per-row explanation belongs on this screen.
            const wchar_t *dump_help = hold(wide(tr::tr(tr::TextId::DebugDumpDescription, mlang())));
            for (const DumpRow &d : rows)
            {
                Row r;
                r.kind = RowKind::Action;
                r.label = text(d.id);
                r.action = d.fn;
                r.help = dump_help;
                push(r);
            }
            // The copy row reports what it would copy, so pressing it is never a guess.
            Row cp;
            cp.kind = g_dump_text.empty() ? RowKind::Info : RowKind::Action;
            cp.label = text(tr::TextId::Copy);
            if (!g_dump_text.empty())
                cp.action = &action_copy_dump;
            wchar_t sz[32];
            _snwprintf_s(sz, _TRUNCATE, L"%zu B", g_dump_text.size());
            cp.value = hold(sz);
            cp.help = dump_help;
            push(cp);

        }
    }

    // add_back_row() lived here until 2026-07-29: a "<< Back" row at the top of every page.
    // Removed as a duplicate - Q and the pad's circle run the engine's own Back, and the row cost a
    // slot out of the sixteen a page can show, plus it was where the cursor started.
    void build_root()
    {
        g_title = kProductName;
        // The master switch, first row. It is runtime state (goblin::icons_hidden), not an ini
        // entry, so it cannot be a Toggle row - those flip a bool* belonging to the schema. The
        // overlay used to carry this and the native menu never picked it up when it took over as
        // the config UI, which left the only way to turn every icon off at once being a hotkey
        // the player may well have unbound.
        {
            Row master;
            master.kind = RowKind::Action;
            master.label = text(tr::TextId::MasterToggle);
            master.value = text(goblin::icons_hidden() ? tr::TextId::ValueOff : tr::TextId::ValueOn);
            master.help = text(tr::TextId::MasterToggleTooltip);
            master.action = [] { goblin::set_icons_hidden(!goblin::icons_hidden()); };
            push(master);
        }
        // Two settings are important enough to sit at the top level rather than inside a
        // page: what the map is allowed to show at all, and how it tells the place you are
        // in from the places that merely sit under (or over) it.
        if (const goblin::IniEntry *rmf = entry_for_key("require_map_fragments"))
            push_entry_row(*rmf, nullptr);
        if (const goblin::IniEntry *le = entry_for_key("location_emphasis"))
            push_entry_row(*le, nullptr);
        for (size_t i = 0; i < kLayoutCount; ++i)
        {
            const LayoutPage &lp = kLayout[i];
            std::wstring name = wide(tr::section_label(lp.label, mlang()));
            if (name.empty())
                name = wide(lp.label);
            // Only the row count. A page row used to borrow the icon of the first entry inside it,
            // which put a loot icon next to "Categories" and a compatibility entry's icon next to
            // "Compatibility" - a picture that stood for one setting out of dozens.
            size_t shown = 0;
            for (size_t si = 0; si < lp.section_count; ++si)
                for (const auto &sec : goblin::ini_schema())
                    if (std::strcmp(sec.name, lp.sections[si]) == 0 && section_visible(sec))
                        for (const auto &e : sec.entries)
                            if (entry_visible(e))
                                ++shown;
            for (size_t ki = 0; ki < lp.key_count; ++ki)
                if (const goblin::IniEntry *e = entry_for_key(lp.keys[ki]))
                    if (entry_visible(*e))
                        ++shown;
            if (!shown)
                continue;
            Row r;
            r.kind = RowKind::SubPage;
            r.page_id = goblin::nmenu::kPageSectionBase + static_cast<int32_t>(i);
            {
                std::wstring tip = wide(tr::section_comment(lp.label, "", mlang()));
                if (!tip.empty())
                    r.help = hold(std::move(tip));
            }
            r.label = hold(std::move(name));
            wchar_t cnt[24];
            _snwprintf_s(cnt, _TRUNCATE, L"%zu  >", shown);
            r.value = hold(cnt);
            push(r);
            // Progress and Hidden sit between Categories and the rest, per the agreed layout.
            if (r.page_id == kPageCategories)
            {
                Row prog;
                prog.kind = RowKind::SubPage;
                prog.page_id = goblin::nmenu::kPageProgress;
                // Red while a category is isolated on the map, so the filter is visible from the
                // top level instead of only inside the page that set it.
                // Both marks, not one: the red text reads at a glance, and the plate is the same
                // background the isolated category itself wears, so the two screens agree.
                const bool filtering = goblin::focus_category() >= 0; // the search row marks its own filter
                prog.label = filtering
                                 ? hold(colored(wide(tr::tr(tr::TextId::TabProgress, mlang())), kColFocus))
                                 : text(tr::TextId::TabProgress);
                prog.plate = filtering;
                prog.value = hold(L">");
                push(prog);

                Row hid;
                hid.kind = RowKind::SubPage;
                hid.page_id = goblin::nmenu::kPageHidden;
                hid.label = text(tr::TextId::HiddenMarkers);
                wchar_t hc[24];
                _snwprintf_s(hc, _TRUNCATE, L"%zu  >", goblin::manual_hidden_count());
                hid.value = hold(hc);
                push(hid);

                Row srch;
                srch.kind = RowKind::SubPage;
                srch.page_id = goblin::nmenu::kPageSearch;
                // Red + plate while the picks isolate the map, the way the Progress row marks a
                // category filter: the filter is visible from the top level.
                const bool picking = goblin::focus_rows_active();
                srch.label = picking
                                 ? hold(colored(wide(tr::tr(tr::TextId::TabSearch, mlang())), kColFocus))
                                 : text(tr::TextId::TabSearch);
                srch.plate = picking;
                const size_t npick = goblin::search::pick_count();
                if (npick)
                {
                    wchar_t sc[24];
                    _snwprintf_s(sc, _TRUNCATE, L"%zu  >", npick);
                    srch.value = hold(sc);
                }
                else
                    srch.value = hold(L">");
                push(srch);
            }
        }
        // About last: version and the links, the least-used page but the one a bug report needs.
        Row ab;
        ab.kind = RowKind::SubPage;
        ab.page_id = goblin::nmenu::kPageAbout;
        ab.label = text(tr::TextId::TabAbout);
        ab.value = hold(L">");
        push(ab);

        // Pages contributed by other mods (sdk/mfg_menu_api.h). They appear as ordinary
        // rows, so a player sees one menu for everything. Not built into this DLL - the SDK host
        // ships as a separate mod (goblin_build_variants.hpp, MFG_MENU_ADDON_HOST).
#if MFG_MENU_ADDON_HOST
        goblin::addons::scan();
        for (size_t i = 0; i < goblin::addons::page_count(); ++i)
        {
            const auto *ap = goblin::addons::page(i);
            if (!ap)
                continue;
            Row r;
            r.kind = RowKind::SubPage;
            r.page_id = goblin::nmenu::kPageAddonBase + static_cast<int32_t>(i);
            r.label = hold(ap->title);
            r.value = hold(L">");
            push(r);
        }
#endif // MFG_MENU_ADDON_HOST
    }

#if MFG_MENU_ADDON_HOST
    void build_addon(size_t index)
    {
        const auto *ap = goblin::addons::build_page(index);
        if (!ap)
        {
            g_title = kProductName;
            Row r;
            r.kind = RowKind::Info;
            r.label = text(tr::TextId::MenuUnavailable);
            push(r);
            return;
        }
        g_title = ap->title;
        for (const auto &ar : ap->rows)
        {
            Row r;
            switch (ar.kind)
            {
            case 1: // toggle
            case 2: // number
            case 3: // enum
                r.kind = RowKind::Info; // the add-on owns the value; decide forwards to it
                break;
            case 4:
                r.kind = RowKind::Info;
                break;
            case 5:
                r.kind = RowKind::Progress;
                break;
            default:
                r.kind = RowKind::Info;
                break;
            }
            r.label = hold(ar.label);
            r.value = ar.kind == 5 && ar.value.empty()
                          ? hold(bar_text(ar.done, ar.total))
                          : hold(ar.value);
            r.collected = ar.done;
            r.total = ar.total;
            // Remember where to route a confirm: page index + the add-on's row id.
            r.page_id = static_cast<int32_t>(index);
            r.type_tag = 0xFF; // marks "belongs to an add-on"
            r.addon_row_id = ar.row_id;
            r.addon_kind = ar.kind;
            push(r);
        }
    }
#endif // MFG_MENU_ADDON_HOST

    // build_section() lived here until 2026-07-29: it built one page per ini section. Pages now
    // come from the layout table above, so a page can merge several sections or list keys by hand.

    void build_progress()
    {
        g_title = wide(tr::tr(tr::TextId::TabProgress, mlang()));
        goblin::progress::rebuild();
        const auto &regions = goblin::progress::snapshot();
        if (regions.empty())
        {
            Row r;
            r.kind = RowKind::Info;
            r.label = text(tr::TextId::ProgressNoMarkers);
            push(r);
            return;
        }
        push_clear_focus_row();
        int done = 0, all = 0;
        for (const auto &reg : regions)
        {
            done += reg.collected;
            all += reg.total;
        }
        Row tot;
        tot.kind = RowKind::Progress;
        tot.label = text(tr::TextId::MenuTotal);
        tot.collected = done;
        tot.total = all;
        tot.value = hold(bar_text(done, all));
        tot.help = hold(long_bar(done, all));
        push(tot);
        // Mega-section headers, exactly where the overlay's progress tab puts them: whenever the
        // group changes, and never before the trailing "Other" bucket (place id < 0). The row
        // clip has a purpose-built look for this - the PadCategory frame, one wide caption and
        // no value field - which a valueless Info row already selects.
        int last_mega = -1;
        for (size_t i = 0; i < regions.size(); ++i)
        {
            const auto &reg = regions[i];
            if (reg.total <= 0)
                continue;
            if (reg.place_name_id >= 0 && static_cast<int>(reg.mega) != last_mega)
            {
                last_mega = static_cast<int>(reg.mega);
                const tr::TextId mid =
                    reg.mega == goblin::progress::Mega::LandsBetween ? tr::TextId::MegaLandsBetween
                    : reg.mega == goblin::progress::Mega::Dungeons   ? tr::TextId::MegaDungeons
                                                                     : tr::TextId::MegaShadow;
                Row head;
                head.kind = RowKind::Info; // valueless -> drawn on the PadCategory frame
                head.label = hold(colored(wide(tr::tr(mid, mlang())), kColHeader));
                push(head);
            }
            // A region opens its own page with the per-category breakdown, the same numbers
            // the overlay's progress tab shows when a region is expanded.
            Row r;
            r.kind = RowKind::SubPage;
            r.page_id = goblin::nmenu::kPageRegionBase + static_cast<int32_t>(i);
            // The region holding the isolated category wears the same red as the category row, so
            // the active filter is visible in the list instead of only inside the region.
            const bool focused_here = goblin::focus_category() >= 0 &&
                                      goblin::focus_region() == reg.place_name_id;
            // Focus wins over completion: a red row says "this is what the map is filtered to", and
            // that has to stay readable even on a zone that is finished.
            r.label = focused_here ? hold(colored(wide(reg.name.c_str()), kColFocus))
                      : zone_done(reg.collected, reg.total)
                          ? hold(colored(wide(reg.name.c_str()), kColBarDone))
                          : hold(wide(reg.name.c_str()));
            r.plate = focused_here;
            r.collected = reg.collected;
            r.total = reg.total;
            r.value = hold(bar_text(reg.collected, reg.total));
            r.help = hold(long_bar(reg.collected, reg.total)); // the wide block below has the room
            push(r);
        }
    }

    void build_region(size_t index)
    {
        const auto &regions = goblin::progress::snapshot();
        if (index >= regions.size())
        {
            g_title = wide(tr::tr(tr::TextId::TabProgress, mlang()));
            return;
        }
        const auto &reg = regions[index];
        g_title = hold(wide(reg.name.c_str()));
        push_clear_focus_row();
        Row tot;
        tot.kind = RowKind::Progress;
        tot.label = text(tr::TextId::MenuTotal);
        tot.collected = reg.collected;
        tot.total = reg.total;
        tot.value = hold(bar_text(reg.collected, reg.total));
        tot.help = hold(long_bar(reg.collected, reg.total));
        push(tot);
        for (int ci = 0; ci < goblin::progress::kCategoryCount; ++ci)
        {
            if (reg.cats[ci].total <= 0)
                continue;
            const auto cat = static_cast<goblin::generated::Category>(ci);
            // Same label source as the overlay: the category's ini key carries the localized
            // name and the icon, with the raw enum name only as a last resort.
            const char *key = goblin::category_config_key(cat);
            Row r;
            r.kind = RowKind::Progress;
            r.label = hold(wide(key ? tr::entry_label(key, mlang())
                                    : goblin::markers::category_name(cat)));
            if (key)
            {
                r.icon_id = icon_for_key(key);
                r.ini_key = key; // the icon route looks the strip cell up by key
            }
            // Confirming a category isolates it on the live map, exactly like the overlay's
            // progress tab: only its uncollected markers stay, and they get the glow icon.
            r.collected = reg.cats[ci].collected;
            r.total = reg.cats[ci].total;
            r.type_tag = static_cast<uint8_t>(ci);
            r.page_id = reg.place_name_id;
            std::wstring val = bar_text(r.collected, r.total);
            // The focused category reads in red. A real background fill is not available:
            // these fields take HTML, and GFx HTML has font colour but no background - that is
            // an ActionScript property of the text field, which we do not drive.
            if (goblin::focus_category() == ci && goblin::focus_region() == reg.place_name_id)
            {
                val = colored(bar_text(r.collected, r.total), kColFocus);
                r.label = hold(colored(std::wstring(r.label), kColFocus)); // label too, like the rows above
                r.plate = true; // the row's own red plate marks what is isolated on the map
            }
            r.value = hold(std::move(val));
            r.help = hold(long_bar(r.collected, r.total));
            push(r);
        }
    }

    // ── hidden markers ───────────────────────────────────────────────────────────────
    // Restoring ONE marker needs its key, and a row's action is a bare function pointer, so
    // the key travels in the row (page_id holds the index into the snapshot the page was
    // built from) and the handler re-reads the snapshot. Deleting from under an index is
    // safe because the page is rebuilt immediately afterwards.
    std::vector<goblin::HiddenMarkerInfo> g_hidden_view;

    // A row action is a bare function pointer, so the row it fired on is handed over here
    // (set by activate() around the call). That keeps Row a plain aggregate - no captures,
    // no allocation - while still letting one handler serve a whole list.
    const Row *g_active_row = nullptr;

    void action_unhide_one()
    {
        if (!g_active_row)
            return;
        const size_t ix = static_cast<size_t>(g_active_row->page_id);
        if (ix >= g_hidden_view.size())
            return;
        goblin::unhide_marker(g_hidden_view[ix].key);
        goblin::persist_manual_hidden();
        goblin::reapply_live_settings();
    }

    void build_hidden()
    {
        g_title = hold(wide(tr::tr(tr::TextId::HiddenMarkers, mlang())));
        g_hidden_view = goblin::manual_hidden_snapshot();
        if (g_hidden_view.empty())
        {
            Row info;
            info.kind = RowKind::Info;
            info.label = text(tr::TextId::HiddenMarkersNone);
            push(info);
            return;
        }
        Row un;
        un.kind = RowKind::Action;
        un.label = text(tr::TextId::UnhideAll);
        wchar_t cnt[24];
        _snwprintf_s(cnt, _TRUNCATE, L"%zu", g_hidden_view.size());
        un.value = hold(cnt);
        un.action = &action_unhide_all;
        push(un);
        for (size_t i = 0; i < g_hidden_view.size(); ++i)
        {
            const auto &h = g_hidden_view[i];
            // Same three parts the overlay's Hidden tab shows: item, where it was, category.
            const wchar_t *name = goblin::lookup_text_any(h.textId);
            const wchar_t *where = h.region > 0 ? goblin::lookup_text_any(h.region) : nullptr;
            const auto cat = static_cast<goblin::generated::Category>(h.cat);
            const char *ckey = goblin::category_config_key(cat);
            std::wstring label = name && *name ? name : L"?";
            if (where && *where)
                label += std::wstring(L"  -  ") + where;
            Row r;
            r.kind = RowKind::Action;
            r.page_id = static_cast<int32_t>(i);
            r.action = &action_unhide_one;
            r.label = hold(std::move(label));
            r.value = text(tr::TextId::Unhide);
            if (ckey)
            {
                r.icon_id = icon_for_key(ckey);
                r.ini_key = ckey; // same reason as the progress rows
                r.help = hold(wide(tr::entry_label(ckey, mlang())));
            }
            if (r.icon_id < 0)
                r.label = hold(colored(kBullet, kColValue) + L"  " +
                               std::wstring(r.label ? r.label : L""));
            push(r);
        }
    }

    // ── item search page ─────────────────────────────────────────────────────────────
    // The typed text lives here (the host feeds it, see search_type); the matching and the
    // pick set live in goblin::search, shared with the overlay's Search tab, so picks made in
    // one menu show in the other.
    std::wstring g_search_text;
    std::vector<goblin::search::Hit> g_search_view; // the hits the page lists, in row order
    // The form pool holds kFormPoolMax native items, two per visual row; the page's fixed rows
    // take a dozen. Past this many hits the page asks for more letters instead of overflowing
    // the pool.
    constexpr size_t kSearchMaxRows = 200;

    std::wstring html_escape(const std::wstring &s)
    {
        std::wstring o;
        o.reserve(s.size());
        for (wchar_t c : s)
        {
            if (c == L'&') o += L"&amp;";
            else if (c == L'<') o += L"&lt;";
            else if (c == L'>') o += L"&gt;";
            else o += c;
        }
        return o;
    }

    std::string narrow(const std::wstring &w)
    {
        std::string s;
        if (w.empty()) return s;
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        if (n > 0)
        {
            s.resize(static_cast<size_t>(n));
            WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
        }
        return s;
    }

    void action_search_clear() { g_search_text.clear(); }
    // Picks apply to the map the moment they change (goblin::search): "show all found" simply
    // ticks every hit, "clear picks" lifts the filter.
    void action_search_show_all()
    {
        const auto res_p = goblin::search::query(narrow(g_search_text));
        const auto &res = *res_p;
        std::vector<uint64_t> keys;
        keys.reserve(res.total);
        for (const auto &g : res.groups)
            for (const auto &h : g.hits) keys.push_back(h.key);
        if (keys.empty()) return;
        goblin::search::pick_many(keys, true);
    }
    void action_search_clear_picks() { goblin::search::clear_picks(); }
    void action_search_toggle_pick()
    {
        if (!g_active_row) return;
        const size_t ix = static_cast<size_t>(g_active_row->page_id);
        if (ix >= g_search_view.size()) return;
        const uint64_t key = g_search_view[ix].key;
        goblin::search::set_picked(key, !goblin::search::is_picked(key));
    }

    void build_search()
    {
        g_title = hold(wide(tr::tr(tr::TextId::TabSearch, mlang())));

        // The text row. Typed text with a caret; the placeholder when empty. This row is where
        // the host listens for keys: with the cursor on it, letters type; below it they are the
        // game's own menu keys again.
        // An Action row with no action: it gets the ordinary row frame and the cursor highlight
        // (an Info row sits on the Grayout plate and never looks selected), and confirming it
        // does nothing.
        Row field;
        field.kind = RowKind::Action;
        if (g_search_text.empty())
            field.label = hold(colored(wide(tr::tr(tr::TextId::SearchTypeHere, mlang())), kColOff));
        else
            field.label = hold(colored(html_escape(g_search_text), kColValue) + L"_");
        field.help = hold(wide(tr::tr(tr::TextId::SearchHint, mlang())));
        // The value column names the layout the keys translate with ("EN" / "RU"); Alt+Shift,
        // Ctrl+Shift or Win+Space cycles it while the cursor is on this row.
        field.value = hold(colored(goblin::overlay::text_layout_tag(), kColOff));
        push(field);

        Row clr;
        clr.kind = RowKind::Action;
        clr.label = text(tr::TextId::SearchClearField);
        clr.action = &action_search_clear;
        push(clr);
        if (const goblin::IniEntry *e = entry_for_key("search_hide_collected"))
            push_entry_row(*e, nullptr); // "only uncollected" toggle (an ordinary schema row)

        const auto res_p = goblin::search::query(narrow(g_search_text)); // immutable snapshot
        const auto &res = *res_p;
        const size_t npick = goblin::search::pick_count();

        // Status line.
        {
            Row st;
            st.kind = RowKind::Info;
            if (g_search_text.empty())
                st.label = text(tr::TextId::SearchHint);
            else if (res.total == 0)
                st.label = text(tr::TextId::SearchNoResults);
            else
            {
                char buf[160];
                _snprintf_s(buf, sizeof buf, _TRUNCATE, tr::tr(tr::TextId::SearchMatches, mlang()),
                            static_cast<int>(res.total), static_cast<int>(res.groups.size()));
                st.label = hold(wide(buf));
            }
            push(st);
        }

        // Actions.
        {
            Row sa;
            sa.kind = RowKind::Action;
            sa.label = res.total ? text(tr::TextId::SearchShowAll)
                                 : hold(colored(wide(tr::tr(tr::TextId::SearchShowAll, mlang())), kColOff));
            sa.action = &action_search_show_all;
            push(sa);
            Row cp;
            cp.kind = RowKind::Action;
            cp.label = npick ? text(tr::TextId::SearchClearPicks)
                             : hold(colored(wide(tr::tr(tr::TextId::SearchClearPicks, mlang())), kColOff));
            cp.action = &action_search_clear_picks;
            push(cp);
        }
        // (No "reset filter" row here: with picks applied live, "clear picks" IS the reset.)

        // Matches, grouped by region: a caption row per region, then one toggle-pick row per
        // hit. The red plate marks a picked row (the same plate the isolated category wears).
        g_search_view.clear();
        if (res.total == 0)
            return;
        const auto done = goblin::hidden_marker_original_ids();
        size_t listed = 0;
        bool truncated = res.truncated;
        for (const auto &g : res.groups)
        {
            if (listed >= kSearchMaxRows) { truncated = true; break; }
            Row hdr;
            hdr.kind = RowKind::Info;
            wchar_t cnt[32];
            _snwprintf_s(cnt, _TRUNCATE, L" (%zu)", g.hits.size());
            hdr.label = hold(colored(html_escape(wide(g.region_name.c_str())) + cnt, kColValue));
            push(hdr);
            for (const auto &h : g.hits)
            {
                if (listed >= kSearchMaxRows) { truncated = true; break; }
                const bool picked = goblin::search::is_picked(h.key);
                Row r;
                r.kind = RowKind::Action;
                r.page_id = static_cast<int32_t>(g_search_view.size());
                r.action = &action_search_toggle_pick;
                r.plate = picked;
                r.label = hold(html_escape(wide(h.name.c_str())));
                // The pick state lives in the value column, worded and coloured like a toggle row
                // (On / Off); a collected marker carries its tag in front of it.
                std::wstring val = colored(wide(tr::tr(picked ? tr::TextId::ValueOn : tr::TextId::ValueOff, mlang())),
                                           picked ? kColOn : kColOff);
                if (done.count(h.key))
                    val = colored(wide(tr::tr(tr::TextId::SearchCollected, mlang())), kColOff) + L"  " + val;
                r.value = hold(std::move(val));
                const auto cat = static_cast<goblin::generated::Category>(h.cat);
                if (const char *ckey = goblin::category_config_key(cat))
                {
                    r.icon_id = icon_for_key(ckey);
                    r.ini_key = ckey;
                    r.help = hold(wide(tr::entry_label(ckey, mlang())));
                }
                push(r);
                g_search_view.push_back(h);
                ++listed;
            }
        }
        if (truncated)
        {
            Row more;
            more.kind = RowKind::Info;
            char buf[160];
            _snprintf_s(buf, sizeof buf, _TRUNCATE, tr::tr(tr::TextId::SearchTruncated, mlang()),
                        static_cast<int>(listed));
            more.label = hold(colored(wide(buf), kColOff));
            push(more);
        }
    }

    // ── value page: the choices behind one Enum entry ────────────────────────────────
    void build_value()
    {
        const goblin::IniEntry *e = g_edit_entry;
        if (!e)
        {
            g_title = kProductName;
            return;
        }
        std::wstring name = wide(tr::entry_label(e->key, mlang()));
        g_title = name.empty() ? wide(e->key) : name;
        // ── a slider gets a screen of its own, holding EXACTLY ONE row ───────────────
        // That single row is the whole reason this branch exists. Left/right belongs to
        // the slider, but the grid claims it too and walks to the neighbouring row; two
        // attempts to take the press away from the grid failed in game (see the note in
        // goblin_stall_probe's form_update_detour). On a one-row list the grid's own
        // bounds refuse the move, so nothing has to be intercepted at all.
        // Therefore: NO "Back" row here, however tempting - a second row would hand the
        // grid somewhere to go and bring the whole problem back. The engine's own cancel
        // (Q / circle) closes this screen, exactly as it closes every other screen of ours.
        if (const NumRange *r = range_for(e->key))
        {
            Row row;
            row.kind = RowKind::Slider;
            row.label = hold(name.empty() ? wide(e->key) : name);
            row.value = hold(slider_text(*e, *r, &row.slider_frac));
            row.ini_key = e->key;
            row.target = e->target;
            row.type_tag = static_cast<uint8_t>(e->type);
            row.icon_id = icon_for_key(e->key);
            std::wstring tip = wide(tr::entry_comment(e->key, e->comment ? e->comment : "", mlang()));
            if (!tip.empty())
                row.help = hold(std::move(tip));
            push(row);
            return;
        }
        const int cur = current_option(*e);
        const size_t n = option_count(*e);
        for (size_t i = 0; i < n; ++i)
        {
            const bool live = static_cast<int>(i) == cur;
            Row r;
            r.kind = RowKind::ValueOption;
            r.page_id = static_cast<int32_t>(i);
            // The live choice is marked in the value column, so the label column stays a
            // clean list and the marker reads the same on every row width.
            r.label = hold(option_label(*e, i));
            if (live)
                r.value = hold(colored(kBullet, kColOn));
            push(r);
        }
    }

    // ── rebind page ──────────────────────────────────────────────────────────────────
    bool g_rebind_waiting = false;

    void build_rebind()
    {
        const goblin::IniEntry *e = g_edit_entry;
        if (!e)
        {
            g_title = kProductName;
            return;
        }
        std::wstring name = wide(tr::entry_label(e->key, mlang()));
        g_title = name.empty() ? wide(e->key) : name;
        const bool pad = e->type == goblin::IniType::GamepadMask;
        Row prompt;
        prompt.kind = RowKind::Info;
        prompt.label = text(pad ? tr::TextId::MenuPressPad : tr::TextId::MenuPressKey);
        prompt.value = hold(value_of(*e));
        push(prompt);
        Row keep;
        keep.kind = RowKind::Back;
        keep.label = text(tr::TextId::MenuKeepCurrent);
        push(keep);
        // A keyboard binding can be cleared by pressing Escape, which the poller reads as "leave
        // it alone" and which players also use as "get me out". A pad has no such key, so the only
        // way to say "no button at all" is to offer it as a row.
        if (pad)
        {
            Row clear;
            clear.kind = RowKind::Action;
            clear.label = text(tr::TextId::MenuUnbind);
            clear.action = [] { goblin::nmenu::rebind_apply_pad(0); };
            push(clear);
        }
    }

    // The Tools page is gone (2026-07-29). Its only surviving row, "Save settings now", duplicated
    // the save that menu_torn_down() already performs on close, and the diagnostic rows below it were
    // retired earlier - so the page had nothing left to show.
    // Build the right-hand preview for `page` (currently: the rows of an ini section as
    // "name  value" lines).
    void build_preview(int32_t page)
    {
        g_preview_rows.clear();
        g_preview_arena.clear();
        if (page < goblin::nmenu::kPageSectionBase)
            return;
        const auto &schema = goblin::ini_schema();
        const size_t ix = static_cast<size_t>(page - goblin::nmenu::kPageSectionBase);
        if (ix >= schema.size())
            return;
        auto hold_preview = [](std::wstring t) -> const wchar_t *
        {
            g_preview_arena.push_back(std::move(t));
            return g_preview_arena.back().c_str();
        };
        std::wstring head = wide(tr::section_label(schema[ix].name, mlang()));
        if (head.empty())
            head = wide(schema[ix].name);
        Row title;
        title.kind = RowKind::Info;
        title.right_column = true;
        title.value = hold_preview(colored(head, kColValue));
        g_preview_rows.push_back(title);
        for (const auto &e : schema[ix].entries)
        {
            if (!entry_visible(e))
                continue;
            std::wstring label = wide(tr::entry_label(e.key, mlang()));
            if (label.empty())
                label = wide(e.key);
            if (label.size() > 26)
                label = label.substr(0, 25) + L"…";
            Row r;
            r.kind = RowKind::Info;
            r.right_column = true;
            r.value = hold_preview(label + L"   " + value_of(e));
            g_preview_rows.push_back(r);
            if (g_preview_rows.size() >= 11) // the form shows 11 slots per column
                break;
        }
    }

    void build_current()
    {
        g_rows.clear();
        g_arena.clear();
        if (g_page == goblin::nmenu::kPageRoot)
            build_root();
        else if (g_page == goblin::nmenu::kPageProgress)
            build_progress();
        else if (g_page == goblin::nmenu::kPageHidden)
            build_hidden();
        else if (g_page == goblin::nmenu::kPageAbout)
            build_about();
        else if (g_page == goblin::nmenu::kPageValue)
            build_value();
        else if (g_page == goblin::nmenu::kPageSearch)
            build_search();
        else if (g_page == goblin::nmenu::kPageRebind)
            build_rebind();
        else if (g_page >= goblin::nmenu::kPageRegionBase)
            build_region(static_cast<size_t>(g_page - goblin::nmenu::kPageRegionBase));
#if MFG_MENU_ADDON_HOST
        else if (g_page >= goblin::nmenu::kPageAddonBase)
            build_addon(static_cast<size_t>(g_page - goblin::nmenu::kPageAddonBase));
#endif
        else if (g_page >= goblin::nmenu::kPageSectionBase)
            build_layout_page(static_cast<size_t>(g_page - goblin::nmenu::kPageSectionBase));
        else
            build_root();
        build_preview(g_preview_page);
    }
}

void goblin::nmenu::rebuild() { build_current(); }

const goblin::nmenu::Row *goblin::nmenu::rows(size_t *count)
{
    if (count)
        *count = g_rows.size();
    return g_rows.empty() ? nullptr : g_rows.data();
}

const wchar_t *goblin::nmenu::page_title() { return g_title.c_str(); }
int32_t goblin::nmenu::current_page() { return g_page; }

void goblin::nmenu::set_page(int32_t page)
{
    // Leaving an editing page ends the edit, exactly as navigate_back() does: nothing may keep
    // polling for a key once its screen is gone, and a stale entry pointer must not be reachable
    // from the page that resumes.
    if (page != kPageValue && page != kPageRebind)
    {
        g_rebind_waiting = false;
        g_edit_entry = nullptr;
    }
    if (g_page == page)
        return;
    g_page = page;
    g_preview_page = -1;
    build_current();
}

void goblin::nmenu::set_nested(bool on)
{
    g_nested = on;
    g_child_page = -1;
    g_want_close = false;
}

int32_t goblin::nmenu::take_child_page()
{
    const int32_t p = g_child_page;
    g_child_page = -1;
    return p;
}

bool goblin::nmenu::take_close_request()
{
    const bool c = g_want_close;
    g_want_close = false;
    return c;
}

// nested(), subpage_target(), reset_to_root() and depth() were exported here and had no callers:
// the host drives the model through set_page / take_child_page / take_close_request only.

bool goblin::nmenu::activate(size_t row_index)
{
    if (row_index >= g_rows.size())
        return false;
    const Row row = g_rows[row_index]; // copy: the rebuild below invalidates the vector
    switch (row.kind)
    {
    case RowKind::Back:
        if (g_nested)
        {
            g_want_close = true; // the engine's own Back does the rest
            return false;
        }
        return navigate_back();
    case RowKind::SubPage:
        if (g_nested)
        {
            g_child_page = row.page_id;
            return false; // this screen stays as it is; the host opens the next one
        }
        // One confirm, one level down. The two-step "preview first, enter on the second
        // press" behaviour went away with the right-hand preview column: making the first
        // press do nothing visible to the list is worse than no preview at all.
        g_stack.push_back(g_page);
        g_page = row.page_id;
        g_preview_page = -1;
        build_current();
        return true;
    case RowKind::Toggle:
        if (row.target)
        {
            bool *v = static_cast<bool *>(row.target);
            *v = !*v;
            g_dirty = true;
            spdlog::info("[nmenu] {} -> {}", row.ini_key ? row.ini_key : "?", *v ? "on" : "off");
            // The flag alone is not the whole setting. Our own icons follow it live (the per-frame
            // visibility asks is_category_enabled directly), which is why flipping a category here
            // LOOKED complete - but the param row also carries the enable flags the GAME gates on,
            // and only apply_category_visibility writes those. The aggregate "all categories" row
            // does call this; a single toggle did not, so after "turn everything off" the rows kept
            // textEnableFlagId = AlwaysOff while the config said the category was back on: our icons
            // returned, the engine still considered those rows disabled, built no pin for them, and
            // the marker popups stayed dead until some OTHER setting happened to trigger a reapply.
            // That is the asymmetry between the button and the per-row switches. Found 2026-07-31.
            goblin::reapply_live_settings();
        }
        build_current();
        return true;
    case RowKind::Slider:
        // ON the slider's own screen there is nothing left to confirm - the row IS the
        // widget and left/right is what edits it.
        if (g_page == kPageValue)
            return false;
        // On a list page the row shows its bar and its number, and confirming it opens the
        // one-row screen where the value is actually changed (see build_value).
        if (const goblin::IniEntry *e = entry_for_key(row.ini_key))
        {
            g_edit_entry = e;
            if (g_nested)
            {
                g_child_page = kPageValue;
                return false;
            }
            g_stack.push_back(g_page);
            g_page = kPageValue;
            g_preview_page = -1;
            build_current();
            return true;
        }
        return false;
    case RowKind::Enum:
        // A list of choices: every option is visible at once and the live one is marked.
        if (const goblin::IniEntry *e = entry_for_key(row.ini_key))
        {
            g_edit_entry = e;
            if (g_nested)
            {
                g_child_page = kPageValue;
                return false;
            }
            g_stack.push_back(g_page);
            g_page = kPageValue;
            g_preview_page = -1;
            build_current();
            return true;
        }
        return false;
    case RowKind::ValueOption:
        if (g_edit_entry)
        {
            apply_option(*g_edit_entry, static_cast<size_t>(row.page_id));
            spdlog::info("[nmenu] {} set from its value page", g_edit_entry->key);
        }
        if (g_nested)
        {
            g_want_close = true; // the value page has done its one job
            return false;
        }
        return navigate_back();
    case RowKind::Rebind:
        if (const goblin::IniEntry *e = entry_for_key(row.ini_key))
        {
            g_edit_entry = e;
            g_rebind_waiting = true;
            if (g_nested)
            {
                g_child_page = kPageRebind;
                return false;
            }
            g_stack.push_back(g_page);
            g_page = kPageRebind;
            g_preview_page = -1;
            build_current();
            return true;
        }
        return false;
    case RowKind::Action:
        if (row.action)
        {
            g_active_row = &row;
            row.action();
            g_active_row = nullptr;
        }
        build_current();
        return true;
    case RowKind::Progress:
        // A per-category row on a region page: same behaviour as the overlay's progress tab -
        // confirming isolates that category's uncollected markers on the live map and gives
        // them the glow icon; confirming the focused one again clears the filter.
        if (row.type_tag != 0xFF && row.ini_key && g_page >= kPageRegionBase)
        {
            const int cat = static_cast<int>(row.type_tag);
            const bool same = goblin::focus_category() == cat &&
                              goblin::focus_region() == row.page_id;
            if (same)
                goblin::set_focus_category(-1);
            else
                goblin::set_focus_category(cat, row.page_id);
            goblin::reapply_live_settings();
            goblin::apply_focus_highlight(); // swap the focused rows to the glow icon
            build_current();
            spdlog::info("[nmenu] progress focus: category {} region {} -> {}", cat, row.page_id,
                         same ? "cleared" : "isolated");
            return true;
        }
        [[fallthrough]];
    case RowKind::Info:
#if MFG_MENU_ADDON_HOST
        // An add-on row: let the owning add-on handle it and rebuild if it changed.
        if (row.type_tag == 0xFF)
        {
            const bool changed =
                goblin::addons::activate(static_cast<size_t>(row.page_id), row.addon_row_id);
            if (changed)
                build_current();
            return changed;
        }
#endif
        return false;
    }
    return false;
}

bool goblin::nmenu::slider_step(size_t row_index, int dir)
{
    if (dir == 0 || row_index >= g_rows.size())
        return false;
    // ONLY on the slider's own one-row screen. On a list page the same press is the grid's
    // to interpret (it walks to the neighbouring row), and a value that also moved under a
    // navigation press is exactly the confusion this design was chosen to end.
    if (g_page != kPageValue)
        return false;
    const Row &row = g_rows[row_index];
    if (row.kind != RowKind::Slider)
        return false;
    const goblin::IniEntry *e = entry_for_key(row.ini_key);
    const NumRange *r = e ? range_for(e->key) : nullptr;
    if (!e || !r || !e->target)
        return false;
    const float cur = e->type == goblin::IniType::Float
                          ? *static_cast<float *>(e->target)
                          : static_cast<float>(*static_cast<uint8_t *>(e->target));
    // Clamp, no wrap. A value the ini holds BETWEEN steps just moves by one step from where
    // it is - snapping it to the grid first would jump the panel the player is trying to nudge.
    float next = cur + (dir > 0 ? r->step : -r->step);
    if (next < r->min)
        next = r->min;
    if (next > r->max)
        next = r->max;
    if (next == cur)
        return false; // already at the end the press points at
    if (e->type == goblin::IniType::Float)
        *static_cast<float *>(e->target) = next;
    else
        *static_cast<uint8_t *>(e->target) = static_cast<uint8_t>(next + 0.5f);
    g_dirty = true;
    // Deliberately NO reapply_live_settings() here: every current slider value is read live
    // per frame by its consumer, and the host runs one reapply when the hold ends - see the
    // header. A log per step would also be noise at repeat rate; the release path logs.
    build_current();
    return true;
}

const goblin::nmenu::Row *goblin::nmenu::right_row(size_t index)
{
    return index < g_preview_rows.size() ? &g_preview_rows[index] : nullptr;
}

bool goblin::nmenu::navigate_back()
{
    if (g_stack.empty())
        return false;
    g_page = g_stack.back();
    g_stack.pop_back();
    g_preview_page = -1;
    // Leaving an editing page ends the edit: nothing else may keep polling for a key, and a
    // stale entry pointer must not be reachable from the next page.
    g_rebind_waiting = false;
    if (g_page != kPageValue && g_page != kPageRebind)
        g_edit_entry = nullptr;
    build_current();
    return true;
}

bool goblin::nmenu::search_active() { return g_page == kPageSearch; }

void goblin::nmenu::search_type(wchar_t c)
{
    if (c < 0x20 || g_search_text.size() >= 64) return;
    g_search_text.push_back(c);
}

bool goblin::nmenu::search_backspace()
{
    if (g_search_text.empty()) return false;
    g_search_text.pop_back();
    return true;
}

void goblin::nmenu::search_clear() { g_search_text.clear(); }

bool goblin::nmenu::rebind_pending()
{
    return g_rebind_waiting && g_page == kPageRebind && g_edit_entry != nullptr;
}

std::atomic<uint32_t> g_swallow_vk{0};

bool goblin::nmenu::key_swallowed(uint32_t vk)
{
    if (!vk || g_swallow_vk.load(std::memory_order_relaxed) != vk)
        return false;
    if (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000)
        return true; // still held from the rebind
    g_swallow_vk.store(0, std::memory_order_relaxed);
    return false;
}

void goblin::nmenu::rebind_apply(uint32_t vk)
{
    if (g_edit_entry && g_edit_entry->target && vk != 0)
    {
        *static_cast<uint32_t *>(g_edit_entry->target) = vk;
        g_dirty = true;
        // Written to disk now rather than at menu close: a key binding is the one setting the player
        // verifies by USING it, and using it may well mean closing the game.
        goblin::save_config(goblin::g_ini_path);
        g_swallow_vk.store(vk, std::memory_order_relaxed); // this very press must not also act
        goblin::reapply_live_settings();
        spdlog::info("[nmenu] {} rebound to 0x{:02X} (saved)", g_edit_entry->key, vk);
    }
    g_rebind_waiting = false;
    if (g_nested)
    {
        g_want_close = true; // the "press a key" screen closes itself once it has one
        return;
    }
    navigate_back();
}

bool goblin::nmenu::rebind_is_pad()
{
    return g_edit_entry && g_edit_entry->type == goblin::IniType::GamepadMask;
}

void goblin::nmenu::rebind_apply_pad(uint16_t mask)
{
    // Unlike the keyboard path, mask 0 is a REAL value here - it is how "no button" is spelled,
    // and the Unbind row is the only way to say it. Cancelling is the Back row instead.
    if (g_edit_entry && g_edit_entry->target)
    {
        *static_cast<uint16_t *>(g_edit_entry->target) = mask;
        g_dirty = true;
        // Same reasoning as the key path: a binding is verified by using it, and using it may
        // well mean closing the game, so it goes to disk now.
        goblin::save_config(goblin::g_ini_path);
        goblin::reapply_live_settings();
        spdlog::info("[nmenu] {} rebound to pad 0x{:04X} ({}) (saved)", g_edit_entry->key, mask,
                     mask ? goblin::format_gamepad_combo(mask) : std::string("none"));
    }
    g_rebind_waiting = false;
    if (g_nested)
    {
        g_want_close = true;
        return;
    }
    navigate_back();
}

bool goblin::nmenu::dirty() { return g_dirty; }
void goblin::nmenu::clear_dirty() { g_dirty = false; }
