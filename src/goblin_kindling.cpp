// Per-spirit visibility for the 5 ERR Kindling Spirits in m60_45_37_00.
//
// Static markers ship via WorldMapPointParam rows with
// `textDisableFlagId1 = 1045377500` (engine auto-sets the flag when the
// player receives ItemLot 1045370500). After all 5 are collected the
// engine hides every marker on its own.
//
// In-run per-spirit hiding (this module):
//   1. Resolve IsEventFlag(EventFlagMan*, uint32_t*) via AOB.
//   2. Each tick, check flag 1045377500. ON → all 5 collected, hide all.
//   3. Otherwise consult read_sfx_alive_set() - discovers the 5 per-spirit
//      EcTestDistance condition objects by heap-scanning for their vftable
//      (self-identifying via cond+0x30 == entity_id) and caches them; a
//      spirit is alive iff its cached cond still validates. (No static
//      pointer chain to them exists and no per-spirit event flag exists -
//      both confirmed by the 2026-05-29 multi-angle research workflow.)
//   4. Write `areaNo = 99` on collected rows, restore on respawned rows.

#include "goblin_kindling.hpp"
#include "goblin_map_data.hpp"
#include "modutils.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <spdlog/spdlog.h>
#include <string>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

using Category = goblin::generated::Category;

namespace
{

// ── Constants pulled from emevd / regulation analysis ──
//
// See plan: m60-45-37-00-ancient-starlight.md
//   permanent_flag (engine auto-set on award of ItemLot 1045370500): 1045377500
//   per-spirit SFX region IDs:                                       1045373501..505
//   the awarded incantation:                                         Goods 6610
constexpr uint32_t PERMANENT_FLAG = 1045377500;
constexpr uint32_t FIRST_SPIRIT_ENTITY_ID = 1045373501;  // KindlingSpirit_0001
constexpr int      SPIRIT_COUNT = 5;
constexpr uint8_t  HIDDEN_AREA = 99;

// WorldSfxMan singleton slot + EcTestDistance vtable are resolved by AOB
// (patch-resilient) instead of hardcoded RVAs - see the resolvers below.

struct KindlingSlot
{
    uint64_t row_id;     // current (post-remap) WorldMapPointParam row ID
    uint64_t orig_id;    // original ID from MAP_ENTRIES (pre-remap)
    int slot;            // 1..5 (kindling slot)
    uint32_t entity_id;  // 1045373501..505
};

struct ParamRef
{
    uint8_t *ptr;
    uint8_t original_areaNo;
};

std::vector<KindlingSlot> g_slots;
std::map<uint64_t, ParamRef> g_param_ptrs;
std::set<uint64_t> g_collected_rows;
std::unordered_map<uint64_t, uint64_t> g_original_to_dynamic;
bool g_initialized = false;

// ── Event-flag query ─────────────────────────────────────────────────
// Same AOBs as goblin_markers.cpp - could be lifted into a shared header
// later, but keeping the resolution local keeps coupling minimal.

using IsEventFlagFn = bool (*)(void *, uint32_t *);
IsEventFlagFn g_is_event_flag = nullptr;
void **g_event_man_slot = nullptr;
bool g_flag_resolve_tried = false;

void resolve_flag_api()
{
    if (g_flag_resolve_tried) return;
    g_flag_resolve_tried = true;
    try
    {
        g_is_event_flag = modutils::scan<bool(void *, uint32_t *)>(
            { .aob = "48 83 EC 28 8B 12 85 D2" });
    }
    catch (...) { g_is_event_flag = nullptr; }

    try
    {
        g_event_man_slot = modutils::scan<void *>(
            { .aob = "48 8B 3D ?? ?? ?? ?? 48 85 FF ?? ?? 32 C0 E9",
              .relative_offsets = { {3, 7} } });
    }
    catch (...) { g_event_man_slot = nullptr; }
}

bool is_flag_set(uint32_t flag_id)
{
    resolve_flag_api();
    if (!g_is_event_flag || !g_event_man_slot) return false;
    void *event_man = *g_event_man_slot;
    if (!event_man) return false;
    uint32_t id = flag_id;
    return g_is_event_flag(event_man, &id);
}

// ── SEH-guarded byte write ───────────────────────────────────────────
// Stale param pointers (other mod swapping params, game reload) would
// otherwise crash the entire refresh tick. Match the safety pattern in
// goblin_collected.cpp:safe_write_byte.

bool safe_write_byte(uint8_t *addr, uint8_t val)
{
    __try
    {
        *addr = val;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// ── Liveness via the EcTestDistance condition objects ────────────────
//
// Each live kindling spirit in m60_45_37_00 has one `CS::EcTestDistance`
// instance in CSEmkSystem's event-condition graph (EMEVD `3:3 IF Entity
// In/Outside Radius`). The instance is freed when the player picks the
// spirit up. So per-spirit liveness == "does an EcTestDistance with this
// entity_id currently exist". These conds spawn dynamically as the world
// warms up after a load - discovery retries every 2 s until they appear
// (typically within ~1 min of entering Misty Forest).
//
// Multi-angle research (2026-05-29 workflow, 3-way confirmed): NO static
// pointer chain to these objects (heap-only scheduler, grid-indexed SFX),
// and NO per-spirit event flag (only the all-5 PERMANENT_FLAG 1045377500).
// So a heap scan is the only per-spirit source. See memory:
// kindling-eventflag-chain.
//
// Scan target is the EcTestDistance vftable (image+0x2A5BB90): each hit
// SELF-IDENTIFIES via cond+0x30 == entity_id - no task lookup, no entry
// array walk, no 8192-hit buffer. Only the eid match is used as the
// discriminator (a radius/threshold gate was tried and over-filtered the
// real conds - removed). ~75 EcTestDistance objects exist once the area
// is loaded; the 5 kindling ones are a subset.
//
// Stable across launches: the vftable RVA and cond+0x30 (eid). Heap
// addresses change per launch (re-discover after a map transition); a
// cached cond ptr is re-validated each tick by
// (qword[cond]==vft && dword[cond+0x30]==expected_eid).

constexpr size_t    COND_TO_EID_OFFSET = 0x30;       // u32 entity_id

// EcTestDistance vftable + WorldSfxMan slot, resolved by AOB (patch-resilient)
// instead of hardcoded RVAs (those moved on game updates). relative_offsets
// {{3,7}} extracts the target of the `lea/mov reg,[rip+X]` xref; AOBs wildcard
// the rip-disp + branch targets. Resolved once, cached, 0 if not found.
static uintptr_t kindling_resolve(const char *aob)
{
    return reinterpret_cast<uintptr_t>(modutils::scan<void>({
        .aob = aob, .relative_offsets = {{3, 7}}}));
}
// EcTestDistance vftable (was RVA 0x2A5BB90) - the constructor's primary vtable
// store. Used to validate per-spirit cond objects ([cond+0] == this vtable).
static uintptr_t distance_vft()
{
    static uintptr_t s = kindling_resolve(
        "48 8D 05 ?? ?? ?? ?? 48 89 01 48 8D 05 ?? ?? ?? ?? 48 89 01 F6 C2 01 74 ?? "
        "BA 40 00 00 00 E8 ?? ?? ?? ?? 90 48 8B C3 48 83 C4 30 5B C3 90 78 ??");
    return s;
}
// WorldSfxMan singleton slot (was RVA 0x3D6F5F8) - "game world loaded"
// indicator (the pointer there is NULL at the menu / loading screen).
static uintptr_t world_sfx_man_slot()
{
    static uintptr_t s = kindling_resolve("48 8B 05 ?? ?? ?? ?? 48 8D 4D 98 48 89 4C 24 60");
    return s;
}

// State shared between the discovery worker and the refresh thread.
// Cached cond pointers keyed by entity_id (1045373501..505). A spirit is
// alive iff its cached cond still validates (vft + eid match); picked-up
// spirits' conds get freed and fail validation.
std::mutex g_state_mutex;
std::map<uint32_t, uintptr_t> g_kindling_conds;  // eid -> EcTestDistance* (cached)
bool g_conds_discovered = false;                 // true once a discovery has succeeded

// Worker thread (one-shot discovery; re-run only when the cached task
// becomes invalid - e.g. map transition freed the task instance).
std::thread g_worker_thread;
std::atomic<bool> g_worker_started{false};
std::atomic<bool> g_worker_should_stop{false};
std::atomic<bool> g_discovery_in_progress{false};
std::condition_variable g_worker_cv;
std::mutex g_worker_mutex;
bool g_discovery_requested = false;

// SEH-guarded primitive reads. Heap addresses we follow are entirely
// controlled by the game and may be freed/remapped between ticks - a
// stray page fault during refresh would otherwise tank the whole DLL.

static uintptr_t seh_read_qword(uintptr_t addr)
{
    __try
    {
        return *reinterpret_cast<const uintptr_t *>(addr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

static uint32_t seh_read_dword(uintptr_t addr)
{
    __try
    {
        return *reinterpret_cast<const uint32_t *>(addr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

static uintptr_t eldenring_base()
{
    HMODULE h = GetModuleHandleA("eldenring.exe");
    return reinterpret_cast<uintptr_t>(h);
}

static bool is_game_world_loaded()
{
    uintptr_t base = eldenring_base();
    if (!base) return false;
    uintptr_t val = seh_read_qword(world_sfx_man_slot());
    return val >= 0x10000;
}

// True if `cond` still points at a kindling EcTestDistance for `eid`:
// vftable matches and cond+0x30 == eid. A picked-up spirit's cond is
// freed (vft mismatch / reused chunk) → returns false.
static bool cond_is_alive(uintptr_t cond, uint32_t eid)
{
    if (cond < 0x10000) return false;
    uintptr_t base = eldenring_base();
    if (!base) return false;
    if (seh_read_qword(cond) != distance_vft()) return false;
    return seh_read_dword(cond + COND_TO_EID_OFFSET) == eid;
}

// Re-validate cached conds → set of currently-alive kindling eids.
// out_valid=false if NONE validate (all picked up, or cache stale after a
// map transition → caller re-discovers).
static std::set<uint32_t> revalidate_conds(const std::map<uint32_t, uintptr_t> &conds,
                                           bool &out_valid)
{
    std::set<uint32_t> alive;
    for (const auto &[eid, cond] : conds)
        if (cond_is_alive(cond, eid))
            alive.insert(eid);
    out_valid = !alive.empty();
    return alive;
}

// Scan one VirtualAlloc'd region for the EcTestDistance vftable. Writes
// matching addresses into `hits` (capped at `hits_max`). Plain C-style:
// no C++ objects in scope, so MSVC accepts SEH around the raw deref.
// Returns count of matches written.
static size_t seh_scan_region_for_vft(uintptr_t base, size_t size,
                                       uintptr_t target_vft,
                                       uintptr_t *hits, size_t hits_max,
                                       size_t start_idx)
{
    size_t out = start_idx;
    __try
    {
        const uintptr_t *p = reinterpret_cast<const uintptr_t *>(base);
        size_t n = size / 8;
        for (size_t i = 0; i < n && out < hits_max; i++)
        {
            if (p[i] == target_vft)
                hits[out++] = reinterpret_cast<uintptr_t>(p + i);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // unmapped page mid-region - keep what we have, drop the rest
    }
    return out;
}

// Cold discovery: heap-scan for the EcTestDistance vftable, then keep the
// hits whose entity_id (cond+0x30) is one of our 5 kindling spirits. No
// task lookup, no array walk - each hit self-identifies. Costly only in the
// memory-read sense (touches the RW-private heap), runs once per session /
// map transition on the worker thread behind the loading screen.
static std::map<uint32_t, uintptr_t> discover_kindling_conds()
{
    auto t0 = std::chrono::steady_clock::now();
    std::map<uint32_t, uintptr_t> found;
    uintptr_t base = eldenring_base();
    if (!base) return found;

    uintptr_t target_cond_vft = distance_vft();
    if (!target_cond_vft) return found;

    // ~71 EcTestDistance objects exist process-wide; 256 hit slots is ample.
    constexpr size_t HITS_MAX = 256;
    static uintptr_t hits[HITS_MAX];
    size_t hits_n = 0;

    HANDLE proc = GetCurrentProcess();
    constexpr uintptr_t HEAP_LO = 0x10000000000ull;
    constexpr uintptr_t HEAP_HI = 0x700000000000ull;
    uintptr_t addr = HEAP_LO;
    size_t regions_scanned = 0;

    while (addr < HEAP_HI && hits_n < HITS_MAX)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQueryEx(proc, reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)))
            break;
        uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= addr) break;
        addr = next;

        if (mbi.State != MEM_COMMIT) continue;
        DWORD prot = mbi.Protect & 0xFF;
        if (prot != PAGE_READWRITE && prot != PAGE_WRITECOPY) continue;
        if (mbi.Protect & PAGE_GUARD) continue;
        if (mbi.Type != MEM_PRIVATE) continue;
        if (mbi.RegionSize > (size_t)64 * 1024 * 1024) continue;
        if (mbi.RegionSize < 0x1000) continue;

        regions_scanned++;
        hits_n = seh_scan_region_for_vft(
            reinterpret_cast<uintptr_t>(mbi.BaseAddress), mbi.RegionSize,
            target_cond_vft, hits, HITS_MAX, hits_n);
    }

    // Out of SEH scope. Keep hits whose eid is a kindling spirit. eid match
    // (cond+0x30) is the only verified discriminator - radius/threshold were
    // an unconfirmed assumption and over-filtered, so we don't gate on them.
    // near_count tracks hits whose eid is in the broad m60_45_37 SFX band so
    // the failure log can distinguish "region not covered by scan filter"
    // (near==0) from "eid range/offset wrong" (near>0 but none in 501..505).
    int near_count = 0;
    for (size_t i = 0; i < hits_n; i++)
    {
        uintptr_t cond = hits[i];
        uint32_t eid = seh_read_dword(cond + COND_TO_EID_OFFSET);
        if (eid >= 1045370000 && eid <= 1045380000) near_count++;
        if (eid < FIRST_SPIRIT_ENTITY_ID || eid >= FIRST_SPIRIT_ENTITY_ID + SPIRIT_COUNT)
            continue;
        found[eid] = cond;
    }

    auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    if (!found.empty())
        spdlog::info("[KINDLING] discovered {} live spirit cond(s) in {} ms ({} regions, {} EcTestDistance hits)",
                     found.size(), dt, regions_scanned, hits_n);
    else
        spdlog::info("[KINDLING] no kindling conds found ({} ms, {} regions, {} hits, {} near-band) - "
                     "near==0 => cond region not covered by scan filter; near>0 => eid range/offset off; "
                     "else not in Misty Forest yet",
                     dt, regions_scanned, hits_n, near_count);
    return found;
}

// ── Worker thread ───────────────────────────────────────────────────

static void worker_loop()
{
    spdlog::info("[KINDLING] worker thread started");
    while (!g_worker_should_stop.load())
    {
        {
            std::unique_lock<std::mutex> lock(g_worker_mutex);
            g_worker_cv.wait_for(lock, std::chrono::seconds(2), []() {
                return g_discovery_requested || g_worker_should_stop.load();
            });
            if (g_worker_should_stop.load()) break;
            if (!g_discovery_requested) continue;
            g_discovery_requested = false;
        }

        // Wait for the world to load before scanning. Heap scanning
        // while the game is reallocating mid-transition races with
        // its allocator and has been observed to lock the game with
        // an "Ошибка" overlay.
        if (!is_game_world_loaded())
        {
            spdlog::info("[KINDLING] world not loaded - retrying discovery in 2 s");
            std::this_thread::sleep_for(std::chrono::seconds(2));
            {
                std::lock_guard<std::mutex> lock(g_worker_mutex);
                g_discovery_requested = true;
            }
            continue;
        }

        g_discovery_in_progress = true;
        auto conds = discover_kindling_conds();
        g_discovery_in_progress = false;

        // Re-check after scan: if world unloaded mid-scan (character
        // switch happened), discard - the cond pointers are now into
        // freed memory.
        if (!is_game_world_loaded())
        {
            spdlog::warn("[KINDLING] world unloaded during discovery - discarding result");
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            g_kindling_conds = std::move(conds);
            g_conds_discovered = !g_kindling_conds.empty();
        }
    }
    spdlog::info("[KINDLING] worker thread stopped");
}

static void ensure_worker_started()
{
    if (g_worker_started.exchange(true)) return;
    g_worker_thread = std::thread(worker_loop);
    g_worker_thread.detach();
}

static void request_discovery()
{
    if (g_discovery_in_progress.load()) return;
    {
        std::lock_guard<std::mutex> lock(g_worker_mutex);
        g_discovery_requested = true;
    }
    g_worker_cv.notify_one();
}

// Public read API. Called from refresh thread (every ~2 s tick).
// Sub-millisecond synchronous walk from the cached task pointer; if no
// task is cached or the cached one is stale, kicks off discovery and
// returns nullopt (refresh treats nullopt as "no info, keep markers
// visible" - the correct cold-start behavior).
std::optional<std::set<uint32_t>> read_sfx_alive_set()
{
    ensure_worker_started();

    std::map<uint32_t, uintptr_t> conds;
    bool discovered;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        conds = g_kindling_conds;
        discovered = g_conds_discovered;
    }

    if (discovered && !conds.empty())
    {
        bool still_valid = false;
        auto alive = revalidate_conds(conds, still_valid);
        if (still_valid) return alive;

        // No cached cond validates anymore. Either all 5 picked up
        // (permanent flag is about to fire and the ON branch in refresh()
        // takes over) or the heap got reused after a map transition.
        // Invalidate + kick re-discovery.
        spdlog::info("[KINDLING] cached conds no longer valid - re-discovering");
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            g_kindling_conds.clear();
            g_conds_discovered = false;
        }
        request_discovery();
        return std::nullopt;
    }

    // Nothing cached yet - request discovery on the worker thread.
    // Returns nullopt this tick; next tick after discovery completes
    // returns the alive set.
    request_discovery();
    return std::nullopt;
}

// ── Helpers ──────────────────────────────────────────────────────────

// Decode "KindlingSpirit_0003" → 3. Returns -1 on failure.
int parse_kindling_slot(const char *object_name)
{
    if (!object_name) return -1;
    static const char prefix[] = "KindlingSpirit_";
    constexpr size_t plen = sizeof(prefix) - 1;
    if (strncmp(object_name, prefix, plen) != 0) return -1;
    int n = 0;
    const char *p = object_name + plen;
    while (*p >= '0' && *p <= '9') { n = n * 10 + (*p++ - '0'); }
    if (*p != '\0') return -1;
    if (n < 1 || n > SPIRIT_COUNT) return -1;
    return n;
}

uint32_t entity_id_for_slot(int slot)
{
    return FIRST_SPIRIT_ENTITY_ID + (uint32_t)(slot - 1);
}

}  // namespace

// ── Public API ───────────────────────────────────────────────────────

void goblin::kindling::initialize()
{
    g_slots.clear();
    g_param_ptrs.clear();
    g_collected_rows.clear();
    g_original_to_dynamic.clear();
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_kindling_conds.clear();
        g_conds_discovered = false;
    }

    for (size_t i = 0; i < generated::MAP_ENTRY_COUNT; i++)
    {
        const auto &e = generated::MAP_ENTRIES[i];
        if (e.category != Category::WorldKindlingSpirits) continue;

        int slot = parse_kindling_slot(e.object_name);
        if (slot < 0)
        {
            spdlog::warn("[KINDLING] Unparseable object_name on row {}: '{}'",
                         e.row_id, e.object_name ? e.object_name : "(null)");
            continue;
        }

        g_slots.push_back({e.row_id, e.row_id, slot, entity_id_for_slot(slot)});
    }

    std::sort(g_slots.begin(), g_slots.end(),
              [](const auto &a, const auto &b) { return a.slot < b.slot; });

    spdlog::info("[KINDLING] Initialized: {} slot(s) tracked", g_slots.size());
    for (const auto &s : g_slots)
        spdlog::debug("[KINDLING]   slot {} entity {} row {}", s.slot, s.entity_id, s.row_id);

    g_initialized = true;

    // Kick the worker thread immediately so chain discovery starts during
    // the loading screen instead of waiting for the first refresh tick.
    // The worker's world-loaded gate retries every 2 s until the world
    // is up, so discovery begins the instant it's safe.
    if (!g_slots.empty())
    {
        ensure_worker_started();
        request_discovery();
    }
}

void goblin::kindling::remap_row_ids(const std::unordered_map<uint64_t, uint64_t> &old_to_new)
{
    g_original_to_dynamic = old_to_new;
    for (auto &slot : g_slots)
    {
        auto it = old_to_new.find(slot.row_id);
        if (it != old_to_new.end())
            slot.row_id = it->second;
    }
}

void goblin::kindling::register_param_ptr(uint64_t row_id, void *param_data)
{
    auto *p = reinterpret_cast<uint8_t *>(param_data);
    g_param_ptrs[row_id] = {p, p[0x20]};
}

int goblin::kindling::refresh()
{
    if (!g_initialized || g_slots.empty())
        return 0;

    // Don't touch anything during character switch / loading screens.
    // The param pointers we'd write to may have been freed and re-
    // allocated for a different character; writing to them at this
    // moment can corrupt game state and cause an "Ошибка" overlay.
    if (!is_game_world_loaded())
        return 0;

    // Build the new collected set.
    std::set<uint64_t> new_collected;

    if (is_flag_set(PERMANENT_FLAG))
    {
        // Incantation already acquired - engine hides via textDisableFlagId1
        // already, but we set areaNo = 99 too for defense in depth.
        // No SFX scan: the in-RAM state is irrelevant once the lot is awarded.
        // This branch also handles character-switch correctly: the new
        // character's flag value is queried fresh via IsEventFlag.
        for (const auto &s : g_slots) new_collected.insert(s.row_id);
    }
    else if (auto alive = read_sfx_alive_set(); alive && !alive->empty())
    {
        // Some spirits found alive in RAM → we're in m60_45_37_00 mid-run.
        // Hide ones that are NOT alive (those got picked up this run).
        for (const auto &s : g_slots)
        {
            if (!alive->count(s.entity_id))
                new_collected.insert(s.row_id);
        }
    }
    else
    {
        // Empty alive set: player isn't in m60_45_37_00 (main menu, another
        // map, freshly loaded character who never visited Misty Forest).
        // Keep all 5 markers VISIBLE - the WorldMapPointParam baseline.
        // The brief "all 5 collected mid-run, flag not yet set" race resolves
        // within one tick when the engine awards the item lot and flips
        // PERMANENT_FLAG, after which the ON branch above takes over.
    }

    if (new_collected == g_collected_rows)
        return 0;

    // Apply: restore everyone first, then hide collected.
    std::vector<uint64_t> stale;
    for (auto &[row_id, ref] : g_param_ptrs)
    {
        if (!safe_write_byte(ref.ptr + 0x20, ref.original_areaNo))
            stale.push_back(row_id);
    }

    int applied = 0;
    for (uint64_t row_id : new_collected)
    {
        auto it = g_param_ptrs.find(row_id);
        if (it == g_param_ptrs.end()) continue;
        if (safe_write_byte(it->second.ptr + 0x20, HIDDEN_AREA))
            applied++;
        else
            stale.push_back(row_id);
    }

    if (!stale.empty())
    {
        std::sort(stale.begin(), stale.end());
        stale.erase(std::unique(stale.begin(), stale.end()), stale.end());
        for (auto id : stale) g_param_ptrs.erase(id);
        spdlog::warn("[KINDLING] Evicted {} stale param pointer(s) (write AV)", stale.size());
    }

    int delta = (int)new_collected.size() - (int)g_collected_rows.size();
    g_collected_rows = std::move(new_collected);
    spdlog::info("[KINDLING] Refresh: {} hidden (delta {:+d}), applied {}",
                 g_collected_rows.size(), delta, applied);
    return delta;
}

bool goblin::kindling::is_row_collected(uint64_t row_id)
{
    if (g_collected_rows.count(row_id)) return true;
    auto it = g_original_to_dynamic.find(row_id);
    if (it != g_original_to_dynamic.end() && g_collected_rows.count(it->second))
        return true;
    return false;
}

int goblin::kindling::collected_count()
{
    return (int)g_collected_rows.size();
}
