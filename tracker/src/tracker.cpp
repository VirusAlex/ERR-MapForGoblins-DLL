#include "tracker.hpp"
#include "modutils.hpp"

#undef min
#undef max
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <mutex>
#include <spdlog/spdlog.h>
#include <thread>
#include <vector>

// ─── constants ─────────────────────────────────────────────────────────────

static constexpr uint16_t GEOM_IDX_MIN   = 0x1194;
static constexpr uint16_t GEOM_IDX_MAX   = 0x11A6;
static constexpr uint32_t PICKUP_FLAG_ID = 1045632900;

static constexpr uintptr_t RVA_WORLD_CHR_MAN = 0x3D65F88;
static constexpr uintptr_t RVA_GEOM_FLAG     = 0x3D69D18;

// ─── globals ───────────────────────────────────────────────────────────────

static uint64_t *g_event_flag_man = nullptr;
static tracker::SetEventFlagFn *g_set_event_flag_original = nullptr;
static tracker::IsEventFlagFn *g_is_event_flag = nullptr;
static uintptr_t g_game_base = 0;
static std::filesystem::path g_mod_folder;
static std::mutex g_data_mutex;

// ─── piece database (from pieces.bin) ──────────────────────────────────────

struct WorldPiece {
    float x, y, z;
    uint32_t tile_id;
};

static std::vector<WorldPiece> g_world_pieces;
static std::vector<bool>       g_piece_collected;
static int                     g_collected_count = 0;
static int                     g_unmatched_geof  = 0;

// ─── GEOF entry from memory / save ────────────────────────────────────────

struct GEOFEntry {
    uint32_t tile_id;
    uint8_t  type;
    uint8_t  flags;
    uint16_t geom_idx;
    uint32_t instance_hash;
};

static std::vector<GEOFEntry> g_geof_entries;

struct Vec3 { float x, y, z; };

static std::atomic<float> g_cur_x{0}, g_cur_y{0}, g_cur_z{0};
static std::atomic<bool>  g_pos_valid{false};

static bool safe_read_ptr(void *addr, void **out)
{
    __try { *out = *(void **)addr; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool safe_read_bytes(void *addr, void *out, size_t count)
{
    __try { memcpy(out, addr, count); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool safe_read_float(void *addr, float *out)
{
    __try { *out = *(float *)addr; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void *get_singleton(uintptr_t rva)
{
    void *val = nullptr;
    if (!safe_read_ptr((void *)(g_game_base + rva), &val))
        return nullptr;
    return val;
}

static void decode_tile(uint32_t v, char *buf, int bufsz)
{
    uint8_t aa = (v >> 24) & 0xFF;
    uint8_t bb = (v >> 16) & 0xFF;
    uint8_t cc = (v >> 8) & 0xFF;
    uint8_t dd = v & 0xFF;
    snprintf(buf, bufsz, "m%02d_%02d_%02d_%02d", aa, bb, cc, dd);
}

static bool load_pieces_database()
{
    auto path = g_mod_folder / "pieces.bin";

    std::ifstream fs(path, std::ios::binary);
    if (!fs.is_open()) {
        spdlog::error("[DB] Cannot open {}", path.string());
        return false;
    }

    char magic[4];
    uint32_t count;
    fs.read(magic, 4);
    fs.read(reinterpret_cast<char *>(&count), 4);

    if (memcmp(magic, "RPDB", 4) != 0) {
        spdlog::error("[DB] Invalid magic in pieces.bin");
        return false;
    }

    g_world_pieces.resize(count);
    g_piece_collected.resize(count, false);

    for (uint32_t i = 0; i < count; i++) {
        float x, y, z;
        uint32_t tile_id;
        fs.read(reinterpret_cast<char *>(&x), 4);
        fs.read(reinterpret_cast<char *>(&y), 4);
        fs.read(reinterpret_cast<char *>(&z), 4);
        fs.read(reinterpret_cast<char *>(&tile_id), 4);
        g_world_pieces[i] = {x, y, z, tile_id};
    }

    std::map<uint32_t, int> tile_counts;
    for (auto &p : g_world_pieces) tile_counts[p.tile_id]++;

    int single_tiles = 0, multi_tiles = 0;
    for (auto &[t, c] : tile_counts) {
        if (c == 1) single_tiles++;
        else multi_tiles++;
    }

    spdlog::info("[DB] Loaded {} pieces from {} tiles ({} single-piece, {} multi-piece)",
                 count, tile_counts.size(), single_tiles, multi_tiles);
    return true;
}

struct PosChain {
    uintptr_t off[4];
    int       depth;
    int       float_off;
};

static const PosChain g_chains[] = {
    {{0x10EF8, 0x190, 0x68}, 3, 0x70},
    {{0x10EF8, 0x190, 0x68}, 3, 0x80},
    {{0x10EF8, 0x190, 0x68}, 3, 0x90},
    {{0x10EF8, 0x190, 0x68}, 3, 0x10},
    {{0x10EF8, 0x68},        2, 0x70},
    {{0x10EF8, 0x68},        2, 0x80},
    {{0x10EF8, 0x190, 0x28}, 3, 0x10},
    {{0x10EF8, 0x190, 0x28}, 3, 0x70},
    {{0x18, 0x190, 0x68},    3, 0x70},
    {{0x18, 0x190, 0x68},    3, 0x80},
    {{0x1A828, 0x190, 0x68}, 3, 0x70},
    {{0x10EF0, 0x190, 0x68}, 3, 0x70},
};
static constexpr int NUM_CHAINS = sizeof(g_chains) / sizeof(g_chains[0]);

static bool try_chain(void *wcm, const PosChain &chain, Vec3 &out)
{
    void *p = wcm;
    for (int i = 0; i < chain.depth; i++) {
        void *next = nullptr;
        if (!safe_read_ptr((char *)p + chain.off[i], &next) || !next) return false;
        p = next;
    }

    Vec3 v;
    if (!safe_read_float((char *)p + chain.float_off,     &v.x)) return false;
    if (!safe_read_float((char *)p + chain.float_off + 4, &v.y)) return false;
    if (!safe_read_float((char *)p + chain.float_off + 8, &v.z)) return false;

    if (std::isnan(v.x) || std::isnan(v.y) || std::isnan(v.z)) return false;
    if (std::isinf(v.x) || std::isinf(v.y) || std::isinf(v.z)) return false;
    if (fabsf(v.x) > 10000.0f || fabsf(v.y) > 10000.0f || fabsf(v.z) > 10000.0f) return false;

    float mag2 = v.x * v.x + v.y * v.y + v.z * v.z;
    if (mag2 < 1.0f) return false;

    out = v;
    return true;
}

static bool try_read_position(Vec3 &pos)
{
    void *wcm = get_singleton(RVA_WORLD_CHR_MAN);
    if (!wcm) return false;

    for (int i = 0; i < NUM_CHAINS; i++) {
        if (try_chain(wcm, g_chains[i], pos))
            return true;
    }
    return false;
}

static void log_all_chains()
{
    void *wcm = get_singleton(RVA_WORLD_CHR_MAN);
    if (!wcm) {
        spdlog::warn("[POS] WorldChrMan not available");
        return;
    }
    spdlog::info("[POS] WorldChrMan @ {:016X} — scanning {} chains:", (uintptr_t)wcm, NUM_CHAINS);

    for (int i = 0; i < NUM_CHAINS; i++) {
        auto &ch = g_chains[i];
        Vec3 v;
        bool ok = try_chain(wcm, ch, v);

        char chain_str[128];
        int pos_written = 0;
        for (int j = 0; j < ch.depth; j++)
            pos_written += snprintf(chain_str + pos_written, sizeof(chain_str) - pos_written,
                                    "+0x%X -> ", (unsigned)ch.off[j]);
        snprintf(chain_str + pos_written, sizeof(chain_str) - pos_written, "+0x%X", ch.float_off);

        if (ok)
            spdlog::info("[POS]   chain[{:2d}] {} => ({:.1f}, {:.1f}, {:.1f}) OK",
                         i, chain_str, v.x, v.y, v.z);
        else
            spdlog::info("[POS]   chain[{:2d}] {} => FAIL", i, chain_str);
    }
}

static bool safe_read_double(void *addr, double *out)
{
    __try { *out = *(double *)addr; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void dump_position_memory()
{
    void *wcm = get_singleton(RVA_WORLD_CHR_MAN);
    if (!wcm) {
        spdlog::warn("[SCAN] WorldChrMan not available");
        return;
    }

    spdlog::info("[SCAN] ========== DEEP POSITION SCAN v2 ==========");
    spdlog::info("[SCAN] WorldChrMan @ {:016X}", (uintptr_t)wcm);

    void *p1 = nullptr, *p2 = nullptr, *p3 = nullptr;
    if (!safe_read_ptr((char *)wcm + 0x10EF8, &p1) || !p1) {
        spdlog::warn("[SCAN] Cannot read PlayerIns");
        return;
    }
    spdlog::info("[SCAN] PlayerIns (p1) = {:016X}", (uintptr_t)p1);

    if (!safe_read_ptr((char *)p1 + 0x190, &p2) || !p2) {
        spdlog::warn("[SCAN] Cannot read ChrPhysicsModule");
        return;
    }
    spdlog::info("[SCAN] ChrPhysicsModule (p2) = {:016X}", (uintptr_t)p2);

    if (!safe_read_ptr((char *)p2 + 0x68, &p3) || !p3) {
        spdlog::warn("[SCAN] Cannot read ChrCoordinate");
        return;
    }
    spdlog::info("[SCAN] ChrCoordinate (p3) = {:016X}", (uintptr_t)p3);

    spdlog::info("[SCAN] --- ChrCoordinate (256 bytes) ---");
    uint8_t raw[256] = {};
    safe_read_bytes(p3, raw, 256);

    for (int row = 0; row < 256; row += 16) {
        char hex[80], ascii[20];
        int h = 0;
        for (int i = 0; i < 16; i++)
            h += snprintf(hex + h, sizeof(hex) - h, "%02X ", raw[row + i]);

        float f0, f1, f2, f3;
        memcpy(&f0, raw + row,      4);
        memcpy(&f1, raw + row + 4,  4);
        memcpy(&f2, raw + row + 8,  4);
        memcpy(&f3, raw + row + 12, 4);

        spdlog::info("[SCAN] +{:02X}: {}  f32: [{:12.3f} {:12.3f} {:12.3f} {:12.3f}]",
                     row, hex, f0, f1, f2, f3);
    }

    spdlog::info("[SCAN] --- ChrCoordinate as double ---");
    for (int off = 0; off < 128; off += 8) {
        double dval = 0;
        if (safe_read_double((char *)p3 + off, &dval)) {
            if (fabs(dval) > 0.01 && fabs(dval) < 100000.0 && !std::isnan(dval) && !std::isinf(dval))
                spdlog::info("[SCAN]   d[+0x{:02X}] = {:.6f} ***", off, dval);
        }
    }

    spdlog::info("[SCAN] --- ChrPhysicsModule (256 bytes) ---");
    uint8_t raw2[256] = {};
    safe_read_bytes(p2, raw2, 256);

    for (int row = 0; row < 256; row += 16) {
        char hex[80];
        int h = 0;
        for (int i = 0; i < 16; i++)
            h += snprintf(hex + h, sizeof(hex) - h, "%02X ", raw2[row + i]);

        float f0, f1, f2, f3;
        memcpy(&f0, raw2 + row,      4);
        memcpy(&f1, raw2 + row + 4,  4);
        memcpy(&f2, raw2 + row + 8,  4);
        memcpy(&f3, raw2 + row + 12, 4);

        spdlog::info("[SCAN] +{:02X}: {}  f32: [{:12.3f} {:12.3f} {:12.3f} {:12.3f}]",
                     row, hex, f0, f1, f2, f3);
    }

    // NPC at WCM+0x10EC8 for comparison
    void *npc = nullptr;
    if (safe_read_ptr((char *)wcm + 0x10EC8, &npc) && npc) {
        void *npc2 = nullptr, *npc3 = nullptr;
        if (safe_read_ptr((char *)npc + 0x190, &npc2) && npc2) {
            if (safe_read_ptr((char *)npc2 + 0x68, &npc3) && npc3) {
                spdlog::info("[SCAN] --- NPC ChrCoordinate (WCM+0x10EC8) ---");
                uint8_t nraw[128] = {};
                safe_read_bytes(npc3, nraw, 128);
                for (int row = 0; row < 128; row += 16) {
                    char hex[80];
                    int h = 0;
                    for (int i = 0; i < 16; i++)
                        h += snprintf(hex + h, sizeof(hex) - h, "%02X ", nraw[row + i]);

                    float f0, f1, f2, f3;
                    memcpy(&f0, nraw + row,      4);
                    memcpy(&f1, nraw + row + 4,  4);
                    memcpy(&f2, nraw + row + 8,  4);
                    memcpy(&f3, nraw + row + 12, 4);

                    spdlog::info("[SCAN] +{:02X}: {}  f32: [{:12.3f} {:12.3f} {:12.3f} {:12.3f}]",
                                 row, hex, f0, f1, f2, f3);
                }
            }
        }
    }

    spdlog::info("[SCAN] --- First-level offsets from WCM ---");
    int found = 0;
    for (uintptr_t off1 = 0; off1 <= 0x20000 && found < 20; off1 += 8) {
        void *candidate = nullptr;
        if (!safe_read_ptr((char *)wcm + off1, &candidate) || !candidate) continue;
        if ((uintptr_t)candidate < 0x10000 || (uintptr_t)candidate > 0x7FFFFFFFFFFF) continue;

        void *a = nullptr, *b = nullptr;
        if (!safe_read_ptr((char *)candidate + 0x190, &a) || !a) continue;
        if ((uintptr_t)a < 0x10000 || (uintptr_t)a > 0x7FFFFFFFFFFF) continue;
        if (!safe_read_ptr((char *)a + 0x68, &b) || !b) continue;
        if ((uintptr_t)b < 0x10000 || (uintptr_t)b > 0x7FFFFFFFFFFF) continue;

        float fx, fy, fz;
        if (!safe_read_float((char *)b + 0x70, &fx)) continue;
        if (!safe_read_float((char *)b + 0x74, &fy)) continue;
        if (!safe_read_float((char *)b + 0x78, &fz)) continue;

        if (std::isnan(fx) || std::isnan(fy) || std::isnan(fz)) continue;
        if (std::isinf(fx) || std::isinf(fy) || std::isinf(fz)) continue;
        if (fabsf(fx) > 10000.0f || fabsf(fy) > 10000.0f || fabsf(fz) > 10000.0f) continue;
        float mag2 = fx * fx + fy * fy + fz * fz;
        if (mag2 < 1.0f) continue;

        spdlog::info("[SCAN] WCM+0x{:05X} => ({:.1f}, {:.1f}, {:.1f})",
                     (unsigned)off1, fx, fy, fz);
        found++;
    }

    spdlog::info("[SCAN] --- Searching for float ~90.4 in +/-128KB around PlayerIns ---");
    found = 0;

    uintptr_t scan_center = (uintptr_t)p1;
    uintptr_t scan_start = scan_center > 0x20000 ? scan_center - 0x20000 : 0;
    uintptr_t scan_end   = scan_center + 0x20000;

    for (uintptr_t addr = scan_start; addr < scan_end && found < 10; addr += 4) {
        float val = 0;
        if (!safe_read_float((void *)addr, &val)) continue;
        // Look for value near 90.423 (within 1.0)
        if (fabsf(val - 90.423f) < 1.0f) {
                float vy = 0, vz = 0;
            safe_read_float((void *)(addr + 4), &vy);
            safe_read_float((void *)(addr + 8), &vz);
            bool triplet = (fabsf(vy - 244.471f) < 5.0f && fabsf(vz - (-29.212f)) < 5.0f);

            spdlog::info("[SCAN] FOUND X≈90.4 at {:016X} (p1{:+d}): ({:.3f}, {:.3f}, {:.3f}) {}",
                         addr, (int64_t)(addr - scan_center), val, vy, vz,
                         triplet ? "<<< POSITION MATCH! >>>" : "");
            found++;
        }
    }
    if (found == 0)
        spdlog::warn("[SCAN] Float ~90.4 NOT found in +/-128KB around PlayerIns");

    spdlog::info("[SCAN] --- Searching for float ~90.4 in +/-128KB around WCM ---");
    found = 0;
    scan_center = (uintptr_t)wcm;
    scan_start = scan_center > 0x20000 ? scan_center - 0x20000 : 0;
    scan_end   = scan_center + 0x20000;

    for (uintptr_t addr = scan_start; addr < scan_end && found < 10; addr += 4) {
        float val = 0;
        if (!safe_read_float((void *)addr, &val)) continue;
        if (fabsf(val - 90.423f) < 1.0f) {
            float vy = 0, vz = 0;
            safe_read_float((void *)(addr + 4), &vy);
            safe_read_float((void *)(addr + 8), &vz);
            bool triplet = (fabsf(vy - 244.471f) < 5.0f && fabsf(vz - (-29.212f)) < 5.0f);

            spdlog::info("[SCAN] FOUND X≈90.4 at {:016X} (WCM{:+d}): ({:.3f}, {:.3f}, {:.3f}) {}",
                         addr, (int64_t)(addr - (uintptr_t)wcm), val, vy, vz,
                         triplet ? "<<< POSITION MATCH! >>>" : "");
            found++;
        }
    }
    if (found == 0)
        spdlog::warn("[SCAN] Float ~90.4 NOT found in +/-128KB around WCM");

    spdlog::info("[SCAN] --- Checking other singletons ---");
    for (uintptr_t rva : {(uintptr_t)0x3D5DEF8, (uintptr_t)0x3D5DF08, (uintptr_t)0x3D66FA8,
                          (uintptr_t)0x3D55E10, (uintptr_t)0x3D685A8}) {
        void *singleton = get_singleton(rva);
        if (singleton) {
            spdlog::info("[SCAN] Singleton at base+0x{:X} = {:016X}", rva, (uintptr_t)singleton);
            for (int off = 0; off < 512; off += 4) {
                float val = 0;
                if (!safe_read_float((char *)singleton + off, &val)) continue;
                if (fabsf(val - 90.423f) < 1.0f) {
                    float vy = 0, vz = 0;
                    safe_read_float((char *)singleton + off + 4, &vy);
                    safe_read_float((char *)singleton + off + 8, &vz);
                    spdlog::info("[SCAN]   +0x{:03X}: ({:.3f}, {:.3f}, {:.3f})", off, val, vy, vz);
                }
            }
        }
    }

    spdlog::info("[SCAN] ========== END SCAN ==========");
}

static std::vector<GEOFEntry> read_geof_from_memory()
{
    std::vector<GEOFEntry> result;

    void *gf = get_singleton(RVA_GEOM_FLAG);
    if (!gf) {
        spdlog::warn("[GEOF] GeomFlagSaveDataManager not available");
        return result;
    }

    int tiles_scanned = 0, total_entries = 0;

    for (int off = 0x08; off < 0x4000; off += 16)
    {
        uint64_t id_val = 0, ptr_val = 0;
        if (!safe_read_bytes((char *)gf + off, &id_val, 8)) break;
        if (!safe_read_bytes((char *)gf + off + 8, &ptr_val, 8)) break;

        if (id_val == 0 && ptr_val == 0) break;

        uint32_t tile_id = (uint32_t)id_val;
        uint8_t area = (tile_id >> 24) & 0xFF;
        if (area < 0x0A || area > 0x3C) continue;
        if (ptr_val < 0x10000 || ptr_val > 0x7FFFFFFFFFFF) continue;

        tiles_scanned++;

        uint8_t header[16] = {};
        if (!safe_read_bytes((void *)ptr_val, header, 16)) continue;

        uint32_t count = 0;
        memcpy(&count, header + 8, 4);
        if (count == 0 || count > 100000) continue;

        total_entries += count;

        for (uint32_t ei = 0; ei < count; ei++)
        {
            uint8_t entry[8] = {};
            if (!safe_read_bytes((void *)(ptr_val + 16 + ei * 8), entry, 8)) break;

            uint16_t geom_idx = entry[2] | (entry[3] << 8);
            if (geom_idx >= GEOM_IDX_MIN && geom_idx <= GEOM_IDX_MAX && entry[1] == 0x00)
            {
                GEOFEntry ge;
                ge.tile_id = tile_id;
                ge.type = entry[0];
                ge.flags = entry[1];
                ge.geom_idx = geom_idx;
                memcpy(&ge.instance_hash, entry + 4, 4);
                result.push_back(ge);
            }
        }
    }

    spdlog::info("[GEOF] Memory: {} tiles scanned, {} total entries, {} AEG099_821 pieces",
                 tiles_scanned, total_entries, result.size());
    return result;
}

static std::filesystem::path find_save_file()
{
    char *appdata = nullptr;
    size_t len = 0;
    _dupenv_s(&appdata, &len, "APPDATA");
    if (!appdata) return {};

    std::filesystem::path er_dir = std::filesystem::path(appdata) / "EldenRing";
    free(appdata);
    if (!std::filesystem::exists(er_dir)) return {};

    for (auto &entry : std::filesystem::directory_iterator(er_dir)) {
        if (!entry.is_directory()) continue;
        for (auto ext : {"ER0000.sl2", "ER0000.err"}) {
            auto candidate = entry.path() / ext;
            if (std::filesystem::exists(candidate))
                return candidate;
        }
    }
    return {};
}

struct GEOFSection {
    size_t offset;
    int piece_count;
    std::vector<GEOFEntry> pieces;
};

static std::vector<GEOFSection> parse_all_geof_sections(const std::vector<uint8_t> &data)
{
    std::vector<GEOFSection> sections;
    const uint8_t magic[] = {'F', 'O', 'E', 'G'};

    for (size_t pos = 4; pos + 4 < data.size(); pos++)
    {
        if (memcmp(data.data() + pos, magic, 4) != 0) continue;

        int32_t total_size = 0;
        memcpy(&total_size, data.data() + pos - 4, 4);
        if (total_size <= 12 || total_size > 0x100000) continue;

        size_t section_start = pos - 4;
        size_t chunk_pos = section_start + 12;
        size_t section_end = section_start + total_size;

        GEOFSection sec;
        sec.offset = section_start;
        sec.piece_count = 0;

        while (chunk_pos + 16 <= section_end && chunk_pos + 16 <= data.size())
        {
            if (data[chunk_pos] == 0xFF && data[chunk_pos+1] == 0xFF &&
                data[chunk_pos+2] == 0xFF && data[chunk_pos+3] == 0xFF)
                break;

            int32_t entry_size = 0;
            memcpy(&entry_size, data.data() + chunk_pos + 4, 4);
            if (entry_size <= 0 || entry_size > 0x100000) break;

            uint32_t tile_id = 0, count = 0;
            memcpy(&tile_id, data.data() + chunk_pos, 4);
            memcpy(&count, data.data() + chunk_pos + 8, 4);

            for (uint32_t ei = 0; ei < count; ei++)
            {
                size_t eoff = chunk_pos + 16 + ei * 8;
                if (eoff + 8 > data.size()) break;

                uint16_t geom_idx = data[eoff + 2] | (data[eoff + 3] << 8);
                if (geom_idx >= GEOM_IDX_MIN && geom_idx <= GEOM_IDX_MAX &&
                    data[eoff + 1] == 0x00)
                {
                    GEOFEntry ge;
                    ge.tile_id = tile_id;
                    ge.type = data[eoff];
                    ge.flags = data[eoff + 1];
                    ge.geom_idx = geom_idx;
                    memcpy(&ge.instance_hash, data.data() + eoff + 4, 4);
                    sec.pieces.push_back(ge);
                    sec.piece_count++;
                }
            }
            chunk_pos += entry_size;
        }
        sections.push_back(std::move(sec));
    }
    return sections;
}

static std::vector<GEOFEntry> read_geof_from_save(const std::filesystem::path &save_path,
                                                    int mem_count_hint)
{
    if (save_path.empty() || !std::filesystem::exists(save_path)) return {};

    std::ifstream fs(save_path, std::ios::binary);
    std::vector<uint8_t> data(std::istreambuf_iterator<char>(fs), {});
    fs.close();

    spdlog::info("[SAVE] Read {} ({} bytes)", save_path.string(), data.size());

    auto sections = parse_all_geof_sections(data);
    for (size_t i = 0; i < sections.size(); i++)
        spdlog::info("[SAVE] GEOF section [{}] at 0x{:08X}: {} pieces",
                     i, sections[i].offset, sections[i].piece_count);

    if (sections.empty()) return {};

    if (mem_count_hint > 0) {
        int best_idx = 0, best_diff = INT_MAX;
        for (size_t i = 0; i < sections.size(); i++) {
            int diff = abs(sections[i].piece_count - mem_count_hint);
            if (diff < best_diff) { best_diff = diff; best_idx = (int)i; }
        }
        spdlog::info("[SAVE] Selected section [{}] ({} entries, closest to hint {})",
                     best_idx, sections[best_idx].piece_count, mem_count_hint);
        return std::move(sections[best_idx].pieces);
    }

    int best_idx = 0;
    for (size_t i = 1; i < sections.size(); i++)
        if (sections[i].piece_count > sections[best_idx].piece_count)
            best_idx = (int)i;

    spdlog::info("[SAVE] Selected section [{}] ({} entries, largest)",
                 best_idx, sections[best_idx].piece_count);
    return std::move(sections[best_idx].pieces);
}

static void match_geof_to_pieces()
{
    std::fill(g_piece_collected.begin(), g_piece_collected.end(), false);
    g_collected_count = 0;
    g_unmatched_geof = 0;

    if (g_world_pieces.empty() || g_geof_entries.empty()) return;

    std::map<uint32_t, std::vector<int>> tile_to_pieces;
    for (int i = 0; i < (int)g_world_pieces.size(); i++)
        tile_to_pieces[g_world_pieces[i].tile_id].push_back(i);

    std::map<uint32_t, int> geof_per_tile;
    for (auto &e : g_geof_entries)
        geof_per_tile[e.tile_id]++;

    int matched_exact = 0, matched_all = 0;

    for (auto &[tile, geof_count] : geof_per_tile)
    {
        auto it = tile_to_pieces.find(tile);
        if (it == tile_to_pieces.end()) {
            g_unmatched_geof += geof_count;
            continue;
        }

        auto &indices = it->second;
        int n_pieces = (int)indices.size();

        if (n_pieces == 1 && geof_count >= 1) {
            g_piece_collected[indices[0]] = true;
            matched_exact++;
            if (geof_count > 1)
                g_unmatched_geof += geof_count - 1;
        }
        else if (geof_count >= n_pieces) {
            for (int idx : indices)
                g_piece_collected[idx] = true;
            matched_all += n_pieces;
            g_unmatched_geof += geof_count - n_pieces;
        }
        else {
            // N < M: can't determine which are collected
            g_unmatched_geof += geof_count;
        }
    }

    g_collected_count = 0;
    for (bool c : g_piece_collected)
        if (c) g_collected_count++;

    spdlog::info("[MATCH] {} pieces marked collected ({} exact, {} all-on-tile), "
                 "{} GEOF unmatched, {} total GEOF entries",
                 g_collected_count, matched_exact, matched_all,
                 g_unmatched_geof, g_geof_entries.size());
}

static int find_nearest_piece(float px, float py, float pz, float max_dist = 20.0f)
{
    int best = -1;
    float best_d2 = max_dist * max_dist;
    for (int i = 0; i < (int)g_world_pieces.size(); i++) {
        if (g_piece_collected[i]) continue;
        float dx = g_world_pieces[i].x - px;
        float dy = g_world_pieces[i].y - py;
        float dz = g_world_pieces[i].z - pz;
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < best_d2) { best_d2 = d2; best = i; }
    }
    return best;
}

static void write_status_json()
{
    auto path = g_mod_folder / "collected_pieces.json";
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) return;

    int total = (int)g_world_pieces.size();

    out << "{\n";
    out << "  \"total_pieces\": " << total << ",\n";
    out << "  \"collected_count\": " << g_collected_count << ",\n";
    out << "  \"remaining\": " << (total - g_collected_count) << ",\n";
    out << "  \"unmatched_geof\": " << g_unmatched_geof << ",\n";
    out << "  \"geof_entries\": " << g_geof_entries.size() << ",\n";
    out << "  \"collected\": [\n";

    bool first = true;
    for (int i = 0; i < (int)g_world_pieces.size(); i++) {
        if (!g_piece_collected[i]) continue;
        auto &p = g_world_pieces[i];
        char tile[32];
        decode_tile(p.tile_id, tile, sizeof(tile));

        if (!first) out << ",\n";
        first = false;
        out << "    {\"index\": " << i
            << ", \"tile\": \"" << tile << "\""
            << ", \"x\": " << p.x
            << ", \"y\": " << p.y
            << ", \"z\": " << p.z
            << "}";
    }
    out << "\n  ]\n}\n";
    out.close();

    spdlog::info("[JSON] Written: {}/{} collected, {} unmatched",
                 g_collected_count, total, g_unmatched_geof);
}

static int g_mem_count_hint = 0;

static void refresh_collected(const char *trigger)
{
    std::lock_guard<std::mutex> lock(g_data_mutex);
    spdlog::info("[REFRESH] Triggered by: {}", trigger);

    auto from_mem = read_geof_from_memory();
    if (!from_mem.empty()) {
        g_mem_count_hint = (int)from_mem.size();
        g_geof_entries = std::move(from_mem);
    } else {
        auto save_path = find_save_file();
        if (!save_path.empty()) {
            g_geof_entries = read_geof_from_save(save_path, g_mem_count_hint);
        } else {
            spdlog::warn("[REFRESH] No data source available");
            return;
        }
    }

    match_geof_to_pieces();
    write_status_json();

    spdlog::info("[STATUS] {} / {} collected | Remaining: {}",
                 g_collected_count, (int)g_world_pieces.size(),
                 (int)g_world_pieces.size() - g_collected_count);
}

static void on_piece_picked_up()
{
    float px = g_cur_x.load(), py = g_cur_y.load(), pz = g_cur_z.load();

    std::lock_guard<std::mutex> lock(g_data_mutex);

    if (!g_pos_valid.load()) {
        spdlog::warn("[PICKUP] Position unknown, incrementing count without match");
        g_collected_count++;
        write_status_json();
        return;
    }

    int idx = find_nearest_piece(px, py, pz);
    if (idx >= 0) {
        auto &p = g_world_pieces[idx];
        char tile[32];
        decode_tile(p.tile_id, tile, sizeof(tile));
        float dist = sqrtf((p.x - px) * (p.x - px) +
                           (p.y - py) * (p.y - py) +
                           (p.z - pz) * (p.z - pz));

        spdlog::info("[PICKUP] Matched piece #{} on {} ({:.1f}, {:.1f}, {:.1f}) dist={:.1f}m",
                     idx, tile, p.x, p.y, p.z, dist);
        g_piece_collected[idx] = true;
        g_collected_count++;
    } else {
        spdlog::warn("[PICKUP] No piece within 20m of ({:.1f}, {:.1f}, {:.1f})",
                     px, py, pz);
        g_collected_count++;
    }

    spdlog::info("[STATUS] {} / {} collected | Remaining: {}",
                 g_collected_count, (int)g_world_pieces.size(),
                 (int)g_world_pieces.size() - g_collected_count);
    write_status_json();
}

static void __fastcall hooked_set_event_flag(uint64_t event_man, uint32_t *event_id, bool state)
{
    uint32_t flag_id = *event_id;

    bool old_state = false;
    if (g_is_event_flag)
        old_state = g_is_event_flag(event_man, event_id);

    g_set_event_flag_original(event_man, event_id, state);

    if (flag_id == PICKUP_FLAG_ID && !old_state && state) {
        spdlog::info("[PICKUP] Flag {} triggered! Player at ({:.1f}, {:.1f}, {:.1f})",
                     flag_id, g_cur_x.load(), g_cur_y.load(), g_cur_z.load());

        std::thread([]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            on_piece_picked_up();
        }).detach();
    }
}

void tracker::initialize(const std::filesystem::path &mod_folder)
{
    g_mod_folder = mod_folder;
    g_game_base = (uintptr_t)GetModuleHandleA("eldenring.exe");
    spdlog::info("[INIT] Game base: {:016X}", g_game_base);

    if (!load_pieces_database())
        spdlog::error("[INIT] Piece database not loaded, matching will not work");

    modutils::initialize();

    g_event_flag_man = modutils::scan<uint64_t>({
        .aob = "48 8B 3D ?? ?? ?? ?? 48 85 FF ?? ?? 32 C0 E9",
        .relative_offsets = {{3, 7}},
    });
    if (g_event_flag_man)
        spdlog::info("[INIT] EventFlagMan ptr @ {:016X}", (uintptr_t)g_event_flag_man);
    else
        spdlog::error("[INIT] EventFlagMan NOT FOUND");

    auto is_flag_addr = modutils::scan({.aob = "48 83 EC 28 8B 12 85 D2"});
    if (is_flag_addr)
        g_is_event_flag = (IsEventFlagFn *)is_flag_addr;

    auto set_flag_addr = modutils::scan({
        .aob = "48 89 74 24 18 57 48 83 EC 30 48 8B DA 41 0F B6 F8 8B 12 48 8B F1 85 D2 0F 84",
    });
    if (set_flag_addr) {
        uintptr_t addr = (uintptr_t)set_flag_addr;
        uintptr_t aligned = addr;
        for (int back = 1; back <= 16; back++) {
            uintptr_t candidate = addr - back;
            if ((candidate & 0xF) == 0) { aligned = candidate; break; }
        }
        spdlog::info("[INIT] SetEventFlag @ {:016X}", aligned);
        modutils::hook((void *)aligned, (void *)&hooked_set_event_flag,
                       (void **)&g_set_event_flag_original);
    }

    modutils::enable_hooks();

    refresh_collected("init");
    log_all_chains();

    spdlog::info("[INIT] Ready. Pickup detection active. {} pieces in database.",
                 g_world_pieces.size());
}

void tracker::hotkey_loop()
{
    spdlog::info("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    spdlog::info("  RunePieceTracker v" PROJECT_VERSION);
    spdlog::info("  F9  = Refresh + log position chains");
    spdlog::info("  F10 = Log current position");
    spdlog::info("  F11 = Deep position memory scan");
    spdlog::info("  Continuous position tracking active");
    spdlog::info("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");

    while (true)
    {
        Vec3 pos;
        if (try_read_position(pos)) {
            g_cur_x.store(pos.x);
            g_cur_y.store(pos.y);
            g_cur_z.store(pos.z);
            if (!g_pos_valid.load()) {
                g_pos_valid.store(true);
                spdlog::info("[POS] First valid position: ({:.1f}, {:.1f}, {:.1f})",
                             pos.x, pos.y, pos.z);
            }
        }

        if (GetAsyncKeyState(VK_F9) & 1) {
            refresh_collected("F9_hotkey");
            log_all_chains();
            if (g_pos_valid.load())
                spdlog::info("[POS] Current: ({:.1f}, {:.1f}, {:.1f})",
                             g_cur_x.load(), g_cur_y.load(), g_cur_z.load());
        }

        if (GetAsyncKeyState(VK_F10) & 1) {
            if (g_pos_valid.load())
                spdlog::info("[POS] Current: ({:.1f}, {:.1f}, {:.1f})",
                             g_cur_x.load(), g_cur_y.load(), g_cur_z.load());
            else
                spdlog::warn("[POS] No valid position yet");
        }

        if (GetAsyncKeyState(VK_F11) & 1) {
            dump_position_memory();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
