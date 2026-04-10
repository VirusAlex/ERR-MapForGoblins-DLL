/*
 * FMG Patcher - merges PlaceName entries into msgbnd.dcx files
 * Standalone tool, no .NET required. Uses zstd for DCX compression.
 *
 * Usage: fmg_patcher <ERR_mod_folder> <PlaceName.fmg>
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ============================================================================
// Oodle (loaded dynamically from oo2core_6_win64.dll)
// ============================================================================

typedef int (*OodleLZ_Decompress_t)(const void *, int, void *, int, int, int, int, void *, int, void *, void *, void *, int, int);
typedef int (*OodleLZ_Compress_t)(int, const void *, int, void *, int, void *, void *, void *, void *, int);
typedef void *(*OodleLZ_CompressOptions_GetDefault_t)(int compressor, int level);

static OodleLZ_Decompress_t oodleDecompress = nullptr;
static OodleLZ_Compress_t oodleCompress = nullptr;
static OodleLZ_CompressOptions_GetDefault_t oodleGetDefaultOpts = nullptr;

struct OodleLZ_CompressOptions
{
    uint32_t verbosity;
    int32_t minMatchLen;
    int32_t seekChunkReset;
    int32_t seekChunkLen;
    int32_t profile;
    int32_t dictionarySize;
    int32_t spaceSpeedTradeoffBytes;
    int32_t maxHuffmansPerChunk;
    int32_t sendQuantumCRCs;
    int32_t maxLocalDictionarySize;
    int32_t makeLongRangeMatcher;
    int32_t matchTableSizeLog2;
};

constexpr int OODLE_KRAKEN = 8;
constexpr int OODLE_LEVEL_OPTIMAL2 = 6;

static bool load_oodle(const fs::path &exe_dir)
{
    auto dll_path = exe_dir / "oo2core_6_win64.dll";
    HMODULE hm = LoadLibraryW(dll_path.wstring().c_str());
    if (!hm)
    {
        hm = LoadLibraryA("oo2core_6_win64.dll");
    }
    if (!hm)
    {
        std::cerr << "ERROR: oo2core_6_win64.dll not found!\n"
                  << "Place it next to fmg_patcher.exe\n";
        return false;
    }
    oodleDecompress = (OodleLZ_Decompress_t)GetProcAddress(hm, "OodleLZ_Decompress");
    oodleCompress = (OodleLZ_Compress_t)GetProcAddress(hm, "OodleLZ_Compress");
    oodleGetDefaultOpts = (OodleLZ_CompressOptions_GetDefault_t)GetProcAddress(hm, "OodleLZ_CompressOptions_GetDefault");
    return oodleDecompress && oodleCompress;
}

// ============================================================================
// Binary helpers
// ============================================================================

static uint32_t r32(const uint8_t *p) { return *(const uint32_t *)p; }
static uint64_t r64(const uint8_t *p) { return *(const uint64_t *)p; }
static int32_t ri32(const uint8_t *p) { return *(const int32_t *)p; }
static void w32(uint8_t *p, uint32_t v) { *(uint32_t *)p = v; }
static void w64(uint8_t *p, uint64_t v) { *(uint64_t *)p = v; }

static std::vector<uint8_t> read_file(const fs::path &path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    size_t sz = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> buf(sz);
    f.read((char *)buf.data(), sz);
    return buf;
}

static bool write_file(const fs::path &path, const std::vector<uint8_t> &data)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write((const char *)data.data(), data.size());
    return true;
}

// ============================================================================
// DCX (zstd variant)
// ============================================================================

static uint32_t rbe32(const uint8_t *p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static void wbe32(uint8_t *p, uint32_t v) { p[0] = (v >> 24); p[1] = (v >> 16) & 0xFF; p[2] = (v >> 8) & 0xFF; p[3] = v & 0xFF; }

static std::vector<uint8_t> dcx_decompress(const std::vector<uint8_t> &data)
{
    if (data.size() < 0x50 || memcmp(data.data(), "DCX\0", 4) != 0)
        return data; // not DCX

    // DCX KRAK header (big-endian):
    // 0x00: "DCX\0"
    // 0x18: "DCS\0"
    // 0x1C: uncompressed_size (BE32)
    // 0x20: compressed_size (BE32)
    // 0x28: "DCP\0"
    // 0x2C: compression_type ("KRAK", "DFLT", etc.)
    // 0x44: "DCA\0"
    // 0x48: compressed_data_offset from here (BE32) -> data starts at 0x48+offset

    uint32_t uncomp_size = rbe32(data.data() + 0x1C);
    uint32_t comp_size = rbe32(data.data() + 0x20);

    constexpr size_t comp_off = 0x4C; // DCA(0x44) + 8

    if (!oodleDecompress)
    {
        std::cerr << "  Oodle not loaded!\n";
        return {};
    }

    std::vector<uint8_t> out(uncomp_size);
    int result = oodleDecompress(data.data() + comp_off, (int)comp_size,
                                 out.data(), (int)uncomp_size,
                                 0, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, 3);
    if (result <= 0)
    {
        std::cerr << "  Oodle decompress failed (uncomp=" << uncomp_size
                  << " comp=" << comp_size << " result=" << result << ")\n";
        return {};
    }
    out.resize(result);
    return out;
}

static std::vector<uint8_t> dcx_compress(const std::vector<uint8_t> &data,
                                          const std::vector<uint8_t> &original_dcx)
{
    if (!oodleCompress)
    {
        std::cerr << "  Oodle not loaded!\n";
        return {};
    }

    // seekChunkReset required for game compatibility
    OodleLZ_CompressOptions opts = {};
    void *pOpts = nullptr;
    if (oodleGetDefaultOpts)
    {
        void *defaults = oodleGetDefaultOpts(OODLE_KRAKEN, OODLE_LEVEL_OPTIMAL2);
        if (defaults)
        {
            memcpy(&opts, defaults, sizeof(opts));
            opts.seekChunkReset = 1;
            opts.seekChunkLen = 0x40000;
            pOpts = &opts;
        }
    }

    size_t bound = data.size() + 274 * ((data.size() + 0x3FFFF) / 0x40000);
    std::vector<uint8_t> comp(bound);
    int comp_size = oodleCompress(OODLE_KRAKEN, data.data(), (int)data.size(),
                                   comp.data(), OODLE_LEVEL_OPTIMAL2,
                                   pOpts, nullptr, nullptr, nullptr, 0);
    if (comp_size <= 0)
    {
        std::cerr << "  Oodle compress failed: " << comp_size << "\n";
        return {};
    }

    constexpr size_t comp_off = 0x4C;

    size_t total_size = (comp_off + comp_size + 0xF) & ~(size_t)0xF; // 16-byte aligned

    std::vector<uint8_t> out(total_size, 0);
    memcpy(out.data(), original_dcx.data(), std::min(comp_off, original_dcx.size()));

    wbe32(out.data() + 0x1C, (uint32_t)data.size());    // uncompressed size (BE)
    wbe32(out.data() + 0x20, (uint32_t)comp_size);       // compressed size (BE)

    memcpy(out.data() + comp_off, comp.data(), comp_size);
    return out;
}

// ============================================================================
// FMG parser
// ============================================================================

struct FmgEntry
{
    int32_t id;
    std::optional<std::u16string> text; // nullopt = null entry, empty string = valid but empty
};

static std::vector<FmgEntry> fmg_parse(const uint8_t *data, size_t size)
{
    std::vector<FmgEntry> entries;
    if (size < 0x28) return entries;

    // ER FMG format:
    // Header (0x28 bytes):
    //   [0x02] version (uint8, 2 = ER)
    //   [0x04] file_size (uint32)
    //   [0x0C] group_count (uint32)
    //   [0x10] string_count (uint32)
    //   [0x18] string_offsets_offset (uint32)
    // Groups at 0x28, each 16 bytes: {first_index(i32), first_id(i32), last_id(i32), pad(i32)}
    // String offsets at string_offsets_offset, each 8 bytes: offset to UTF-16LE string

    int32_t group_count = ri32(data + 0x0C);
    int32_t string_count = ri32(data + 0x10);
    uint32_t str_off_table = r32(data + 0x18);

    for (int32_t g = 0; g < group_count; g++)
    {
        size_t goff = 0x28 + g * 16;
        if (goff + 16 > size) break;

        int32_t first_idx = ri32(data + goff);
        int32_t first_id = ri32(data + goff + 4);
        int32_t last_id = ri32(data + goff + 8);

        for (int32_t id = first_id; id <= last_id; id++)
        {
            int32_t idx = first_idx + (id - first_id);
            if (idx < 0 || idx >= string_count) break;

            size_t soff_pos = str_off_table + idx * 8;
            if (soff_pos + 8 > size) break;

            int64_t str_off = *(const int64_t *)(data + soff_pos);

            std::optional<std::u16string> text;
            if (str_off > 0 && (size_t)str_off < size)
            {
                std::u16string s;
                for (size_t i = (size_t)str_off; i + 1 < size; i += 2)
                {
                    char16_t ch = *(const char16_t *)(data + i);
                    if (ch == 0) break;
                    s += ch;
                }
                text = std::move(s);
            }
            entries.push_back({id, text});
        }
    }
    return entries;
}

// ============================================================================
// BND4 parser (msgbnd)
// ============================================================================

struct BndFile
{
    std::string name;
    size_t data_offset;
    size_t data_size;
};

static std::vector<BndFile> bnd4_parse(const uint8_t *data, size_t size)
{
    std::vector<BndFile> files;
    if (size < 0x40 || memcmp(data, "BND4", 4) != 0)
        return files;

    int32_t file_count = ri32(data + 0x0C);

    // Entries start at 0x40, each 0x24 bytes:
    // +0x08: data_size (int64)
    // +0x18: data_offset (uint32)
    // +0x1C: ID (uint32)
    // +0x20: name_offset (uint32)
    for (int32_t i = 0; i < file_count; i++)
    {
        size_t entry_off = 0x40 + i * 0x24;
        if (entry_off + 0x24 > size) break;

        uint64_t data_size = r64(data + entry_off + 0x08);
        uint32_t data_off = r32(data + entry_off + 0x18);
        uint32_t name_off = r32(data + entry_off + 0x20);

        std::string name;
        if (name_off > 0 && name_off < size)
        {
            for (size_t j = name_off; j + 1 < size; j += 2)
            {
                char16_t ch = *(const char16_t *)(data + j);
                if (ch == 0) break;
                name += (ch < 128) ? (char)ch : '?';
            }
        }
        files.push_back({name, (size_t)data_off, (size_t)data_size});
    }
    return files;
}

// ============================================================================
// FMG merge (simple approach: find FMG in BND, add missing entries)
// ============================================================================

static bool merge_fmg_into_msgbnd(const fs::path &msgbnd_path, const std::vector<FmgEntry> &new_entries)
{
    auto dcx_data = read_file(msgbnd_path);
    if (dcx_data.empty())
    {
        std::cerr << "  Cannot read: " << msgbnd_path << "\n";
        return false;
    }

    auto bnd_data = dcx_decompress(dcx_data);
    if (bnd_data.empty())
        return false;

    auto files = bnd4_parse(bnd_data.data(), bnd_data.size());

    int target_idx = -1;
    for (size_t i = 0; i < files.size(); i++)
    {
        if (files[i].name.find("PlaceName") != std::string::npos)
        {
            target_idx = (int)i;
            break;
        }
    }

    if (target_idx < 0)
    {
        // Fallback: search by content
        for (size_t i = 0; i < files.size(); i++)
        {
            auto existing = fmg_parse(bnd_data.data() + files[i].data_offset, files[i].data_size);
            for (auto &e : existing)
            {
                if (e.id >= 9000000 && e.id < 10000000)
                {
                    target_idx = (int)i;
                    break;
                }
            }
            if (target_idx >= 0)
                break;
        }
    }

    if (target_idx < 0)
    {
        std::cerr << "  PlaceName FMG not found in BND (parsed " << files.size() << " files)\n";
        std::cerr << "  BND header: ";
        for (int i = 0; i < 64 && i < (int)bnd_data.size(); i++)
            fprintf(stderr, "%02X ", bnd_data[i]);
        std::cerr << "\n";
        for (size_t i = 0; i < std::min(files.size(), (size_t)3); i++)
            std::cerr << "  File[" << i << "]: name='" << files[i].name
                      << "' off=" << files[i].data_offset << " sz=" << files[i].data_size << "\n";
        return false;
    }

    auto &target = files[target_idx];
    auto existing = fmg_parse(bnd_data.data() + target.data_offset, target.data_size);

    std::map<int32_t, std::optional<std::u16string>> merged;
    for (auto &e : existing) merged[e.id] = e.text;
    int added = 0;
    for (auto &e : new_entries)
    {
        auto it = merged.find(e.id);
        if (it == merged.end())
        {
            merged[e.id] = e.text;
            added++;
        }
        else if (!it->second.has_value() || it->second->empty())
        {
            if (e.text.has_value() && !e.text->empty())
            {
                it->second = e.text;
                added++;
            }
        }
    }
    if (added == 0) return true;

    std::vector<std::pair<int32_t, std::optional<std::u16string>>> sorted(merged.begin(), merged.end());

    struct Group { int32_t first_idx, first_id, last_id; };
    std::vector<Group> groups;
    int idx = 0;
    for (size_t i = 0; i < sorted.size();)
    {
        int32_t sid = sorted[i].first, eidx = idx;
        int32_t eid = sid;
        idx++; i++;
        while (i < sorted.size() && sorted[i].first == eid + 1) { eid++; idx++; i++; }
        groups.push_back({eidx, sid, eid});
    }

    int total_strings = idx;
    size_t header_sz = 0x28;
    size_t groups_sz = groups.size() * 16;
    size_t str_off_table = header_sz + groups_sz;
    size_t offsets_sz = total_strings * 8;
    size_t strings_start = str_off_table + offsets_sz;

    std::vector<uint8_t> str_data;
    std::vector<int64_t> str_offsets(total_strings);
    int si = 0;
    for (auto &[id, text] : sorted)
    {
        if (!text.has_value())
        {
            str_offsets[si++] = 0;
        }
        else
        {
            str_offsets[si++] = strings_start + str_data.size();
            for (char16_t ch : *text) { str_data.push_back(ch & 0xFF); str_data.push_back((ch >> 8) & 0xFF); }
            str_data.push_back(0); str_data.push_back(0);
        }
    }

    size_t fmg_size = strings_start + str_data.size();
    std::vector<uint8_t> new_fmg(fmg_size, 0);

    memcpy(new_fmg.data(), bnd_data.data() + target.data_offset, std::min((size_t)0x28, target.data_size));
    w32(new_fmg.data() + 0x04, (uint32_t)fmg_size);
    w32(new_fmg.data() + 0x0C, (uint32_t)groups.size());
    w32(new_fmg.data() + 0x10, (uint32_t)total_strings);
    w64(new_fmg.data() + 0x18, (uint64_t)str_off_table);

    for (size_t i = 0; i < groups.size(); i++)
    {
        size_t off = header_sz + i * 16;
        w32(new_fmg.data() + off + 0, groups[i].first_idx);
        w32(new_fmg.data() + off + 4, groups[i].first_id);
        w32(new_fmg.data() + off + 8, groups[i].last_id);
        w32(new_fmg.data() + off + 12, 0);
    }
    for (int i = 0; i < total_strings; i++)
        w64(new_fmg.data() + str_off_table + i * 8, str_offsets[i]);
    memcpy(new_fmg.data() + strings_start, str_data.data(), str_data.size());

    size_t next_file_start = (target_idx + 1 < (int)files.size())
        ? files[target_idx + 1].data_offset
        : bnd_data.size();

    std::vector<uint8_t> new_bnd;
    new_bnd.insert(new_bnd.end(), bnd_data.begin(), bnd_data.begin() + target.data_offset);
    new_bnd.insert(new_bnd.end(), new_fmg.begin(), new_fmg.end());

    size_t cur_pos = new_bnd.size();
    size_t aligned_pos = (cur_pos + 0xF) & ~(size_t)0xF;
    new_bnd.insert(new_bnd.end(), aligned_pos - cur_pos, 0);

    new_bnd.insert(new_bnd.end(), bnd_data.begin() + next_file_start, bnd_data.end());

    int64_t size_diff = (int64_t)aligned_pos - (int64_t)next_file_start;

    // BND entry: +0x08=compressedSize, +0x10=uncompressedSize
    constexpr size_t entries_start = 0x40;
    size_t entry_off = entries_start + target_idx * 0x24;
    w64(new_bnd.data() + entry_off + 0x08, new_fmg.size());
    w64(new_bnd.data() + entry_off + 0x10, new_fmg.size());

    for (size_t i = target_idx + 1; i < files.size(); i++)
    {
        size_t e_off = entries_start + i * 0x24;
        uint32_t old_data_off = r32(new_bnd.data() + e_off + 0x18);
        w32(new_bnd.data() + e_off + 0x18, (uint32_t)((int64_t)old_data_off + size_diff));
    }

    auto new_dcx = dcx_compress(new_bnd, dcx_data);
    if (new_dcx.empty())
        return false;

    return write_file(msgbnd_path, new_dcx);
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char *argv[])
{
    std::cout << "FMG Patcher v1.0 - Map For Goblins\n\n";

    if (!load_oodle(fs::path(argv[0]).parent_path()))
    {
        /* pause */
        return 1;
    }

    if (argc < 2)
    {
        std::cout << "Usage: fmg_patcher <ERR_mod_folder> [PlaceName.fmg]\n\n"
                  << "Drag and drop your ERR \"mod\" folder onto this exe.\n"
                  << "PlaceName_dlc01.fmg should be next to the exe.\n";
        /* pause */
        return 0;
    }

    fs::path mod_folder = argv[1];
    fs::path fmg_path;

    if (argc >= 3)
        fmg_path = argv[2];
    else
        fmg_path = fs::path(argv[0]).parent_path() / "PlaceName_dlc01.fmg";

    if (!fs::exists(fmg_path))
    {
        std::cerr << "ERROR: " << fmg_path << " not found!\n";
        /* pause */
        return 1;
    }

    auto msg_dir = mod_folder / "msg";
    if (!fs::exists(msg_dir))
    {
        std::cerr << "ERROR: " << msg_dir << " not found!\n"
                  << "Make sure you dragged the ERR \"mod\" folder.\n";
        /* pause */
        return 1;
    }

    auto fmg_data = read_file(fmg_path);
    auto entries = fmg_parse(fmg_data.data(), fmg_data.size());
    std::cout << "Loaded " << entries.size() << " text entries from " << fmg_path.filename() << "\n\n";

    if (entries.empty())
    {
        std::cerr << "ERROR: No entries in FMG file!\n";
        /* pause */
        return 1;
    }

    const char *langs[] = {
        "engus", "deude", "frafr", "itait", "jpnjp", "korkr", "polpl",
        "porbr", "rusru", "spaar", "spaes", "thath", "zhocn", "zhotw"};

    int ok = 0, fail = 0, skip = 0;
    for (auto lang : langs)
    {
        auto target = msg_dir / lang / "item_dlc02.msgbnd.dcx";
        if (!fs::exists(target))
        {
            std::cout << "  [SKIP] " << lang << "\n";
            skip++;
            continue;
        }

        if (merge_fmg_into_msgbnd(target, entries))
        {
            std::cout << "  [OK]   " << lang << "\n";
            ok++;
        }
        else
        {
            std::cout << "  [FAIL] " << lang << "\n";
            fail++;
        }
    }

    std::cout << "\nDone! " << ok << " patched, " << fail << " failed, " << skip << " skipped.\n";
    DWORD mode;
    if (GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode))
        /* pause */
    return fail > 0 ? 1 : 0;
}
