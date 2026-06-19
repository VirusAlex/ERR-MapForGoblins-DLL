#include "goblin_messages.hpp"
#include "goblin_map_data.hpp"
#include "goblin_enemy_names.hpp"
#include "goblin_location_alt.hpp"
#include "goblin_config.hpp"
#include "goblin_inject.hpp"
#include "from/paramdef/WORLD_MAP_POINT_PARAM_ST.hpp"
#include "modutils.hpp"

#include <cstring>
#include <functional>
#include <set>
#include <spdlog/spdlog.h>
#include <deque>
#include <string>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace from::CS
{
class MsgRepositoryImp;
}

static from::CS::MsgRepositoryImp *msg_repository = nullptr;
static void *fmg_allocation = nullptr;

// Every PlaceName id that resolves to a real string after the FMG patch.
// Populated in patch_fmg_in_memory; read by sanitize_injected_textids().
static std::unordered_set<int32_t> g_placename_valid_ids;

// Toggle state for PlaceName FMG (slot 19). Only this slot is a pointer
// swap - other slots get surgical in-place additions we don't try to undo.
static uint8_t **g_placename_slot_ptr = nullptr;
static uint8_t *g_vanilla_placename_fmg = nullptr;
static uint8_t *g_expanded_placename_fmg = nullptr;
static bool g_fmg_injection_active = false;

// ── SEH-guarded slot access ──
// Some runtimes keep STALE pointers in MsgRepository slots the game never
// initialized - observed under ERR's loader with the DLC-layer slots
// (dereferencing one access-violates; pre-1.0.15 code carried the same
// warning for ActionButtonText 365/465). A bad slot must degrade to a
// skipped layer, not kill the whole setup_messages init step (that is the
// "?PlaceName? everywhere" failure: the PlaceName patch never commits).
// seh_call has no C++ objects (MSVC C2712), the job body lives outside it.
static int seh_run_job_thunk(void *ctx)
{
    return (*static_cast<std::function<int()> *>(ctx))();
}

static int seh_call(int (*fn)(void *), void *ctx)
{
    __try { return fn(ctx); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

static std::string detect_language()
{
    HMODULE steam = GetModuleHandleA("steam_api64.dll");
    if (!steam) return "english";
    typedef void *(*SteamApps_fn)();
    typedef const char *(*GetLang_fn)(void *);
    auto steamApps = (SteamApps_fn)GetProcAddress(steam, "SteamAPI_SteamApps_v008");
    auto getLang   = (GetLang_fn)GetProcAddress(steam, "SteamAPI_ISteamApps_GetCurrentGameLanguage");
    if (steamApps && getLang)
    {
        void *apps = steamApps();
        if (apps)
        {
            const char *lang = getLang(apps);
            if (lang && lang[0]) return lang;
        }
    }
    return "english";
}

// Map a Steam game-language string (from detect_language) to an index into the
// embedded enemy-name table's ENEMY_NAME_LANGS (msgbnd codes). Falls back to 0
// (engus) for anything unmapped.
static int enemy_name_lang_index(const std::string &steam_lang)
{
    static const std::pair<const char *, const char *> STEAM_TO_MSGBND[] = {
        {"english", "engus"}, {"japanese", "jpnjp"}, {"german", "deude"},
        {"french", "frafr"}, {"italian", "itait"}, {"koreana", "korkr"},
        {"polish", "polpl"}, {"brazilian", "porbr"}, {"portuguese", "porbr"},
        {"russian", "rusru"}, {"spanish", "spaes"}, {"latam", "spaar"},
        {"thai", "thath"}, {"schinese", "zhocn"}, {"tchinese", "zhotw"},
        {"arabic", "araae"},
    };
    const char *code = "engus";
    for (auto &m : STEAM_TO_MSGBND)
        if (steam_lang == m.first) { code = m.second; break; }
    for (int i = 0; i < goblin::generated::ENEMY_NAME_LANG_COUNT; i++)
        if (std::strcmp(code, goblin::generated::ENEMY_NAME_LANGS[i]) == 0)
            return i;
    return 0;
}

// In-memory FMG binary layout (Elden Ring, version 2):
//   0x00: uint32 header    (0x00020000 = version 2 packed)
//   0x04: uint32 fileSize
//   0x08: uint32 unk08     (1)
//   0x0C: uint32 groupCount
//   0x10: uint32 stringCount
//   0x14: uint32 unk14     (0xFF)
//   0x18: uint64 stringOffsetsOffset (relative to FMG start)
//   0x20: 8 bytes zeros
//   0x28: groups[groupCount], each 16 bytes:
//         int32 stringIndex, int32 firstId, int32 lastId, int32 pad(0)
//   stringOffsetsOffset: uint64[stringCount] - each is offset from FMG start to UTF-16LE string
//   after offsets: UTF-16LE null-terminated string data

struct FmgGroup
{
    int32_t string_index;
    int32_t first_id;
    int32_t last_id;
    int32_t pad;
};

struct NewEntry
{
    int32_t id;
    const wchar_t *text;
};

static bool patch_fmg_in_memory(uint8_t *fmg_ptr, uint8_t **slot_ptr,
                                const std::vector<NewEntry> &new_entries_in,
                                bool capture_valid_ids = false)
{
    // De-duplicate injected entries by id (first occurrence wins). Source FMG
    // group tables can cover the same id twice (observed in modded msgbnds),
    // and the two passes below (string-data append, then offset rebuild) MUST
    // agree on exactly which entries carry new strings: a single skipped
    // duplicate desynchronized every later string offset - labels after it
    // showed truncated/foreign text (reported in-game under The Convergence).
    std::vector<NewEntry> new_entries;
    {
        std::unordered_set<int32_t> seen;
        new_entries.reserve(new_entries_in.size());
        for (auto &ne : new_entries_in)
            if (seen.insert(ne.id).second)
                new_entries.push_back(ne);
        if (new_entries.size() != new_entries_in.size())
            spdlog::info("[PATCH] {} duplicate injected id(s) dropped",
                         new_entries_in.size() - new_entries.size());
    }
    uint32_t orig_file_size  = *reinterpret_cast<uint32_t *>(fmg_ptr + 0x04);
    uint32_t orig_group_cnt  = *reinterpret_cast<uint32_t *>(fmg_ptr + 0x0C);
    uint32_t orig_string_cnt = *reinterpret_cast<uint32_t *>(fmg_ptr + 0x10);
    uint64_t raw_str_off     = *reinterpret_cast<uint64_t *>(fmg_ptr + 0x18);

    // Detect fixup: game converts relative offset to absolute pointer at runtime
    uint64_t orig_str_off_rel;
    uint8_t *orig_offsets_ptr;
    if (raw_str_off > 0x1000000)
    {
        orig_offsets_ptr = reinterpret_cast<uint8_t *>(raw_str_off);
        orig_str_off_rel = (uint64_t)(orig_offsets_ptr - fmg_ptr);
    }
    else
    {
        orig_str_off_rel = raw_str_off;
        orig_offsets_ptr = fmg_ptr + orig_str_off_rel;
    }

    spdlog::debug("[PATCH] Original: fileSize={}, groups={}, strings={}, strOffRel=0x{:X} (raw=0x{:X})",
                  orig_file_size, orig_group_cnt, orig_string_cnt, orig_str_off_rel, raw_str_off);

    auto *orig_groups = reinterpret_cast<FmgGroup *>(fmg_ptr + 0x28);
    auto *orig_offsets = reinterpret_cast<uint64_t *>(orig_offsets_ptr);

    struct ExistingEntry
    {
        int32_t id;
        uint64_t str_offset; // offset from FMG start
    };
    std::vector<ExistingEntry> all_entries;
    all_entries.reserve(orig_string_cnt + new_entries.size());

    for (uint32_t g = 0; g < orig_group_cnt; g++)
    {
        int32_t idx = orig_groups[g].string_index;
        int32_t fid = orig_groups[g].first_id;
        int32_t lid = orig_groups[g].last_id;
        for (int32_t id = fid; id <= lid; id++)
        {
            int32_t si = idx + (id - fid);
            if (si >= 0 && si < (int32_t)orig_string_cnt)
                all_entries.push_back({id, orig_offsets[si]});
        }
    }

    spdlog::debug("[PATCH] Parsed {} existing entries", all_entries.size());

    for (size_t i = 0; i < (std::min)(all_entries.size(), (size_t)5); i++)
    {
        auto &e = all_entries[i];
        spdlog::debug("[PATCH] Entry[{}]: id={}, strOff=0x{:X}",
                      i, e.id, e.str_offset);
    }

    // Injected entries OVERRIDE pre-existing ids. Overhaul mods can pre-seed
    // rows at ids our offset encoding also uses (The Convergence ships
    // boss-text PlaceName rows in the 9xxM band, some with EMPTY text -
    // observed shadowing our "Summoning Pools" label at 900301690). Keeping
    // the old row would make the merge below skip ours, so drop it first.
    if (!new_entries.empty())
    {
        std::unordered_set<int32_t> override_ids;
        for (auto &ne : new_entries)
            override_ids.insert(ne.id);
        size_t before = all_entries.size();
        all_entries.erase(std::remove_if(all_entries.begin(), all_entries.end(),
                                         [&](const ExistingEntry &e)
                                         { return override_ids.count(e.id) != 0; }),
                          all_entries.end());
        if (before != all_entries.size())
            spdlog::info("[PATCH] {} existing FMG ids overridden by injected entries",
                         before - all_entries.size());
    }

    uint64_t orig_str_data_start = orig_str_off_rel + orig_string_cnt * 8;

    std::vector<uint8_t> new_str_data;
    size_t orig_str_data_len = orig_file_size - orig_str_data_start;
    new_str_data.resize(orig_str_data_len);
    memcpy(new_str_data.data(), fmg_ptr + orig_str_data_start, orig_str_data_len);

    struct AllEntry
    {
        int32_t id;
        uint64_t str_offset;
    };

    std::vector<AllEntry> merged;
    merged.reserve(all_entries.size() + new_entries.size());

    for (auto &e : all_entries)
        merged.push_back({e.id, e.str_offset});

    struct PendingStr
    {
        size_t merged_idx;
        size_t data_offset; // offset within new_str_data
    };
    std::vector<PendingStr> pending;

    for (auto &ne : new_entries)
    {
        bool exists = false;
        for (auto &m : merged)
        {
            if (m.id == ne.id)
            {
                exists = true;
                break;
            }
        }
        if (exists) continue;

        size_t str_start = new_str_data.size();
        size_t wlen = wcslen(ne.text);
        size_t byte_len = (wlen + 1) * sizeof(wchar_t);
        new_str_data.resize(new_str_data.size() + byte_len);
        memcpy(new_str_data.data() + str_start, ne.text, byte_len);

        pending.push_back({merged.size(), str_start});
        merged.push_back({ne.id, 0}); // offset TBD
    }

    std::sort(merged.begin(), merged.end(),
              [](const AllEntry &a, const AllEntry &b) { return a.id < b.id; });

    // Record every PlaceName id that now resolves to a real string, so
    // sanitize_injected_textids() can strip marker textIds that point at a
    // missing entry (game's GetMessage returns null → wstring(null) → crash).
    // ONLY for the PlaceName (slot 19) patch - this function is also used for
    // the TutorialBody (codex toast) patch, whose id set is unrelated; capturing
    // that would wrongly flag every marker textId as missing and clear them all.
    if (capture_valid_ids)
    {
        g_placename_valid_ids.clear();
        for (auto &m : merged)
            g_placename_valid_ids.insert(m.id);
    }

    uint32_t total_strings = (uint32_t)merged.size();

    spdlog::debug("[PATCH] Total entries after merge: {}", total_strings);

    // One group per entry (sparse IDs)
    uint32_t total_groups = total_strings;

    constexpr size_t HEADER_SIZE = 0x28;
    size_t groups_size = total_groups * sizeof(FmgGroup);
    size_t new_str_off_pos = HEADER_SIZE + groups_size;
    size_t offsets_size = total_strings * sizeof(uint64_t);
    size_t new_str_data_start = new_str_off_pos + offsets_size;
    size_t new_file_size = new_str_data_start + new_str_data.size();

    spdlog::debug("[PATCH] New layout: groups={}, strings={}, fileSize={}",
                  total_groups, total_strings, new_file_size);

    // Allocate the expanded FMG buffer from the process heap.
    fmg_allocation = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, new_file_size);
    if (!fmg_allocation)
    {
        spdlog::error("[PATCH] HeapAlloc failed ({} bytes)", new_file_size);
        return false;
    }

    auto *nfmg = reinterpret_cast<uint8_t *>(fmg_allocation);

    *reinterpret_cast<uint32_t *>(nfmg + 0x00) = 0x00020000;
    *reinterpret_cast<uint32_t *>(nfmg + 0x04) = (uint32_t)new_file_size;
    *reinterpret_cast<uint32_t *>(nfmg + 0x08) = 1;
    *reinterpret_cast<uint32_t *>(nfmg + 0x0C) = total_groups;
    *reinterpret_cast<uint32_t *>(nfmg + 0x10) = total_strings;
    *reinterpret_cast<uint32_t *>(nfmg + 0x14) = 0xFF;
    // Game expects fixed-up absolute pointer, not relative offset
    *reinterpret_cast<uint64_t *>(nfmg + 0x18) = (uint64_t)(nfmg + new_str_off_pos);

    // Rebuild pending map: merged_idx is stale after sort, so re-derive from IDs
    std::unordered_map<int32_t, size_t> new_entry_data_offset;
    {
        size_t data_pos = orig_str_data_len; // where new strings start in new_str_data
        for (auto &ne : new_entries)
        {
            bool exists_in_orig = false;
            for (auto &e : all_entries)
            {
                if (e.id == ne.id) { exists_in_orig = true; break; }
            }
            if (exists_in_orig) continue;

            new_entry_data_offset[ne.id] = new_str_data_start + data_pos;
            size_t wlen = wcslen(ne.text);
            data_pos += (wlen + 1) * sizeof(wchar_t);
        }
    }

    auto *new_groups = reinterpret_cast<FmgGroup *>(nfmg + HEADER_SIZE);
    auto *new_offsets = reinterpret_cast<uint64_t *>(nfmg + new_str_off_pos);

    for (uint32_t i = 0; i < total_strings; i++)
    {
        new_groups[i].string_index = (int32_t)i;
        new_groups[i].first_id = merged[i].id;
        new_groups[i].last_id = merged[i].id;
        new_groups[i].pad = 0;

        if (merged[i].str_offset != 0)
        {
            // Remap old relative offset into new layout
            uint64_t old_off = merged[i].str_offset;
            if (old_off >= orig_str_data_start)
            {
                uint64_t within = old_off - orig_str_data_start;
                new_offsets[i] = new_str_data_start + within;
            }
            else
            {
                new_offsets[i] = 0;
            }
        }
        else
        {
            auto it = new_entry_data_offset.find(merged[i].id);
            if (it != new_entry_data_offset.end())
                new_offsets[i] = it->second;
            else
                new_offsets[i] = 0;
        }
    }

    memcpy(nfmg + new_str_data_start, new_str_data.data(), new_str_data.size());

    spdlog::debug("[PATCH] Swapping FMG pointer: {:p} -> {:p}", (void *)*slot_ptr, fmg_allocation);
    *slot_ptr = nfmg;

    spdlog::debug("[PATCH] PlaceName FMG patched: {} entries", total_strings);
    return true;
}

void goblin::setup_messages()
{
    using namespace goblin::generated;

    auto lang = detect_language();
    spdlog::debug("Detected language: {}", lang);

    std::vector<NewEntry> new_entries;


    // Collect item IDs used as textId - need to copy from various FMGs to PlaceName
    // FmgId slots: GoodsName=10, WeaponName=11, ProtectorName=12, AccessoryName=13,
    //              MagicName=14, NpcName=18, PlaceName=19, GemName=35, ArtsName=42,
    //              TutorialTitle=207
    // textId encoding: real_id + category_offset
    //   100000000+id:      WeaponName (cat=2, weapons)
    //   200000000+id:      ProtectorName (cat=3, armour)
    //   300000000+id:      AccessoryName (cat=4, talismans)
    //   400000000+id:      GemName (cat=5, ashes of war)
    //   500000000+id:      GoodsName (cat=1, goods)
    //   (600M band = EventTextForMap is RESERVED but unused - no marker emits it in
    //    any profile; resolving it would need the menu msgbnd bank. See
    //    docs/messages_offline_research.md.)
    //   700000000+id:      NpcName (named-NPC and 9MMMMVVV "Characters" boss names)
    //   800000000+id:      ActionButtonText (in-game interact prompts, e.g. "Examine statue")
    //   900000000+id:      TutorialTitle (enemy names)
    //   950000000+id:      BloodMsg (message-builder vocabulary words - generic
    //                      enemy-type labels for the vanilla build: "skeleton",
    //                      "demi-human", ... localized in all languages)
    // NpcName has two ID ranges in vanilla+ERR: small ids (Patches=130900,
    // Garris=137600) and 9MMMMVVV "Characters" (Stray Mimic Tear=903320300).
    // With +700M offset, small ids land in [700M..800M); the large 9MMMMVVV ids
    // land in [1600M..1700M). Both ranges classify here.
    // Legacy (pre-offset): raw PlaceName IDs < 100M for location names
    std::set<int32_t> goods_ids_needed;      // GoodsName FMG, slot 10
    std::set<int32_t> weapon_ids_needed;     // WeaponName FMG, slot 11
    std::set<int32_t> protector_ids_needed;  // ProtectorName FMG, slot 12
    std::set<int32_t> accessory_ids_needed;  // AccessoryName FMG, slot 13
    std::set<int32_t> gem_ids_needed;        // GemName FMG, slot 14
    std::set<int32_t> npc_name_ids_needed;   // NpcName FMG, slot 18 (+ DLC 328, 428)
    std::set<int32_t> action_btn_ids_needed; // ActionButtonText FMG, slot 32 (+ DLC 365, 465)
    std::set<int32_t> tutorial_ids_needed;   // TutorialTitle FMG, slot 207 (enemy names from ERR Codex)
    std::set<int32_t> bloodmsg_ids_needed;   // BloodMsg FMG, slot 2 (vanilla enemy-type words, offset 950M)
    for (size_t i = 0; i < generated::MAP_ENTRY_COUNT; i++)
    {
        const auto &e = generated::MAP_ENTRIES[i];
        const int32_t *text_ids[] = {
            &e.data.textId1, &e.data.textId2, &e.data.textId3, &e.data.textId4,
            &e.data.textId5, &e.data.textId6, &e.data.textId7, &e.data.textId8,
        };
        for (auto *tidp : text_ids)
        {
            int32_t tid = *tidp;
            if (tid <= 0) continue;
            if (tid >= 1600000000 && tid < 1700000000)   // NpcName "Characters" hi-range (9MMMMVVV+700M)
                npc_name_ids_needed.insert(tid);
            else if (tid >= 950000000 && tid < 960000000) // BloodMsg vocabulary (offset 950M)
                bloodmsg_ids_needed.insert(tid);
            else if (tid >= 900000000)          // TutorialTitle (enemy names)
                tutorial_ids_needed.insert(tid);
            else if (tid >= 800000000 && tid < 900000000) // ActionButtonText (offset 800M)
                action_btn_ids_needed.insert(tid);
            else if (tid >= 700000000 && tid < 800000000) // NpcName low-range (small id+700M)
                npc_name_ids_needed.insert(tid);
            else if (tid >= 500000000 && tid < 600000000) // GoodsName (goods, offset 500M)
                goods_ids_needed.insert(tid);
            else if (tid >= 400000000)          // GemName (ashes of war, offset 400M)
                gem_ids_needed.insert(tid);
            else if (tid >= 300000000)          // AccessoryName (talismans, offset 300M)
                accessory_ids_needed.insert(tid);
            else if (tid >= 200000000)          // ProtectorName (armour, offset 200M)
                protector_ids_needed.insert(tid);
            else if (tid >= 100000000)          // WeaponName (weapons, offset 100M)
                weapon_ids_needed.insert(tid);
            // IDs < 100M are raw PlaceName IDs (location names, no copy needed)
        }
    }
    spdlog::debug("{} Goods + {} Weapon + {} Protector + {} Accessory + {} Gem + {} NpcName + {} ActionBtn + {} Tutorial IDs need lookup",
                  goods_ids_needed.size(), weapon_ids_needed.size(),
                  protector_ids_needed.size(), accessory_ids_needed.size(),
                  gem_ids_needed.size(),
                  npc_name_ids_needed.size(), action_btn_ids_needed.size(),
                  tutorial_ids_needed.size());

    auto msg_repository_address = modutils::scan<from::CS::MsgRepositoryImp *>({
        .aob = "48 8B 3D ?? ?? ?? ?? 44 0F B6 30 48 85 FF 75",
        .relative_offsets = {{3, 7}},
    });
    if (!msg_repository_address) { spdlog::error("MsgRepositoryImp AOB not found"); return; }

    while (!(msg_repository = *msg_repository_address))
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    spdlog::debug("MsgRepositoryImp at {:p}", (void *)msg_repository);

    // PlaceName = bnd index 19 in MsgRepositoryImp
    auto *repo = reinterpret_cast<uint8_t *>(msg_repository);
    auto base_array = *reinterpret_cast<uint8_t ***>(repo + 0x08);
    int32_t count2 = *reinterpret_cast<int32_t *>(repo + 0x14);

    if (!base_array || !base_array[0] || 19 >= count2)
    {
        spdlog::error("Cannot navigate to PlaceName FMG slot");
        return;
    }

    auto **sub = reinterpret_cast<uint8_t **>(base_array[0]);

    // Helper: copy entries from an FMG to PlaceName, with offset remapping.
    // needed_ids contains offset-encoded IDs; real FMG id = offset_id - offset_base.
    // The PlaceName entry is written at the offset-encoded ID. Satisfied ids
    // are ERASED from needed_ids so layered calls only fill what is missing.
    auto copy_fmg_entries = [&](uint8_t *fmg_ptr, std::set<int32_t> &needed_ids,
                                int32_t offset_base, const char *label, int slot) -> int
    {
        if (needed_ids.empty() || !fmg_ptr) return 0;

        uint32_t grp_cnt = *reinterpret_cast<uint32_t *>(fmg_ptr + 0x0C);
        uint32_t str_cnt = *reinterpret_cast<uint32_t *>(fmg_ptr + 0x10);
        uint64_t raw_off = *reinterpret_cast<uint64_t *>(fmg_ptr + 0x18);

        // Sanity-guard the header before walking: a stale/foreign slot pointer
        // would otherwise send us through garbage group tables.
        if (grp_cnt > 0x100000 || str_cnt > 0x100000 || raw_off == 0)
        {
            spdlog::debug("{}: slot {} header implausible (groups={}, strings={}) - skipped",
                          label, slot, grp_cnt, str_cnt);
            return 0;
        }

        uint8_t *off_ptr = (raw_off > 0x1000000)
            ? reinterpret_cast<uint8_t *>(raw_off)
            : fmg_ptr + raw_off;

        auto *groups = reinterpret_cast<FmgGroup *>(fmg_ptr + 0x28);
        auto *str_offs = reinterpret_cast<uint64_t *>(off_ptr);

        // Build reverse lookup: real_id -> offset_id (for ammo: offset_base=0, id stored as-is)
        std::unordered_map<int32_t, int32_t> real_to_offset;
        for (int32_t oid : needed_ids)
            real_to_offset[oid - offset_base] = oid;

        int copied = 0;
        for (uint32_t g = 0; g < grp_cnt; g++)
        {
            int32_t first = groups[g].first_id;
            int32_t last = groups[g].last_id;
            int32_t si = groups[g].string_index;
            for (int32_t id = first; id <= last; id++, si++)
            {
                if (si < 0 || si >= (int32_t)str_cnt) continue;
                auto it = real_to_offset.find(id);
                if (it == real_to_offset.end()) continue;

                uint64_t s_off = str_offs[si];
                if (s_off == 0) continue;
                const wchar_t *text = (s_off > 0x1000000)
                    ? reinterpret_cast<const wchar_t *>(s_off)
                    : reinterpret_cast<const wchar_t *>(fmg_ptr + s_off);
                if (text && text[0])
                {
                    new_entries.push_back({it->second, text});  // write at offset-encoded ID
                    needed_ids.erase(it->second);
                    // also drop from the reverse map: FMG group tables can
                    // cover the same id twice - never push it twice
                    real_to_offset.erase(it);
                    copied++;
                }
            }
        }
        if (copied)
            spdlog::info("Copied {} {} entries to PlaceName (slot {})", copied, label, slot);
        return copied;
    };

    // The runtime keeps every msgbnd FMG layer in its OWN MsgRepository slot
    // (base / _dlc01 / _dlc02) and merges them at LOOKUP time, highest layer
    // first - verified live: WeaponName base slot 11 does NOT contain ids that
    // ship in WeaponName_dlc01 (slot 310). Overhaul mods (The Convergence) put
    // their added strings in the DLC layers, so the base slot alone misses
    // them. Walk the layers in the game's lookup priority; copy_fmg_entries
    // erases satisfied ids, so later (lower-priority) layers only fill gaps.
    auto copy_fmg_layered = [&](std::initializer_list<int> slots,
                                std::set<int32_t> &needed_ids,
                                int32_t offset_base, const char *label) -> int
    {
        int total = 0;
        for (int slot : slots)
        {
            if (needed_ids.empty()) break;
            if (slot >= count2 || !sub[slot]) continue;
            // SEH-guard each slot: stale slot pointers (ERR loader, or a
            // vanilla install without the DLC) must not kill the init step.
            // Roll both outputs back on failure so a partial walk over a
            // garbage FMG never leaks bogus entries into the patch.
            size_t entries_mark = new_entries.size();
            std::set<int32_t> ids_snapshot = needed_ids;
            std::function<int()> job = [&, slot]() -> int
            { return copy_fmg_entries(sub[slot], needed_ids, offset_base, label, slot); };
            int copied = seh_call(&seh_run_job_thunk, &job);
            if (copied < 0)
            {
                new_entries.resize(entries_mark);
                needed_ids = std::move(ids_snapshot);
                spdlog::warn("{}: slot {} not readable in this runtime - layer skipped", label, slot);
                continue;
            }
            total += copied;
        }
        if (!needed_ids.empty())
            spdlog::debug("{}: {} ids not found in any layer", label, needed_ids.size());
        return total;
    };

    // Live-loot labels (config::liveLootLabels): the randomizer can place ANY
    // item at a loot light-point, so we can't know at bake time which item id
    // a marker will need. Copy EVERY entry from a source FMG family into
    // PlaceName at its offset-encoded id, so refresh_loot_from_itemlot() can
    // point a marker's textId1 at whatever item the live ItemLotParam now gives.
    // first-hit-wins across layers (dlc02 → dlc01 → base), same as the targeted
    // walk. ammo_as_is: WeaponName ammo ids (>=50M) are stored unshifted.
    auto copy_fmg_all_layered = [&](std::initializer_list<int> slots, int32_t offset_base,
                                    const char *label, bool ammo_as_is) -> int
    {
        std::unordered_set<int32_t> seen;
        int total = 0;
        for (int slot : slots)
        {
            if (slot >= count2 || !sub[slot]) continue;
            size_t entries_mark = new_entries.size();
            std::function<int()> job = [&, slot]() -> int
            {
                int copied = 0;
                uint8_t *f = sub[slot];
                uint32_t grp_cnt = *reinterpret_cast<uint32_t *>(f + 0x0C);
                uint32_t str_cnt = *reinterpret_cast<uint32_t *>(f + 0x10);
                uint64_t raw_off = *reinterpret_cast<uint64_t *>(f + 0x18);
                if (grp_cnt > 0x100000 || str_cnt > 0x100000 || raw_off == 0) return 0;
                uint8_t *off_ptr = (raw_off > 0x1000000)
                    ? reinterpret_cast<uint8_t *>(raw_off) : f + raw_off;
                auto *groups = reinterpret_cast<FmgGroup *>(f + 0x28);
                auto *str_offs = reinterpret_cast<uint64_t *>(off_ptr);
                for (uint32_t g = 0; g < grp_cnt; g++)
                {
                    int32_t si = groups[g].string_index;
                    for (int32_t id = groups[g].first_id; id <= groups[g].last_id; id++, si++)
                    {
                        if (si < 0 || si >= (int32_t)str_cnt) continue;
                        if (!seen.insert(id).second) continue; // higher layer already won
                        uint64_t s_off = str_offs[si];
                        if (s_off == 0) continue;
                        const wchar_t *text = (s_off > 0x1000000)
                            ? reinterpret_cast<const wchar_t *>(s_off)
                            : reinterpret_cast<const wchar_t *>(f + s_off);
                        if (!text || !text[0]) continue;
                        int32_t enc = (ammo_as_is && id >= 50000000) ? id : id + offset_base;
                        new_entries.push_back({enc, text});
                        copied++;
                    }
                }
                return copied;
            };
            int copied = seh_call(&seh_run_job_thunk, &job);
            if (copied < 0)
            {
                new_entries.resize(entries_mark);
                spdlog::warn("{}(all): slot {} not readable in this runtime - layer skipped", label, slot);
                continue;
            }
            total += copied;
        }
        if (total)
            spdlog::info("Copied ALL {} {} entries to PlaceName (live-loot labels)", total, label);
        return total;
    };

    // Slot lists per FMG family, in the game's lookup-priority order
    // (dlc02 → dlc01 → base). The ERR build walks ONLY the base slots:
    // everything its bake references lives there (proven through v1.0.14),
    // and the DLC-layer slots were observed to hold stale pointers under
    // ERR's loader - touching them caused the "?PlaceName?" incident in
    // v1.0.15 (SEH killed setup_messages before the PlaceName patch).
#ifdef MFG_VANILLA
    const std::initializer_list<int>
        goods_slots{419, 319, 10}, weapon_slots{410, 310, 11},
        protector_slots{413, 313, 12}, accessory_slots{416, 316, 13},
        gem_slots{422, 322, 35, 42}, npc_slots{428, 328, 18},
        bloodmsg_slots{461, 361, 2}, tutorial_slots{475, 375, 207};
#else
    const std::initializer_list<int>
        goods_slots{10}, weapon_slots{11},
        protector_slots{12}, accessory_slots{13},
        gem_slots{35, 42}, npc_slots{18},
        bloodmsg_slots{2}, tutorial_slots{207};
#endif

    // GoodsName. Offset 500M.
    copy_fmg_layered(goods_slots, goods_ids_needed, 500000000, "GoodsName");

    // WeaponName. Ammo IDs are stored as-is (>=50M), weapon IDs are offset by 100M.
    {
        std::set<int32_t> ammo_ids, weapon_offset_ids;
        for (int32_t tid : weapon_ids_needed)
        {
            if (tid >= 100000000)
                weapon_offset_ids.insert(tid);
            else
                ammo_ids.insert(tid);
        }
        copy_fmg_layered(weapon_slots, ammo_ids, 0, "WeaponName(ammo)");
        copy_fmg_layered(weapon_slots, weapon_offset_ids, 100000000, "WeaponName(weapon)");
    }

    // ProtectorName. Offset 200M.
    copy_fmg_layered(protector_slots, protector_ids_needed, 200000000, "ProtectorName");

    // AccessoryName. Offset 300M.
    copy_fmg_layered(accessory_slots, accessory_ids_needed, 300000000, "AccessoryName");

    // GemName (ArtsName 42 as fallback). Offset 400M.
    copy_fmg_layered(gem_slots, gem_ids_needed, 400000000, "GemName");

    // Live-loot labels: pull the WHOLE item-name space into PlaceName so a
    // marker can be relabeled to any randomized item at runtime. Offsets match
    // the encoding above; the targeted copies stay (they're a subset, deduped
    // first-wins by patch_fmg_in_memory). Gated - off by default it adds nothing.
    if (goblin::config::liveLootLabels)
    {
        copy_fmg_all_layered(goods_slots, 500000000, "GoodsName", false);
        copy_fmg_all_layered(weapon_slots, 100000000, "WeaponName", true);
        copy_fmg_all_layered(protector_slots, 200000000, "ProtectorName", false);
        copy_fmg_all_layered(accessory_slots, 300000000, "AccessoryName", false);
        copy_fmg_all_layered(gem_slots, 400000000, "GemName", false);
    }

    // NpcName. Offset 700M.
    // (Was break-on-first-success from base 18 - that missed every id living
    // only in a DLC layer, e.g. The Convergence's added bosses in slot 328.)
    copy_fmg_layered(npc_slots, npc_name_ids_needed, 700000000, "NpcName");

    // Read ActionButtonText FMG (slots 32 + DLC 365, 465): in-game interact
    // prompts ("Examine statue", "Light bonfire", ...). Offset 800M.
    // ActionButtonText lives in menu.msgbnd but MsgRepositoryImp's FMG slot
    // array is indexed by global BND file ID - slot 32 returns the menu
    // ActionButtonText FMG directly, no separate bank navigation needed.
    // DLC slots merge their own entries on top (e.g. DLC2 7041 = "Examine statue").
    // Slot 32 = ActionButtonText.fmg. ER runtime already merges DLC entries
    // into the base FMG at this slot - we observed 7041 ("Examine statue",
    // added in DLC02) being readable from slot 32 in-game. Don't poke DLC
    // slots 365/465 directly: in past testing accessing those past the
    // valid count2 range derailed downstream FMG copies (TutorialTitle
    // never ran and PlaceName patch never committed → "?PlaceName?" text).
    if (!action_btn_ids_needed.empty() && 32 < count2 && sub[32])
        copy_fmg_entries(sub[32], action_btn_ids_needed, 800000000, "ActionButtonText", 32);

    // Read BloodMsg FMG (slot 2, menu.msgbnd): message-builder vocabulary.
    // The vanilla build labels generic enemy drops with the closest vocabulary
    // word ("skeleton", "demi-human", ...) - localized for free, same as the
    // in-game message composer. Offset 950M. Same direct-slot access as
    // ActionButtonText above (slot array is indexed by global BND file ID).
    // BloodMsg layers (vocabulary is base-game, but overhauls may extend it).
    // Spoiler-free mode: pull the generic localized label ("something",
    // BloodMsg word 32004) into PlaceName so anonymous loot markers can point at
    // it. Offset 950M, same band as the vanilla enemy-type vocabulary.
    // ALWAYS inject it (not gated on anonymousLoot): the marker re-point to this
    // id happens live when the user toggles anonymous_loot in the overlay, but FMG
    // text injection is init-only - so without this, a live toggle shows
    // "?PlaceName?" (the id has no PlaceName entry). One harmless extra entry.
    bloodmsg_ids_needed.insert(950000000 + 32004);

    copy_fmg_layered(bloodmsg_slots, bloodmsg_ids_needed, 950000000, "BloodMsg");

    // TutorialTitle (ERR Codex enemy names + category labels like "Summoning
    // Pools"). textId in MASSEDIT = real TutorialTitle ID + 900000000 (to
    // avoid collision with GoodsName).
    copy_fmg_layered(tutorial_slots, tutorial_ids_needed, 900000000, "TutorialTitle");

    // Non-ERR builds carry their own localized enemy-name table (the runtime
    // FMG has only generic tutorial entries, no enemy names). Inject the names
    // for the detected language into PlaceName at the same +900M encoding the
    // markers use, so enemy-drop labels read properly instead of falling back to
    // the generic vocabulary words. The ERR build ships an empty table and uses
    // its runtime names, so this loop is a no-op there.
    if (generated::ENEMY_NAME_COUNT > 0)
    {
        int li = enemy_name_lang_index(lang);
        int added = 0;
        for (size_t i = 0; i < generated::ENEMY_NAME_COUNT; i++)
        {
            const auto &en = generated::ENEMY_NAMES[i];
            const wchar_t *nm = en.names[li] ? en.names[li] : en.names[0];
            if (nm && nm[0])
            {
                new_entries.push_back({en.id + 900000000, nm});
                added++;
            }
        }
        spdlog::info("Injected {} enemy names into PlaceName (lang index {})", added, li);
    }

    auto *fmg_ptr = sub[19];

    if (!fmg_ptr)
    {
        spdlog::error("PlaceName FMG (bnd=19) is null");
        return;
    }

    spdlog::debug("PlaceName FMG at {:p}", (void *)fmg_ptr);

    // Composed labels for duplicate-named sub-zones (e.g. the two Hallowhorn Grounds):
    // synthesize "<sub> (<super>)" from the game's OWN PlaceName strings so the text is
    // correct in any language. Storage is a function-local static deque - pointer-stable.
    {
        auto fmg_find = [&](uint8_t *fmg, int32_t id) -> const wchar_t *
        {
            uint32_t grp_cnt = *reinterpret_cast<uint32_t *>(fmg + 0x0C);
            uint32_t str_cnt = *reinterpret_cast<uint32_t *>(fmg + 0x10);
            uint64_t raw_off = *reinterpret_cast<uint64_t *>(fmg + 0x18);
            uint8_t *off_ptr = (raw_off > 0x1000000) ? reinterpret_cast<uint8_t *>(raw_off) : fmg + raw_off;
            auto *groups = reinterpret_cast<FmgGroup *>(fmg + 0x28);
            auto *str_offs = reinterpret_cast<uint64_t *>(off_ptr);
            for (uint32_t g = 0; g < grp_cnt; g++)
            {
                if (id < groups[g].first_id || id > groups[g].last_id) continue;
                int32_t si = groups[g].string_index + (id - groups[g].first_id);
                if (si < 0 || si >= (int32_t)str_cnt) return nullptr;
                uint64_t s_off = str_offs[si];
                if (s_off == 0) return nullptr;
                return (s_off > 0x1000000) ? reinterpret_cast<const wchar_t *>(s_off)
                                           : reinterpret_cast<const wchar_t *>(fmg + s_off);
            }
            return nullptr;
        };
        // Look the parts up the way the game does: DLC PlaceName layers first
        // (429, 329), then the base slot - overhauls keep reworked zone names
        // in the DLC layers only. ERR build: base slot only (its DLC slot
        // pointers can be stale - see the slot-list comment above).
        auto find_layered = [&](int32_t id) -> const wchar_t *
        {
#ifdef MFG_VANILLA
            for (int slot : {429, 329})
                if (slot < count2 && sub[slot])
                    if (const wchar_t *t = fmg_find(sub[slot], id); t && t[0])
                        return t;
#endif
            return fmg_find(fmg_ptr, id);
        };
        static std::deque<std::wstring> compose_storage;
        int composed = 0;
        for (size_t i = 0; i < generated::LOCATION_COMPOSE_COUNT; i++)
        {
            const auto &c = generated::LOCATION_COMPOSE[i];
            const wchar_t *sub_txt = find_layered(c.subId);
            const wchar_t *sup_txt = find_layered(c.superId);
            if (!sub_txt || !sup_txt)
            {
                spdlog::warn("Compose label {}: PlaceName {} or {} not found in FMG", c.id, c.subId, c.superId);
                continue;
            }
            compose_storage.emplace_back(std::wstring(sub_txt) + L" (" + sup_txt + L")");
            new_entries.push_back({c.id, compose_storage.back().c_str()});
            composed++;
        }
        if (composed)
            spdlog::info("Composed {} duplicate-zone labels into PlaceName", composed);
    }

    if (patch_fmg_in_memory(fmg_ptr, &sub[19], new_entries, /*capture_valid_ids=*/true))
    {
        spdlog::info("PlaceName FMG patched ({} entries)", new_entries.size());
        // Capture toggle state. fmg_ptr was the original buffer; sub[19] now
        // points at our expanded buffer (set inside patch_fmg_in_memory).
        g_placename_slot_ptr = &sub[19];
        g_vanilla_placename_fmg = fmg_ptr;
        g_expanded_placename_fmg = sub[19];
        g_fmg_injection_active = true;

#ifdef MFG_VANILLA
        // The game resolves PlaceName through the DLC layers FIRST (slots 429,
        // 329) and falls back to base slot 19. Ids that live only in a DLC
        // layer (vanilla DLC zones; The Convergence's reworked zones and boss
        // texts) are perfectly valid marker textIds even though our expanded
        // base FMG doesn't carry them - whitelist them so the sanitizer
        // doesn't clear those labels (observed: 1330 false-cleared under The
        // Convergence, whose layers hold 520+ dlc01-only zone ids).
        // Not compiled for ERR: its bake never relies on DLC-layer-only ids,
        // and its DLC slot pointers can be stale (see the slot-list comment).
        size_t dlc_valid = 0;
        for (int slot : {329, 429})
        {
            if (slot >= count2 || !sub[slot]) continue;
            std::function<int()> job = [&]() -> int
            {
                int found = 0;
                uint8_t *f = sub[slot];
                uint32_t grp_cnt = *reinterpret_cast<uint32_t *>(f + 0x0C);
                uint32_t str_cnt = *reinterpret_cast<uint32_t *>(f + 0x10);
                uint64_t raw_off = *reinterpret_cast<uint64_t *>(f + 0x18);
                if (grp_cnt > 0x100000 || str_cnt > 0x100000 || raw_off == 0) return 0;
                uint8_t *off_ptr = (raw_off > 0x1000000)
                    ? reinterpret_cast<uint8_t *>(raw_off) : f + raw_off;
                auto *groups = reinterpret_cast<FmgGroup *>(f + 0x28);
                auto *str_offs = reinterpret_cast<uint64_t *>(off_ptr);
                for (uint32_t g = 0; g < grp_cnt; g++)
                {
                    int32_t si = groups[g].string_index;
                    for (int32_t id = groups[g].first_id; id <= groups[g].last_id; id++, si++)
                    {
                        if (si < 0 || si >= (int32_t)str_cnt) continue;
                        uint64_t s_off = str_offs[si];
                        if (s_off == 0) continue;
                        const wchar_t *text = (s_off > 0x1000000)
                            ? reinterpret_cast<const wchar_t *>(s_off)
                            : reinterpret_cast<const wchar_t *>(f + s_off);
                        if (text && text[0] && g_placename_valid_ids.insert(id).second)
                            found++;
                    }
                }
                return found;
            };
            int found = seh_call(&seh_run_job_thunk, &job);
            if (found < 0)
            {
                spdlog::warn("PlaceName layer slot {} not readable - skipped", slot);
                continue;
            }
            dlc_valid += found;
        }
        if (dlc_valid)
            spdlog::info("PlaceName DLC layers contribute {} additional valid textIds", dlc_valid);
#endif
    }
    else
        spdlog::error("PlaceName FMG patching failed");

    // Inject NEW TutorialBody entries (slot 208 = 0xD0) for the codex toasts.
    // CSPopupMenu::ShowTutorialPopup (trampoline, AOB-resolved at runtime - its
    // RVA shifts on game updates) looks up TutorialParam[id].textId then
    // TutorialBody.fmg[textId]. Matching TutorialParam rows are injected by
    // goblin::inject_tutorial_popup_rows. Using fresh ids leaves all vanilla/ERR
    // codex text untouched. All four entries (ON/OFF/DUMP_OK/DUMP_FAIL) are
    // STATIC strings written below - there is no runtime text rewriting.
    if (count2 > 208 && sub[208])
    {
        std::vector<NewEntry> tb_entries = {
            {goblin::TUTORIAL_FMG_ID_ON,        L"Map icons: ON"},
            {goblin::TUTORIAL_FMG_ID_OFF,       L"Map icons: OFF"},
            {goblin::TUTORIAL_FMG_ID_DUMP_OK,   L"Markers dumped"},
            {goblin::TUTORIAL_FMG_ID_DUMP_FAIL, L"Marker dump failed - press again"},
        };
        if (patch_fmg_in_memory(sub[208], &sub[208], tb_entries))
            spdlog::info("[TOAST] TutorialBody.fmg expanded (ON={}, OFF={}, DUMP_OK={}, DUMP_FAIL={})",
                         goblin::TUTORIAL_FMG_ID_ON, goblin::TUTORIAL_FMG_ID_OFF,
                         goblin::TUTORIAL_FMG_ID_DUMP_OK, goblin::TUTORIAL_FMG_ID_DUMP_FAIL);
        else
            spdlog::warn("[TOAST] TutorialBody.fmg patch failed - codex banners unavailable");
    }
    else
        spdlog::warn("[TOAST] TutorialBody (slot 208) unavailable - codex banners unavailable");

    // Final safety pass: strip any marker textId that didn't end up with a real
    // string in the expanded PlaceName FMG. The game's GetMessage returns null
    // for a missing id and some callers build a std::wstring from it → null
    // deref in game code on load transitions (grace teleport / character swap).
    // Root cause was e.g. a double-offset enemy-name id (npc id already +900M
    // tutorial-encoded, then +700M again → 1.6B, in no FMG). Clearing it to -1
    // makes that text line simply absent instead of crashing.
    goblin::sanitize_injected_textids();
}

void goblin::sanitize_injected_textids()
{
    if (g_placename_valid_ids.empty())
    {
        spdlog::warn("[SANITIZE] PlaceName valid-id set empty - skipping (FMG patch ran?)");
        return;
    }
    auto valid = [](int32_t id) {
        // Logic sentinels the DLL/engine special-case (camp/boss markers) - leave alone.
        if (id == 5000 || id == 5100 || id == 5300 || id == 8800) return true;
        return g_placename_valid_ids.count(id) != 0;
    };
    const auto &rows = goblin::injected_row_ptrs();
    int cleared = 0, scanned = 0;
    for (uint8_t *p : rows)
    {
        auto *row = reinterpret_cast<from::paramdef::WORLD_MAP_POINT_PARAM_ST *>(p);
        int32_t *tids[8]  = {&row->textId1, &row->textId2, &row->textId3, &row->textId4,
                             &row->textId5, &row->textId6, &row->textId7, &row->textId8};
        unsigned int *fl[8] = {&row->textDisableFlagId1, &row->textDisableFlagId2,
                               &row->textDisableFlagId3, &row->textDisableFlagId4,
                               &row->textDisableFlagId5, &row->textDisableFlagId6,
                               &row->textDisableFlagId7, &row->textDisableFlagId8};
        ++scanned;
        for (int i = 0; i < 8; ++i)
        {
            int32_t id = *tids[i];
            if (id >= 0 && !valid(id))
            {
                *tids[i] = -1;
                *fl[i] = 0;
                ++cleared;
            }
        }
    }
    spdlog::info("[SANITIZE] Scanned {} injected rows, cleared {} dangling textId(s) "
                 "(missing from PlaceName FMG → would null-deref on load)", scanned, cleared);
}

void goblin::set_fmg_injection_active(bool active)
{
    if (!g_placename_slot_ptr)
    {
        spdlog::warn("[TOGGLE] FMG swap state not initialized - setup_messages didn't run");
        return;
    }
    if (active == g_fmg_injection_active)
        return;
    *g_placename_slot_ptr = active ? g_expanded_placename_fmg : g_vanilla_placename_fmg;
    g_fmg_injection_active = active;
    spdlog::info("[TOGGLE] PlaceName FMG -> {}", active ? "EXPANDED" : "VANILLA");
}

bool goblin::is_fmg_injection_active()
{
    return g_fmg_injection_active;
}

const wchar_t *goblin::lookup_text(int32_t id)
{
    uint8_t *fmg = g_expanded_placename_fmg;
    if (!fmg || id <= 0)
        return nullptr;
    // Same FMG-v2 layout + relative/absolute offset fixup as patch_fmg_in_memory.
    uint32_t group_cnt  = *reinterpret_cast<uint32_t *>(fmg + 0x0C);
    uint32_t string_cnt = *reinterpret_cast<uint32_t *>(fmg + 0x10);
    uint64_t raw        = *reinterpret_cast<uint64_t *>(fmg + 0x18);
    uint64_t off_rel = (raw > 0x1000000)
                           ? static_cast<uint64_t>(reinterpret_cast<uint8_t *>(raw) - fmg)
                           : raw;
    auto *groups  = reinterpret_cast<FmgGroup *>(fmg + 0x28);
    auto *offsets = reinterpret_cast<uint64_t *>(fmg + off_rel);
    for (uint32_t g = 0; g < group_cnt; ++g)
    {
        if (id >= groups[g].first_id && id <= groups[g].last_id)
        {
            int32_t si = groups[g].string_index + (id - groups[g].first_id);
            if (si < 0 || si >= static_cast<int32_t>(string_cnt))
                return nullptr;
            uint64_t so = offsets[si];
            const wchar_t *s = (so > 0x1000000)
                                   ? reinterpret_cast<const wchar_t *>(so)
                                   : reinterpret_cast<const wchar_t *>(fmg + so);
            return (s && *s) ? s : nullptr;
        }
    }
    return nullptr;
}
