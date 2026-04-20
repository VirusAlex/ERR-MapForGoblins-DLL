#include "goblin_messages.hpp"
#include "generated/goblin_map_data.hpp"
#include "modutils.hpp"

#include <cstring>
#include <set>
#include <spdlog/spdlog.h>
#include <string>
#include <thread>
#include <chrono>
#include <unordered_map>
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
//   stringOffsetsOffset: uint64[stringCount] — each is offset from FMG start to UTF-16LE string
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
                                const std::vector<NewEntry> &new_entries)
{
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

    fmg_allocation = VirtualAlloc(nullptr, new_file_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!fmg_allocation)
    {
        spdlog::error("[PATCH] VirtualAlloc failed ({} bytes)", new_file_size);
        return false;
    }
    memset(fmg_allocation, 0, new_file_size);

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

    // Collect item IDs used as textId — need to copy from various FMGs to PlaceName
    // FmgId slots: GoodsName=10, WeaponName=11, ProtectorName=12, AccessoryName=13,
    //              PlaceName=19, GemName=35, ArtsName=42, TutorialTitle=207
    // textId encoding: real_id + category_offset
    //   100000000+id:      WeaponName (cat=2, weapons)
    //   200000000+id:      ProtectorName (cat=3, armour)
    //   300000000+id:      AccessoryName (cat=4, talismans)
    //   400000000+id:      GemName (cat=5, ashes of war)
    //   500000000+id:      GoodsName (cat=1, goods)
    //   600000000+id:      EventTextForMap (map event text, e.g. "Closed with an imp's seal")
    //   900000000+id:      TutorialTitle (enemy names)
    // Legacy (pre-offset): raw PlaceName IDs < 100M for location names
    std::set<int32_t> goods_ids_needed;      // GoodsName FMG, slot 10
    std::set<int32_t> weapon_ids_needed;     // WeaponName FMG, slot 11
    std::set<int32_t> protector_ids_needed;  // ProtectorName FMG, slot 12
    std::set<int32_t> accessory_ids_needed;  // AccessoryName FMG, slot 13
    std::set<int32_t> gem_ids_needed;        // GemName FMG, slot 14
    std::set<int32_t> event_text_ids_needed; // EventTextForMap FMG, slot 34/467
    std::set<int32_t> tutorial_ids_needed;   // TutorialTitle FMG, slot 207 (enemy names from ERR Codex)
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
            if (tid >= 900000000)               // TutorialTitle (enemy names)
                tutorial_ids_needed.insert(tid);
            else if (tid >= 600000000)          // EventTextForMap (offset 600M)
                event_text_ids_needed.insert(tid);
            else if (tid >= 500000000)          // GoodsName (goods, offset 500M)
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
    spdlog::debug("{} Goods + {} Weapon + {} Protector + {} Accessory + {} Gem + {} EventText + {} Tutorial IDs need lookup",
                  goods_ids_needed.size(), weapon_ids_needed.size(),
                  protector_ids_needed.size(), accessory_ids_needed.size(),
                  gem_ids_needed.size(), event_text_ids_needed.size(),
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

    // Helper: copy entries from an FMG to PlaceName, with offset remapping
    // needed_ids contains offset-encoded IDs; real FMG id = offset_id - offset_base
    // The PlaceName entry is written at the offset-encoded ID
    auto copy_fmg_entries = [&](uint8_t *fmg_ptr, const std::set<int32_t> &needed_ids,
                                int32_t offset_base, const char *label) -> int
    {
        if (needed_ids.empty() || !fmg_ptr) return 0;

        uint32_t grp_cnt = *reinterpret_cast<uint32_t *>(fmg_ptr + 0x0C);
        uint32_t str_cnt = *reinterpret_cast<uint32_t *>(fmg_ptr + 0x10);
        uint64_t raw_off = *reinterpret_cast<uint64_t *>(fmg_ptr + 0x18);

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
                    copied++;
                }
            }
        }
        spdlog::info("Copied {} {} entries to PlaceName", copied, label);
        return copied;
    };

    // Read GoodsName FMG (slot 10): goods items, offset 500M
    if (!goods_ids_needed.empty() && 10 < count2 && sub[10])
        copy_fmg_entries(sub[10], goods_ids_needed, 500000000, "GoodsName");

    // Read WeaponName FMG (slot 11): ammo (offset=0, id>=50M) + weapons (offset=100M)
    if (11 < count2 && sub[11])
    {
        // Ammo IDs are stored as-is (>=50M), weapon IDs are offset by 100M
        std::set<int32_t> ammo_ids, weapon_offset_ids;
        for (int32_t tid : weapon_ids_needed)
        {
            if (tid >= 100000000)
                weapon_offset_ids.insert(tid);
            else
                ammo_ids.insert(tid);
        }
        if (!ammo_ids.empty())
            copy_fmg_entries(sub[11], ammo_ids, 0, "WeaponName(ammo)");
        if (!weapon_offset_ids.empty())
            copy_fmg_entries(sub[11], weapon_offset_ids, 100000000, "WeaponName(weapon)");
    }

    // Read ProtectorName FMG (slot 12): armour, offset 200M
    if (!protector_ids_needed.empty() && 12 < count2 && sub[12])
        copy_fmg_entries(sub[12], protector_ids_needed, 200000000, "ProtectorName");

    // Read AccessoryName FMG (slot 13): talismans, offset 300M
    if (!accessory_ids_needed.empty() && 13 < count2 && sub[13])
        copy_fmg_entries(sub[13], accessory_ids_needed, 300000000, "AccessoryName");

    // Read GemName FMG (slot 35 = FmgId::GemName): ashes of war, offset 400M
    if (!gem_ids_needed.empty())
    {
        // GemName = slot 35, ArtsName = slot 42 (alternative), DLC: 322, 422
        for (int try_slot : {35, 42, 322, 422})
        {
            if (try_slot >= count2 || !sub[try_slot]) continue;
            auto copied = copy_fmg_entries(sub[try_slot], gem_ids_needed, 400000000, "GemName");
            if (copied > 0)
            {
                spdlog::info("GemName: copied {} from slot {}", copied, try_slot);
                break;
            }
        }
    }

    // TODO: EventTextForMap FMG (slots 34/367/467) is in a separate MsgRepository bank
    // (menu msgbnd, not item msgbnd). Need to find the menu bank pointer to access it.
    // For now, 600M+ textIds will show as ?PlaceName? until this is implemented.
    if (!event_text_ids_needed.empty())
        spdlog::warn("EventTextForMap: {} IDs requested but menu FMG bank not yet supported",
                      event_text_ids_needed.size());

    // Read TutorialTitle FMG (slot 207) for ERR Codex enemy names
    // textId in MASSEDIT = real TutorialTitle ID + 900000000 (to avoid collision with GoodsName)
    // We look up by real ID but write to PlaceName at the remapped ID
    if (!tutorial_ids_needed.empty() && 207 < count2 && sub[207])
    {
        auto *tut_fmg = sub[207];
        uint32_t tut_group_cnt = *reinterpret_cast<uint32_t *>(tut_fmg + 0x0C);
        uint32_t tut_string_cnt = *reinterpret_cast<uint32_t *>(tut_fmg + 0x10);
        uint64_t tut_raw_str_off = *reinterpret_cast<uint64_t *>(tut_fmg + 0x18);

        uint8_t *tut_offsets_ptr;
        if (tut_raw_str_off > 0x1000000)
            tut_offsets_ptr = reinterpret_cast<uint8_t *>(tut_raw_str_off);
        else
            tut_offsets_ptr = tut_fmg + tut_raw_str_off;

        auto *tut_groups = reinterpret_cast<FmgGroup *>(tut_fmg + 0x28);
        auto *tut_str_offs = reinterpret_cast<uint64_t *>(tut_offsets_ptr);

        // Build set of real TutorialTitle IDs (without offset)
        std::set<int32_t> real_tut_ids;
        for (int32_t remapped : tutorial_ids_needed)
            real_tut_ids.insert(remapped - 900000000);

        int tut_copied = 0;
        for (uint32_t g = 0; g < tut_group_cnt; g++)
        {
            int32_t first = tut_groups[g].first_id;
            int32_t last = tut_groups[g].last_id;
            int32_t si = tut_groups[g].string_index;

            for (int32_t id = first; id <= last; id++, si++)
            {
                if (si < 0 || si >= (int32_t)tut_string_cnt)
                    continue;
                if (real_tut_ids.count(id) == 0)
                    continue;

                uint64_t str_off = tut_str_offs[si];
                if (str_off == 0) continue;

                const wchar_t *text;
                if (str_off > 0x1000000)
                    text = reinterpret_cast<const wchar_t *>(str_off);
                else
                    text = reinterpret_cast<const wchar_t *>(tut_fmg + str_off);

                if (text && text[0])
                {
                    // Write to PlaceName at remapped ID (real + 900000000)
                    new_entries.push_back({id + 900000000, text});
                    tut_copied++;
                }
            }
        }
        spdlog::info("Copied {} TutorialTitle entries to PlaceName (enemy names)", tut_copied);
    }
    else if (!tutorial_ids_needed.empty())
    {
        spdlog::warn("TutorialTitle FMG (slot 207) not available ({} IDs needed)", tutorial_ids_needed.size());
    }

    auto *fmg_ptr = sub[19];

    if (!fmg_ptr)
    {
        spdlog::error("PlaceName FMG (bnd=19) is null");
        return;
    }

    spdlog::debug("PlaceName FMG at {:p}", (void *)fmg_ptr);

    if (patch_fmg_in_memory(fmg_ptr, &sub[19], new_entries))
        spdlog::info("PlaceName FMG patched ({} entries)", new_entries.size());
    else
        spdlog::error("PlaceName FMG patching failed");
}
