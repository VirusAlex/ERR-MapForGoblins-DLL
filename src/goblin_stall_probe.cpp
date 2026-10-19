#include "goblin_stall_probe.hpp"
#include "goblin_anchors.hpp" // every base+RVA engine helper resolves through this
#include "goblin_config.hpp"
#include "goblin_map_api.hpp"
#include "goblin_guarded.hpp" // faults we ask for must not be recorded as crashes

#if MFG_STALL_PROFILER
namespace goblin::watch { void request(uintptr_t address, unsigned long thread_id); }
#endif
#include "goblin_build_variants.hpp"
#include "goblin_config_schema.hpp"
#include "goblin_native_menu.hpp"
#include "goblin_own_movie.hpp"
#include "goblin_overlay.hpp" // gamepad_mask_down: the pad state is polled there
// (goblin_map_icons.hpp was included TWICE here - bare and generated_shared, which resolve to the
//  same generated header - for MAP_ICON_TAGS, whose last user in this file was icon_resource_for.)
#include "generated_shared/goblin_menu_icon_tags.hpp"
#include "goblin_maphover.hpp"
#include "goblin_map_timing.hpp" // the [opentime] probe's burst t0 + relayout counters
#include "goblin_mapproject.hpp"
#include "goblin_gfx_probe.hpp"
#include "goblin_collected.hpp" // read_player_map_id() for the location emphasis
#include "goblin_crashdiag.hpp" // measurement, not behaviour: see the header
#include "goblin_inject.hpp"
#include "goblin_safemem.hpp" // validate-then-read: v3_read*/v3_write_bytes are built on it
#include "modutils.hpp"

#include <spdlog/spdlog.h>
// (miniz.h was included for icon_resource_for's inflate; no mz_ symbol is used here any more.)

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <intrin.h> // _ReturnAddress

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <tlhelp32.h>

// Sampling profiler for one game thread. capture() is called on the thread we
// want to observe (map UI thread); the actual sampling runs on a detached helper
// thread so the observed thread keeps running the stall we want to measure.
// While the target is paused we only read its context and scan its frozen stack
// for return addresses into the exe image - no allocation, no logging, so the
// helper can never block on a lock the paused thread holds.
namespace
{
    // The engine classes whose vtable identity these probes check. Looked up by RTTI name at
    // runtime (goblin::anchors::vtable_of) because a vtable lives in .rdata, where a byte
    // anchor has nothing to match - a baked address only ever matched one game build. The
    // names live in goblin::anchors::kRttiClasses, which init resolves before these run.
    constexpr auto kVtAutoHideCtrl = goblin::anchors::RttiClass::CSFeAutoHideCtrl;
    constexpr auto kVtKeyConfigDialog = goblin::anchors::RttiClass::KeyConfigDialog;
    constexpr auto kVtWorldMapDialog = goblin::anchors::RttiClass::WorldMapDialog;

    std::atomic<bool> g_running{false};

    // ── pin-registration cost counters ──────────────────────────────────
    // Pass-through wraps on the three per-marker map-widget virtual methods
    // (vtable slots at 0x142cbc840/848/850; entries 0x1410dbb70 / 0x1410dbea0 /
    // 0x1410dc260 in v1.16) and the typed-find walk (0x14113feb0) they drive.
    // These fns are virtual-dispatched (no static xrefs), so recording the REAL
    // return addresses here is the only reliable way to identify the per-marker
    // driver loop for the soft-populate design. Counting only happens while a
    // capture window is open; otherwise each wrap is one relaxed load + branch.
    // Slots 4/5 (2026-07-14 round 2): the per-frame while-map-open hot spots the
    // sampler surfaced - the child-step loop (0x1411d3980), reached through a
    // tail-call from the display visibility setter (0x1411c1c20), and the
    // transform getter (0x14117e140, full matrix decompose with sqrt+atan2 when
    // its cache ptr [this+0x50] is null, cheap copy when cached).
    // Slots 6-8 (release-in-fade expedition): the GFx render batch internals.
    // item-proc (0x140d716a0) runs per movie-slot on a WORKER; next-capture
    // (0x1411577b0) applies the accumulated display-tree changelist (teardown of
    // ~9k objects lands here per the working theory); display (0x14115cde0) draws.
    // The timings decide whether the close freeze is changelist-apply or draw.
    // Slot 9: the batch job entry itself (0x140d7d720, runs on a worker). Its max
    // vs the ~140ms scheduler wait separates "long job" from "job stuck in queue"
    // (priority inversion in the task system).
    constexpr int N_REG = 10;
    const char *const REG_NAME[N_REG] = {"widget-a",   "widget-b",     "widget-c",
                                         "typed-find", "child-step",   "xform-get",
                                         "item-proc",  "next-capture", "movie-display",
                                         "batch-job"};
    using Fn4 = void *(void *, void *, void *, void *);
    Fn4 *o_reg[N_REG] = {};
    std::atomic<bool> g_count{false};
    std::atomic<uint64_t> g_reg_calls[N_REG];
    std::atomic<uint64_t> g_reg_ticks[N_REG];
    std::atomic<uint64_t> g_reg_max[N_REG]; // worst single call (attributes one-off stalls)

    constexpr size_t RET_SLOTS = 32; // per-fn return-address histogram (linear probe)
    struct RetSlot
    {
        std::atomic<uintptr_t> addr{0};
        std::atomic<uint32_t> cnt{0};
    };
    RetSlot g_ret[N_REG][RET_SLOTS];

    // V3 native-marker layer discovery. The hooked child propagation loop receives
    // a live Scaleform display object in RCX. Container subclasses own the vector
    // walked by vtbl+0x348 at +0xd8 (count at +0xe0, stride 0x10). Keep only the
    // busiest object seen during each capture window. For a large object, scan
    // the object and three sampled children for the distinct PlaceObject Execute
    // context layout: list@+0x28/+0x30, pointer stride 8, and the ctx/list node
    // sharing qword 0. This distinguishes the real timeline display list from
    // the 16-byte display-object vector and from hash tables.
    constexpr int V3_LINK_CAP = 16;
    struct V3LinkSnapshot
    {
        uintptr_t root = 0;
        uintptr_t source_off = 0;
        uintptr_t candidate = 0;
        uintptr_t list = 0;
        uintptr_t first_node = 0;
        uint64_t count = 0;
        uint64_t shared_key = 0;
        uint32_t first_depth = 0;
        bool indirect = false;
    };
    struct V3ContainerSnapshot
    {
        uintptr_t parent = 0;
        uintptr_t entries = 0;
        uintptr_t ret = 0;
        uint64_t count = 0;
        uint64_t parent_vt = 0;
        uint64_t parent_28 = 0;
        uint64_t parent_30 = 0;
        uint64_t parent_b8 = 0;
        uint64_t parent_name[3] = {};
        uintptr_t child[3] = {};
        uintptr_t child_aux[3] = {};
        uint64_t child_vt[3] = {};
        uint64_t child_28[3] = {};
        uint64_t child_30[3] = {};
        uint64_t child_name[3][3] = {};
        V3LinkSnapshot link[V3_LINK_CAP] = {};
        uint32_t link_count = 0;
    };
    V3ContainerSnapshot g_v3_container;
    std::atomic<uint64_t> g_v3_published_count{0};
    std::atomic_flag g_v3_snapshot_lock = ATOMIC_FLAG_INIT;

    // Exact DisplayObjContainer mutation path. FUN_14113e970 receives the
    // container's 16-byte child vector (RCX), its owner (RDX), sorted insertion
    // index (R8), and the newly-created display object (R9). Higher-level map/UI
    // builders call this insert core directly, bypassing the 0x14113eb60
    // PlaceObject replace wrapper.
    // Recording the before/after count proves that the 6468-object map layer is
    // built through this API and preserves the arguments needed for a later,
    // deliberately-scoped native insertion experiment.
    //
    // PROFILER-ONLY, all of it: the hook, its detour, the recorder v3_note_add and the report
    // block that drains these counters. The whole cluster used to sit outside the guard while
    // only the detour and the hook were inside it, which meant that in a default build the
    // recorder and the report compiled with no way to run - and their string literals reached
    // .rdata, which is the one thing the AV heuristics read. That split is what 947216c set out
    // to close; it stopped one level short.
#if MFG_STALL_PROFILER
    using V3AddFn = void(void *, void *, void *, void *);
    V3AddFn *o_v3_add = nullptr;

    struct V3AddSnapshot
    {
        uintptr_t vector = 0;
        uintptr_t parent = 0;
        uintptr_t child = 0;
        uintptr_t ret = 0;
        uint64_t before_count = 0;
        uint64_t after_count = 0;
        uint64_t insert_index = 0;
        uint64_t child_vt = 0;
        uint32_t child_depth = 0;
        bool owner_match = false;
    };
    V3AddSnapshot g_v3_add;
    std::atomic<uint64_t> g_v3_add_calls{0};
    std::atomic<uint64_t> g_v3_add_grew{0};
    std::atomic<uint64_t> g_v3_add_same{0};
    std::atomic<uint64_t> g_v3_add_owner_match{0};
    std::atomic<uint64_t> g_v3_add_max_count{0};
    std::atomic_flag g_v3_add_lock = ATOMIC_FLAG_INIT;
#endif // MFG_STALL_PROFILER

    // PlaceObject insertion path. LOAD-BEARING: v3_place_detour matches the marker factory's
    // production slots by depth + charId and carries the factory's ready DisplayObjects into
    // g_v3_native - it is not a spike probe. (The comment here used to describe a retired
    // one-shot experiment - "filter to our spike's (first injected charId, depth 24) pair" -
    // which the detour has not done for a long time; the custom path it belonged to was a
    // compile-time-false constant and is gone.)
    using V3PlaceFn = void(void *, void *, void *, void *, uint64_t);
    V3PlaceFn *o_v3_place = nullptr;
    // V3CustomSnapshot, its instance, hit counter and lock lived here.

    // FUN_1410c8440 is the high-level attach API: given its display-list wrapper, a child and an
    // index, it removes the child from any old parent through the engine's own path, inserts it
    // into the new parent's +0xd8 vector, and fixes parent/depth/flags/transform state.
    // LOAD-BEARING in every shipping build, and the comment here called it a "one-shot visual
    // experiment" until 2026-07-31: its detour holds v3_note_movie_attach, the ONLY writer of the
    // native-marker anchor, and the seed, the per-frame tick and the viewport reconcile all bail
    // out when that anchor is 0. Do not gate it on a diagnostic flag.
    using V3AttachFn = void(void *, void *, uint32_t);
    V3AttachFn *o_v3_attach = nullptr;
    using V3AttachMovieFn = uint32_t(void *, void *, void *, const char *, void *, int, void *);
    V3AttachMovieFn *o_v3_attach_movie = nullptr;
    // The eight matrix slots (children, base translations, state, start time) lived here.
    constexpr uint32_t V3_CANDIDATE_CAP = 128; // still live: the build-correlation candidate arrays

    struct V3BuildParentStat
    {
        uintptr_t wrapper = 0;
        uintptr_t parent = 0;
        uint64_t calls = 0;
        uint64_t min_count = UINT64_MAX;
        uint64_t max_count = 0;
    };
    struct V3BuildCorrelation
    {
        uint32_t depth = 0;
        void *owner = nullptr;
        void *ctx = nullptr;
        uint64_t total_calls = 0;
        uint32_t parent_count = 0;
        V3BuildParentStat parents[V3_CANDIDATE_CAP]{};
    };
    thread_local V3BuildCorrelation g_v3_build_corr;

    struct V3AttachMovieContext
    {
        bool active = false;
        char export_name[48]{};
        int requested_depth = 0;
        uint64_t init_count = 0;
        char key[4][24]{};
        uint64_t value[4][2]{};
    };
    thread_local V3AttachMovieContext g_v3_movie_ctx;

    void v3_note_build_attach(uintptr_t wrapper, uintptr_t parent, uint64_t count)
    {
        auto &corr = g_v3_build_corr;
        if (corr.depth == 0)
            return;
        ++corr.total_calls;
        for (uint32_t i = 0; i < corr.parent_count; ++i)
        {
            auto &s = corr.parents[i];
            if (s.parent != parent)
                continue;
            ++s.calls;
            s.wrapper = wrapper;
            s.min_count = (std::min)(s.min_count, count);
            s.max_count = (std::max)(s.max_count, count);
            return;
        }
        if (corr.parent_count < V3_CANDIDATE_CAP)
        {
            auto &s = corr.parents[corr.parent_count++];
            s.wrapper = wrapper;
            s.parent = parent;
            s.calls = 1;
            s.min_count = count;
            s.max_count = count;
        }
    }

    // A V3Candidate table stood here (wrapper / parent / caller / count per candidate marker
    // parent, cap V3_CANDIDATE_CAP) with its count and a spinlock. Its only outside reader was
    // v3_try_matrix_batch, removed 2026-07-30 with the custom-capture path; after that the
    // recorder only ever read its OWN entries to dedupe by parent, and `caller` was written twice
    // and read nowhere. Cost while dead: 4096 bytes of .bss zeroed on every single map close.
    // V3_CANDIDATE_CAP itself STAYS - it sizes V3BuildParentStat::parents[] further down.

    std::atomic<uintptr_t> g_v3_target_wrapper{0};
    std::atomic<uintptr_t> g_v3_target_parent{0};
    // v3_movie_of(anchor) captured when the anchor was taken. Arm D of the anchor test
    // compares against it: a candidate in a different Scaleform movie is a new generation
    // by construction, because our children live in the old one and are unreachable from it.
    std::atomic<uintptr_t> g_v3_target_movie{0};
    std::atomic<uint64_t> g_v3_target_count{0};
    std::atomic<int> g_v3_target_layer{-1};
    // True once the map closed after the current target was published. Gates the
    // parent-changed retarget arm: within ONE open session another 100+ child
    // WorldMapItem list must NOT steal the target (observed: a count=180 list
    // arriving 450ms after the build burst hijacked it and orphaned our batch).
    std::atomic<bool> g_v3_map_closed{false};
    // GetTickCount64() stamped when on_map_close ran. A quick reopen is proven the moment the map
    // dialog's per-frame Update advances maphover::last_activity_ms() PAST this stamp: the dialog
    // resumed, so the map is live NOW, as opposed to the close-then-teardown window where a stale
    // parent still looks heap-valid. This is what gates the safe clear of g_v3_map_closed for
    // reopens that emit no attachMovie burst (the game reused the movie), and it replaces an
    // earlier child-vtable heuristic that the shared marker vtable fooled into relinking a dying
    // parent - heap corruption on fast reopens.
    std::atomic<uint64_t> g_v3_close_ms{0};

    // (a g_solidfill_spike_done one-shot latch lived here and was reset on every map close so the
    // spike could "re-fire"; the spike it guarded had already been removed)

    // Factory BATCH: one pulse queues up to V3_FACTORY_BATCH timeline records
    // on the live ctx, then runs the engine's record-materialization driver on
    // them itself (see g_v3_mat_driver below) - the whole batch is created,
    // captured and transferred inside ONE pulse. RM2 pulses are a finite
    // build-burst pool (~2 per remaining native widget, ~3900/session), so at
    // batch=16 the pool covers ~60k markers - far above any profile's needs.
    //
    // That ~3900 figure is a BASE-MAP measurement and does not hold on the DLC map. Measured
    // 2026-08-01 from a player log in m61: the whole burst there yields ~213 pulses, so batch=16
    // caps the build at 3408 - and the player's every map open reported exactly
    // "CATEGORIES READY: layer=2 created=3407" against 7136 requested, i.e. the cap, not a bug in
    // the markers themselves. The pool is what is scarce on that map, so take more per pulse.
    // The pipeline stays strictly one-record-in-flight (see v3_native_factory_consume), so this
    // only lengthens the loop, it does not widen it - the shared per-icon tag stays legal.
    //
    // DO NOT RAISE THIS TO CHASE A SHORT BURST. Tried 2026-08-05, 48 -> 1024 (with the per-frame
    // budget raised to match), on the theory that ~9 us per marker makes a 1024-bite cost ~9 ms.
    // It was measured WRONG the same evening and reverted: one pulse then cost 305 ms, a single
    // map frame 75 ms, most creates came back "request produced no timeline node", and the FIRST
    // reopen built 100 markers of 7137. Per-marker cost is not flat in the bite - somewhere
    // between 48 and 1024 the engine stops materializing what one pulse queues, and the extra
    // requests are not merely wasted, they are slow. 48 is the value with a full 7136 on nearly
    // every open behind it; the rare open where the engine emits ~10 pulses instead of ~7877 is a
    // real defect, but this is not its lever.
    constexpr size_t V3_FACTORY_BATCH = 48;

    // How often the periodic snapshot+merge runs when nothing has asked for one. It was 200 ms,
    // and that was 80% of everything an idle open map cost: measured on Convergence,
    // `tick 27361 us, of which 5 merges cost 23409 us` in a one-second window - ~4.7 ms per
    // merge, because each one rebuilds the whole 8425-row snapshot and re-runs the de-overlap
    // spread (3.5 ms on its own) whether or not a single input changed.
    //
    // A USER ACTION DOES NOT WAIT FOR THIS. The master switch, a category, and the progress
    // focus all bump goblin::visibility_epoch(), and the tick merges IMMEDIATELY on that - the
    // epoch branch does not consult next_refresh_ms at all. What this cadence covers is state
    // that changes with no notification of its own: collected pickups, boss flags, the player's
    // own map. None of those can change while the world map is up, because the map is a menu and
    // the player stands still inside it - so four fifths of an idle frame's cost was being spent
    // to notice things that cannot happen while it is being spent.
    // STAYS AT 1000. Raising it to 10000 on 2026-08-07 looked right on paper and on three of the
    // four numbers - the 4.3 ms merge left one map frame in every second, the worst steady frame
    // fell from 4.5 ms to 1.5 ms, the open got cheaper - and the tester reported the stutter had
    // got WORSE. He was right and the instrument was not measuring the thing he was feeling.
    //
    // What the poll also does, which nothing here recorded, is keep v3_viewport_reconcile's
    // inputs fresh. Measured across both runs, steady seconds only: reconcile cost 3402 us/s with
    // the 1 Hz poll and 4669 us/s (+37%) with a 10 s one, because a stale snapshot makes the
    // window pass re-decide more per frame - and every extra attach/detach is engine work we do
    // not time at all. Reconcile is the movement path, so the regression lands exactly where he
    // was looking: scrolling the map with W+D, once per second, independent of zoom and of how
    // many icons are on screen (this pass walks every tracked object, not the visible ones).
    //
    // So the merge is not pure cost. Making it rarer is not the lever; making it CHEAP is, or
    // removing reconcile's dependency on a freshly rebuilt snapshot. Do not raise this again
    // without an instrument that can see a scrolling map - the per-frame max cannot, because
    // reconcile is spread across frames rather than spiking in one.
    constexpr uint64_t V3_IDLE_REFRESH_MS = 1000;

    // The staging array is NOT the batch: the pipeline keeps exactly one record in flight (see
    // v3_native_factory_consume, which arms slot 0 and finishes it before touching the next), so
    // this stays at one entry rather than sizing a thread_local by the batch.
    constexpr size_t V3_FACTORY_SLOTS = 1;

    // kPickerMinItems (100) stood here and is GONE, 2026-08-03. It gated who may become the
    // anchor, on the theory that a high floor keeps tiny side clips from stealing it. Measured, it
    // did the opposite: every retarget that fired at count=100 FAILED to build (0 of 4), because a
    // list topping out near 105 only reaches 100 after the build burst has spent its RM2 pulses -
    // "pulses seen=0", created=0, and an empty map on that open. Every count=48 retarget built all
    // 7136 (18 of 18). The replacement is a state test, not a floor: see v3_note_movie_attach.
    //
    // kSeedMinItems stays, and is a different question - it gates when we may START BUILDING on the
    // anchor we already have, and it is still doing its job.
    constexpr uint64_t kSeedMinItemsValue = 48;

    // ERR-look placement for the "cleared" badge twin. The badge frame is
    // authored like every icon (full footprint, centred), so at transfer its
    // basis/pivot are rescaled to ~26px over the ~92px icon and re-centred
    // up-left of the anchor - the effective centre (-335,-336) twips matches
    // the gfx placement in build_vanilla_gfx.py. Baked into the captured
    // basis/pivot once, so zoom counter-scale and show/hide stay uniform.
    constexpr float V3_BADGE_SCALE = 39.0f / 92.0f; // user-tuned: 1.5x the gfx look
    constexpr float V3_BADGE_OFF_X = -335.0f;
    constexpr float V3_BADGE_OFF_Y = -336.0f;
    struct V3FactorySlot
    {
        uint32_t depth = UINT32_MAX; // requested timeline depth (unique match key)
        uint32_t char_id = 0;
        uintptr_t node = 0;  // timeline record from create_native_icon_instance
        uintptr_t child = 0; // captured display child (PlaceObject hook)
        uint32_t age = 0;    // pulses waited for materialization
        uintptr_t root = 0;  // the child's movie view (v3_movie_of at capture)
        float base_tx = 0.0f, base_ty = 0.0f;
        float basis[4] = {};
        bool held = false;
        // The queue entry this slot consumed (pending_index advances at issue).
        goblin::NativeMarkerPoint point{};
        float map_x = 0.0f, map_z = 0.0f;
    };
    thread_local V3FactorySlot g_v3_factory_slots[V3_FACTORY_SLOTS];
    thread_local uint32_t g_v3_factory_active = 0; // slots with node != 0

    // Record-materialization driver (RE: scratch/re_materialize_driver.md,
    // FUN_1411bf1b0 in v2.6.2.0): walks the ctx's record list and creates a
    // real child for EVERY pending place record via the sprite's vtable+0x3b8
    // path. PO Executes only insert RECORDS; the engine itself runs this
    // driver solely on sprite frame advance, and gotoAndStop'd composites
    // never advance again - that was the hard ~1891-creations-per-session
    // ceiling. Calling it ourselves right after queueing a batch materializes
    // all K synchronously. sprite back-ref lives at ctx+0x58.
    using V3MatDriverFn = void(void *, void *); // (execCtx, sprite)
    V3MatDriverFn *g_v3_mat_driver = nullptr;

    // [v3pump] The frame pump (variants::kFramePump). Markers can only be created inside a
    // timeline snapshot that Sprite::GotoFrame is filling: the RM2 tags of every frame it covers
    // call our RM2 hook, and each such "pulse" lets the factory build a batch. The engine does that
    // only in its open burst, so until 2.1.5 all ~10k markers landed in ONE frame (80-100 ms).
    // The pump makes one hidden sprite-171 instance of our own per generation (the "generator":
    // created in the first pulse through the marker pipeline, re-parented to the marker parent,
    // never in `objects`, dies with the movie) and drives GotoFrame on it through its vtable:
    //   1. first pulse: create it, then one GotoFrame NESTED inside the engine's own, with pulses
    //      only counted (the proof). Pulses arrived -> the burst builds only the rows visible at
    //      seed (they sort first, so draw order is unchanged) and the pump owns the rest. No pulse
    //      or no generator -> this open builds everything in the burst, exactly as before.
    //   2. every map tick until the queue is spent: a budget of markers (adapted toward
    //      V3_PUMP_TARGET_US) built from the generator's own pulses, a stroke or a few per frame.
    // Measured with the probe (ERR 2026-09-24, 36 calls, 0 faults): a stroke over M of our frames
    // gives exactly 2M pulses (our frames are RM2 d1 + RM2 d2 + PO), a stroke back to our first
    // frame refills from 0 and gives the stock count (143 on ERR); the calls themselves cost
    // 12-300 us, the markers ~5 us each.
    // RE (live exe): GotoFrame exe+0x11C3360 = Sprite vtable +0x378, frame count +0x380; entry
    // guard word +0x6A bit 0x800 set and 0x1000 clear, dword +0x2C >= -1; current frame +0x11C
    // (0-based). A forward jump covers cur+1..target-1, a backward one 0..target-1; the target
    // frame itself runs through the tags' Execute (no pulse). GotoFrame stores +0x118 = 1
    // (stopped) itself on every path past its entry test (exe+0x11C35F6), so no SetPlayState call.
    // Flags (whole-.text scan): 0x400 = is a Sprite (ctor, never cleared; the engine's
    // gotoAndStop tests it too), 0x800 = AS3 MovieClip timeline (instance init; sprite 171 is a
    // MovieClip class), 0x1000 = unloading. Nested use is safe only on a sprite that is neither the
    // one whose GotoFrame is running nor its descendant: the generator is re-parented to the marker
    // parent before any call. No globals, no recursion guard, the snapshot's pool is per call on
    // the movie heap (RE report, scratch/create_cost/gotoframe/nested/).
    using V3GotoFrameFn = void(void *, uint32_t);      // (sprite, 0-based frame)
    using V3FrameCountFn = uint32_t(void *);
    V3GotoFrameFn *g_v3_goto_frame = nullptr;
    constexpr uint32_t V3_SLOT_GOTO_FRAME = 0x378;
    constexpr uint32_t V3_SLOT_FRAME_COUNT = 0x380;
    constexpr uint16_t V3_PUMP_SPRITE_CHAR = 171;      // the world-map icon sprite
    // Per-frame row budget, adapted on the measured cost of one row (full frames only, so the
    // short last frame of an open cannot ratchet it up). The target is the pump's own time; the
    // viewport reconcile detaches the same hidden rows in the same frame at about the same cost
    // per row (~5 us each on ERR), so a frame carries roughly twice this.
    constexpr size_t V3_PUMP_BUDGET_START = 512;
    constexpr size_t V3_PUMP_BUDGET_MIN = 128;
    constexpr size_t V3_PUMP_BUDGET_MAX = 2048;
    constexpr int64_t V3_PUMP_TARGET_US = 2500;
    constexpr uint32_t V3_PUMP_MAX_STROKES = 4;        // GotoFrame calls per frame, at most
    struct V3PumpSession
    {
        bool off = false;        // a call raised or a proof came back empty: bursts only, for good
        uint32_t no_gen_logs = 0;
        size_t budget = V3_PUMP_BUDGET_START; // adapted per pump frame, kept across opens
    };
    V3PumpSession g_v3_pump; // map thread only
    // Per thread, so a pulse on any other thread is never counted (or swallowed) as ours.
    struct V3PumpPulses
    {
        bool active = false;   // the proof's GotoFrame is on the stack: pulses counted, not built
        uintptr_t expect = 0;  // the generator; a pulse whose ctx+0x58 names it is ours
        uint32_t pulses = 0, own = 0, other = 0;
        bool pumping = false;  // a pump stroke is on the stack: pulses BUILD, and are counted
        uint32_t pumped = 0;
    };
    thread_local V3PumpPulses t_v3_pump;

    // Lever C self-detach primitives (the engine's own remove-from-container path,
    // as used by the high-level reparent FUN_1410c8440):
    //   find-index  FUN_14113f8d0(childVecHeader=owner+0xd8, child) -> index or -1
    //   remove-at   FUN_1410c87c0(wrapper, index) -> erase child at index from
    //               *(wrapper+0x18) (=owner); unlinks its render node, clears
    //               child+0x38 (parent), marks the container dirty and drops the
    //               container's reference (freeing the child if it was the last).
    using V3RemoveAtFn = void(void *, uint32_t);
    V3RemoveAtFn *g_v3_remove_at = nullptr;

    // Scaleform GFx DrawingContext primitives (the C++ backing of AS Graphics),
    // used by the dev-only solid-fill spike. A SOLID color fill needs NO GPU
    // texture (unlike beginBitmapFill, which hit a texture wall), so drawing one
    // filled rectangle proves whether route (c) - a native settings panel built
    // from our own display objects (backgrounds, progress bars, checkbox frames) -
    // is viable. ctx is fetched from a MovieClip child via its vtbl+0x2a0 getter.
    //   begin      FUN_14119cb50(ctx, char clear)   - open/clear the drawing
    //   beginFill  FUN_14119cc20(ctx, u32 argb)     - SOLID color 0xAARRGGBB, no texture
    //   moveTo     FUN_14119d690(ctx, int, int)     - twips
    //   lineTo     FUN_14119d7a0(ctx, int, int)     - twips
    //   endFill    FUN_14119d650(ctx)
    //   shapeReset FUN_14119d0c0(ctx)               - clear accumulator (begin returns 0 on a fresh ctx)
    // (the six matching function pointers were declared here and resolved at startup; no call
    // site ever existed, so both the pointers and their scans are gone - the signatures above
    // are kept as the record of what these entry points are)

    // Leaf 1: scan the parent's logical child vector (owner+0xd8: base@[0],
    // count@+0xe0, entry stride 0x10 with the child ptr at +0) and record the
    // indices whose child ptr is in the pre-sorted `sorted[0..n)` set (our
    // markers). Pure raw memory + POD only, so SEH is legal here (no C++ unwind).
    // Returns the count of ascending indices written to out_idx (capped). `out_faulted` (POD, so SEH stays
    // legal) is set to 1 if the scan took an AV; the caller must then remove NOTHING - see below.
    uint32_t v3_detach_scan(uintptr_t parent, const uintptr_t *sorted, uint32_t n,
                            uint32_t *out_idx, uint32_t out_cap, uint32_t *out_faulted)
    {
        uint32_t found = 0;
        if (out_faulted)
            *out_faulted = 0;
        __try
        {
            uintptr_t base = *reinterpret_cast<uintptr_t *>(parent + 0xd8);
            uint32_t count = *reinterpret_cast<uint32_t *>(parent + 0xe0);
            if (!base || !count || count > 300000)
                return 0;
            for (uint32_t i = 0; i < count && found < out_cap; ++i)
            {
                uintptr_t child =
                    *reinterpret_cast<uintptr_t *>(base + static_cast<uint64_t>(i) * 0x10);
                if (!child)
                    continue;
                uint32_t lo = 0, hi = n; // binary search in sorted[]
                while (lo < hi)
                {
                    uint32_t mid = lo + ((hi - lo) >> 1);
                    if (sorted[mid] < child)
                        lo = mid + 1;
                    else
                        hi = mid;
                }
                if (lo < n && sorted[lo] == child)
                    out_idx[found++] = i;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // Do NOT hand back the partial list. An AV here means the child vector we were walking
            // changed shape under us (freed/reallocated), so the indices collected before the fault
            // describe a layout that no longer exists - and remove-at drops the container's reference,
            // freeing whatever child now sits at that index. That frees an engine child we never owned:
            // heap corruption, and by then nothing points back here. Removing nothing is always safe -
            // our children simply stay attached and the next pass retries against a settled vector.
            found = 0;
            if (out_faulted)
                *out_faulted = 1;
        }
        return found;
    }

    // Leaf 2: remove the collected indices via the engine's remove-at primitive,
    // DESCENDING (out_idx is ascending) so each surviving index stays valid and the
    // per-remove tail memmove shrinks. Removing drops the container's reference,
    // freeing the child. SEH-guarded. Returns the count removed.
    // Every index is RE-VERIFIED against the live vector immediately before its removal: read the child
    // pointer currently at that index and remove only if it is still one of ours (`sorted[0..n)`).
    //
    // Why: the scan above and this removal are two separate walks of a vector the engine owns and can
    // reshape in between - a marker that is added, removed or reordered between the two turns an index
    // into a pointer at somebody else's child, and remove-at drops the container's reference, freeing an
    // engine object we never owned. That needs no fault to happen, so the AV guard cannot catch it. It
    // runs every map frame while the viewport window is on, which matches map TILES going transparent
    // and coming back during scroll and zoom. `out_skipped` counts indices that no longer matched.
    uint32_t v3_detach_remove(uintptr_t wrapper, uintptr_t parent, const uint32_t *idx, uint32_t n,
                              const uintptr_t *sorted, uint32_t sorted_n, uint32_t *out_skipped)
    {
        uint32_t removed = 0;
        uint32_t skipped = 0;
        __try
        {
            for (uint32_t k = n; k-- > 0;)
            {
                const uintptr_t base = *reinterpret_cast<uintptr_t *>(parent + 0xd8);
                const uint32_t count = *reinterpret_cast<uint32_t *>(parent + 0xe0);
                if (!base || idx[k] >= count)
                {
                    ++skipped;
                    continue;
                }
                const uintptr_t child =
                    *reinterpret_cast<uintptr_t *>(base + static_cast<uint64_t>(idx[k]) * 0x10);
                uint32_t lo = 0, hi = sorted_n; // binary search in sorted[]
                while (lo < hi)
                {
                    const uint32_t mid = lo + ((hi - lo) >> 1);
                    if (sorted[mid] < child)
                        lo = mid + 1;
                    else
                        hi = mid;
                }
                if (lo >= sorted_n || sorted[lo] != child)
                {
                    ++skipped; // no longer ours - the vector moved since the scan
                    continue;
                }
                g_v3_remove_at(reinterpret_cast<void *>(wrapper), idx[k]);
                ++removed;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        if (out_skipped)
            *out_skipped = skipped;
        return removed;
    }

    uint32_t v3_guarded_materialize(void *ctx, uintptr_t sprite)
    {
        __try
        {
            g_v3_mat_driver(ctx, reinterpret_cast<void *>(sprite));
            return 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return static_cast<uint32_t>(GetExceptionCode());
        }
    }

    struct V3NativeObject
    {
        // (row_id and source_icon_id were members here, assigned at creation and read by nothing -
        //  lookups go through g_v3_native.by_row, and the icon is baked into the child's character.
        //  12 bytes x ~9500 objects.)
        uintptr_t child = 0;
        float map_x = 0.0f;
        float map_z = 0.0f;
        float base_tx = 0.0f;
        float base_ty = 0.0f;
        float base_m[4] = {}; // authored 2x2 basis {m0,m1,m4,m5} captured at staging
        // The MAP this marker belongs to (WorldMapPointParam areaNo/gridXNo/gridZNo),
        // kept per object so the location emphasis can be re-decided whenever the
        // player moves, with no rebuild. Rings carry the coords of whatever marker
        // they are currently sitting on, so they follow its emphasis for free.
        uint8_t area = 0;
        uint16_t gx = 0;
        uint16_t gz = 0;
        float emph = 1.0f;    // location-emphasis size factor (1.0 = untouched)
        // A focus ring, not a marker. Rings take the location emphasis's SIZE (they must stay on
        // top of the icon they circle, whatever size that icon is) but never its COLOUR: a ring is
        // a "look here" mark, and dimming it to match a marker on another map is the one thing it
        // must not do. Until 2026-08-03 they inherited both, because a ring rides its host's
        // coordinates and the emphasis is decided from coordinates alone.
        bool is_ring = false;
        // Location-emphasis colour, as last WRITTEN (1.0 / 0.0 = untouched). Memoised so a
        // re-decide only pays for a colour write when the answer actually moved - that
        // write is a copy-on-write inside the engine, not a store.
        float fade = 1.0f;
        float cool = 0.0f;
        bool visible = false;
        bool attached = true; // lever B: currently linked into the marker parent (has a
                              // render node). Always true unless the viewport-window variant.
        // Has the reconcile ever ATTACHED this child successfully? Splits the two reasons a
        // marker can be missing from the display list, which need opposite fixes: never reached
        // (the per-pass budget has not got to it yet) versus attached and lost again (the engine
        // is dropping it back out, and re-attaching forever is pure waste). Convergence
        // 2026-08-05 showed 4098 stragglers holding EXACTLY constant while every pass spent its
        // full 1024-attach budget - that can only be the second case, and this flag proves it.
        bool ever_attached = false;
        bool ref_held = false; // lever B: we kept an extra reference at build so this
                               // child survives being detached. Only children built with
                               // the viewport window ON have it -> only they may be
                               // detached/re-attached (guards a mid-session flag flip).
    };
    struct V3NativeManager
    {
        int layer = -1;
        uintptr_t wrapper = 0;
        uintptr_t parent = 0;
        uint64_t observed_count = 0;
        uint32_t stable_frames = 0;
        bool seeded = false;
        uint64_t build_started_ms = 0;
        uint64_t next_refresh_ms = 0;
        size_t pending_index = 0;
        size_t frame_budget = 0;
        uint32_t failed = 0;
        // ...and WHY it failed. One number could not tell the two causes apart, and they call for
        // opposite fixes: a projection that is not ready yet is a TIMING problem (the row is fine,
        // we asked too early), a missing charId is an INJECTION problem (our frames are not in
        // this movie yet). Split 2026-08-05, when opens started reporting failed=5035 and 6091
        // against failed=1 on a healthy one.
        uint32_t failed_project = 0;  // mapproject::to_map said no
        // ...split by worldmap_probe::ProjFail: `stale` / `no_view` = the game's converter was
        // never asked (its last own call is older than the 300 ms gate), `declined` = it was
        // asked and refused the tile. A slow first open that lost every underground and DLC tile
        // (2026-09-22 22:57, project=2027) could not say which of the two it was.
        uint32_t failed_project_stale = 0;
        uint32_t failed_project_declined = 0;
        uint32_t failed_project_noview = 0;
        uint32_t failed_project_faulted = 0;  // the call raised and was caught (report-19 class)
        // Placed from a tile origin learned earlier (v3_prelearn_tiles or a previous open) while
        // the converter could not be asked. Without it a slow open that the prelearn rescued
        // reads exactly like a fast one - this is the number that shows it engaged.
        uint32_t placed_from_origin = 0;
        // ...and WHERE: per tile (area<<16 | gx<<8 | gz), so a profile whose count is not the
        // known one can be read off the log instead of guessed at from three capped warnings.
        // Reported once per open at CATEGORIES READY, only when the set differs from the
        // last one printed.
        std::map<uint32_t, uint32_t> failed_project_tiles;
        uint32_t failed_charid = 0;   // no injected character for this icon
        // The three ways a request that got PAST those two can still come back empty. They all
        // fed one `failed` and their logs are capped at four lines each, so a build that lost
        // 6198 rows looked identical whichever one it was. Split 2026-08-05 for the same reason
        // the project/charId split was made - and that split is what proved those two innocent
        // (`failed=6198 (project=1 charId=0)`), which is how this layer got found at all.
        uint32_t failed_nonode = 0;    // the Execute produced no timeline record
        uint32_t failed_material = 0;  // the record never materialized into a display child
        uint32_t failed_attach = 0;    // attach into the marker parent, or positioning, failed
        uint32_t wrong_contexts = 0;
        uint64_t settle_count = 0;
        uint32_t settle_frames = 0;
        bool completion_reported = false;
        bool in_factory = false;
        // [v3pump] this generation's hidden sprite-171 instance (a child of `parent`, never in
        // `objects`, dies with the movie) and one creation attempt. `pump_capped` = the burst
        // stopped at the rows visible at seed and the pump owns the rest of the queue.
        uintptr_t pump_gen = 0;
        bool pump_tried = false;
        bool pump_capped = false;
        // Pending index where the burst's share ends: the rows visible at seed (they sort first)
        // plus hidden rows on tiles whose origin the prelearn could not learn (moved right behind
        // them: only the burst has the engine converter fresh).
        size_t burst_end = 0;
        uint32_t pump_dry = 0;         // pump frames in a row that got no pulse
        uint32_t pump_stuck = 0;       // pump frames in a row with pulses but no queue progress
        bool pump_merged = false;      // this tick already ran a merge: the pump sits it out
        uint64_t last_progress_ms = 0; // last creation (or seed); queue watchdog
        uint16_t next_depth = 24;      // timeline depth allocator (never reused)
        // Focus rings live ABOVE every marker. Depth is what decides who draws over whom, and
        // with one allocator the rings took whatever number their turn in the queue gave them -
        // which put them under the icons they are meant to point at. The band starts far past
        // any marker count we can reach (about 9.5k children today) and still fits a u16.
        uint16_t next_ring_depth = 50000;
        // Counter-zoom state: the marker layer's parent transform scales with the
        // map and nothing in the engine touches our raw children (they are not
        // widgets). The manager copies the live 2x2 the engine writes into a REAL
        // WorldMapItem widget of the same parent - that is the exact native
        // adaptation curve (incl. clamps), no reference zoom needed.
        float cur_fx = 1.0f;      // last applied basis factor, x row
        float cur_fy = 1.0f;      // last applied basis factor, y row
        // (sample_fx / sample_fy / sample_zoom stood here - "the last successfully sampled widget
        //  scale" and the MapView.zoom at that sample, kept for a zoom-ratio fallback. One
        //  occurrence each, their own declaration. The live rule is not a ratio at all, it is the
        //  power law at the counter-zoom pass: V3_SIZE_TRIM * powf(V3_ZOOM_PIVOT / view.zoom,
        //  V3_ZOOM_EXP). last_dump_zoom and sample_logged below ARE live.)
        float last_dump_zoom = 0.0f; // last zoom the diagnostic probe logged at
        bool sample_logged = false;
        // The native node the widget sampler last read, revalidated against the parent's
        // display list each tick before use. Rediscovery (the slot scan with its per-node
        // membership test) runs only when this is 0 or the node left the list: report 27
        // measured the every-tick rediscovery at 480-540 us per map frame on that display
        // list - each foreign candidate pays a FULL pass over all ~8.6k objects.
        uint64_t sample_node = 0;
        // Location emphasis: the map the player stands in, resampled while the map is
        // open (fast travel and the map screen never coexist, but a reopen after a
        // move must not carry the old answer). 0 = unknown -> every marker plain.
        uint32_t player_map = 0;
        uint64_t emph_sig = 0;   // player map + the two scales + on/off, as one compare
        // Does the TAB being looked at contain the player's own location at all? Standing
        // in Siofra (an underground map) and looking at the surface tab, nothing on screen
        // is "here" - emphasising then would mute the entire tab and say nothing. Decided
        // per merge from the snapshot, which is the only place that knows every marker's
        // layer, and false until a snapshot says otherwise.
        bool emph_active = false;
        std::vector<goblin::NativeMarkerPoint> pending;
        std::vector<V3NativeObject> objects;
        std::unordered_map<uint64_t, size_t> by_row;
        std::unordered_set<uint64_t> queued;
        // ── dead-generation guards ───────────────────────────────────────────────────
        // The display-object vtable of THIS generation's children, captured lazily from the first
        // live one. A child our own reference kept alive across a map close, whose memory the
        // engine later freed and reused, passes every heap-range check but reads back a foreign
        // vtable here - so we never write a matrix into whatever now owns that block.
        uint64_t child_vtable = 0;
        bool child_vtable_logged = false;
        uint32_t cx_attempts = 0;  // frames spent waiting for a child's render entry
        bool emph_dirty = false;   // a re-decide is owed; spent a slice per frame
        size_t emph_cursor = 0;
        // The parent (WorldMapItem container) vtable, captured lazily once the parent is genuinely
        // alive. Unlike child_vtable (shared by thousands of marker sprites) there is exactly one
        // such container, so a match is strong proof the cached parent pointer is still THIS live
        // parent. A freed parent reads 0 here, which is the reliable death signal.
        uint64_t parent_vtable = 0;
        // The MOVIE the parent belonged to when we seeded on it. The vtable above is a good death
        // signal for a FREED block (it reads 0) but not for a freed and REUSED one: the allocator
        // hands the block to another display object, that object's own non-zero vtable lands in the
        // slot, and if it happens to be the same class the check passes outright. Movie identity
        // ([node+0x20]->+0x10, see v3_movie_of) is written once in a node's ctor and never cleared,
        // so a reused block answers with the NEW owner's movie - or fails to resolve - and the
        // mismatch is caught. Captured with parent_vtable and compared before any per-frame engine
        // attach or detach. Four player minidumps (report 19, 2026-08-02) fault inside Scaleform's
        // display-list and pool code on the very thread that owns this manager, with a freed-memory
        // poison value in the object register; the vtable-only gate is what let those through.
        uintptr_t parent_movie = 0;
    };
    V3NativeManager g_v3_native;

    // WHY the queue stalled. The watchdog below could only say "no factory pulses", which does not
    // separate "the engine never executed one of our sprite-171 RM2 tags" from "pulses arrived and
    // every one of them bounced off an early return in consume". Both look identical in the log and
    // they need opposite fixes, and the DLC map hits this every open (created=0 here, ~half at the
    // reporter). These count the pulse path so the warning can name the gate.
    std::atomic<uint32_t> g_v3_pulse_seen{0};      // passed the gfx-side gate, entered the pulse
    std::atomic<uint32_t> g_v3_pulse_unseeded{0};  // seed_from_live_callback() refused
    std::atomic<uint32_t> g_v3_consume_nodriver{0};
    std::atomic<uint32_t> g_v3_consume_nobudget{0};
    std::atomic<uint32_t> g_v3_consume_nosprite{0};
    std::atomic<uint32_t> g_v3_consume_busy{0};    // !seeded or re-entered while in_factory
    std::atomic<uint32_t> g_v3_consume_hbwait{0};  // budget 0 but the map heartbeat is fresh:
                                                   // bounced to wait for the tick refill

    void v3_pulse_counters_reset()
    {
        g_v3_pulse_seen.store(0, std::memory_order_relaxed);
        g_v3_pulse_unseeded.store(0, std::memory_order_relaxed);
        g_v3_consume_nodriver.store(0, std::memory_order_relaxed);
        g_v3_consume_nobudget.store(0, std::memory_order_relaxed);
        g_v3_consume_nosprite.store(0, std::memory_order_relaxed);
        g_v3_consume_busy.store(0, std::memory_order_relaxed);
        g_v3_consume_hbwait.store(0, std::memory_order_relaxed);
    }
    // g_v3_custom_child, g_v3_visual_state, V3VisualSnapshot and its publish flag lived here.

    // A V3ScaleState (the transplanted child's reference zoom, authored basis and pivot) and its
    // ready flag lived here, for the counter-scale pass in on_map_frame. The flag was never set
    // true and the state never written; both went with that pass on 2026-07-30.

    // Plain SEH ON PURPOSE, not goblin_safemem - these four are the hot-walk primitives.
    // The per-frame marker walk plus the factory's node search push them ~20k times per
    // frame, and routing that through the validated path was measured (v3perf, 2026-08-05)
    // at 0.6-1M copies and 250-470 ms of every second on the map thread - avg 6-16 ms of
    // our overhead per frame, factory pulses at 60-75 ms each, the reopen build starved.
    // The memory they touch is generation-gated and faults only exceptionally: a full
    // 7136-marker build under these SEH readers raised ZERO first-chance AVs in the
    // ERSS-FG log. Validate-then-read stays for the probes where a dead pointer is the
    // routine case (goblin_safemem.hpp has the list); a hot path with liveness gates is
    // exactly what its header's "NOT for" note is about.
    bool v3_read64(uintptr_t addr, uint64_t &out)
    {
        __try
        {
            out = *reinterpret_cast<const uint64_t *>(addr);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            out = 0;
            return false;
        }
    }

    bool v3_read32(uintptr_t addr, uint32_t &out)
    {
        __try
        {
            out = *reinterpret_cast<const uint32_t *>(addr);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            out = 0;
            return false;
        }
    }

    // Raw block read/write. SEH leaves, no C++ temporaries (MSVC rejects __try in a
    // function that owns objects with destructors). Used by the colour lever, which
    // writes into the same render-node data the matrix already goes into.
    bool v3_read_bytes(uintptr_t addr, void *out, size_t n)
    {
        __try
        {
            memcpy(out, reinterpret_cast<const void *>(addr), n);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool v3_write_bytes(uintptr_t addr, const void *src, size_t n)
    {
        __try
        {
            memcpy(reinterpret_cast<void *>(addr), src, n);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool v3_heap_ptr(uint64_t p)
    {
        return p >= 0x10000 && p < 0x7fffffffffffULL;
    }

    // v3_read8() stood here, unused - the SEH readers that ARE used are v3_read64 / v3_read32 /
    // v3_read_bytes.

    // v3_executable_ptr() (a VirtualQuery-based "does this point at executable memory" test)
    // stood here with no callers.

    bool v3_copy_ascii(uintptr_t addr, char *dst, size_t capacity)
    {
        if (!v3_heap_ptr(addr) || !dst || capacity < 2)
            return false;
        __try
        {
            size_t i = 0;
            for (; i + 1 < capacity; ++i)
            {
                const unsigned char c = *reinterpret_cast<const unsigned char *>(addr + i);
                if (c == 0)
                    break;
                if (c < 0x20 || c > 0x7e)
                    return false;
                dst[i] = static_cast<char>(c);
            }
            dst[i] = 0;
            return i != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            dst[0] = 0;
            return false;
        }
    }

    uint32_t v3_attach_movie_detour(void *a1, void *a2, void *a3, const char *export_name,
                                    void *a5, int requested_depth, void *init_object)
    {
        const V3AttachMovieContext saved = g_v3_movie_ctx;
        auto &ctx = g_v3_movie_ctx;
        ctx = {};
        ctx.active = goblin::variants::kNativeMarkers;
        ctx.requested_depth = requested_depth;
        if (ctx.active)
        {
            v3_copy_ascii(reinterpret_cast<uintptr_t>(export_name),
                          ctx.export_name, sizeof(ctx.export_name));
            uint64_t entries = 0;
            if (init_object)
            {
                v3_read64(reinterpret_cast<uintptr_t>(init_object), entries);
                v3_read64(reinterpret_cast<uintptr_t>(init_object) + 8, ctx.init_count);
            }
            if (v3_heap_ptr(entries) && ctx.init_count <= 256)
            {
                const uint32_t n = static_cast<uint32_t>((std::min<uint64_t>)(ctx.init_count, 4));
                for (uint32_t i = 0; i < n; ++i)
                {
                    const uintptr_t entry = static_cast<uintptr_t>(entries) + i * 0x38;
                    uint64_t tagged_key = 0, key_header = 0;
                    v3_read64(entry, tagged_key);
                    const uintptr_t key_node = static_cast<uintptr_t>(tagged_key & ~3ULL);
                    if (v3_heap_ptr(key_node) && v3_read64(key_node, key_header))
                    {
                        const uint64_t key_len = key_header & 0x7fffffffffffffffULL;
                        if (key_len > 0 && key_len < sizeof(ctx.key[i]))
                            v3_copy_ascii(key_node + 0xc, ctx.key[i], sizeof(ctx.key[i]));
                    }
                    v3_read64(entry + 8, ctx.value[i][0]);
                    v3_read64(entry + 16, ctx.value[i][1]);
                }
            }
        }
        const uint32_t result = o_v3_attach_movie(a1, a2, a3, export_name, a5,
                                                   requested_depth, init_object);
        g_v3_movie_ctx = saved;
        return result;
    }

    // Movie identity for any display node: the engine's own primitive redone in
    // guarded reads (DisplayObjectBase::FindMovieImpl, exe+0x117dd40):
    //   while (obj && !(flags(+0x6A) & 0x80)) obj = pParent(+0x38); // 0x80 = InteractiveObject
    //   return obj ? pASRoot(+0x20)->pMovieImpl(+0x10) : NULL;
    // pASRoot is written once in the node's ctor and never cleared on detach, so
    // this answers correctly for a DETACHED node - which the old 16-level +0x38
    // up-walk could not: the engine's removal primitive (FUN_1410c87c0) zeroes
    // pParent and sets depth to -1, so a detached node's up-walk terminates at
    // itself. That is exactly the "roots ...(lv 1)" in the 2.1.1 layer-2
    // rejection storm, and a tree deeper than the walk cap returned a mid-chain
    // node instead (a Linux player report rejected every child that way). Our
    // anchor candidates are containers, i.e. InteractiveObjects, so the flag
    // test passes on the first iteration and [node+0x20] alone identifies the
    // movie view. Pure guarded reads only; per the validate-before-call rule the
    // engine function itself is never called.
    // ── liveness gate for the two node validators ────────────────────────────────────
    // These two run on the ENGINE'S ATTACH PATH - once per attached child, i.e. thousands
    // of times per build burst - and they routinely probe a node that is already dead (the
    // previous anchor during a cold open). Two measured failures bracket the right design:
    //
    //   * bare __try inside them: 36 first-chance AVs per cold open, and ModEngine3's host
    //     symbolized every one through dbghelp on the faulting thread - that IS the 2-4 s
    //     cold-open freeze ([stallcap] caught the map thread in me3_mod_host -> dbghelp).
    //   * every read routed through safemem: no AVs, but 6k-20k VirtualQuery calls per
    //     second (a refusal is never cached, by design) - factory pulses went from 6.5 us
    //     to 14 ms and reopens starved again.
    //
    // So: ONE validated probe of the node's own header, then the cheap SEH readers for the
    // walk itself - and a memo of the dead verdict, because the burst keeps asking about
    // the SAME dead pointer. A live node costs a cache hit (a few compares); a dead one
    // costs one VirtualQuery for the whole burst, and raises nothing.
    //
    // The memo is per-thread (these all run on the map thread) and EXPIRES: the allocator
    // can hand that address to a live object, and a stale "dead" verdict would reject a
    // healthy anchor - the one failure mode that costs icons rather than time.
    constexpr uint64_t kDeadMemoMs = 1000;
    thread_local uintptr_t t_dead_node = 0;
    thread_local uint64_t t_dead_ms = 0;

    // The header window the two validators read: +0x2c depth, +0x38 parent, +0x68 flags.
    constexpr size_t kNodeHeaderBytes = 0x70;

    // `bytes` is how far into the object the caller is about to read: the display-list
    // fields the tick validates sit at +0xe0, well past a node header, and gating a 0x70
    // window would have left exactly those reads unguarded. The memo keys on the POINTER
    // only, so a refusal recorded for a wide read also short-circuits a narrower one for up
    // to kDeadMemoMs - harmless, because a live object has its whole header mapped and the
    // memo expires anyway.
    // ── the same gate, minus the syscall, for the PER-ATTACH callers ────────────────────
    // VirtualQuery is the wrong instrument on a path that runs thousands of times per build.
    // Measured 2026-08-05 with the walk fully gated: one burst issued 45898 lookups and spent
    // 2.42 SECONDS inside the pulses - [stallcap] caught the map thread in
    // KERNELBASE!VirtualQuery <- safemem::query_region <- range_ok, i.e. our own gate WAS the
    // freeze. The region cache does not save it: these display objects do not share a region,
    // so nearly every probe missed.
    //
    // What the gate is actually for is ONE rare pointer - a dead anchor - probed over and over.
    // So: consult the memo (pure compares, no syscall); if it says nothing, just do the SEH read
    // and record the pointer if it faults. That costs ONE first-chance exception per dead
    // pointer per kDeadMemoMs instead of the 36-47 per open that the bare __try produced, and
    // zero syscalls. The VirtualQuery-backed v3_node_unusable stays for the per-FRAME callers
    // (tick, viewport reconcile), where two lookups a frame are free.
    bool v3_memo_dead(uintptr_t object)
    {
        if (!v3_heap_ptr(object))
            return true;
        return t_dead_node == object && GetTickCount64() - t_dead_ms <= kDeadMemoMs;
    }

    void v3_note_dead(uintptr_t object)
    {
        t_dead_node = object;
        t_dead_ms = GetTickCount64();
    }

    bool v3_node_unusable(uintptr_t object, size_t bytes = kNodeHeaderBytes)
    {
        if (!v3_heap_ptr(object))
            return true;
        const uint64_t now = GetTickCount64();
        if (t_dead_node == object && now - t_dead_ms <= kDeadMemoMs)
            return true;
        if (!goblin::safemem::readable(reinterpret_cast<const void *>(object), bytes))
        {
            t_dead_node = object;
            t_dead_ms = now;
            return true;
        }
        return false;
    }

    // gated=true asks the OS before every hop (v3_node_unusable) instead of letting the SEH read
    // find out. ONLY for the previous anchor in v3_note_movie_attach, which after the
    // big_enough/screen_gone skip is reached a handful of times per session. NOT for the tick:
    // gating its two walks (the parent_movie capture and the unseeded dead-anchor test) cost
    // 24-59 ms single-frame spikes on map open (2026-09-23, VirtualQuery ~4 ms a call on this
    // box, and each hop's region pushed the anchor's own out of safemem's 8-slot cache, so the
    // tick's entry gate missed too). The per-child attach callers keep the memo-only form above
    // (the 45898-lookup freeze).
    uintptr_t v3_movie_of(uintptr_t object, bool gated = false)
    {
        uintptr_t current = object;
        for (uint32_t level = 0;; ++level)
        {
            if (level >= 16 || (gated ? v3_node_unusable(current) : v3_memo_dead(current)))
                return 0;
            // The u16 flags live at +0x6A; through the aligned u32 at +0x68
            // their 0x80 bit reads as 0x00800000.
            uint32_t packed = 0;
            if (!v3_read32(current + 0x68, packed))
            {
                v3_note_dead(current); // one fault per dead pointer, then the memo answers
                return 0;
            }
            if (packed & 0x00800000u)
                break;
            uint64_t next = 0;
            if (!v3_read64(current + 0x38, next))
            {
                v3_note_dead(current);
                return 0;
            }
            current = static_cast<uintptr_t>(next);
        }
        // The ROOT object this node points at needs the same gate as the node itself, and for
        // one build it did not have it: `v3_heap_ptr(as_root)` is a range test, not a liveness
        // test, so a live node whose root had already been freed faulted here on every attach.
        // Measured 2026-08-05 (ERSS-FG named the exact frame: v3_read64 <- v3_movie_of+0xba <-
        // v3_note_movie_attach <- the attach detour): 47 AVs across one cold open, ~77 ms apart
        // because that is what ModEngine3's host spends symbolizing each one - the 2 s freeze.
        uint64_t as_root = 0, movie = 0;
        if (!v3_read64(current + 0x20, as_root) ||
            (gated ? v3_node_unusable(static_cast<uintptr_t>(as_root), 0x18)
                   : v3_memo_dead(static_cast<uintptr_t>(as_root))))
            return 0;
        if (!v3_read64(static_cast<uintptr_t>(as_root) + 0x10, movie))
        {
            v3_note_dead(static_cast<uintptr_t>(as_root));
            return 0;
        }
        if (!v3_heap_ptr(movie))
            return 0;
        return static_cast<uintptr_t>(movie);
    }

    // Has the engine taken this node out of its display list? Removal
    // (FUN_1410c87c0) zeroes pParent(+0x38) and sets depth(+0x2C) to -1. Our
    // anchor candidates are always containers deep inside the dialog tree,
    // never the movie root, so a zero parent is itself disqualifying and the
    // depth is the cross-check. Unreadable memory reports detached too: a freed
    // block is certainly not a live anchor.
    bool v3_node_detached(uintptr_t object)
    {
        if (v3_memo_dead(object))
            return true;
        uint32_t depth = 0;
        uint64_t parent = 0;
        if (!v3_read32(object + 0x2c, depth) || !v3_read64(object + 0x38, parent))
        {
            v3_note_dead(object);
            return true;
        }
        return depth == 0xFFFFFFFFu || !v3_heap_ptr(parent);
    }

    std::atomic<uintptr_t> g_menuman{0};

    // ── the engine's own map phase ───────────────────────────────────────────────────────────────
    // CS::CSMenuManImp+0x90 is a 0x47-byte array of per-menu-window lifecycle bytes indexed by the
    // window's menu id; the world map's is 0x3D. Established by disassembling MenuWindow::update
    // (exe+0x745570): `cmp ax,0x47` bounds the id, `movzx eax,[rdx+rcx+0x90]` reads the byte at
    // exe+0x745708, `or [rdx+rcx+0x90],7` sets it at exe+0x74571A, and the close path writes 0 at
    // exe+0x7ADB83 - two engine steps BEFORE the destructor hook we currently rely on.
    //
    // Why this replaces g_v3_map_closed: that flag was SET by the destructor but CLEARED by our own
    // retargets, so it read "open" throughout the window in which every 2026-08-03 fault happened.
    // A retarget is something WE do; it is not evidence the map opened. This byte has exactly one
    // writer - the engine.
    //
    // Scope, honestly: this is hygiene plus the epoch FIX 2 needs. It does NOT fix the crashes.
    // Measured: all four fault groups had WorldMapDialog::update on the stack, so the map was open
    // and this byte read 3 or 7 in every one of them. Gate B is the crash fix.
    // Last visibility epoch this manager has applied (goblin::visibility_epoch). Map thread only.
    uint32_t g_v3_seen_vis_epoch = 0;

    // ── timing primitives + PER-OPEN cost ────────────────────────────────────────────────────
    // Declared here, well above every user: the 1-second v3perf windows further down do not line
    // up with map opens, so they cannot answer "does each open cost more than the last" - and that
    // is exactly the question the Convergence report raised (2026-08-05: "the more opens, the
    // worse the freeze"). This pair is reset on the open edge and reported at CATEGORIES READY.
    int64_t v3_perf_now()
    {
        LARGE_INTEGER li;
        QueryPerformanceCounter(&li);
        return li.QuadPart;
    }

    int64_t v3_perf_freq()
    {
        static const int64_t f = [] {
            LARGE_INTEGER li;
            QueryPerformanceFrequency(&li);
            return li.QuadPart ? li.QuadPart : 1;
        }();
        return f;
    }

    // Map-frame heartbeat: stamped by on_map_frame, read by the stall sampler AND by the
    // factory's budget gate (which must know whether frames are actually arriving before it
    // grants itself work outside one).
    std::atomic<uint64_t> g_map_hb_ms{0};
    std::atomic<uint32_t> g_map_hb_tid{0};

    struct V3OpenCost
    {
        int64_t pulse_qpc = 0;
        int64_t frame_qpc = 0;
        uint32_t frames = 0;
        uint32_t pulses = 0;
        // The four stages inside a pulse, so a rising total can be attributed instead of guessed.
        // Convergence, 2026-08-05: the factory went from 55-67 ms on the first opens to 112-169 ms
        // by the thirtieth, with the pulse count identical (30789) - so the per-pulse cost itself
        // grew ~2.5x. One number cannot say which stage grew; these four can.
        // Where the PER-FRAME time goes while the map just sits open. Idle cost is ~32 ms/s at
        // 8424 objects, against ~11 ms/s before this session's work, and the frame does exactly
        // three things - so split them rather than guess which one grew.
        int64_t tick_qpc = 0;    // v3_native_tick (emphasis pass, merges, liveness)
        int64_t recon_qpc = 0;   // v3_viewport_reconcile (two full walks of every object)
        int64_t proj_qpc = 0;    // mapproject::to_map
        int64_t create_qpc = 0;  // create_native_icon_instance (tag Execute + node search)
        int64_t mat_qpc = 0;     // the engine's record-materialization driver
        int64_t attach_qpc = 0;  // attach into the parent + the transform write
        // [v3pump] The pulse fields above are the BURST only; the pump's own work is here, so
        // "factory" stays the number that decides the open frame.
        int64_t gen_qpc = 0;     // generator creation + the nested proof, inside the burst
        uint32_t proof_pulses = 0;
        bool capped = false;     // the proof passed: the burst was capped and the pump ran
        size_t burst_cap = 0;    // rows the burst was allowed (visible at seed)
        size_t burst_built = 0;  // objects when the pump took over
        int64_t pump_qpc = 0, pump_max_qpc = 0;
        uint32_t pump_frames = 0, pump_calls = 0, pump_pulses = 0;
        size_t pump_built = 0;
        // The generator's own child count, worst seen after a pump frame. 1 is its frame's icon;
        // more would be markers left inside it instead of moved to the marker parent.
        uint32_t gen_kids_max = 0;
        size_t pump_dropped = 0; // rows a pump stop left unbuilt this open (dropped from the queue)
    };
    V3OpenCost g_v3_open;

    // The 1-second window. Declared here beside V3OpenCost for the same reason: the tick writes
    // to it and the tick is defined well above the frame code that reports it.
    struct V3Perf
    {
        uint64_t window_ms = 0;
        uint32_t frames = 0;
        int64_t total_qpc = 0;
        int64_t max_qpc = 0;
        uint32_t pulses = 0;
        int64_t pulse_qpc = 0;
        int64_t tick_qpc = 0;
        int64_t recon_qpc = 0;
        // The periodic snapshot+merge inside the tick: it rebuilds the whole marker snapshot
        // (all ~8425 rows, plus the de-overlap pass) and merges it five times a second whether
        // or not anything changed. Measured on its own because the tick is 27 of the 29 ms/s an
        // idle map costs, and this is the only thing in it that touches every row.
        int64_t merge_qpc = 0;
        uint32_t merges = 0;
        // The other two things inside the tick that touch children in bulk, split out because
        // heavy tick windows survived the merge fix (Convergence: tick 118 ms/s against 1 merge
        // = 5.8 ms) and the remainder was unattributed. The emphasis slice writes up to 192
        // colour/matrix pairs a frame while dirty; the counter-zoom pass reapplies EVERY object
        // in one frame whenever the zoom moves past its 0.3% deadband.
        int64_t emph_qpc = 0;
        uint32_t emph_writes = 0;
        int64_t zoom_qpc = 0;
        uint32_t zoom_passes = 0;
        // The tick SPLIT INTO ITS FOUR PARTS, because the three counters above never added up:
        // measured on ERR 2026-08-28, an idle map cost 26 ms of tick per second with ONE merge
        // worth 2.4 ms, emphasis 0 and zoom 0 - and a single frame in that second took 24 ms,
        // which is a visible hitch at 60 fps. 21 of those milliseconds had no counter at all,
        // so the next round would have been another guess. These four partition the whole
        // function; emph/merge/zoom above stay as sub-measures inside them.
        int64_t head_qpc = 0;   // entry -> the emphasis section (drain, retarget, liveness)
        // head came back as 8-10 ms/s of an 11-20 ms tick while holding about ten machine
        // operations, so it is split again rather than argued about: the liveness gate (two
        // safemem range checks + three guarded reads) against everything after it. head_max is
        // per-FRAME, because "8 ms spread over 58 frames" and "8 ms in one frame" are different
        // defects and a per-second total cannot tell them apart.
        int64_t head_gate_qpc = 0;
        int64_t head_rest_qpc = 0;
        int64_t head_max_qpc = 0;
        int64_t emphsec_qpc = 0;// the emphasis section -> the refresh gate (includes emph_qpc)
        int64_t mergesec_qpc = 0;// the refresh gate itself (includes merge_qpc)
        int64_t tail_qpc = 0;   // everything after it (includes zoom_qpc)
        uint64_t copies0 = 0, queries0 = 0, refused0 = 0, vq_qpc0 = 0;
        // One-page probe answers (process-wide), and THIS thread's VirtualQuery count and ticks.
        // The snapshot and the line both run on the map thread, so the second pair is the map
        // frame's own share - the process-wide lookups above mix in the watcher and loader threads.
        uint64_t pages0 = 0, tq0 = 0, tvq0 = 0, tpq0 = 0;
    };
    V3Perf g_v3_perf;

    std::atomic<uint8_t> g_map_phase{0xFF};   // last sampled raw byte: 0 / 1 / 3 / 7, 0xFF = unknown
    std::atomic<uint16_t> g_map_menu_id{0x3D};
    std::atomic<bool> g_map_menu_id_resolved{false};
    // LATCH: "a real map screen has come and gone since we last anchored". Set only by the sampler
    // when the byte reads 0; cleared only when an anchor is taken. A retarget can never set it,
    // which is the whole structural point.
    std::atomic<bool> g_map_screen_gone{true};

    uint8_t v3_map_phase_read(uintptr_t mm, uint16_t id)
    {
        __try
        {
            return *reinterpret_cast<const volatile uint8_t *>(mm + 0x90 + id);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0xFF;
        }
    }

    uint8_t v3_map_phase_sample()
    {
        const uintptr_t mm = g_menuman.load(std::memory_order_acquire);
        if (!v3_heap_ptr(mm)) return 0xFF;
        const uint16_t id = g_map_menu_id.load(std::memory_order_relaxed);
        if (id >= 0x47) return 0xFF;
        const uint8_t raw = v3_map_phase_read(mm, id);
        // Self-validation instead of trusting the offset blindly: the engine only ever stores
        // 0, 1, 3 or 7 here. Anything else means this is not the field we think it is, and an
        // unrecognised signal must never be allowed to gate anything off.
        if (raw != 0 && raw != 1 && raw != 3 && raw != 7) return 0xFF;
        return raw;
    }

    // "a WorldMapDialog object exists at all". 0xFF (unknown) counts as alive on purpose: an
    // unresolved signal must never be able to switch the mod off.
    bool v3_map_object_alive()
    {
        return g_map_phase.load(std::memory_order_acquire) != 0;
    }
    // Stricter: the dialog is enabled and not closing.
    bool v3_map_drivable()
    {
        const uint8_t p = g_map_phase.load(std::memory_order_acquire);
        return p == 0xFF || (p & 2) != 0;
    }

    void v3_note_movie_attach(uintptr_t wrapper, uintptr_t parent, uint64_t count)
    {
        const auto &ctx = g_v3_movie_ctx;
        if (!ctx.active)
            return;
        // Never take an anchor while no dialog object exists. Counting on the same parent is still
        // fine and keeps prev_count honest, so this gates the take, not the observation.
        const bool phase_alive = v3_map_object_alive();
        const int live_layer = goblin::maphover::map_layer();
        const uintptr_t prev_parent = g_v3_target_parent.load(std::memory_order_relaxed);
        const int prev_layer = g_v3_target_layer.load(std::memory_order_relaxed);
        const uint64_t prev_count = g_v3_target_count.load(std::memory_order_relaxed);
        if (strcmp(ctx.export_name, "WorldMapItem") == 0)
        {
            if (parent == prev_parent)
            {
                // Same list growing: track its count. Stamp the layer LATE -
                // map_layer() is still -1 while the build burst runs, and a
                // -1-stamped target let a count=2 side clip steal the anchor
                // the moment the dialog resolved (13:39 retarget storm).
                if (count > prev_count)
                    g_v3_target_count.store(count, std::memory_order_release);
                if (prev_layer < 0 && live_layer >= 0)
                    g_v3_target_layer.store(live_layer, std::memory_order_relaxed);
                // Quick reopen that reuses the SAME movie: a burst arriving on this parent while
                // map_closed is set proves the parent is alive and the game is populating it right
                // now, which a teardown never does (it removes, it does not burst). So the gate can
                // be cleared and v3_viewport_reconcile may re-attach the children we kept. This is
                // sound where the old vtable heuristic was not: the signal is live game activity on
                // THIS parent, not a pointer that might have been recycled.
                // (A burst on this parent used to CLEAR g_v3_map_closed here. That is the defect:
                //  a retarget, or a burst we happen to observe, is not evidence that the map opened.
                //  The gate now has exactly one writer per direction, the engine's phase byte.)
            }
            else
            {
                // The anchor condition, as a STATE TEST. No count floor, no delay, no retry.
                //
                // Measured across the two 2.1.2 sessions of 2026-08-03: every retarget logging
                // count=48 built all 7136 markers (18 of 18), and every retarget logging count=100
                // failed (0 of 4 - three produced created=0 with parentCount=105, one never reached
                // CATEGORIES READY). The parent was never wrong; the anchor was LATE. kPickerMinItems
                // = 100 on a list that tops out near 105 fires after the build burst has already
                // spent its pulses, which is why those opens report "pulses seen=0" and the player
                // sees an empty map on that open and a full one on the next.
                //
                // Arm C is the structural fix: it keys off the engine's own phase byte reading 0,
                // i.e. the dialog was destroyed, so it fires on the FIRST attach into the new screen
                // (count=1) - the earliest possible moment, with the whole pulse pool still ahead.
                const uintptr_t cand_movie = v3_movie_of(parent);
                const bool cand_live = cand_movie != 0 && !v3_node_detached(parent);
                const uintptr_t anchor_movie = g_v3_target_movie.load(std::memory_order_relaxed);
                const bool big_enough = count >= kSeedMinItemsValue;
                const bool screen_gone = g_map_screen_gone.load(std::memory_order_acquire);
                // The previous anchor is chased ONLY when that can decide arm B: B needs
                // big_enough, and with the screen gone arm C makes the same move without touching
                // the old movie - whose memory is exactly what a close releases. Chasing it there
                // cost one first-chance AV on about 1 in 13 reopens (14 of 169, ERR 09-16..09-22:
                // v3_read32/64 <- v3_movie_of(prev_parent) <- here). The skip is the fix; the gate
                // on what is left is a best effort only - safemem caches "readable" for 5 s and
                // the tick keeps the anchor's region warm, so a death with the screen still up
                // can still cost one fault before the memo answers (no worse than before).
                const bool anchor_dead =
                    prev_parent != 0 && big_enough && !screen_gone &&
                    (v3_node_unusable(prev_parent) || v3_node_detached(prev_parent) ||
                     v3_movie_of(prev_parent, /*gated=*/true) == 0);

                // The SIZE FLOOR on every arm that moves to a DIFFERENT container. Measured
                // 2026-08-03 the hard way: with no floor at all, arm B fired on the first attach of
                // each new burst and anchored onto a 2-child side clip, nine opens running. The
                // markers still built, but the counter-zoom samples a REAL WorldMapItem widget of
                // the anchor's parent, and a 2-child clip has none - so cur_fx/cur_fy froze at
                // whatever the open started with and every icon kept that scale.
                //
                // 48 is not a guess: it is kSeedMinItemsValue, and every retarget logged at
                // count=48 built all 7136 markers (18 of 18). The floor that had to GO was the
                // separate picker floor of 100, which fired only after the burst had spent its
                // pulses on lists topping out near 105 (0 of 4 built). Late was the disease; this
                // is not a return to it.
                //
                // Arm A keeps no floor (there is nothing to lose by anchoring early when we have no
                // anchor at all, and arm E can still upgrade), and arm E is an upgrade within one
                // movie, which is already ordered by count. (big_enough is computed above, with
                // anchor_dead, which depends on it.)

                const char *arm = nullptr;
                // Arm A takes the floor too. Without it the first anchor of a session landed on a
                // 2-child clip (measured: "arm=A-first count=2"), and since kSeedMinItems is 48 we
                // could not have built on it anyway - so waiting for 48 costs nothing and spares
                // that session's first open the frozen counter-zoom.
                if (prev_parent == 0 && big_enough)                  arm = "A-first";
                else if (anchor_dead && big_enough)                  arm = "B-anchor-dead";
                else if (screen_gone && big_enough)                  arm = "C-new-screen";
                else if (cand_movie != anchor_movie && big_enough)   arm = "D-new-movie";
                else if (g_v3_native.objects.empty() && count > prev_count) arm = "E-upgrade";

                // Why it cannot storm the way 2026-08-01 did: once objects is non-empty only B, C
                // or D can move the anchor. B is one-way per movie generation, C needs the dialog
                // to have been destroyed, D needs a genuinely different movie. That storm was six
                // moves off a HEALTHY anchor inside one live movie, which no arm here allows.
                bool take = arm != nullptr && cand_live && phase_alive;
                if (arm != nullptr && !cand_live)
                {
                    static uintptr_t s_refused_parent = 0;
                    if (parent != s_refused_parent)
                    {
                        s_refused_parent = parent;
                        spdlog::info("[v3movie] re-anchor refused: parent=0x{:X} count={} "
                                     "layer={} arm={} not in the live tree",
                                     parent, count, live_layer, arm);
                    }
                }
                if (take)
                {
                    // Re-anchor ONLY on: first target / monotonically-largest list while we have
                    // not committed yet / rebuild after a REAL teardown / an actual known layer
                    // switch / a live list replacing an anchor that is out of the live tree.
                    //
                    // The "largest list" arm is now gated on NOT being seeded. Measured 2026-08-01
                    // on the DLC map: we seeded on a parent of 81, built 3072+ markers in 18 ms,
                    // and then a list of 82 - one item larger - took the target, which forces a
                    // reseed, and a reseed calls v3_native_reset() and throws every built child
                    // away. The burst's pulses were spent by then, so the rebuild produced
                    // created=0 and the icons that had just appeared vanished. Discovery is what
                    // that arm is for; once we are building, only a real teardown, a layer switch
                    // or a dead anchor may move it.
                    spdlog::info("[v3movie] RETARGET arm={} parent 0x{:X} -> 0x{:X} count={} "
                                 "layer={} movie 0x{:X} -> 0x{:X} screenGone={}",
                                 arm, prev_parent, parent, count, live_layer, anchor_movie,
                                 cand_movie, screen_gone);
                    g_v3_target_movie.store(cand_movie, std::memory_order_relaxed);
                    // One-shot per REAL screen. Only the phase byte reading 0 can re-arm it, so a
                    // retarget can never hand itself permission to retarget again - the exact
                    // defect g_v3_map_closed had.
                    g_map_screen_gone.store(false, std::memory_order_release);
                    g_v3_target_wrapper.store(wrapper, std::memory_order_relaxed);
                    g_v3_target_parent.store(parent, std::memory_order_relaxed);
                    g_v3_target_layer.store(live_layer, std::memory_order_relaxed);
                    g_v3_target_count.store(count, std::memory_order_release);
                }
            }
        }
        if (!(count == 500 || count == 6400 || count == 6468 || count % 1000 == 0))
            return;
        spdlog::info("[v3movie] parent=0x{:X} wrapper=0x{:X} count={} export='{}' "
                     "requestedDepth={} initCount={}",
                     parent, wrapper, count,
                     ctx.export_name[0] ? ctx.export_name : "<unreadable>",
                     ctx.requested_depth, ctx.init_count);
        if (count == 6400 || count == 6468)
        {
            const uint32_t n = static_cast<uint32_t>((std::min<uint64_t>)(ctx.init_count, 4));
            for (uint32_t i = 0; i < n; ++i)
                spdlog::info("[v3movie]   init[{}] key='{}' raw=0x{:016X}/0x{:016X}",
                             i, ctx.key[i][0] ? ctx.key[i] : "?",
                             ctx.value[i][0], ctx.value[i][1]);
        }
    }

    // v3_parent_root() stood here: a 16-level +0x38 up-walk that returned the topmost
    // reachable node, used as the "same movie" test in the place detour and the factory
    // consume loop. It answered wrong in exactly the two cases that mattered - a DETACHED
    // node's walk terminates at itself (the removal primitive zeroes pParent), and a tree
    // deeper than 16 levels returned a mid-chain node - so both callers now compare
    // v3_movie_of() identities instead, and the walk went with them on 2026-08-02.

    bool v3_hold_ref(uintptr_t child, uint32_t &before, uint32_t &after)
    {
        // Scaleform::RefCountNTSImpl lives at DisplayObject+0 and stores its
        // non-atomic count at +8. Native move callers hold a Ptr<> across
        // FUN_1410c8440; without this extra reference, removing the old parent
        // can destroy the child before the new parent inserts it.
        __try
        {
            auto *refs = reinterpret_cast<uint32_t *>(child + 8);
            before = *refs;
            if (before == 0 || before >= 0x10000000)
                return false;
            after = ++*refs;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            before = after = 0;
            return false;
        }
    }

    bool v3_drop_held_ref(uintptr_t child, uint32_t &before, uint32_t &after)
    {
        __try
        {
            auto *refs = reinterpret_cast<uint32_t *>(child + 8);
            before = *refs;
            // A successful attach leaves one target-parent reference in
            // addition to ours. Never manufacture a zero-count object here;
            // retain the diagnostic reference if the contract was violated.
            if (before < 2 || before >= 0x10000000)
            {
                after = before;
                return false;
            }
            after = --*refs;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            before = after = 0;
            return false;
        }
    }

    // Counters for the acceptance test: non-zero here is Gate B earning its place, and the
    // breakdown says WHICH precondition the engine would have tripped over.
    // Why the seed did not run. Every gate in v3_native_seed_from_live_callback increments one of
    // these; they are printed together at CATEGORIES READY and at the queue-drop warning and reset
    // per generation. Three separate causes for "the map came up empty" were fixed one at a time on
    // 2026-08-03, each found only by adding a log after the fact, because a refusal on this path is
    // silent by construction: nothing seeds, so no watchdog fires either. One line, whole chain.
    std::atomic<uint32_t> g_seed_no_screen{0};   // the phase byte says no dialog exists
    std::atomic<uint32_t> g_seed_epoch{0};       // a screen has come and gone since we anchored
    std::atomic<uint32_t> g_seed_map_closed{0};  // the close flag is still up
    std::atomic<uint32_t> g_seed_anchor_bad{0};  // wrapper/parent do not read as a live pair
    std::atomic<uint32_t> g_seed_small{0};       // the anchor's list is under the seed floor
    std::atomic<uint32_t> g_seed_ok{0};          // a NEW seed actually started
    std::atomic<uint32_t> g_seed_managing{0};    // already seeded, kept managing (the common case)

    void v3_seed_trace(const char *when)
    {
        // The anchor itself, printed rather than inferred. A stale anchor shows up here as a
        // liveCount that never grows: healthy opens go 81 -> 988 or 1526 as the game populates the
        // container, a stale one sits at 81 while the game fills a different one entirely.
        const uintptr_t anchor = g_v3_target_parent.load(std::memory_order_relaxed);
        uint64_t anchor_count = 0;
        if (!v3_heap_ptr(anchor) || !v3_read64(anchor + 0xe0, anchor_count))
            anchor_count = UINT64_MAX;
        spdlog::info("[v3seed] {}: anchor=0x{:X} liveCount={} managing={} ok={} refused: "
                     "noScreen={} epoch={} mapClosed={} anchorBad={} small={}",
                     when, anchor,
                     anchor_count == UINT64_MAX ? -1 : static_cast<int64_t>(anchor_count),
                     g_seed_managing.exchange(0, std::memory_order_relaxed),
                     g_seed_ok.exchange(0, std::memory_order_relaxed),
                     g_seed_no_screen.exchange(0, std::memory_order_relaxed),
                     g_seed_epoch.exchange(0, std::memory_order_relaxed),
                     g_seed_map_closed.exchange(0, std::memory_order_relaxed),
                     g_seed_anchor_bad.exchange(0, std::memory_order_relaxed),
                     g_seed_small.exchange(0, std::memory_order_relaxed));
    }

    std::atomic<uint32_t> g_v3_reject_slot{0};   // null snapshot slot - the exe+0x11CC530 fault
    std::atomic<uint32_t> g_v3_reject_array{0};  // corrupt display-list Array header

    // GATE B1 - what SetMatrix will dereference, checked before we call it.
    //
    // Every fault in the 2026-08-03 runs was the engine dereferencing an allocator or lookup result
    // it never null-checks, on a child whose Scaleform snapshot state is gone or exhausted. The
    // chain SetMatrix enters:
    //     vt+0x18 -> exe+0x117F050 -> exe+0x1179520
    //     exe+0x1179520: if (*(this+0x60) == 0) { node = GetWritableNodeData(this) exe+0x117DE00;
    //                                             exe+0x11CC510(node, m) }
    //     exe+0x11CC510 -> exe+0x1157A70 -> exe+0x11CC530 stores through the snapshot slot
    //     exe+0x1157A70, when *(node+0x10)==0, appends a change record and needs a fresh 0x3F0
    //                    chunk from exe+0x115AB20, whose result is stored unchecked at exe+0x115AB88
    // so the two things that can be missing are the snapshot slot (the 0x11CC530 fault, write 0x10)
    // and the room for one more change record (the 0x115AB88 fault, write 0x0).
    //
    // The child_vtable guard in v3_position_child is necessary but NOT sufficient: in the
    // full-memory hang dump all 50,823 objects carrying that vtable had the right vtable and
    // refcount 1, and 8,946 of them had a null snapshot slot. A vtable says what a block IS, not
    // whether its render state still exists.
    //
    // Validate before calling, never catch after: the __try around SetMatrix stays as a net, but it
    // must stop being the mechanism.
    bool v3_node_context_ok(uintptr_t child)
    {
        uint64_t entry = 0;
        if (!v3_read64(child + 0x48, entry) || !v3_heap_ptr(entry)) return false;
        const uint64_t page = entry & ~0xFFFull;
        const uint64_t off = entry - page;
        if (off < 0x30 || ((off - 0x30) % 0x48) != 0) return false;
        uint64_t tbl = 0, snap = 0;
        if (!v3_read64(page + 0x20, tbl) || !v3_heap_ptr(tbl)) return false;
        if (!v3_read64(tbl + 0x28 + ((off - 0x30) / 0x48) * 8, snap) || !v3_heap_ptr(snap))
        {
            g_v3_reject_slot.fetch_add(1, std::memory_order_relaxed);
            return false;  // the exe+0x11CC530 fault
        }
        // The change-record chunk check that stood here is GONE, 2026-08-03, on its own numbers.
        // It refused whenever the owner's current 0x3F0 chunk was full, on the theory that
        // exe+0x1157A70 would then have to allocate a fresh one and might get NULL back
        // (the exe+0x115AB88 fault). But a full chunk is the NORMAL state - the engine allocates
        // the next one and almost always succeeds - so the check was a prediction of failure, not
        // a precondition for it. Measured over one session: slot=0, array=0, chunk=4559. It caught
        // nothing real and skipped 4,559 legitimate SetMatrix calls, which is why every icon it hit
        // kept the scale it had at map-open instead of counter-zooming.
        //
        // What stays is the snapshot-slot test above: that one IS a precondition (the engine stores
        // through the slot without checking it) and it has never yet rejected a live child.
        return true;
    }

    // GATE B2 - the parent container's display-list Array header, checked before the engine attach
    // (DisplayObjectContainer::InsertChildAtDepth, exe+0x10C8440) walks it. In the 13:00:46 crash
    // `data` read 0x7FF7DB74908D - an address inside eldenring.exe's .text - and `size` read
    // 0x00007FF7_00000003, a pointer-shaped value sitting in a count field. Both are caught here.
    bool v3_container_array_ok(uintptr_t cont)
    {
        uint64_t data = 0, size = 0, cap = 0;
        if (!v3_read64(cont + 0xD8, data) || !v3_read64(cont + 0xE0, size) ||
            !v3_read64(cont + 0xE8, cap))
            return false;
        if (size > cap || cap > 0x10000) return false;
        if ((cap == 0) != (data == 0)) return false;
        if (data && (data & 7)) return false;
        return true;
    }

    // Counters for the acceptance test: a non-zero value here is Gate B earning its place, and the
    // breakdown says which precondition the engine would have tripped over.

    // Hand our build reference back to Scaleform. RefCountNTSImpl::Release is
    // `if (--RefCount == 0) delete this;` and MSVC compiles `delete this` on a class with a virtual
    // destructor into vtable slot 0, the scalar deleting destructor - verified on the marker child
    // type (vtable exe+0x2CBA380, slot 0 = exe+0x10EF320, which calls the real dtor exe+0x10C6220
    // and then frees through the engine's own allocator singleton). Only ever called while the
    // owning movie is still alive; see the call site for why that is the whole point.
    // ── stage 2: expire the parked player instead of exploiting the reuse ───────────────────────
    //
    // The world map's SwfPlayer is "mode 2": every close PARKS it on a list at mgr+0xD00 with a
    // countdown initialised to exactly 1.0f, and a reopen before that countdown drains hands back
    // the IDENTICAL MovieView - display list intact and, crucially, with NO attachMovie burst. Our
    // markers are built only from the RM2 pulses that a burst produces, so those opens built
    // nothing and the map came up empty. Measured 19:45-19:49: 61 opens, 61 closes, 49 retargets -
    // 12 opens with no burst at all, which is exactly the symptom.
    //
    // Rather than try to hold children across that window (their render entry is bound to the movie
    // instance and cannot be re-pointed - see v3_child_releasable), remove the window: ask the
    // engine to drop the parked entry. Then every open constructs a fresh MovieView and emits a
    // burst, and the build path stops depending on how fast the player pressed the button.
    //
    // The only mutation is a float the engine itself writes negative one second later. If we lose a
    // race on it the entry simply is not expired and we get today's behaviour - no corruption. The
    // dangerous part would be touching a freed node, so the node is re-validated inside the same
    // guarded block immediately before the store, and the identity test is an exact pointer match
    // against the movie we just released against, which cannot match another menu's player.
    // When the world map's dialog byte last went non-zero (the OPEN edge), for the
    // [opentime] line: it splits the user's "the map takes long to open" into the
    // engine's dialog->relayout-burst stretch and the burst->READY build we already time.
    std::atomic<uint64_t> g_open_dialog_ms{0};

    std::atomic<uintptr_t> g_park_target{0};   // the movie whose parked entry we want expired
    uint32_t g_park_frames = 0;
    uint64_t g_vt_sfmgr = 0;
    uint64_t g_vt_swfplayer = 0;

    // Candidate slots for the movie-manager singleton, built once.
    //
    // This chain used to start at a baked exe+0x3D83148, measured on 2.6.2 - wrong on 1.17 and on
    // 2.2.x, where the probe then read a stranger, failed its own checks and disarmed after 600
    // frames without ever saying why. There is no need to bake it: the walk below ends in an EXACT
    // test - a node whose player points at the very movie we just released against - so the slot
    // that produces a match IS the right slot, and the wrong ones simply never match. Collect the
    // structurally plausible candidates once, then let that test pick.
    std::vector<uintptr_t> g_park_slots;      // written by the scan thread, then published
    std::atomic<bool> g_park_slots_built{false}; // release-stored AFTER g_park_slots is final
    std::atomic<bool> g_park_scan_started{false};
    // Set when a scan came back EMPTY (the manager slot was not populated yet); the launcher
    // waits at least 5 s before trying again, so an early launch cannot spin the scan thread.
    std::atomic<uint64_t> g_park_scan_last_empty_ms{0};
    uintptr_t g_park_slot = 0;          // learned once a walk actually matched

    // ── park-probe reads: validate-then-read, with NEGATIVE verdicts cached ─────────────────────
    //
    // The scan below and the per-frame walk dereference values that are ROUTINELY dead: the scan
    // is a blind pass over writable .data (most plausible-looking words are garbage), and the walk
    // is armed at map close, which is exactly when the marker heap is released. v3_read64's SEH is
    // the wrong primitive for a routine fault: our __except swallows it, but the first-chance
    // exception is served to every exception filter in the process first, and two residents
    // (measured 2026-08-31, vanilla profile) walk a full stack per event at ~70 ms each - a
    // handful of faults per frame and the session never recovers. So ask the OS first. The general
    // safemem cache cannot be used here because it refuses to store negative verdicts (a cached
    // "no" would blind marker creation); a park probe is the opposite trade - a stale "no" delays
    // one candidate by a TTL, a fault costs the whole session. One VirtualQuery covers a region,
    // and garbage values overwhelmingly land in a few huge free regions, so the cache stays small.
    struct ParkRegion
    {
        uintptr_t base = 0, end = 0;
        uint64_t stamp = 0;
        bool ok = false;
    };
    constexpr uint64_t kParkTtlMs = 1000;
    // The cache is an argument, not a global: the one-shot scan runs on its OWN thread (see
    // v3_park_scan_worker) while the tick keeps probing with this one, and sharing the slots
    // between them would be the unlocked-shared-state class all over again.
    struct ParkRegionCache
    {
        std::array<ParkRegion, 32> r{};
        unsigned victim = 0;
        // What the misses cost, owned like the slots by the one thread that owns the cache:
        // misses the one-page probe answered, misses that went to VirtualQuery, and the QPC
        // ticks of each. Only ever added to; a reader takes a difference.
        uint64_t page_answers = 0;
        uint64_t vq_calls = 0;
        int64_t probe_qpc = 0;
        int64_t vq_qpc = 0;
    };
    ParkRegionCache g_park_cache; // the map tick's own; touched by no other thread

    // A miss asks safemem's one-page residency probe first (1-9 us) and VirtualQuery only when it
    // cannot answer. ERR 2026-09-23: this walk cost 46-60 ms on 15 of 16 closes (28-69 ms over the
    // day's 68), in ONE frame of the game's menu update, and 0 ms on the two closes under 1 s after
    // the previous walk (cache still warm). VirtualQuery walks the whole run of identical pages after
    // the address: 12-30 ms per call at ~20 GB committed. A resident readable page is proof enough
    // for a positive and covers that page only (park_read64's addr + 7 check probes the next page on
    // a straddle). Everything else takes the VirtualQuery path as before; negatives stay cached.
    // Safe on the scan worker: the probe keeps only atomics and its own thread-local tally.
    bool park_point_ok(ParkRegionCache &c, uintptr_t a, uint64_t now)
    {
        ParkRegion *slot = nullptr;
        for (auto &r : c.r)
            if (r.end && a >= r.base && a < r.end)
            {
                if (now - r.stamp <= kParkTtlMs)
                    return r.ok;
                slot = &r; // stale entry for this region: refresh in place
                break;
            }
        ParkRegion r{};
        r.stamp = now;
        goblin::safemem::Region page{};
        const int64_t p0 = v3_perf_now();
        const bool resident = goblin::safemem::page_probe(a, now, page) && page.read_ok;
        const int64_t p1 = v3_perf_now();
        c.probe_qpc += p1 - p0;
        if (resident)
        {
            ++c.page_answers;
            r.base = page.base;
            r.end = page.end;
            r.ok = true;
        }
        else
        {
            MEMORY_BASIC_INFORMATION mbi{};
            const SIZE_T got = VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof mbi);
            ++c.vq_calls;
            c.vq_qpc += v3_perf_now() - p1;
            if (got == sizeof mbi)
            {
                r.base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
                r.end = r.base + mbi.RegionSize;
                r.ok = mbi.State == MEM_COMMIT && goblin::safemem::prot_readable(mbi.Protect);
            }
            else
            {
                r.base = a & ~0xFFFull; // not a queryable address at all: remember the page as a "no"
                r.end = r.base + 0x1000;
            }
        }
        if (!slot)
            slot = &c.r[c.victim++ % c.r.size()];
        *slot = r;
        return r.ok;
    }

    // The read every park HEAP probe goes through (slot words in the exe's own writable sections
    // are committed by definition and keep the plain read). The SEH inside v3_read64 stays as the
    // net for a region unmapped between the check and the read; it is no longer the routine path.
    bool park_read64(ParkRegionCache &c, uintptr_t addr, uint64_t &out)
    {
        const uint64_t now = GetTickCount64();
        if (!park_point_ok(c, addr, now) || !park_point_ok(c, addr + 7, now))
        {
            out = 0;
            return false;
        }
        return v3_read64(addr, out);
    }

    // The one-shot scan body. Runs on its OWN thread, never on a frame: measured 2026-08-31,
    // the same pass cost 260 ms on the vanilla install and 3.07 s on the ERR one (whatever
    // .data holds decides how many words survive the prefilter, and each cache-missed probe
    // pays a VirtualQuery, which this machine has measured at 2+ ms under a memory-API
    // filter) - and 3 s on the map tick is exactly the "freeze on first map close" report.
    void v3_park_scan_worker()
    {
        const uint64_t t0 = GetTickCount64();
        std::vector<uintptr_t> found;
        ParkRegionCache cache; // this thread's own; the tick keeps g_park_cache to itself
        const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        // The manager is not anonymous: the chain is a static slot -> CS::CSScaleformImp -> +8 ->
        // CS::CSScaleformSystem, so filter by those two vtables, resolved by RTTI name (never
        // baked - the slot itself moved 0x4070 between builds, the names do not). This matters
        // because the purely structural filter is hopeless at shortlist scale: replayed over the
        // 2026-08-31 hang dump it admits 22714 .data words and the real slot ranks 9857th, so any
        // cap that keeps the walk affordable also guarantees the real slot is never in the list -
        // the walk then disarms empty, the parked entry survives, and a quick reopen reuses the
        // parked movie with no burst: the map comes up with no icons. The vtable filter admitted
        // exactly ONE candidate on that dump: the real slot. When RTTI resolution fails we keep
        // the structural shortlist as the degraded path, cap and all.
        const uintptr_t vt_sys =
            exe ? goblin::anchors::vtable_of(goblin::anchors::RttiClass::CSScaleformSystem) : 0;
        const uintptr_t vt_imp =
            exe ? goblin::anchors::vtable_of(goblin::anchors::RttiClass::CSScaleformImp) : 0;
        if (exe)
        {
        const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(exe);
        const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(exe + dos->e_lfanew);
        const auto *sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec)
        {
            if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE))
                continue;
            const uintptr_t begin = exe + sec->VirtualAddress;
            for (size_t off = 0; off + 8 <= sec->Misc.VirtualSize; off += 8)
            {
                uint64_t outer = 0, ovt = 0, mgr = 0, mvt = 0, node = 0;
                if (!v3_read64(begin + off, outer) || !v3_heap_ptr(outer) || (outer & 7))
                    continue;
                if (!park_read64(cache, static_cast<uintptr_t>(outer) + 8, mgr) ||
                    !v3_heap_ptr(mgr) || (mgr & 7))
                    continue;
                if (!park_read64(cache, static_cast<uintptr_t>(mgr), mvt) || !v3_heap_ptr(mvt))
                    continue;
                if (vt_sys)
                {
                    if (mvt != vt_sys)
                        continue;
                    if (vt_imp && (!park_read64(cache, static_cast<uintptr_t>(outer), ovt) ||
                                   ovt != vt_imp))
                        continue;
                }
                else
                {
                    // Degraded structural-only path. mgr+0xD00 (the parked list head) may be
                    // legitimately empty on the real manager, so this test lives only here.
                    if (!park_read64(cache, static_cast<uintptr_t>(mgr) + 0xD00, node) ||
                        !v3_heap_ptr(node))
                        continue;
                }
                found.push_back(begin + off);
                if (found.size() >= 64)
                    break;               // more than this is not a shortlist, it is noise
            }
        }
        }
        if (found.empty())
        {
            // Nothing matched: the launch was too early for the manager slot (the scan now
            // starts at the first phase-byte read, in the main menu) or RTTI did not resolve.
            // Do NOT publish an empty list as "built" - that would latch the expiry walk off for
            // the whole session. Leave the door open for the next launch point instead.
            g_park_scan_last_empty_ms.store(GetTickCount64(), std::memory_order_relaxed);
            g_park_scan_started.store(false, std::memory_order_release);
            spdlog::info("[v3park] no candidate manager slot yet ({} vtable filter, {} ms); "
                         "will look again",
                         vt_sys ? "by" : "NO", GetTickCount64() - t0);
            return;
        }
        g_park_slots = std::move(found); // the tick reads this only after the release below
        g_park_slots_built.store(true, std::memory_order_release);
        spdlog::info("[v3park] {} candidate manager slot(s) ({} vtable filter) in {} ms off-frame; "
                     "the parked-entry match picks one",
                     g_park_slots.size(), vt_sys ? "by" : "NO", GetTickCount64() - t0);
    }

    // Non-blocking launcher. Called from the FIRST valid phase-byte read (the main menu, minutes
    // before any map), from every map-open edge, and from the expire tick as the net for a close
    // that arrives before any of those. Never costs the calling frame more than an exchange.
    //
    // Why the first phase read and not the open edge alone (2026-09-16, Convergence + ERR): the
    // scan takes 0.3..5.2 s depending on the machine, and it used to start only when the first map
    // opened. A close inside that window could not expire the parked entry, so a quick reopen
    // reused the movie with our children already freed and the map came up EMPTY until the next
    // slow reopen. Starting in the main menu puts the whole loading screen between the scan and
    // the first map; the empty-result retry covers a launch that is still too early.
    void v3_park_build_slots()
    {
        if (g_park_slots_built.load(std::memory_order_acquire)) return;
        const uint64_t last_empty = g_park_scan_last_empty_ms.load(std::memory_order_relaxed);
        if (last_empty && GetTickCount64() - last_empty < 5000) return;
        if (g_park_scan_started.exchange(true)) return;
        std::thread(v3_park_scan_worker).detach();
    }

    // Cost account of the parked-entry walk: per target (from its first frame) and a running total
    // that [closeframes] takes a one-frame difference of. UI thread only, like the walk.
    struct ParkWalkCost
    {
        int64_t qpc = 0;
        uint64_t answers0 = 0, vq0 = 0;
        int64_t vq_qpc0 = 0;
    };
    ParkWalkCost g_park_walk;
    int64_t g_park_walk_total_qpc = 0;
    int64_t park_us(int64_t qpc) { return qpc * 1000000 / v3_perf_freq(); }

    // One walk over the manager's parked-player list for `want`: the node whose countdown was set to
    // -1 (expired), or 0 (no exact match this frame, or the store did not land - retried next frame).
    uintptr_t v3_park_walk_once(uintptr_t want)
    {
        // Try the slot that matched before; failing that, every candidate. The loop below ends on
        // an exact movie-pointer match, so a wrong candidate costs one short walk and nothing else.
        for (size_t cand = 0; cand < (g_park_slot ? 1u : g_park_slots.size()); ++cand)
        {
        const uintptr_t slot_addr = g_park_slot ? g_park_slot : g_park_slots[cand];
        uint64_t outer = 0, mgr = 0, mvt = 0;
        if (!v3_read64(slot_addr, outer) || !v3_heap_ptr(outer)) continue;
        if (!park_read64(g_park_cache, static_cast<uintptr_t>(outer) + 8, mgr) ||
            !v3_heap_ptr(mgr)) continue;
        if (!park_read64(g_park_cache, static_cast<uintptr_t>(mgr), mvt) ||
            !v3_heap_ptr(mvt)) continue;
        if (g_park_slot && g_vt_sfmgr && mvt != g_vt_sfmgr) continue;  // the object we learned moved

        uint64_t node = 0;
        if (!park_read64(g_park_cache, static_cast<uintptr_t>(mgr) + 0xD00, node) ||
            !v3_heap_ptr(node)) continue;
        const uint64_t head = node;
        for (int hop = 0; hop < 16; ++hop)
        {
            uint64_t player = 0, pvt = 0, pmovie = 0;
            if (park_read64(g_park_cache, static_cast<uintptr_t>(node) + 0x10, player) &&
                v3_heap_ptr(player) &&
                park_read64(g_park_cache, static_cast<uintptr_t>(player), pvt) &&
                v3_heap_ptr(pvt) &&
                park_read64(g_park_cache, static_cast<uintptr_t>(player) + 0x18, pmovie) &&
                pmovie == static_cast<uint64_t>(want) &&
                (g_vt_swfplayer == 0 || pvt == g_vt_swfplayer))
            {
                if (g_vt_swfplayer == 0) g_vt_swfplayer = pvt;   // learn it from the exact match
                // This candidate produced an exact movie match, so it IS the manager slot.
                // Remember it and stop scanning on later ticks.
                g_park_slot = slot_addr;
                g_vt_sfmgr = mvt;
                const float expired = -1.0f;
                return v3_write_bytes(static_cast<uintptr_t>(node) + 0x18, &expired, sizeof expired)
                           ? static_cast<uintptr_t>(node)
                           : 0;
            }
            uint64_t next = 0;
            if (!park_read64(g_park_cache, static_cast<uintptr_t>(node), next) ||
                !v3_heap_ptr(next)) break;
            node = next;
            if (node == head) break;   // circular list, one lap done
        }
        }   // candidate slots
        return 0;
    }

    void v3_park_expire_tick()
    {
        const uintptr_t want = g_park_target.load(std::memory_order_relaxed);
        if (!want) return;
        v3_park_build_slots();
        // No frame is charged while the scan thread is still running: on a machine where the
        // scan outlives 600 frames the walk would otherwise disarm before its list ever existed.
        if (!g_park_slots_built.load(std::memory_order_acquire))
        {
            // Say so ONCE per armed movie: this is the exact state behind "the map came up empty
            // on a quick reopen" - the parked entry survives because the shortlist is not there.
            static uintptr_t s_not_ready_for = 0;
            if (s_not_ready_for != want)
            {
                s_not_ready_for = want;
                spdlog::info("[v3park] shortlist not ready at close for movie 0x{:X}; "
                             "a reopen before it lands may come up empty",
                             want);
            }
            return;
        }
        if (++g_park_frames > 600)   // ~10 s of frames: the entry is not there, stop looking
        {
            g_park_target.store(0, std::memory_order_relaxed);
            spdlog::info("[v3park] no parked entry for movie 0x{:X} after {} frames - disarmed; "
                         "walk {} us: {} page probes, {} VirtualQuery ({} us)",
                         want, g_park_frames, park_us(g_park_walk.qpc),
                         g_park_cache.page_answers - g_park_walk.answers0,
                         g_park_cache.vq_calls - g_park_walk.vq0,
                         park_us(g_park_cache.vq_qpc - g_park_walk.vq_qpc0));
            g_park_frames = 0;
            return;
        }
        if (g_park_frames == 1) // first walk for this target: start its cost account
        {
            g_park_walk = ParkWalkCost{};
            g_park_walk.answers0 = g_park_cache.page_answers;
            g_park_walk.vq0 = g_park_cache.vq_calls;
            g_park_walk.vq_qpc0 = g_park_cache.vq_qpc;
        }
        const int64_t w0 = v3_perf_now();
        const uintptr_t node = v3_park_walk_once(want);
        const int64_t dw = v3_perf_now() - w0;
        g_park_walk.qpc += dw;
        g_park_walk_total_qpc += dw;
        if (!node)
            return; // no match or the store did not land: try again next frame, as before
        // The FIRST expiry of a session always prints (the one-line proof the mechanism is alive on
        // this build); the per-close repeats only under debug_logging - they said the same thing a
        // hundred times per session.
        static bool s_reported_once = false;
        if (!s_reported_once || goblin::config::debugLogging)
        {
            s_reported_once = true;
            spdlog::info("[v3park] expired the parked entry for movie 0x{:X} (node 0x{:X}, {} frames); "
                         "walk {} us: {} page probes, {} VirtualQuery ({} us) - the next open "
                         "rebuilds and bursts",
                         want, node, g_park_frames, park_us(g_park_walk.qpc),
                         g_park_cache.page_answers - g_park_walk.answers0,
                         g_park_cache.vq_calls - g_park_walk.vq0,
                         park_us(g_park_cache.vq_qpc - g_park_walk.vq_qpc0));
        }
        g_park_target.store(0, std::memory_order_relaxed);
        g_park_frames = 0;
    }

    // [closeframes] (debug_logging): frame intervals around a map close from menu_update_detour's
    // own entry stamps (updateTask runs once a frame). t[0] the frame before the close frame, t[1]
    // the close frame (the dialog destructor ran inside it), t[2] the frame that first reads the
    // byte at 0 and runs the parked-entry walk, then four more. Printed once when complete. UI
    // thread only.
    struct CloseFrames
    {
        int64_t last = 0, before_last = 0, t[8] = {}, walk0 = 0, walk_next = 0;
        int n = 0;
    };
    CloseFrames g_close_frames;
    void close_frames_note(int64_t now, bool close_edge)
    {
        CloseFrames &c = g_close_frames;
        if (c.n == 0)
        {
            if (close_edge && goblin::config::debugLogging && c.before_last && c.last)
            {
                c.t[0] = c.before_last;
                c.t[1] = c.last;
                c.t[2] = now;
                c.n = 3;
                c.walk0 = g_park_walk_total_qpc;
            }
        }
        else
        {
            if (c.n == 3)
                c.walk_next = g_park_walk_total_qpc - c.walk0;
            c.t[c.n++] = now;
            if (c.n == 8)
            {
                const double f = static_cast<double>(v3_perf_freq()) / 1000.0;
                spdlog::info("[closeframes] frame times around a map close, ms: before {:.1f} | "
                             "close {:.1f} | next {:.1f} (walk {} us) | then {:.1f} {:.1f} {:.1f} {:.1f}",
                             (c.t[1] - c.t[0]) / f, (c.t[2] - c.t[1]) / f, (c.t[3] - c.t[2]) / f,
                             park_us(c.walk_next), (c.t[4] - c.t[3]) / f, (c.t[5] - c.t[4]) / f,
                             (c.t[6] - c.t[5]) / f, (c.t[7] - c.t[6]) / f);
                c.n = 0;
            }
        }
        c.before_last = c.last;
        c.last = now;
    }

    // ── the world map's CURRENT movie, read every frame without an attachMovie burst ────────────
    //
    //     dialog = MapArea - 0x27D8 ;  cell = *(dialog + 0x140) ;  movie = *cell
    //
    // dialog+0x140 is not a copy of the movie pointer - it is the ADDRESS of the owning
    // CS::MenuWindowJob's movie slot (job+0x10), so the dialog always reads through to whatever the
    // job holds now and cannot go stale while the job lives. The field belongs to the MenuWindow
    // base class, not to WorldMapDialog: it holds for 8 unrelated dialog classes in the dump.
    //
    // This is what we never had. Until now the only way to learn the movie was to wait for a burst,
    // and a quick reopen emits none - which is exactly the case we kept getting wrong.
    //
    // The round trip through job+0x130 is MANDATORY, not belt-and-braces: the job pool threads its
    // free list through the vptr word, and a free slot's +0x10 was measured holding plausible
    // garbage. Pointer shape alone cannot tell a live job from a recycled one.
    uint64_t g_job_vtable = 0;    // learned once, then required constant
    uint64_t g_movie_vtable = 0;

    uintptr_t v3_movie_now()
    {
        if (!v3_map_object_alive()) return 0;
        void *area = goblin::maphover::map_dialog();
        if (!area) return 0;
        const uintptr_t dialog = reinterpret_cast<uintptr_t>(area) - 0x27D8;
        uint64_t cell = 0;
        if (!v3_read64(dialog + 0x140, cell) || !v3_heap_ptr(cell) || (cell & 7)) return 0;
        const uintptr_t job = static_cast<uintptr_t>(cell) - 0x10;
        uint64_t jvt = 0, back = 0, movie = 0, mvt = 0;
        if (!v3_read64(job, jvt) || !v3_heap_ptr(jvt)) return 0;
        if (g_job_vtable == 0) g_job_vtable = jvt;
        else if (jvt != g_job_vtable) return 0;
        // The round trip: this job must point back at the dialog we started from.
        if (!v3_read64(job + 0x130, back) || back != dialog) return 0;
        if (!v3_read64(static_cast<uintptr_t>(cell), movie) || !v3_heap_ptr(movie)) return 0;
        if (!v3_read64(static_cast<uintptr_t>(movie), mvt) || !v3_heap_ptr(mvt)) return 0;
        if (g_movie_vtable == 0) g_movie_vtable = mvt;
        else if (mvt != g_movie_vtable) return 0;
        return static_cast<uintptr_t>(movie);
    }

    // Is this child safe to destroy RIGHT NOW? Validate before calling, never catch after.
    //
    // The SEH net around the destructor is not a substitute for this. By the time the destructor
    // faults it has already mutated live engine state: it overwrites three vtable slots and runs a
    // movie-registry unregister that AddRefs and Releases the movie, and only then reaches the free
    // of child+0x50 that trips EnterCriticalSection on a dead heap. A "refused" from the SEH path
    // therefore means "we already did damage", not "we declined".
    //
    // The slot test checks the slot's VTABLE, not merely that it is non-null. Measured over the
    // 50,825 children in the full dump: 8,946 null slots, 11,366 correct, and 29,801 carrying the
    // WRONG vtable - every one of which sails through a plain null check.
    // The snapshot-slot class's vtable, found at runtime by two of its own anchored slot
    // functions. It used to be baked as exe+0x2CB9EF0 - correct on 2.6.2/2.6.1/2.6.0 and WRONG on
    // 2.7.0, 2.2.3 and 2.2.0, where every child then failed the pre-test, the whole generation
    // leaked at every map close, and the fixed 160 MiB Scaleform arena eventually could not serve
    // the engine a 0x110-byte request: DL_PANIC (player report, 1.17, 2026-08-28). The class has
    // no RTTI, so vtable_of cannot name it; its slot FUNCTIONS are code and the resolver finds
    // those anywhere. Layout verified unchanged on all six builds - every virtual function
    // identical instruction-for-instruction and reading the same field offsets
    // (scratch/verify_slot_layout_all.py), so the rest of the walk below stands.
    uint64_t v3_slot_vtable()
    {
        return goblin::anchors::vtable_with(goblin::AnchorId::snapshot_slot_vt_fn4, 4,
                                            goblin::AnchorId::snapshot_slot_vt_fn6, 6);
    }

    // The snapshot-slot vtable this child reports, or 0 if the walk does not hold up. Split out of
    // v3_child_releasable so the close-time survey below can ASK what the live value is without
    // acting on it. Everything here is a struct OFFSET, which a rebase does not move; the one
    // ADDRESS in the chain is the vtable the caller compares against.
    uint64_t v3_child_slot_vtable(uintptr_t child, uintptr_t movie)
    {
        uint64_t root = 0, owner = 0;
        if (!v3_read64(child + 0x20, root) || !v3_heap_ptr(root)) return 0;
        if (!v3_read64(static_cast<uintptr_t>(root) + 0x10, owner) ||
            owner != static_cast<uint64_t>(movie))
            return 0;
        uint64_t entry = 0;
        if (!v3_read64(child + 0x48, entry) || !v3_heap_ptr(entry)) return 0;
        const uint64_t page = entry & ~0xFFFull;
        const uint64_t off = entry - page;
        if (off < 0x30 || ((off - 0x30) % 0x48) != 0) return 0;
        uint64_t tbl = 0, slot = 0, svt = 0;
        if (!v3_read64(page + 0x20, tbl) || !v3_heap_ptr(tbl)) return 0;
        if (!v3_read64(tbl + 0x28 + ((off - 0x30) / 0x48) * 8, slot) || !v3_heap_ptr(slot))
            return 0;
        if (!v3_read64(static_cast<uintptr_t>(slot), svt)) return 0;
        return svt;
    }

    bool v3_child_releasable(uintptr_t child, uint64_t expect_vtable, uintptr_t movie)
    {
        if (!v3_heap_ptr(child) || expect_vtable == 0 || !v3_heap_ptr(movie)) return false;
        const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (!exe) return false;
        uint64_t vt = 0;
        if (!v3_read64(child, vt) || vt != expect_vtable) return false;          // T1
        uint64_t probe = 0;
        if (!v3_read64(movie + 0x5338, probe)) return false;                     // T2
        uint64_t root = 0, owner = 0;
        if (!v3_read64(child + 0x20, root) || !v3_heap_ptr(root)) return false;  // T3
        if (!v3_read64(static_cast<uintptr_t>(root) + 0x10, owner) ||
            owner != static_cast<uint64_t>(movie))
            return false;
        uint64_t entry = 0;                                                       // T4
        if (!v3_read64(child + 0x48, entry) || !v3_heap_ptr(entry)) return false;
        const uint64_t page = entry & ~0xFFFull;
        const uint64_t off = entry - page;
        if (off < 0x30 || ((off - 0x30) % 0x48) != 0) return false;
        uint64_t tbl = 0, slot = 0, svt = 0;
        if (!v3_read64(page + 0x20, tbl) || !v3_heap_ptr(tbl)) return false;
        if (!v3_read64(tbl + 0x28 + ((off - 0x30) / 0x48) * 8, slot) || !v3_heap_ptr(slot))
            return false;
        const uint64_t want = v3_slot_vtable();
        if (!want)
            return false;   // cannot identify the class on this build - refuse, never guess
        if (!v3_read64(static_cast<uintptr_t>(slot), svt) || svt != want)
            return false;
        return true;
    }

    bool v3_release_held(uintptr_t child, uint64_t expect_vtable, bool &destroyed)
    {
        destroyed = false;
        if (!v3_heap_ptr(child) || expect_vtable == 0) return false;
        __try
        {
            const uint64_t vt = *reinterpret_cast<const uint64_t *>(child);
            if (vt != expect_vtable) return false;
            auto *refs = reinterpret_cast<uint32_t *>(child + 8);
            const uint32_t before = *refs;
            if (before == 0 || before >= 0x10000000) return false;
            if (--*refs != 0) return true;  // somebody else still owns it and will destroy it
            using DeletingDtor = void *(__fastcall *)(void *, unsigned);
            (*reinterpret_cast<DeletingDtor **>(child))[0](reinterpret_cast<void *>(child), 1);
            destroyed = true;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    uint32_t v3_guarded_attach(uintptr_t wrapper, uintptr_t child)
    {
        // Render order follows child-list order: index 0 parked our icons
        // BEHIND every native pin, so append at the END of the parent's list
        // (index == current count) to draw on top, like the param pipeline did.
        uint64_t parent = 0, count = 0;
        if (!v3_read64(wrapper + 0x18, parent) || !v3_heap_ptr(parent) ||
            !v3_read64(static_cast<uintptr_t>(parent) + 0xe0, count) ||
            count > 65536)
            count = 0;
        // Gate B2. InsertChildAtDepth walks the parent's display-list Array without checking it.
        // In the 13:00:46 crash that Array's data pointer read as an address inside the exe's own
        // .text and its size field held a pointer-shaped value; the engine walked it and faulted.
        // Refusing one child is a missing icon, walking a corrupt array is a dead process.
        if (!v3_container_array_ok(static_cast<uintptr_t>(parent)))
        {
            g_v3_reject_array.fetch_add(1, std::memory_order_relaxed);
            return 0xE0000001u;  // our own sentinel: refused, never entered the engine
        }
        // Keep SEH in a destructor-free leaf; MSVC rejects __try in the
        // spdlog-heavy caller because that function owns C++ temporaries.
        __try
        {
            o_v3_attach(reinterpret_cast<void *>(wrapper),
                        reinterpret_cast<void *>(child),
                        static_cast<uint32_t>(count));
            return 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return static_cast<uint32_t>(GetExceptionCode());
        }
    }

    // ── [v3pump] frame pump (see V3PumpSession) ──────────────────────────────────────────────────────
    // SEH leaves first: no C++ temporaries in these frames.

    // GotoFrame through the vtable slot the caller has checked. 0 or the exception code.
    uint32_t v3_pump_guarded_goto(uintptr_t sprite, uint32_t frame0)
    {
        __try
        {
            const uintptr_t vt = *reinterpret_cast<const uintptr_t *>(sprite);
            (*reinterpret_cast<V3GotoFrameFn *const *>(vt + V3_SLOT_GOTO_FRAME))(
                reinterpret_cast<void *>(sprite), frame0);
            return 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return static_cast<uint32_t>(GetExceptionCode());
        }
    }

    uint32_t v3_pump_guarded_frame_count(uintptr_t sprite)
    {
        __try
        {
            const uintptr_t vt = *reinterpret_cast<const uintptr_t *>(sprite);
            return (*reinterpret_cast<V3FrameCountFn *const *>(vt + V3_SLOT_FRAME_COUNT))(
                reinterpret_cast<void *>(sprite));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    // Is `child` in `parent`'s child vector (+0xD8 data, stride 0x10, u32 count +0xE0)? The
    // generator sits right after the native pins, so a forward walk finds it early.
    bool v3_pump_parent_holds(uintptr_t parent, uintptr_t child)
    {
        bool found = false;
        __try
        {
            const uintptr_t base = *reinterpret_cast<const uintptr_t *>(parent + 0xd8);
            const uint32_t count = *reinterpret_cast<const uint32_t *>(parent + 0xe0);
            if (base && count <= 0x10000)
                for (uint32_t i = 0; i < count && !found; ++i)
                    found = *reinterpret_cast<const uintptr_t *>(
                                base + static_cast<uint64_t>(i) * 0x10) == child;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            found = false;
        }
        return found;
    }

    struct V3PumpGenState
    {
        uint64_t vt = 0, slot = 0, parent = 0;
        uint32_t flags = 0;  // u16 at +0x6A
        int32_t depth = 0;   // +0x2C
        uint32_t cur = 0;    // +0x11C, 0-based
        uint32_t play = 0;   // +0x118
        uint32_t kids = 0;   // own child count, +0xE0
    };

    bool v3_pump_read_state(uintptr_t gen, V3PumpGenState &st)
    {
        uint32_t packed = 0, depth = 0;
        if (!v3_read64(gen, st.vt) || !v3_heap_ptr(st.vt) ||
            !v3_read64(st.vt + V3_SLOT_GOTO_FRAME, st.slot) || !v3_read64(gen + 0x38, st.parent) ||
            !v3_read32(gen + 0x68, packed) || !v3_read32(gen + 0x2c, depth) ||
            !v3_read32(gen + 0x11c, st.cur) || !v3_read32(gen + 0x118, st.play) ||
            !v3_read32(gen + 0xe0, st.kids))
            return false;
        st.flags = packed >> 16;
        st.depth = static_cast<int32_t>(depth);
        return true;
    }

    // GotoFrame's own entry test, plus: the slot we call through IS the resolved GotoFrame.
    const char *v3_pump_refusal(const V3PumpGenState &st)
    {
        if (st.slot != reinterpret_cast<uint64_t>(g_v3_goto_frame))
            return "vtable slot +0x378 is not the resolved GotoFrame";
        if (!(st.flags & 0x0400))
            return "flag 0x400 (Sprite) clear";
        if (!(st.flags & 0x0800) || (st.flags & 0x1000))
            return "flags fail the GotoFrame entry test";
        if (st.depth < -1)
            return "depth field below -1";
        return nullptr;
    }

    // The proof: one GotoFrame with pulses COUNTED, not built (v3_native_factory_pulse returns at
    // its top while t_v3_pump.active).
    struct V3PumpCall
    {
        uint32_t from = 0, after = 0;
        uint32_t pulses = 0, own = 0, other = 0;
        int64_t qpc = 0;
        uint32_t exc = 0;
    };

    void v3_pump_count_call(uintptr_t gen, uint32_t to, V3PumpCall &c)
    {
        v3_read32(gen + 0x11c, c.from);
        t_v3_pump.expect = gen;
        t_v3_pump.pulses = t_v3_pump.own = t_v3_pump.other = 0;
        t_v3_pump.active = true;
        const int64_t t0 = v3_perf_now();
        c.exc = v3_pump_guarded_goto(gen, to);
        c.qpc = v3_perf_now() - t0;
        t_v3_pump.active = false;
        c.pulses = t_v3_pump.pulses;
        c.own = t_v3_pump.own;
        c.other = t_v3_pump.other;
        v3_read32(gen + 0x11c, c.after);
    }

    // Our appended frames as 0-based frame indices. The pump needs three of them at least: a
    // forward stroke from the first must be able to cover one frame.
    bool v3_pump_own_frames(uint32_t &first0, uint32_t &last0)
    {
        uint32_t lo = 0, hi = 0;
        goblin::gfx_probe::injected_iid_range(lo, hi);
        if (lo == 0 || hi < lo + 2)
            return false;
        first0 = lo - 1;
        last0 = hi - 1;
        return true;
    }

    // Every later open builds in the burst, as before the pump. Rows this open still owed the pump
    // have no pulse source left: the queue watchdog drops them, the close sees an unfinished
    // generation and the next open (a fresh burst) builds everything.
    void v3_pump_off(const char *why, uint32_t code)
    {
        g_v3_pump.off = true;
        const size_t left = g_v3_native.pump_capped
                                ? g_v3_native.pending.size() - g_v3_native.pending_index
                                : 0;
        spdlog::warn("[v3pump] off for this session: {} (code 0x{:08X}); {} markers of this open "
                     "left unbuilt, later opens build all markers at once",
                     why, code, left);
        // Only a queue the pump owned is dropped (see v3_pump_stop_open); from the generator's
        // creation the burst still owns its whole budget and keeps building.
        if (g_v3_native.pump_capped)
        {
            g_v3_open.pump_dropped += left;
            g_v3_native.pending_index = g_v3_native.pending.size();
        }
        g_v3_native.pump_gen = 0;
        g_v3_native.pump_capped = false;
    }

    // This open only: what is left of its queue is dropped at once (no pulse source remains, and a
    // queue that is not spent holds back the periodic merge), as the queue watchdog would. The
    // queue then counts as spent, so READY still follows, and the "[v3pump] this open" line says
    // how many rows were dropped. A new open gets a new parent and builds from scratch; later opens
    // pump again.
    void v3_pump_stop_open(const char *why, uint32_t code)
    {
        const size_t left = g_v3_native.pending.size() - g_v3_native.pending_index;
        spdlog::warn("[v3pump] stopped for this open: {} (code 0x{:08X}); {} markers left unbuilt "
                     "until the next open",
                     why, code, left);
        g_v3_open.pump_dropped += left;
        g_v3_native.pump_gen = 0;
        g_v3_native.pump_capped = false;
        g_v3_native.pending_index = g_v3_native.pending.size();
    }

    // In the FIRST pulse of a generation, before any marker (called from the factory with slot 0
    // free, before in_factory). Same slot / materialize / stage / attach sequence as a marker, with
    // charId 171 and a Sprite placement; our staging reference is dropped, so the marker parent
    // owns it. Then the proof, nested in the engine's own GotoFrame, and the burst cap.
    void v3_pump_make_generator(void *live_ctx, uint32_t frame, uintptr_t sprite,
                                uintptr_t target_movie)
    {
        g_v3_native.pump_tried = true;
        uint32_t first0 = 0, last0 = 0;
        if (!v3_pump_own_frames(first0, last0))
            return;
        const int64_t t0 = v3_perf_now();
        const uint16_t depth = g_v3_native.next_depth++;
        // The place detour counts a staging rejection into `failed`, which is the MARKER count
        // READY and the close's unfinished test read: keep the generator out of it.
        const uint32_t failed0 = g_v3_native.failed;
        auto &s = g_v3_factory_slots[0];
        s = V3FactorySlot{};
        s.depth = depth;
        s.char_id = V3_PUMP_SPRITE_CHAR;
        g_v3_factory_active = 1;
        const uintptr_t node = goblin::gfx_probe::create_native_sprite_child(
            V3_PUMP_SPRITE_CHAR, depth, live_ctx, frame);
        uint32_t mat_exc = 0, att_exc = 0;
        if (v3_heap_ptr(node) && (!s.held || !v3_heap_ptr(s.child)))
            mat_exc = v3_guarded_materialize(live_ctx, sprite);
        uintptr_t gen = 0;
        const char *why = nullptr;
        const bool rejected = g_v3_native.failed != failed0;
        g_v3_native.failed = failed0;
        if (!v3_heap_ptr(node))
            why = "the placement left no timeline record";
        else if (rejected)
            why = "the staging rejected the child (see the line above)";
        else if (!s.held || !v3_heap_ptr(s.child))
            why = "the record was not materialized";
        else if (s.root != target_movie)
            why = "the child belongs to another movie";
        else
        {
            att_exc = v3_guarded_attach(g_v3_native.wrapper, s.child);
            uint64_t par = 0;
            v3_read64(s.child + 0x38, par);
            if (att_exc == 0 && par == g_v3_native.parent)
                gen = s.child;
            else
                why = "attach to the marker parent failed";
        }
        const uintptr_t child = s.child;
        if (s.held && v3_heap_ptr(s.child))
        {
            uint32_t rb = 0, ra = 0;
            v3_drop_held_ref(s.child, rb, ra);
        }
        if (v3_heap_ptr(node))
            goblin::gfx_probe::remove_native_icon_record(depth, live_ctx, frame);
        s = V3FactorySlot{};
        g_v3_factory_active = 0;

        V3PumpGenState st{};
        uint32_t frames = 0;
        if (gen)
        {
            why = v3_pump_read_state(gen, st) ? v3_pump_refusal(st) : "state unreadable";
            // A virtual call only once the vtable is proven to be the Sprite one (slot +0x378).
            frames = why ? 0 : v3_pump_guarded_frame_count(gen);
            if (!why && frames <= last0)
                why = "the instance's timeline ends before our frames";
        }
        if (!gen || why)
        {
            // This open builds everything in the burst; a later open tries again - unless the
            // vtable slot holds some other function, which is this exe, not this open. (An attached
            // generator that failed the checks just stays hidden on the parent until teardown.)
            g_v3_open.gen_qpc += v3_perf_now() - t0;
            if (gen && st.slot != 0 && st.slot != reinterpret_cast<uint64_t>(g_v3_goto_frame))
                g_v3_pump.off = true;
            if (g_v3_pump.no_gen_logs++ < 4)
                spdlog::warn("[v3pump] no generator this open, all markers built at once: {} "
                             "(record 0x{:X} child 0x{:X} driver 0x{:08X} attach 0x{:08X} flags "
                             "0x{:04X} frames {})",
                             why, node, child, mat_exc, att_exc, st.flags, frames);
            return;
        }
        g_v3_native.pump_gen = gen;
        V3PumpCall c{};
        v3_pump_count_call(gen, first0, c);
        g_v3_open.gen_qpc += v3_perf_now() - t0;
        g_v3_open.proof_pulses = c.own;
        if (c.exc)
        {
            v3_pump_off("the nested GotoFrame raised", c.exc);
            return;
        }
        if (c.own == 0)
        {
            v3_pump_off("the nested GotoFrame produced no pulse", 0);
            return;
        }
        // The rows visible at seed sort first (merge_snapshot), so capping the burst at their end
        // builds exactly them now and leaves draw order unchanged.
        const size_t head = g_v3_native.burst_end > g_v3_native.pending_index
                                ? g_v3_native.burst_end - g_v3_native.pending_index
                                : 0;
        g_v3_native.frame_budget = (std::min)(g_v3_native.frame_budget, head);
        g_v3_native.pump_capped = true;
        g_v3_open.capped = true;
        g_v3_open.burst_cap = head;
        if (goblin::config::debugLogging)
            spdlog::info("[v3pump] generator 0x{:X} depth {} ({} frames, ours {}..{}, flags "
                         "0x{:04X}); proof {} -> {}: {} pulses ({} other) in {} us; the burst "
                         "builds {} visible rows, the pump the other {}",
                         gen, depth, frames, first0, last0, st.flags, c.from, c.after, c.own,
                         c.other, park_us(c.qpc), head,
                         g_v3_native.pending.size() - g_v3_native.pending_index - head);
    }

    void v3_factory_clear_request(bool drop_held_ref); // defined with the factory, below

    // Map tick tail, while the pump owns part of the queue: a budget of markers from the
    // generator's own pulses. Strokes run forward through our frames (2 pulses per frame covered)
    // and, at the end of them, back to the first (the fill restarts at 0: the stock count).
    void v3_pump_tick()
    {
        // A merge costs about what a pump frame does (~3 ms); one of them per frame is enough.
        const bool merged = g_v3_native.pump_merged;
        g_v3_native.pump_merged = false;
        if (!g_v3_native.pump_capped)
            return;
        if (g_v3_native.pending_index >= g_v3_native.pending.size())
        {
            g_v3_native.pump_capped = false; // done; READY follows once the parent settles
            return;
        }
        if (merged || g_v3_native.in_factory || g_v3_map_closed.load(std::memory_order_acquire) ||
            !v3_map_drivable())
            return;
        // No pulse is on the stack here, so a slot still armed was left by one that threw; the
        // next pulse would clear it, but the pump IS the next pulse source.
        if (g_v3_factory_active != 0)
            v3_factory_clear_request(true);
        const uintptr_t gen = g_v3_native.pump_gen;
        uint32_t first0 = 0, last0 = 0;
        V3PumpGenState st{};
        const char *refusal =
            !v3_pump_own_frames(first0, last0)              ? "our frames are gone"
            : !v3_pump_parent_holds(g_v3_native.parent, gen) ? "the generator left the marker parent"
            : !v3_pump_read_state(gen, st)                   ? "generator state unreadable"
                                                             : v3_pump_refusal(st);
        if (refusal)
        {
            // A different function in the GotoFrame slot says something about this exe, not
            // this open; everything else (a generator that left, an unloading one) is local.
            if (st.slot != 0 && st.slot != reinterpret_cast<uint64_t>(g_v3_goto_frame))
                v3_pump_off(refusal, st.flags);
            else
                v3_pump_stop_open(refusal, st.flags);
            return;
        }
        if (g_v3_open.pump_frames == 0)
            g_v3_open.burst_built = g_v3_native.objects.size();
        const int64_t t0 = v3_perf_now();
        const size_t built0 = g_v3_native.objects.size();
        const size_t index0 = g_v3_native.pending_index;
        const size_t budget = (std::min)(g_v3_pump.budget,
                                         g_v3_native.pending.size() - g_v3_native.pending_index);
        g_v3_native.frame_budget = budget;
        uint32_t pulses = 0, calls = 0, exc = 0;
        while (calls < V3_PUMP_MAX_STROKES && g_v3_native.frame_budget != 0 &&
               g_v3_native.pending_index < g_v3_native.pending.size())
        {
            uint32_t cur = 0;
            v3_read32(gen + 0x11c, cur);
            // One pulse per batch of what is left of the budget, plus one.
            const uint32_t want = static_cast<uint32_t>(
                (g_v3_native.frame_budget + V3_FACTORY_BATCH - 1) / V3_FACTORY_BATCH + 1);
            uint32_t target = first0;
            if (cur >= first0 && cur + 2 <= last0)
                target = cur + 1 + std::min<uint32_t>((want + 1) / 2, last0 - cur - 1);
            ++calls;
            if (target < cur)
            {
                // The return stroke refills the snapshot from frame 0 (MarkAll / UnmarkAll on the
                // generator's list around it). The engine's own burst never builds in that mode -
                // its pins only go forward from 0 - so nothing is built here: pulses are counted,
                // as in the proof, and the forward strokes do the building.
                V3PumpCall c{};
                v3_pump_count_call(gen, target, c);
                g_v3_open.pump_pulses += c.own;
                exc = c.exc;
                if (exc || c.own == 0)
                    break;
                continue;
            }
            t_v3_pump.pumped = 0;
            t_v3_pump.pumping = true;
            exc = v3_pump_guarded_goto(gen, target);
            t_v3_pump.pumping = false;
            pulses += t_v3_pump.pumped;
            if (exc || t_v3_pump.pumped == 0)
                break;
        }
        g_v3_native.frame_budget = 0; // the tail hands the usual 96 right after
        const int64_t dt = v3_perf_now() - t0;
        uint32_t kids = 0;
        if (v3_read32(gen + 0xe0, kids))
            g_v3_open.gen_kids_max = (std::max)(g_v3_open.gen_kids_max, kids);
        g_v3_open.pump_qpc += dt;
        g_v3_open.pump_max_qpc = (std::max)(g_v3_open.pump_max_qpc, dt);
        ++g_v3_open.pump_frames;
        g_v3_open.pump_calls += calls;
        g_v3_open.pump_pulses += pulses;
        if (g_v3_native.objects.size() >= built0)
            g_v3_open.pump_built += g_v3_native.objects.size() - built0;
        if (exc)
        {
            v3_pump_off("a pump GotoFrame raised", exc);
            return;
        }
        // A reset inside a stroke (dead anchor, unfinished-generation drop) leaves nothing to pump.
        if (!g_v3_native.pump_capped)
            return;
        if (pulses == 0)
        {
            if (++g_v3_native.pump_dry >= 3)
                v3_pump_stop_open("three pump frames in a row got no pulse", 0);
            return;
        }
        g_v3_native.pump_dry = 0;
        // Rows done this frame, built or failed: pulses that move nothing mean a gate in the
        // factory refuses them, and with the queue watchdog off while the pump owns the queue
        // nothing else would ever end the open's build.
        const size_t rows = g_v3_native.pending_index >= index0 ? g_v3_native.pending_index - index0 : 0;
        if (rows == 0)
        {
            if (++g_v3_native.pump_stuck >= 3)
                v3_pump_stop_open("three pump frames in a row moved no row", 0);
            return;
        }
        g_v3_native.pump_stuck = 0;
        // Adapt on the cost of one row, from full frames only (not the short last one, whose
        // budget the queue's remainder cut), halfway toward the new figure.
        const int64_t us = park_us(dt);
        if (budget == g_v3_pump.budget && rows >= budget && us > 0)
        {
            const size_t fit = static_cast<size_t>(V3_PUMP_TARGET_US * static_cast<int64_t>(rows) / us);
            g_v3_pump.budget = (std::min)(V3_PUMP_BUDGET_MAX,
                                          (std::max)(V3_PUMP_BUDGET_MIN, (g_v3_pump.budget + fit) / 2));
        }
        if (g_v3_native.pending_index >= g_v3_native.pending.size())
            g_v3_native.pump_capped = false;
    }

    // v3_record_candidate() stood here: it retained a growing display list's wrapper the first
    // time that list passed 20 and 1000 entries, so the real marker parent could be picked out
    // after the build settled. That question is long answered - the parent arrives through
    // v3_note_movie_attach - and the table it filled had no reader left. Removed 2026-07-31.

    constexpr float V3_HIDDEN_MAP_POS = -100000.0f;

    // Where the colour transform sits inside the render-node data, learned from the first
    // child built (see v3_find_cx_offset). 0 = not proven -> no colour lever at all.
    // The window below is only the range the one-shot scan reports identity runs over, for
    // the log: the offset itself is anchored on the MATRIX, not on an identity run.
    constexpr size_t V3_CX_SCAN_FIRST = 0x10;
    constexpr size_t V3_CX_SCAN_LAST = 0x100;
    size_t g_v3_cx_offset = 0;
    bool g_v3_cx_searched = false;

    // ── Location emphasis ───────────────────────────────────────────────────────────
    // A dungeon lies UNDER the overworld, so its markers are drawn on the same patch of
    // map as the surface ones around its entrance. Which icon belongs to the cave you are
    // standing in is then only answerable by hovering every one of them. Both sides of the
    // comparison are already here: a marker carries the map it belongs to (the
    // WorldMapPointParam area/grid triple) and the player carries one too.
    //
    // The whole overworld counts as ONE location - a marker two tiles away is still "out
    // here with me", and a per-tile rule would flicker the emphasis every time the player
    // walked over a tile seam. Each interior map is its own location, so a cave, the cave
    // next door and the surface above them all read differently from each other.
    bool v3_same_location(uint8_t a1, uint16_t x1, uint16_t z1,
                          uint8_t a2, uint16_t x2, uint16_t z2)
    {
        const bool over1 = (a1 == 60 || a1 == 61);
        const bool over2 = (a2 == 60 || a2 == 61);
        if (over1 || over2) return over1 && over2 && a1 == a2;
        return a1 == a2 && x1 == x2 && z1 == z2;
    }

    // Is the emphasis answerable at all? Off, or a player map we could not read, means an
    // unresolved read can only ever leave the map looking exactly as it did before this
    // feature existed.
    bool v3_emphasis_possible()
    {
        return goblin::config::locationEmphasis && g_v3_native.player_map != 0;
    }

    // Does this marker belong to the map the player is standing in? Pure comparison - used
    // to decide whether the emphasis has anything to say on this tab in the first place.
    bool v3_own_raw(uint8_t area, uint16_t gx, uint16_t gz)
    {
        const uint32_t pm = g_v3_native.player_map;
        return v3_same_location(area, gx, gz,
                                static_cast<uint8_t>((pm >> 24) & 0xFF),
                                static_cast<uint16_t>((pm >> 16) & 0xFF),
                                static_cast<uint16_t>((pm >> 8) & 0xFF));
    }

    bool v3_is_own_location(uint8_t area, uint16_t gx, uint16_t gz)
    {
        return v3_emphasis_possible() && g_v3_native.emph_active &&
               v3_own_raw(area, gx, gz);
    }

    // Size factor for one marker against the player's current map.
    float v3_emphasis(uint8_t area, uint16_t gx, uint16_t gz)
    {
        if (!v3_emphasis_possible() || !g_v3_native.emph_active)
            return 1.0f;
        const float f = v3_is_own_location(area, gx, gz)
                            ? goblin::config::locationEmphasisOwnScale
                            : goblin::config::locationEmphasisOtherScale;
        // A bad ini value must not park markers on top of each other or blow them up
        // across the whole screen; clamp to a range that still reads as "an icon".
        return std::clamp(f, 0.35f, 2.5f);
    }

    // Colour factor for one marker. The player's own map keeps the icon untouched; every
    // other map fades. Clamped away from 0 so a marker can never be faded into invisibility
    // - the whole feature's contract is that nothing is ever hidden.
    float v3_fade(uint8_t area, uint16_t gx, uint16_t gz)
    {
        // No proven colour offset means no colour at all - and saying so HERE keeps every
        // caller consistent, so nothing ends up re-writing transforms every merge chasing
        // a fade that can never be applied.
        if (g_v3_cx_offset == 0 || !v3_emphasis_possible() || !g_v3_native.emph_active ||
            v3_is_own_location(area, gx, gz))
            return 1.0f;
        return std::clamp(goblin::config::locationEmphasisOtherFade, 0.2f, 1.0f);
    }

    // How far this marker's hue is pushed cold, on top of the fade. Same gating as the
    // fade, so an own-location marker is never touched at all.
    float v3_cool(uint8_t area, uint16_t gx, uint16_t gz)
    {
        if (g_v3_cx_offset == 0 || !v3_emphasis_possible() || !g_v3_native.emph_active ||
            v3_is_own_location(area, gx, gz))
            return 0.0f;
        return std::clamp(goblin::config::locationEmphasisOtherCool, 0.0f, 1.0f);
    }

    // ── Colour: the second emphasis lever ───────────────────────────────────────────
    // Scaleform GFx SDK 4.0 (Src/Render/Render_TreeNode.h, Src/GFx/GFx_DisplayObject.h)
    // settles what the vtable could not: DisplayObjectBase::Get/SetCxform are NOT virtual
    // - they forward to the render node, `GetWritableData(Change_CxForm)->Cx = cx`. So the
    // colour is a field of the same node data our matrix already goes into, not a vtable
    // slot, and no amount of dumping slots would ever have found it.
    //
    // The SDK declares Cxform as `float M[4][2]` = [R,G,B,A][mult,add], but THIS build
    // stores it the other way round - four multipliers, then four addends, the shape the
    // SDK's own GetAsFloat2x4() produces (newer Scaleform keeps it that way for SIMD).
    // Dumped live at nodeData+0x50: 1,1,1,1 / 0,0,0,0. Searching for the DECLARED
    // interleaving finds nothing at all, which is exactly what the first attempt did.
    //
    // The offset is FOUND rather than hardcoded - a game patch would move it, and the
    // surrounding layout is this build's own business - but it is found off the matrix we
    // wrote ourselves, not off the identity run (see v3_find_cx_offset). Anything less than
    // one unambiguous hit means "not proven": the tint stays off and size and order do the job.
    // The render side of a marker child is NOT at child+0x80: that field is the SDK's
    // `pRenNode` and a live dump showed it NULL for every one of ours (which is why the
    // matrix goes through the display object's fallback branch instead). It is the
    // Context::Entry at `child+0x48`, and the node data behind it is reached by pure
    // arithmetic - Scaleform packs entries 0x48 bytes apart inside 4K pages starting at
    // page+0x30, and the data pointer for entry i lives at `*(page+0x20) + 0x28 + i*8`.
    // That is GetReadOnlyData inlined; safe to mirror for READS. Live check on the first
    // child: entry 0x...198, (0x198 - 0x30) / 0x48 = 5 exactly.
    uintptr_t v3_node_data_ro(uintptr_t child)
    {
        uint64_t entry = 0;
        if (!v3_read64(child + 0x48, entry) || !v3_heap_ptr(entry)) return 0;
        const uint64_t page = entry & ~0xFFFull;
        const uint64_t off = entry - page;
        // Refuse anything that is not exactly on an entry slot rather than compute a
        // plausible-looking pointer out of a field that turned out to be something else.
        if (off < 0x30 || ((off - 0x30) % 0x48) != 0) return 0;
        uint64_t arr = 0, data = 0;
        if (!v3_read64(page + 0x20, arr) || !v3_heap_ptr(arr)) return 0;
        if (!v3_read64(arr + 0x28 + ((off - 0x30) / 0x48) * 8, data) || !v3_heap_ptr(data))
            return 0;
        return static_cast<uintptr_t>(data);
    }

    // The WRITABLE node data. Not mirrored: GetWritableData does copy-on-write and
    // registers the change with the context, which is how the renderer learns anything
    // happened - reimplementing that would be reimplementing the snapshot machinery.
    // Resolved by AOB; a miss simply leaves the colour lever off.
    using V3GetWritableDataFn = void *(void *entry, uint32_t change_flags);
    V3GetWritableDataFn *g_v3_get_writable_data = nullptr;
    constexpr uint32_t V3_CHANGE_CXFORM = 0x2;  // Render_Constants.h Change_CxForm

    uintptr_t v3_node_data_rw(uintptr_t child)
    {
        if (!g_v3_get_writable_data) return 0;
        uint64_t entry = 0;
        if (!v3_read64(child + 0x48, entry) || !v3_heap_ptr(entry)) return 0;
        const uint64_t off = entry - (entry & ~0xFFFull);
        if (off < 0x30 || ((off - 0x30) % 0x48) != 0) return 0;
        void *data = nullptr;
        __try
        {
            data = g_v3_get_writable_data(reinterpret_cast<void *>(entry),
                                          V3_CHANGE_CXFORM);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            data = nullptr;
        }
        return v3_heap_ptr(reinterpret_cast<uint64_t>(data))
                   ? reinterpret_cast<uintptr_t>(data)
                   : 0;
    }

    bool v3_read_matrix(uintptr_t node, float *m8);  // defined with the other matrix helpers

    void v3_find_cx_offset(uintptr_t child)
    {
        g_v3_cx_searched = true;
        const uintptr_t node = v3_node_data_ro(child);
        if (!node)
        {
            spdlog::info("[v3native] colour: no node data behind child 0x{:X} "
                         "(entry at +0x48 did not resolve); emphasis keeps to size and order",
                         child);
            return;
        }
        uint8_t buf[V3_CX_SCAN_LAST + 32]{};
        if (!v3_read_bytes(node, buf, sizeof buf))
        {
            spdlog::info("[v3native] colour: node data 0x{:X} unreadable", node);
            return;
        }
        // An identity colour transform is 1,1,1,1,0,0,0,0 - a common enough run of bytes
        // that a live node offered TWO candidates (+0x50 and +0xF0). So it is not the
        // anchor. The MATRIX is: we wrote it ourselves, its translation is a pair of large
        // distinctive twip values, and the colour sits a fixed distance behind it -
        // Matrix3F (12 floats) then StateBag, per the SDK's member order. Find the matrix,
        // step over it, and only accept what is sitting there if it IS an identity.
        float mine[8]{};
        size_t mat_off = 0, mat_hits = 0;
        if (v3_read_matrix(child, mine))
            for (size_t off = 0; off + sizeof mine <= sizeof buf; off += 4)
                if (memcmp(buf + off, mine, sizeof mine) == 0)
                {
                    mat_off = off;
                    ++mat_hits;
                }

        std::string found;
        for (size_t off = V3_CX_SCAN_FIRST; off <= V3_CX_SCAN_LAST; off += 4)
        {
            float m[8]{};
            memcpy(m, buf + off, sizeof m);
            bool identity = true;
            for (int i = 0; i < 8 && identity; ++i)
                identity = (i < 4) ? m[i] == 1.0f : m[i] == 0.0f;
            if (!identity) continue;
            char b[16];
            snprintf(b, sizeof b, " +0x%zX", off);
            found += b;
        }

        // 0x30 = Matrix3F's own 12 floats, 0x10 = the StateBag between it and the colour.
        constexpr size_t V3_MATRIX_TO_CX = 0x30 + 0x10;
        const size_t cand = mat_hits == 1 ? mat_off + V3_MATRIX_TO_CX : 0;
        bool ok = false;
        if (cand && cand + 32 <= sizeof buf)
        {
            float m[8]{};
            memcpy(m, buf + cand, sizeof m);
            ok = true;
            for (int i = 0; i < 8 && ok; ++i)
                ok = (i < 4) ? m[i] == 1.0f : m[i] == 0.0f;
        }
        if (ok && g_v3_get_writable_data)
        {
            g_v3_cx_offset = cand;
            spdlog::info("[v3native] colour transform at nodeData+0x{:X} (matrix at +0x{:X};"
                         " identity runs seen:{})",
                         cand, mat_off, found.empty() ? std::string(" none") : found);
        }
        else
        {
            spdlog::info("[v3native] colour transform not usable (matrix matches={} at "
                         "+0x{:X}, identity runs:{}, writable getter {}); emphasis keeps to "
                         "size and order",
                         mat_hits, mat_off,
                         found.empty() ? std::string(" none") : found,
                         g_v3_get_writable_data ? "ok" : "MISSING");
        }
    }

    // Write one marker's colour. `fade` scales all four multipliers: our icons are
    // PREMULTIPLIED alpha, so scaling colour and alpha by the same number is exactly a
    // fade - scaling alpha alone would leave the colour too strong. 1.0 restores the
    // untouched icon. The caller re-writes the matrix right after, which is what runs the
    // node's change bookkeeping.
    bool v3_write_cxform(uintptr_t child, float fade, float cool)
    {
        if (g_v3_cx_offset == 0) return false;
        // Same dead-generation guard the matrix write carries: a child our own reference
        // kept alive across a map close, whose block the engine has since freed and handed
        // to something else, passes every heap check - and writing into it corrupts
        // whatever now lives there.
        uint64_t vt = 0;
        if (!v3_read64(child, vt) || vt == 0 ||
            (g_v3_native.child_vtable != 0 && vt != g_v3_native.child_vtable))
            return false;
        const uintptr_t node = v3_node_data_rw(child);
        if (!node) return false;
        // Multipliers are [R, G, B, A]. Alpha carries the fade; holding blue while dropping
        // red and green is the nearest a per-channel transform gets to "washed out", and it
        // reads as cold/distant rather than grey. The weights are picked so the shift is
        // visible without turning an icon blue: at cool = 1 red keeps 0.65 and green 0.85.
        // Addends stay ZERO - the art is premultiplied, so a positive addend would light up
        // the transparent part of every icon quad, not just the drawn pixels.
        const float r = fade * (1.0f - 0.35f * cool);
        const float g = fade * (1.0f - 0.15f * cool);
        const float cx[8] = {r, g, fade, fade, 0.0f, 0.0f, 0.0f, 0.0f};
        return v3_write_bytes(node + g_v3_cx_offset, cx, sizeof cx);
    }

    // Read a child's matrix back through the engine's own getter. POD-only frame so the SEH is
    // legal: this is a deliberate call into engine code, which no address check can pre-validate.
    bool v3_matrix_readback(void *child, uint64_t get_addr, float *out8)
    {
        using GetMatrixFn = const float *(void *);
        __try
        {
            const float *cur = reinterpret_cast<GetMatrixFn *>(get_addr)(child);
            if (!cur)
                return false;
            memcpy(out8, cur, sizeof(float) * 8);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool v3_position_child(uintptr_t child, float map_x, float map_y,
                           float base_tx, float base_ty,
                           const float *basis = nullptr,
                           float fx = 1.0f, float fy = 1.0f)
    {
        using GetMatrixFn = const float *(void *);
        using SetMatrixFn = void(void *, const float *);
        uint64_t vt = 0, get_addr = 0, set_addr = 0;
        if (!v3_read64(child, vt) ||
            !v3_read64(static_cast<uintptr_t>(vt) + 0x10, get_addr) ||
            !v3_read64(static_cast<uintptr_t>(vt) + 0x18, set_addr) ||
            !v3_heap_ptr(get_addr) || !v3_heap_ptr(set_addr))
            return false;
        // Dead-generation guard: adopt the first live child's vtable as this generation's
        // signature and refuse any child that does not carry it. A child kept alive by our own
        // reference across a close, whose block the engine has since freed and handed to something
        // else, passes the heap checks above - and writing a matrix into it corrupts whatever now
        // lives there. Cleared by the manager reset.
        if (g_v3_native.child_vtable == 0)
            g_v3_native.child_vtable = vt;  // reported by the tick (no spdlog in an SEH leaf)
        else if (vt != g_v3_native.child_vtable)
            return false;
        // Gate B1. The vtable above proves the block is one of ours; this proves the engine can
        // actually complete the SetMatrix we are about to ask for. 8,946 of 50,823 vtable-correct
        // children in the hang dump would have failed here.
        if (!v3_node_context_ok(child))
            return false;

        float matrix[8]{};
        bool ok = false;
        __try
        {
            auto *get_matrix = reinterpret_cast<GetMatrixFn *>(get_addr);
            auto *set_matrix = reinterpret_cast<SetMatrixFn *>(set_addr);
            const float *current = get_matrix(reinterpret_cast<void *>(child));
            memcpy(matrix, current, sizeof(matrix));
            // Restore the authored centring translation captured before staging;
            // do not assume the source timeline left the offscreen matrix intact.
            // fx/fy is the sampled native-widget scale: the native icon chain is
            // layer * widgetMatrix * authoredBasis, our raw child collapses that
            // to layer * (widgetScale * authoredBasis). The map anchor lives in
            // parent (map) space and must NOT be scaled; the centring pivot and
            // the authored 2x2 basis are icon-local and scale with the widget.
            matrix[3] = base_tx * fx + map_x * 20.0f;
            matrix[7] = base_ty * fy + map_y * 20.0f;
            if (basis)
            {
                matrix[0] = basis[0] * fx;
                matrix[1] = basis[1] * fx;
                matrix[4] = basis[2] * fy;
                matrix[5] = basis[3] * fy;
            }
            set_matrix(reinterpret_cast<void *>(child), matrix);
            ok = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ok = false;
        }
        return ok;
    }

    bool v3_stage_source_child(uintptr_t child, float &base_tx, float &base_ty,
                               float *basis = nullptr)
    {
        using GetMatrixFn = const float *(void *);
        using SetMatrixFn = void(void *, const float *);
        uint64_t vt = 0, get_addr = 0, set_addr = 0;
        if (!v3_read64(child, vt) ||
            !v3_read64(static_cast<uintptr_t>(vt) + 0x10, get_addr) ||
            !v3_read64(static_cast<uintptr_t>(vt) + 0x18, set_addr) ||
            !v3_heap_ptr(get_addr) || !v3_heap_ptr(set_addr))
            return false;

        bool ok = false;
        __try
        {
            auto *get_matrix = reinterpret_cast<GetMatrixFn *>(get_addr);
            auto *set_matrix = reinterpret_cast<SetMatrixFn *>(set_addr);
            float matrix[8]{};
            memcpy(matrix, get_matrix(reinterpret_cast<void *>(child)), sizeof(matrix));
            base_tx = matrix[3];
            base_ty = matrix[7];
            if (basis)
            {
                basis[0] = matrix[0];
                basis[1] = matrix[1];
                basis[2] = matrix[4];
                basis[3] = matrix[5];
            }
            constexpr float STAGING_TWIPS = -2000000.0f;
            matrix[3] += STAGING_TWIPS;
            matrix[7] += STAGING_TWIPS;
            set_matrix(reinterpret_cast<void *>(child), matrix);
            ok = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ok = false;
        }
        return ok;
    }

    void v3_native_merge_snapshot(const std::vector<goblin::NativeMarkerPoint> &snapshot,
                                  bool initial);

    // The REAL Scaleform parent of our transplanted children: the manager's
    // attach parent is a C++/AS-side object whose +0x28/+0x30 is NOT a display
    // list (the first probe pass silently bailed on it). Our children carry a
    // backref to the true GFx sprite at child+0x38 - its +0x28/+0x30 IS the
    // display list holding both our children and the native icon siblings.
    uintptr_t v3_native_gfx_parent()
    {
        for (const auto &obj : g_v3_native.objects)
        {
            if (!v3_heap_ptr(obj.child)) continue;
            uint64_t par = 0;
            if (v3_read64(obj.child + 0x38, par) && v3_heap_ptr(par))
                return static_cast<uintptr_t>(par);
        }
        return 0;
    }

    // GetMatrix (DisplayObject vtbl+0x10) into m8[8], SEH-guarded.
    // Debug, one-shot: print a marker child and the object its matrix lives in, as bytes.
    // The colour transform is a run of four 1.0f and four 0.0f (0x3F800000 / 0x00000000)
    // somewhere in there while the icon is untinted; the byte view is what says WHERE,
    // which no amount of reading the vtable answered - the display-object accessors carry
    // no symbols and slots 2..11 turned out to be matrix/projection/view, not colour.
    void v3_dump_child_layout(uintptr_t child)
    {
        constexpr size_t N = 0x120;
        uint8_t buf[N]{};
        uint64_t entry = 0;
        v3_read64(child + 0x48, entry);
        const uintptr_t node = v3_node_data_ro(child);
        for (int pass = 0; pass < 3; ++pass)
        {
            const uintptr_t base = pass == 0   ? child
                                   : pass == 1 ? static_cast<uintptr_t>(entry)
                                               : node;
            const char *what = pass == 0 ? "child" : pass == 1 ? "entry" : "nodeData";
            if (!base || !v3_read_bytes(base, buf, N))
            {
                spdlog::info("[v3dump] {} 0x{:X}: unreadable", what, base);
                continue;
            }
            for (size_t off = 0; off < N; off += 32)
            {
                std::string line;
                for (size_t i = 0; i < 32; ++i)
                {
                    char b[4];
                    snprintf(b, sizeof(b), "%02X", buf[off + i]);
                    line += b;
                    if ((i & 3) == 3) line += ' ';
                }
                spdlog::info("[v3dump] {} +0x{:02X}: {}", what, off, line);
            }
        }
    }

    bool v3_read_matrix(uintptr_t node, float *m8)
    {
        uint64_t vt = 0, get_addr = 0;
        if (!v3_read64(node, vt) ||
            !v3_read64(static_cast<uintptr_t>(vt) + 0x10, get_addr) ||
            !v3_heap_ptr(get_addr))
            return false;
        bool ok = false;
        __try
        {
            using GetMatrixFn = const float *(void *);
            const float *cur = reinterpret_cast<GetMatrixFn *>(get_addr)(
                reinterpret_cast<void *>(node));
            memcpy(m8, cur, sizeof(float) * 8);
            ok = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ok = false;
        }
        return ok;
    }

    bool v3_native_node_is_ours(uint64_t node)
    {
        // The [v3pump] generator is a sprite-171 child of the parent that no engine scale ever
        // reaches: the widget sampler must never take it for a native pin.
        if (node != 0 && node == g_v3_native.pump_gen)
            return true;
        for (const auto &obj : g_v3_native.objects)
            if (obj.child == node)
                return true;
        return false;
    }

    // Read the live 2x2 the engine wrote into a REAL native pin node of our
    // target parent. The parent is a sprite: display list base @+0x28, count
    // @+0x30, exactly the layout create_native_icon_instance scans (our own
    // transplanted children live in the same list - skipped by pointer).
    bool v3_native_sample_widget(float &fx, float &fy)
    {
        const uintptr_t gparent = v3_native_gfx_parent();
        uint64_t base = 0, count = 0;
        if (!gparent ||
            !v3_read64(gparent + 0x28, base) ||
            !v3_read64(gparent + 0x30, count) ||
            !v3_heap_ptr(base) || count == 0 || count > 65536)
            return false;
        const uint64_t scan = count < 24 ? count : 24;
        // The node sampled last time, if it still sits in the scanned slots. Membership in
        // the CURRENT list is the liveness test - a node that left it (teardown, layer
        // retarget) must not be dereferenced, and a node still in it is the same real
        // widget as last tick. This keeps the steady state at a few dozen qword reads;
        // the discovery below, with its full-population membership test per candidate,
        // runs only on the first tick and after the sampled node dies.
        if (const uint64_t cached = g_v3_native.sample_node)
        {
            bool present = false;
            for (uint64_t i = 0; i < scan && !present; ++i)
            {
                uint64_t node = 0;
                present = v3_read64(base + i * 8, node) && node == cached;
            }
            if (present)
            {
                float m[8]{};
                if (v3_read_matrix(static_cast<uintptr_t>(cached), m))
                {
                    const float sx = m[0] < 0.0f ? -m[0] : m[0];
                    const float sy = m[5] < 0.0f ? -m[5] : m[5];
                    if (sx > 0.0001f && sx < 1000.0f && sy > 0.0001f && sy < 1000.0f)
                    {
                        fx = sx;
                        fy = sy;
                        return true;
                    }
                }
            }
            g_v3_native.sample_node = 0; // left the list or unreadable: rediscover
        }
        for (uint64_t i = 0; i < scan; ++i)
        {
            uint64_t node = 0;
            if (!v3_read64(base + i * 8, node) || !v3_heap_ptr(node) ||
                v3_native_node_is_ours(node))
                continue;
            float m[8]{};
            if (!v3_read_matrix(static_cast<uintptr_t>(node), m))
                continue;
            const float sx = m[0] < 0.0f ? -m[0] : m[0];
            const float sy = m[5] < 0.0f ? -m[5] : m[5];
            if (sx > 0.0001f && sx < 1000.0f && sy > 0.0001f && sy < 1000.0f)
            {
                g_v3_native.sample_node = node;
                fx = sx;
                fy = sy;
                return true;
            }
        }
        return false;
    }

    // One-line-per-node probe of where the engine writes the zoom adaptation:
    // logs the node-level and first-child-level 2x2 of the first two native
    // pins plus our first object. Called on >2% zoom moves (debug only), so a
    // single min->max zoom sweep in game pins the level and the exact curve.
    void v3_native_dump_probe(float zoom)
    {
        const uintptr_t gparent = v3_native_gfx_parent();
        uint64_t base = 0, count = 0;
        if (!gparent ||
            !v3_read64(gparent + 0x28, base) ||
            !v3_read64(gparent + 0x30, count) ||
            !v3_heap_ptr(base) || count == 0 || count > 65536)
        {
            static bool bail_logged = false;
            if (!bail_logged)
            {
                bail_logged = true;
                spdlog::info("[v3probe] no readable gfx parent (gp=0x{:X} base=0x{:X} "
                             "count={}) - dump unavailable this session",
                             gparent, base, count);
            }
            return;
        }
        int dumped = 0;
        const uint64_t scan = count < 24 ? count : 24;
        for (uint64_t i = 0; i < scan && dumped < 2; ++i)
        {
            uint64_t node = 0;
            if (!v3_read64(base + i * 8, node) || !v3_heap_ptr(node) ||
                v3_native_node_is_ours(node))
                continue;
            float m[8]{};
            if (!v3_read_matrix(static_cast<uintptr_t>(node), m))
                continue;
            uint32_t depth = 0;
            v3_read32(static_cast<uintptr_t>(node) + 0x14, depth);
            uint64_t cbase = 0, ccount = 0, child0 = 0;
            v3_read64(static_cast<uintptr_t>(node) + 0x28, cbase);
            v3_read64(static_cast<uintptr_t>(node) + 0x30, ccount);
            if (v3_heap_ptr(cbase) && ccount > 0 && ccount < 4096)
                v3_read64(cbase, child0);
            float c[8]{};
            const bool cgot =
                v3_heap_ptr(child0) && v3_read_matrix(static_cast<uintptr_t>(child0), c);
            spdlog::info("[v3probe] zoom={:.4f} native[{}] node=0x{:X} depth={} "
                         "m=({:.4f},{:.4f}) t=({:.0f},{:.0f}) kids={} "
                         "child0 m=({:.4f},{:.4f})",
                         zoom, dumped, node, depth, m[0], m[5], m[3], m[7],
                         ccount, cgot ? c[0] : 0.0f, cgot ? c[5] : 0.0f);
            ++dumped;
        }
        if (!g_v3_native.objects.empty())
        {
            const auto &obj = g_v3_native.objects.front();
            float m[8]{};
            if (v3_heap_ptr(obj.child) && v3_read_matrix(obj.child, m))
                spdlog::info("[v3probe] zoom={:.4f} ours node=0x{:X} "
                             "m=({:.4f},{:.4f}) t=({:.0f},{:.0f}) curF=({:.4f},{:.4f})",
                             zoom, obj.child, m[0], m[5], m[3], m[7],
                             g_v3_native.cur_fx, g_v3_native.cur_fy);
        }
    }

    // g_v3_native owns four heap containers (pending / objects / by_row / queued) that are freed wholesale
    // by v3_native_reset and grown from the factory pulse and the engine's place hook, with no lock
    // anywhere in this file. Whether that is a live race or accidentally single-threaded depends purely on
    // which threads reach those sites, and nothing in the code states it: the factory slots are
    // thread_local (so the author expected several threads), yet v3_native_tick gates on that same TLS (so
    // the gate only works if it is one thread). Report each site's thread once - at most a dozen lines for
    // the whole process - so the next run settles it instead of us guessing. Not gated on debug logging:
    // this is the input to a heap-corruption decision, exactly the case where a quiet build must still say.
    void v3_note_thread(const char *site)
    {
        struct SiteTid { std::atomic<const char *> site; std::atomic<uint32_t> tid; };
        static SiteTid seen[12];
        const uint32_t tid = GetCurrentThreadId();
        for (auto &s : seen)
        {
            const char *sp = s.site.load(std::memory_order_relaxed);
            if (sp == site && s.tid.load(std::memory_order_relaxed) == tid)
                return; // already reported this site/thread pair
            if (sp == nullptr)
            {
                const char *expect = nullptr;
                if (s.site.compare_exchange_strong(expect, site, std::memory_order_relaxed))
                {
                    s.tid.store(tid, std::memory_order_relaxed);
                    spdlog::info("[v3] thread map: {} runs on tid {}.", site, tid);
                }
                return;
            }
        }
    }

    // MEASURED 2026-07-27: tick, reset and the factory pulse all ran on one thread (tid 27276), and the
    // second thread seen in v3_place_detour (27080) belongs to another movie's load, where the
    // thread_local g_v3_factory_active is 0 - so it never enters the capture block and never touches the
    // manager. The four containers are therefore single-threaded IN PRACTICE, which is why they carry no
    // lock. That is an invariant of the engine's behaviour, not of our code: if a game update moves the
    // map build onto a worker, every container site races and the heap goes. So watch it instead of
    // hoping - this costs one atomic compare per mutation and says so out loud if it ever changes.
    std::atomic<uint32_t> g_v3_owner_tid{0};
    void v3_check_owner(const char *site)
    {
        const uint32_t tid = GetCurrentThreadId();
        uint32_t expect = 0;
        if (g_v3_owner_tid.compare_exchange_strong(expect, tid, std::memory_order_relaxed))
            return; // first mutator seen - this thread owns the manager
        if (expect == tid)
            return;
        static std::atomic<int> warned{0};
        if (warned.fetch_add(1, std::memory_order_relaxed) < 4)
            spdlog::error("[v3native] MANAGER TOUCHED FROM A SECOND THREAD: {} on tid {}, owner tid {}. "
                          "The unlocked containers now race - expect heap corruption.",
                          site, tid, expect);
    }

    void v3_factory_clear_request(bool drop_held_ref)
    {
        for (auto &s : g_v3_factory_slots)
        {
            if (drop_held_ref && s.held && v3_heap_ptr(s.child))
            {
                uint32_t before = 0, after = 0;
                v3_drop_held_ref(s.child, before, after);
            }
            s = V3FactorySlot{};
        }
        g_v3_factory_active = 0;
    }

    void v3_native_reset(const char *site)
    {
        // NEVER touch the abandoned children here. Under the all-layers model
        // a reset only ever happens when the manager re-anchors to a NEW
        // parent, i.e. the old movie generation is torn down or dying - its
        // objects may already be freed AND reallocated, so a SetMatrix "park"
        // writes into foreign live engine memory without faulting (SEH cannot
        // see it) and corrupts the rebuild: crash 21:39 (null-write inside the
        // engine's own map-build chain on reopen-after-freeze). Layer switches
        // no longer reset at all, so there is nothing visible to park anyway.
        // Same reasoning for held factory refs: dropping them would write a
        // refcount into the dead movie's memory.
        //
        //
        // 2026-08-03: an attempt to reclaim these children (a graveyard drained per frame through
        // the engine's own deleting destructor) was written and MEASURED, and it did nothing at
        // all - "0 destroyed, 7136 refused" on every single generation, the refusals accumulating
        // to 64,224 over eleven map opens. Whatever state the children are in by the time a drain
        // reaches them, it is not the one a release requires. It came back out; the probe below is
        // what stays, because the honest next step is to learn what that state actually is rather
        // than to guess at another release.
        v3_note_thread("v3_native_reset (frees all four containers)");
        v3_check_owner("v3_native_reset");
        // Read-only sample of this generation's children, re-checked once the NEXT generation has
        // been built. `site` matters: on 2.1.1 only the tick's re-anchor arm reset, and the sample
        // came back committed=16 vtableOk=16 refs1=16 every time. The anchor fix resets from more
        // places (gen=23 across 11 opens, i.e. two per open), so WHICH caller abandoned a
        // generation may be exactly what decides whether its children are still there.
        {
            uintptr_t sample[16] = {};
            size_t n = 0;
            const size_t total = g_v3_native.objects.size();
            if (total)
            {
                const size_t want = total < 16 ? total : 16;
                const size_t stride = total / want;
                for (size_t i = 0; i < want && n < 16; ++i)
                    sample[n++] = g_v3_native.objects[i * stride].child;
            }
            goblin::crashdiag::sample_generation(g_v3_native.child_vtable, sample, n, total, site);
        }
        goblin::crashdiag::note_generation(g_v3_native.parent, g_v3_native.wrapper);
        v3_factory_clear_request(false);
        g_v3_native = V3NativeManager{};
        goblin::map_api::reset_depths();
        v3_pulse_counters_reset(); // per generation, like the manager itself
    }

    // Drop an anchor that is provably out of the live tree, instead of holding it for
    // the rest of the session. Detach is one-way for a movie generation (pASRoot is
    // ctor-written and the dialog teardown never re-attaches its old lists), so a
    // detached anchor can only ever keep refusing - which is exactly how the 2.1.1
    // layer-2 regression stayed broken until restart. Clearing the target re-arms the
    // picker's prev_parent==0 / largest-list discovery on the very next WorldMapItem
    // attach, and the manager is reset the same way a reseed does it: the abandoned
    // children are never touched (see v3_native_reset), no row is marked failed or
    // skip-for-session, and the fresh seed's snapshot merge rebuilds the queue - so
    // the pulses of a still-running burst remain usable for the re-anchored build.
    // Only called from manager-owner contexts (seed / tick / factory pulse), like
    // v3_native_reset itself.
    void v3_drop_dead_anchor(const char *where, uintptr_t parent)
    {
        // An anchor from a screen that has since been destroyed is EXPECTED to be dead: that is
        // every quick reopen (close released the generation, the parked entry was expired, the
        // reopen's first tick finds the old list torn down - 47 of 47 in the test-install logs
        // 08-04..09-18, every one after a close with no anchor taken since). Only a death while
        // the screen we anchored on is still up - the 2.1.1 layer-2 kind - is worth a warning.
        if (g_map_screen_gone.load(std::memory_order_acquire))
            spdlog::info("[v3native] anchor 0x{:X} belonged to a closed screen ({}); dropping it "
                         "for rediscovery",
                         parent, where);
        else
            spdlog::warn("[v3native] anchor 0x{:X} left the live tree ({}); dropping it "
                         "for rediscovery",
                         parent, where);
        // If the manager's children sit on a DIFFERENT parent that is still live (the
        // target moved on and then died before we ever seeded on it), take them off
        // first, exactly like the reseed path does - resetting past a live parent is
        // what left the 13:52:34 ghost icons drawn on the overworld. When the dead
        // node IS the manager's parent the children are on the dead generation and
        // must be left alone, which is v3_native_reset's own rule.
        if (g_v3_native.seeded && !g_v3_native.objects.empty() &&
            g_v3_native.parent && g_v3_native.parent != parent &&
            !v3_node_detached(g_v3_native.parent) &&
            v3_movie_of(g_v3_native.parent) != 0)
        {
            const uint32_t removed = goblin::stall_probe::v3_detach_all_children();
            spdlog::info("[v3native] anchor drop: detached {} of {} children from the "
                         "still-live parent 0x{:X}",
                         removed, g_v3_native.objects.size(), g_v3_native.parent);
        }
        g_v3_target_wrapper.store(0, std::memory_order_relaxed);
        g_v3_target_parent.store(0, std::memory_order_relaxed);
        g_v3_target_layer.store(-1, std::memory_order_relaxed);
        g_v3_target_count.store(0, std::memory_order_release);
        v3_native_reset("drop_dead_anchor");
    }

    bool v3_native_seed_from_live_callback()
    {
        // Phase gate, and this is the one that was missing.
        //
        // This seed runs from the FACTORY PULSE, not from v3_native_tick, so the gate at the top of
        // the tick never covered it. Measured 19:02-19:03: a close released the generation, and 0.7 s
        // later - with the dialog already gone - this path reseeded onto the anchor we had kept.
        // That anchor was about to be torn down, so the burst produced nothing: "pulses seen=7876,
        // attached=0, created=0, parentCount=81" against 230 or 1526 on a healthy open, and no
        // CATEGORIES READY at all. Seconds later a real burst arrived and arm B-anchor-dead
        // re-anchored - correctly, but the open had already been spent. That is the "icons invisible
        // on a quick reopen" report, and it survived keeping the anchor because keeping the anchor
        // was never the problem: seeding onto it while no map screen exists is.
        if (!v3_map_object_alive())
        {
            g_seed_no_screen.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        // ...and the anchor must belong to THIS screen.
        //
        // Liveness alone does not decide it, because the engine's teardown is deferred by about a
        // second: 0.17 s after a close the previous parent is still attached and still resolves, so
        // every check we have passes and we seed onto a container that is about to die. Measured
        // 19:21:55 - release at close, seed 0.17 s later, "pulses seen=7876 attached=0", no
        // CATEGORIES READY, and five seconds after that arm B-anchor-dead re-anchors onto the real
        // list. By then the burst's pulse pool is spent, so the correct anchor has nothing left to
        // build with, and the player sees an empty map that fills in only on the next slow reopen.
        //
        // g_map_screen_gone is exactly the fact needed: set by the phase byte when the dialog is
        // destroyed, cleared only when an anchor is taken. While it is up, any anchor we hold is
        // from the previous screen, whatever it looks like. Waiting costs nothing - the pulses we
        // decline to waste here are the ones the real anchor uses.
        if (g_map_screen_gone.load(std::memory_order_acquire))
        {
            g_seed_epoch.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const int layer = goblin::maphover::map_layer();
        const uintptr_t wrapper = g_v3_target_wrapper.load(std::memory_order_acquire);
        const uintptr_t parent = g_v3_target_parent.load(std::memory_order_relaxed);
        const int target_layer = g_v3_target_layer.load(std::memory_order_relaxed);
        if (g_v3_native.seeded &&
            g_v3_native.wrapper == wrapper && g_v3_native.parent == parent)
        {
            // KEEP MANAGING IS ONLY LEGAL FOR A GENERATION THAT FINISHED. Carrying an
            // unfinished one across a real open/close preserves the shortfall for as long as
            // the anchor stays alive: the reopen reuses the movie, emits no burst, so nothing
            // can rebuild, and the map keeps showing the fragment. Worse, the fragment is
            // shaped: the seed sorts the player's OWN map's markers LAST so they draw on top,
            // so a build that stopped early is missing exactly those - which is what the
            // player sees ("only the dimmed, shrunken icons of other locations are there, the
            // ones where I'm standing are gone, but their popups still work").
            //
            // Measured 2026-08-05 on Convergence: a generation stopped at 1968 of 8425, the
            // next open logged NO seed and NO CATEGORIES READY at all, and the parent list
            // held 2138 entries (354 the engine's + our 1784) for minutes while the map sat
            // open. `completion_reported` is set only when CATEGORIES READY fires, so it is
            // exactly the "this generation finished" bit; `g_map_screen_gone` is set only by
            // the engine's phase byte reaching 0, so it is exactly "a real screen has come and
            // gone since we anchored". Both together mean: rebuild rather than adopt.
            if (!g_v3_native.completion_reported &&
                g_map_screen_gone.load(std::memory_order_acquire))
            {
                spdlog::warn("[v3native] carrying an UNFINISHED generation ({} of {} built) "
                             "into a new screen; dropping the anchor so the next burst can "
                             "rebuild instead of leaving the map half-drawn",
                             g_v3_native.objects.size(), g_v3_native.pending.size());
                v3_drop_dead_anchor("unfinished generation", parent);
                return false;
            }
            g_seed_managing.fetch_add(1, std::memory_order_relaxed);
            return true; // same live session (incl. quick reopen) - keep managing
        }
        // The manager survives map close. If the map closed and no fresh burst
        // re-anchored the target yet, both manager and target may point into a
        // torn-down movie - never build there. RM2 factory pulses exist ONLY
        // during a build burst (proven 12:37 session: zero creations after the
        // burst), so on a stale manager the reseed must happen INLINE below,
        // not in the map-frame tick - by then the pulses are gone.
        if (g_v3_map_closed.load(std::memory_order_relaxed))
        {
            g_seed_map_closed.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        // Liveness before the floors: a heap-valid anchor that has left the live tree
        // can never seed correctly, however many children its frozen count reports.
        // The 2.1.1 layer-2 runs proved the shape: the OLD view's pin list kept 105
        // children through the ~1 s deferred teardown, passed every floor, and every
        // child we then created was rejected against it. Drop it outright (see
        // v3_drop_dead_anchor) rather than warn-and-hold - the count of a detached
        // list never changes, so a plain refusal here would repeat forever.
        if (v3_heap_ptr(wrapper) && v3_heap_ptr(parent) &&
            (v3_node_detached(parent) || v3_movie_of(parent) == 0))
        {
            v3_drop_dead_anchor("seed", parent);
            return false;
        }
        uint64_t wrapper_parent = 0, live_count = 0;
        // Enough WorldMapItems that this is unambiguously the active native marker parent, while
        // most of the stock build burst's sprite-171 callbacks still lie ahead to act as factory
        // pulses. The old value was 100, chosen on the BASE map where the parent blows past it in
        // the first moments of the burst. On the DLC map (layer 2) the same list only ever reaches
        // ~105, so 100 was satisfied - if at all - at the very END of the burst: measured 2026-08-01
        // in m61, this seed refused at liveCount 81 and 82 while pulses were live, and the parent
        // only reached 105 later, by which time the map-frame tick reseeded with no pulses left and
        // CATEGORIES READY reported created=0. Hence a threshold that fires EARLY in a ~105 burst.
        // It is still far above the "tiny side clip" case the target picker guards against, and the
        // identity checks below (export name upstream, wrapper+0x18 == parent) do the real work.
        constexpr uint64_t kSeedMinItems = kSeedMinItemsValue;
        if (layer < 0 || layer > 2 ||
            !v3_heap_ptr(wrapper) || !v3_heap_ptr(parent) ||
            !v3_read64(wrapper + 0x18, wrapper_parent) || wrapper_parent != parent ||
            !v3_read64(parent + 0xe0, live_count) || live_count < kSeedMinItems)
        {
            // A refusal here is INVISIBLE downstream: nothing seeds, so build_started_ms and
            // last_progress_ms stay 0 and the queue watchdog never fires either - the DLC map
            // then reports no icons and no warning at all. Say it once per generation, with the
            // numbers, so "we anchored to a parent we will never seed on" is readable.
            static uintptr_t s_last_parent = 0;
            if (parent != s_last_parent)
            {
                s_last_parent = parent;
                spdlog::warn("[v3native] seed refused: layer={} wrapper=0x{:X} parent=0x{:X} "
                             "wrapperParent=0x{:X} liveCount={} (needs >={})",
                             layer, wrapper, parent, wrapper_parent, live_count,
                             kSeedMinItems);
            }
            g_seed_small.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        g_seed_ok.fetch_add(1, std::memory_order_relaxed);
        if (target_layer < 0)
            // The burst ran before map_layer() resolved; stamp the live layer
            // (informational only - the shared parent hosts every layer).
            g_v3_target_layer.store(layer, std::memory_order_relaxed);

        // Take the OLD generation's children off the OLD parent before the reset drops every
        // reference to them. v3_native_reset deliberately never touches them, on the reasoning
        // that a reset only happens once the previous movie is torn down - which was true while
        // "layer switches no longer reset at all". It is not true here: the layer-switch arm of
        // the target picker re-anchors to a different parent, so opening the map inside the DLC
        // and switching to the Lands Between resets while the old parent is still very much
        // alive, and its children stay drawn - inert pictures with no popup, because the popups
        // belong to the game's own pins and never to ours. Measured on ERR 2026-08-01: the
        // 13:52:34 switch retargeted, reseeded, and left the DLC icons on the overworld map.
        // Guarded on the old parent still carrying the vtable we saw it alive with, so the
        // torn-down case still takes the old path of leaving freed memory alone.
        if (g_v3_native.seeded && !g_v3_native.objects.empty() &&
            g_v3_native.parent && g_v3_native.parent != parent)
        {
            // Liveness WITHOUT g_v3_native.parent_vtable: that field is only ever written inside
            // the map-closed branch of the tick, so on a layer switch (mapClosed=false) it is
            // still 0 and any test against it silently refuses - measured on ERR 2026-08-01, the
            // detach never ran and printed nothing. Both parents are WorldMapItem list objects of
            // the same class, so a live old one carries the SAME vtable as the new one; a freed
            // one does not. That test needs no state we might have failed to seed.
            uint64_t old_vt = 0, new_vt = 0;
            const bool old_parent_live =
                v3_read64(g_v3_native.parent, old_vt) && old_vt != 0 &&
                v3_read64(parent, new_vt) && new_vt != 0 && old_vt == new_vt;
            if (old_parent_live)
            {
                const uint32_t removed = goblin::stall_probe::v3_detach_all_children();
                spdlog::info("[v3native] re-anchor: detached {} of {} children from the previous "
                             "parent 0x{:X} before reseeding on 0x{:X}",
                             removed, g_v3_native.objects.size(), g_v3_native.parent, parent);
            }
            else
                // Say so too. A silent skip here is what cost a whole test run.
                spdlog::info("[v3native] re-anchor: leaving {} children on parent 0x{:X} "
                             "(vtable {:X} vs new {:X}) - it does not read as live",
                             g_v3_native.objects.size(), g_v3_native.parent, old_vt, new_vt);
        }
        v3_native_reset("re-anchor");
        g_v3_native.layer = layer;
        g_v3_native.wrapper = wrapper;
        g_v3_native.parent = parent;
        g_v3_native.observed_count = live_count;
        g_v3_native.seeded = true;
        g_v3_native.build_started_ms = GetTickCount64();
        g_v3_native.next_refresh_ms = g_v3_native.build_started_ms + 200;
        g_v3_native.last_progress_ms = g_v3_native.build_started_ms;
        // ┌─ REMOVABLE: location-emphasis DRAW ORDER, seed-time player map ──────────────┐
        // │ Delete this block and nothing else needs touching - the feature reverts to   │
        // │ what it did before 2026-07-31, which is nothing.                             │
        // │                                                                              │
        // │ The seed sort in v3_native_merge_snapshot puts the player's OWN map's        │
        // │ markers last (= on top). It decides that through v3_is_own_location, which    │
        // │ is gated on v3_emphasis_possible() = locationEmphasis && player_map != 0.     │
        // │ player_map has exactly ONE other assignment in this file, in v3_native_tick - │
        // │ and v3_native_reset() three lines above has just zeroed it. So at sort time   │
        // │ the test was false for EVERY marker and the ordering half of the emphasis     │
        // │ had never once applied. Measured 2026-07-31: the seed logged at 10:11:52.976, │
        // │ the player map first read at 10:11:53.051 - 75 ms too late, every open.       │
        // │ Size and colour were never affected: the tick re-applies those once it reads  │
        // │ the map. Depth cannot be re-applied - it is fixed when a child is created.    │
        // │                                                                              │
        // │ emph_active is deliberately NOT set here: merge_snapshot computes it from     │
        // │ the snapshot at the top of the same call, before the sort, and it only needs  │
        // │ player_map to be non-zero to come out right.                                  │
        {
            uint32_t seed_player_map = 0;
            if (goblin::config::locationEmphasis &&
                goblin::collected::read_player_map_id(seed_player_map))
                g_v3_native.player_map = seed_player_map;
            // Logged BEFORE the sort runs, so the order of the lines in the log is the proof:
            // this must appear ahead of "item categories", and the tick's own map line after it.
            // A zero here means the read failed and the ordering is inert again, exactly as before.
            spdlog::info("[v3native] seed map for draw order: m{:02d}_{:02d}_{:02d}_{:02d}",
                         (g_v3_native.player_map >> 24) & 0xFF,
                         (g_v3_native.player_map >> 16) & 0xFF,
                         (g_v3_native.player_map >> 8) & 0xFF,
                         g_v3_native.player_map & 0xFF);
        }
        // └─ end removable block ────────────────────────────────────────────────────────┘
        v3_native_merge_snapshot(goblin::native_marker_snapshot(layer, true), true);
        // Initial construction happens across exact sprite-171 ExecuteTag calls
        // in this same stock build burst, before the first map-frame callback.
        g_v3_native.frame_budget = g_v3_native.pending.size();
        return true;
    }

    // One marker's final basis factor: the widget scale the whole layer follows, times
    // this marker's location emphasis. Both ride the SAME multiplier on purpose - the
    // centring pivot inside v3_position_child is multiplied by it too, so an icon grows
    // and shrinks around its own point instead of walking off it.
    inline float v3_obj_fx(const V3NativeObject &obj) { return g_v3_native.cur_fx * obj.emph; }
    inline float v3_obj_fy(const V3NativeObject &obj) { return g_v3_native.cur_fy * obj.emph; }

    void v3_native_set_visible(V3NativeObject &obj, bool visible)
    {
        if (!v3_heap_ptr(obj.child) || obj.visible == visible) return;
        if (v3_position_child(obj.child,
                              visible ? obj.map_x : V3_HIDDEN_MAP_POS,
                              visible ? obj.map_z : V3_HIDDEN_MAP_POS,
                              obj.base_tx, obj.base_ty,
                              obj.base_m, v3_obj_fx(obj), v3_obj_fy(obj)))
            obj.visible = visible;
    }

    // Re-apply an object's transform in place: same position and visibility, whatever
    // emphasis and widget scale say NOW. Used when the player's location changes under
    // an open map (or the emphasis is toggled), where nothing about the marker moved.
    void v3_native_reapply(V3NativeObject &obj)
    {
        if (!v3_heap_ptr(obj.child)) return;
        // Colour first, matrix second, deliberately: the matrix goes in through the display
        // object's own setter, so whatever change bookkeeping that setter does runs AFTER
        // the colour landed in the same node data.
        const float want_fade = obj.is_ring ? 1.0f : v3_fade(obj.area, obj.gx, obj.gz);
        const float want_cool = obj.is_ring ? 0.0f : v3_cool(obj.area, obj.gx, obj.gz);
        if ((want_fade != obj.fade || want_cool != obj.cool) &&
            v3_write_cxform(obj.child, want_fade, want_cool))
        {
            obj.fade = want_fade;
            obj.cool = want_cool;
        }
        v3_position_child(obj.child,
                          obj.visible ? obj.map_x : V3_HIDDEN_MAP_POS,
                          obj.visible ? obj.map_z : V3_HIDDEN_MAP_POS,
                          obj.base_tx, obj.base_ty, obj.base_m,
                          v3_obj_fx(obj), v3_obj_fy(obj));
    }

    // Move a child that already exists. Costs exactly what showing or hiding one costs - a transform
    // written to the child - because that is all any of those do; nothing is created or destroyed, so
    // this is safe outside a build burst, which creating is not.
    // Returns whether the position actually changed - a successful projection onto the same
    // spot is a no-op, and callers that count "moved" must not count those.
    bool v3_native_move(V3NativeObject &obj, float mx, float mz)
    {
        if (!v3_heap_ptr(obj.child) || (mx == obj.map_x && mz == obj.map_z)) return false;
        obj.map_x = mx;
        obj.map_z = mz;
        if (obj.visible)
            v3_position_child(obj.child, mx, mz, obj.base_tx, obj.base_ty, obj.base_m,
                              v3_obj_fx(obj), v3_obj_fy(obj));
        return true;
    }

    // Ask the converter about every non-overworld tile ONCE, at the seed, before the burst walks
    // the queue. A tile the converter has answered for is remembered by mapproject::to_map (its
    // origin), and every later point on it is placed from that even when the converter may no
    // longer be called. Without this, a tile was first met wherever the queue happened to reach
    // it - visible layer-0 rows first, then underground and DLC - and on a slow first open that
    // was past the 300 ms window: 2026-09-22 22:57 lost every m12 / m20-m45 tile and the dungeon
    // the player stood in (project=2027, 8 such first opens since 09-07), all back on a reopen.
    // One representative point per tile: 94-99 tiles per profile, each a memo hit when the same
    // point was converted before (most opens after the first) or one converter call. A tile the
    // converter declines is simply not learned - the per-marker path then declines it exactly as
    // before. Whether the learned origins were then USED shows as fromOrigin= at READY.
    uint64_t v3_tile_key(uint8_t area, uint16_t gx, uint16_t gz)
    {
        return (static_cast<uint64_t>(area) << 32) | (static_cast<uint64_t>(gx) << 16) | gz;
    }

    // `unknown` (optional) receives every non-overworld tile the converter could not be ASKED
    // about in this pass: rows there need it, and it answers only while fresh. Declined tiles are
    // left out - the converter refuses them in any frame, so nothing is gained by hurrying them.
    void v3_prelearn_tiles(std::unordered_set<uint64_t> *unknown = nullptr)
    {
        using goblin::worldmap_probe::ProjFail;
        const int64_t t0 = v3_perf_now();
        std::unordered_set<uint64_t> seen;
        uint32_t asked = 0, known = 0, declined = 0, not_asked = 0;
        for (size_t i = g_v3_native.pending_index; i < g_v3_native.pending.size(); ++i)
        {
            const auto &p = g_v3_native.pending[i];
            if (p.area == 60 || p.area == 61)
                continue;  // plain arithmetic in to_map, no converter involved
            const uint64_t tile = v3_tile_key(p.area, p.gx, p.gz);
            if (!seen.insert(tile).second)
                continue;
            ++asked;
            float mx = 0.0f, mz = 0.0f;
            ProjFail why = ProjFail::none;
            if (goblin::mapproject::to_map(p.area, p.gx, p.gz, p.px, p.pz, mx, mz, &why))
                ++known;
            else if (why == ProjFail::declined)
                ++declined;
            else
            {
                ++not_asked;
                if (unknown)
                    unknown->insert(tile);
            }
        }
        const int64_t f = v3_perf_freq();
        spdlog::info("[v3native] tile origins known before the build: {} of {} ({} declined, {} "
                     "not asked) in {} us",
                     known, asked, declined, not_asked,
                     f > 0 ? (v3_perf_now() - t0) * 1000000 / f : 0);
    }

    void v3_native_merge_snapshot(const std::vector<goblin::NativeMarkerPoint> &snapshot,
                                  bool initial)
    {
        // Master switch off: park every child we own. The snapshot itself already comes back with
        // every point invisible (that is what native_marker_snapshot's include_hidden does), so the
        // merge below would park them anyway; this also covers objects that are not in the snapshot
        // at all. What must NOT happen here is returning early. The rule this function states
        // further down is that the merge keeps every migrated row, hidden ones included, because
        // factory pulses exist only during the build burst and a row that is not created NOW can
        // never be shown by a later toggle. Skipping the merge while hidden seeded an empty
        // manager: turning the switch back on then queued the rows with no pulses left to build
        // them, the watchdog dropped the queue after 5 s, and the icons stayed gone. Reported from
        // the Deck as "master off, open map, master on -> invisible until you close the map, WAIT,
        // and reopen" - the wait being what forces a real teardown and therefore a fresh burst; a
        // quick reopen reuses the movie and emits none, which is why it did not help.
        if (goblin::icons_hidden())
            for (auto &obj : g_v3_native.objects) v3_native_set_visible(obj, false);

        // Does this tab hold any marker of the player's own map? Answered BEFORE anything
        // is sized or sorted, because both read it. The layer of the tab, not of the
        // manager: a tab switch keeps the same parent, and it is the tab that decides
        // whether "here" is on screen at all. Visibility is deliberately not part of the
        // test - collecting the last item in a cave should not flip the whole map's look.
        {
            const int shown_layer = goblin::maphover::map_layer();
            bool active = false;
            if (v3_emphasis_possible())
                for (const auto &point : snapshot)
                    if (point.layer == shown_layer &&
                        v3_own_raw(point.area, point.gx, point.gz))
                    {
                        active = true;
                        break;
                    }
            if (active != g_v3_native.emph_active)
            {
                g_v3_native.emph_active = active;
                g_v3_native.emph_dirty = true;   // the tick re-applies, a slice per frame
                g_v3_native.emph_cursor = 0;
                spdlog::info("[v3native] location emphasis {} on layer {} (player map "
                             "m{:02d}_{:02d}_{:02d}_{:02d})",
                             active ? "applies" : "has nothing here - neutral", shown_layer,
                             (g_v3_native.player_map >> 24) & 0xFF,
                             (g_v3_native.player_map >> 16) & 0xFF,
                             (g_v3_native.player_map >> 8) & 0xFF,
                             g_v3_native.player_map & 0xFF);
            }
        }

        // Focus-ring accounting for one merge. Rings are the only children that CHANGE which marker
        // they belong to, so every symptom about them is really a question about this loop: how many
        // were seen, how many were re-pointed at a marker on a different map (the only case that
        // re-decides emphasis and repaints), how many actually moved, and how many could not be
        // projected at all.
        uint32_t ring_seen = 0, ring_visible = 0, ring_other_map = 0;
        uint32_t ring_remapped = 0, ring_moved = 0, ring_unprojected = 0;
        size_t visible_count = 0;
        const size_t queued_before = g_v3_native.pending.size();
        for (const auto &point : snapshot)
        {
            if (point.visible) ++visible_count;
            const auto found = g_v3_native.by_row.find(point.original_row_id);
            if (found != g_v3_native.by_row.end())
            {
                // Icon swaps (focus glow / live-loot reveal) CANNOT recreate the
                // child after the build burst - factory pulses are gone. Keep
                // the existing image until the next movie rebuild and only
                // drive visibility (retire+recreate here used to dead-queue
                // the row and stall the refresh).
                V3NativeObject &obj = g_v3_native.objects[found->second];
                const bool ring =
                    (point.original_row_id & goblin::NATIVE_HIGHLIGHT_KEY_BIT) != 0;
                if (ring)
                {
                    ++ring_seen;
                    if (point.visible) ++ring_visible;
                    if (!v3_is_own_location(point.area, point.gx, point.gz)) ++ring_other_map;
                }
                // A ring is re-pointed at a different marker between merges, so its map
                // travels with it; a marker's own never changes. Re-deciding the emphasis
                // from the point costs a compare and keeps both correct.
                if (obj.area != point.area || obj.gx != point.gx || obj.gz != point.gz)
                {
                    obj.area = point.area;
                    obj.gx = point.gx;
                    obj.gz = point.gz;
                    obj.emph = v3_emphasis(obj.area, obj.gx, obj.gz);
                    v3_native_reapply(obj);
                    if (ring) ++ring_remapped;
                }
                // Deliberately NOT re-deciding the emphasis of every object here: a merge
                // walks all ~9500 of them, and doing a transform write for each in one call
                // is the spike that has to be spread over frames instead. Only an object
                // whose MAP changed is handled above (in practice just the focus rings,
                // which are re-pointed at other markers), and everything else is left to
                // the budgeted pass in the tick.
                // EVERY child follows its point's position, not just the focus rings. Rings were the
                // only ones that ever moved, so this used to be theirs alone - and then the live
                // de-overlap started moving markers too (a marker's spot depends on which of its
                // neighbours are visible, so isolating a category or collecting an item changes it).
                // With markers frozen, a ring would step onto the marker's new spot while the icon
                // stayed on the old one until the map was reopened. Moving costs a transform write,
                // the same as the show/hide right below it.
                float mx = 0.0f, mz = 0.0f;
                bool projected = false;
                if (goblin::mapproject::to_map(point.area, point.gx, point.gz, point.px, point.pz,
                                               mx, mz))
                {
                    projected = true;
                    // Count rings that actually CHANGED position, not every successful
                    // projection: 128 parked rings re-projecting onto their own spot printed
                    // `moved=128` every merge and the "static map stays quiet" goal was gone.
                    const bool did_move = v3_native_move(obj, mx, mz);
                    if (ring && did_move) ++ring_moved;
                }
                else if (ring)
                {
                    // to_map goes through the engine converter for anything outside the overworld,
                    // and that call is now gated on a freshness stamp. If rings pile up here, the
                    // gate is the reason they sit at stale positions - a self-inflicted suspect,
                    // counted rather than assumed.
                    ++ring_unprojected;
                }
                // The master switch is already folded into `point.visible` by the snapshot, so a
                // switched-off map merges as "every point invisible" and nothing can be shown
                // against the switch here.
                //
                // A RING THAT COULD NOT BE PLACED MUST NOT BE DRAWN. A ring's position is only
                // meaningful once it has been moved onto its host: before that it still carries
                // the previous host's spot, or the borrowed coordinates an unused ring is parked
                // at. Showing it anyway produced exactly the reported pair - "one icon with no
                // ring, and one ring sitting somewhere odd with no icon under it" - because both
                // halves are the SAME ring. Measured 2026-08-05 on a progress-category switch:
                // `focus demand: 11 ... 0 without` (the pool was never the limit) with
                // `moved=127 unprojected=1`, one ring exactly, matching the report.
                // Markers are deliberately NOT gated this way: their coordinates are static and
                // already correct from build time, so a failed refresh is no reason to hide one.
                v3_native_set_visible(obj, point.visible && (projected || !ring));
                continue;
            }
            // At seed time queue EVERY migrated row, hidden ones included:
            // factory pulses exist only during the build burst, so an object
            // that is not created NOW can never be shown by a later category
            // toggle. Hidden rows are created parked offscreen.
            if ((initial || point.visible) &&
                g_v3_native.queued.insert(point.original_row_id).second)
                g_v3_native.pending.push_back(point);
        }
        // [v3pump] While the pump owns the queue, rows not built yet still carry the point they had
        // at seed. Hand them the current one, so a tab switch, a category toggle or a moved ring
        // made during the pump is what gets built. One hash of the snapshot, only in that window.
        if (!initial && g_v3_native.pump_capped &&
            g_v3_native.pending_index < g_v3_native.pending.size())
        {
            std::unordered_map<uint64_t, const goblin::NativeMarkerPoint *> now;
            now.reserve(snapshot.size());
            for (const auto &point : snapshot)
                now.emplace(point.original_row_id, &point);
            for (size_t i = g_v3_native.pending_index; i < g_v3_native.pending.size(); ++i)
            {
                // Rings keep their seed point: a built ring follows its host through later merges,
                // while a ring re-pointed now at a host on an unlearned tile could fail for good.
                if (g_v3_native.pending[i].original_row_id & goblin::NATIVE_HIGHLIGHT_KEY_BIT)
                    continue;
                const auto hit = now.find(g_v3_native.pending[i].original_row_id);
                if (hit != now.end())
                    g_v3_native.pending[i] = *hit->second;
            }
            // Rows that became visible go first, in the order the seed sort gave them, so a tab
            // switched to mid-pump fills in before the rest.
            std::stable_partition(
                g_v3_native.pending.begin() + static_cast<ptrdiff_t>(g_v3_native.pending_index),
                g_v3_native.pending.end(),
                [](const goblin::NativeMarkerPoint &p) { return p.visible; });
        }
        // (A "sweep the rows this snapshot did not mention" pass stood here for one build. It could
        //  never fire: native_marker_snapshot returns EVERY row whatever the layer - measured
        //  migrated=9552 requested=9552 on both layer 0 and layer 2 - and encodes the layer in
        //  point.visible alone, which the loop above already applies. The tab leak it was meant to
        //  fix comes from the re-anchor path abandoning the previous generation's children.)
        if (g_v3_native.pending.size() != queued_before)
        {
            g_v3_native.completion_reported = false;
            g_v3_native.settle_frames = 0;
        }
        if (initial)
            // Seed build order = draw order (append = on top), so sort by:
            // (1) visible rows FIRST - the pulse pool is finite and any
            //     shortfall must land on rows the player cannot see anyway;
            // (2) markers of the player's OWN map after everything else - where a
            //     dungeon's icons and the surface's land on the same spot, the ones
            //     that belong to the place the player is actually in are the ones
            //     that must be readable. Only ordering: nothing is hidden, and with
            //     the emphasis off the key is constant and the old order stands;
            // (3) row id DESCENDING - the registry z-order contract is
            //     "lower row id draws on top" (row_id_registry LAYER_ORDER),
            //     so lower ids must be created LAST;
            // (4) a cleared-badge twin right AFTER its base row - the badge
            //     draws above its own icon.
            // This is the one part of the emphasis that a build decides rather than a
            // transform: depth is fixed when a child is created and the factory only
            // pulses during a build burst. A reopen that reuses the movie therefore
            // keeps the previous open's order (size and, later, colour still update
            // live) - it re-sorts itself the next time the map builds from scratch.
            std::stable_sort(
                g_v3_native.pending.begin(), g_v3_native.pending.end(),
                [](const goblin::NativeMarkerPoint &a,
                   const goblin::NativeMarkerPoint &b) {
                    if (a.visible != b.visible)
                        return a.visible > b.visible;
                    const bool oa = v3_is_own_location(a.area, a.gx, a.gz);
                    const bool ob = v3_is_own_location(b.area, b.gx, b.gz);
                    if (oa != ob)
                        return oa < ob;
                    const uint64_t ab =
                        a.original_row_id & ~goblin::NATIVE_CLEARED_KEY_BIT;
                    const uint64_t bb =
                        b.original_row_id & ~goblin::NATIVE_CLEARED_KEY_BIT;
                    if (ab != bb)
                        return ab > bb;
                    return (a.original_row_id & goblin::NATIVE_CLEARED_KEY_BIT) <
                           (b.original_row_id & goblin::NATIVE_CLEARED_KEY_BIT);
                });
        if (initial)
        {
            spdlog::info("[v3native] item categories layer={} seedParentCount={}; "
                         "migrated={} visible={} requested={}",
                         g_v3_native.layer, g_v3_native.observed_count,
                         snapshot.size(), visible_count,
                         g_v3_native.pending.size() - g_v3_native.pending_index);
            std::unordered_set<uint64_t> unknown_tiles;
            v3_prelearn_tiles(&unknown_tiles);
            // [v3pump] The burst's share: the visible head of the sorted queue, then the hidden
            // rows on tiles the prelearn could not learn. Those need the engine converter, which
            // answers only while its own last call is under 300 ms old - true in this burst, not
            // necessarily frames later (a slow frame rate, the Deck). Moving them up changes draw
            // order among hidden rows only, and only for rows the converter has to place.
            size_t v = g_v3_native.pending_index;
            while (v < g_v3_native.pending.size() && g_v3_native.pending[v].visible)
                ++v;
            if (goblin::variants::kFramePump && g_v3_goto_frame && !g_v3_pump.off &&
                !unknown_tiles.empty())
            {
                const auto mid = std::stable_partition(
                    g_v3_native.pending.begin() + static_cast<ptrdiff_t>(v),
                    g_v3_native.pending.end(), [&](const goblin::NativeMarkerPoint &p) {
                        return p.area != 60 && p.area != 61 &&
                               unknown_tiles.count(v3_tile_key(p.area, p.gx, p.gz)) != 0;
                    });
                const size_t moved = static_cast<size_t>(mid - g_v3_native.pending.begin()) - v;
                if (moved && goblin::config::debugLogging)
                    spdlog::info("[v3pump] {} hidden rows on {} tile(s) without a known origin go "
                                 "into the burst",
                                 moved, unknown_tiles.size());
                v += moved;
            }
            g_v3_native.burst_end = v;
        }
        // MARKER visibility belongs on this line too. It read rings only, and rings are all
        // invisible unless a focus category is set - so "visible=0" said nothing about the icons
        // and the log could not answer the one question asked of it: after the master switch goes
        // back on over an open map, do the markers actually become visible again? (Convergence
        // 2026-08-05: two opens seeded with `visible=0` because the switch was off, which is
        // correct, and nothing in the log showed what happened after it was switched on.)
        // markers= is the count the snapshot says SHOULD be on screen this merge.
        static size_t s_last_markers = SIZE_MAX;
        const bool markers_changed = visible_count != s_last_markers;
        s_last_markers = visible_count;
        // Print when a ring did something OR when the visible marker count moved, so a static
        // map stays quiet but a visibility change never passes unrecorded.
        if (ring_remapped || ring_moved || ring_unprojected || markers_changed)
            spdlog::info("[v3ring] merge{}: markers={} | rings seen={} visible={} onOtherMap={} "
                         "remapped={} moved={} unprojected={}",
                         initial ? " (seed)" : "", visible_count, ring_seen, ring_visible,
                         ring_other_map, ring_remapped, ring_moved, ring_unprojected);
    }

    void v3_native_tick()
    {
        if (!goblin::variants::kNativeMarkers) return;
        const int layer = goblin::maphover::map_layer();
        if (layer < 0 || layer > 2) return;
        // Phase gate. No dialog object exists, so a retarget/reset/reseed here would be work
        // against a screen that is not there. 0xFF (unresolved) counts as alive, so an unknown
        // signal can never switch the markers off.
        if (!v3_map_object_alive()) return;
        v3_note_thread("v3_native_tick (map frame)");
        v3_check_owner("v3_native_tick");

        // Running split of this function (see V3Perf). A mark attributes everything since the
        // previous one; the early returns below simply leave their last segment unattributed,
        // which is correct - those are the frames that did nothing.
        const int64_t t_head0 = v3_perf_now();
        int64_t t_mark = t_head0;
        const auto mark = [&t_mark](int64_t &into) {
            const int64_t n = v3_perf_now();
            into += n - t_mark;
            t_mark = n;
        };

        // Return the previous generation's children to the heap, a slice per frame. This runs
        // before the early returns below on purpose: an abandoned generation must drain even
        // on frames where the current target does not validate, which is exactly the window a
        // teardown-and-reopen spends here. 256 destructors is well under a frame's worth of
        // the seed burst that shares this tick.
        const uintptr_t wrapper = g_v3_target_wrapper.load(std::memory_order_acquire);
        const uintptr_t parent = g_v3_target_parent.load(std::memory_order_relaxed);
        // The shared parent hosts every layer's markers - the target's stamp
        // layer says which layer was live at anchor time, nothing more, so the
        // live layer must NOT gate this validation (it silently froze the
        // whole manager on UG/DLC).
        // Both go through the liveness gate, not just v3_heap_ptr: this runs on EVERY map frame
        // against an anchor that a teardown may already have freed, so a dead read here is the
        // routine case, not the exceptional one. With the range test alone it raised 10
        // first-chance AVs across 38 opens (ERSS-FG, 2026-08-05: v3_read64/v3_read32 <-
        // v3_native_tick+0xf1 <- placename_detour), each costing ~77 ms in somebody else's
        // exception filter. Same defect as the as_root read in v3_movie_of, one site over.
        // 0xE8 because live_count is read at parent+0xE0 - a node-header-sized window would
        // have left the very read that faults outside the gate.
        //
        // An anchor kept from a screen that has since closed is not read at all. The gate cannot
        // vouch for it: safemem's verdict on a page is cached for up to kTtlMs (5 s), the close
        // expired the parked movie, and a reopen 70-110 ms later ran this gate on the torn-down
        // list while the cache still said readable - 2 first-chance faults, 84 and 95 ms frames
        // spent in other modules' exception filters (vanilla ME3 2026-09-24, where the new anchor
        // arrives ~100 ms into the open). The close released the generation, so there is nothing
        // on that anchor to keep (an empty generation has nothing to keep either): drop it unread and let the next burst re-anchor (taking an anchor
        // is what clears g_map_screen_gone).
        // EXCEPT while the manager still runs a live generation on this very parent - the close did
        // not release it (the self-detach variant off, no remove-at function, or a phase byte that
        // read 0 while the map was up). Dropping then would strand thousands of attached children
        // with nobody managing them; the gate below reads that anchor as it always did.
        if (parent != 0 && g_map_screen_gone.load(std::memory_order_acquire) &&
            !(g_v3_native.seeded && g_v3_native.parent == parent &&
              !g_v3_native.objects.empty()))
        {
            v3_drop_dead_anchor("closed screen, before the gate", parent);
            mark(g_v3_perf.head_gate_qpc);
            const int64_t d = t_mark - t_head0;
            g_v3_perf.head_qpc += d;
            if (d > g_v3_perf.head_max_qpc)
                g_v3_perf.head_max_qpc = d;
            return;
        }
        uint64_t wrapper_parent = 0, live_count = 0;
        if (v3_node_unusable(wrapper, 0x20) || v3_node_unusable(parent, 0xE8) ||
            !v3_read64(wrapper + 0x18, wrapper_parent) || wrapper_parent != parent ||
            !v3_read64(parent + 0xe0, live_count))
        {
            // A refused gate is still a frame we paid for (a failed probe plus a VirtualQuery
            // each time the dead-anchor memo lapses), so it goes into gate / head / worst frame.
            mark(g_v3_perf.head_gate_qpc);
            const int64_t d = t_mark - t_head0;
            g_v3_perf.head_qpc += d;
            if (d > g_v3_perf.head_max_qpc)
                g_v3_perf.head_max_qpc = d;
            return;
        }
        mark(g_v3_perf.head_gate_qpc);
        if (g_v3_target_layer.load(std::memory_order_relaxed) < 0)
            g_v3_target_layer.store(layer, std::memory_order_relaxed);

        // Report the marker children's display-object vtable as an exe RVA, once per
        // generation. The colour transform - the one emphasis lever the matrix call
        // cannot reach - would be reachable from here IF it were virtual. It is not: Get/SetCxform
        // are non-virtual (goblin_scaleform notes + GFx_DisplayObject.h), which is why no vtable
        // dump could ever find them and why the colour lever goes through the node data instead
        // (v3_write_cxform, node data + g_v3_cx_offset). Kept as the record of a dead end.
        // of these bytes offline rather than another live probe.
        if (g_v3_native.child_vtable != 0 && !g_v3_native.child_vtable_logged)
        {
            g_v3_native.child_vtable_logged = true;
            const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            const uint64_t vt = g_v3_native.child_vtable;
            spdlog::info("[v3native] marker child vtable = 0x{:X} (exe+0x{:X})", vt,
                         (exe && vt > exe) ? static_cast<uintptr_t>(vt) - exe : 0);
        }
        if (g_v3_native.parent != parent)
        {
            v3_native_reset("tick_new_parent");
            g_v3_native.layer = layer;
            g_v3_native.wrapper = wrapper;
            g_v3_native.parent = parent;
            // Do NOT capture parent_vtable here. At retarget time (count=100, early in the build
            // burst) the container's vtable slot can still read 0 - capturing that stored a zero
            // signature and made the liveness guard reject the parent forever. It is adopted
            // lazily below, the first frame the parent shows a real vtable.
            g_v3_native.parent_vtable = 0;
            g_v3_native.parent_movie = 0;  // adopted together with the vtable, on the same frame
            g_v3_native.observed_count = live_count;
            // Close the head split on this return too. It comes after the gate's mark, so without
            // this the frame that first validates a NEW anchor counted in `gate` but not in `head`
            // or `worst frame` - read on ERR 2026-09-23 as "head 69 (gate 33934 + rest 43, worst
            // frame 38)", where the 34 ms frame was this one.
            mark(g_v3_perf.head_rest_qpc);
            {
                const int64_t d = t_mark - t_head0;
                g_v3_perf.head_qpc += d;
                if (d > g_v3_perf.head_max_qpc)
                    g_v3_perf.head_max_qpc = d;
            }
            return;
        }
        // The QUICK-REOPEN heartbeat arm stood here and is GONE, 2026-08-03. It cleared
        // g_v3_map_closed on the theory that a fresh maphover heartbeat plus a parent still
        // carrying this generation's vtable proves the map reopened. Both halves are unsound: the
        // heartbeat is stamped by placename_detour, which is on EVERY faulting stack, so it is
        // "alive" precisely when we crash; and a vtable says what a block is, not whether its
        // render state exists. It was the third of three places that cleared the gate without the
        // engine's say-so. The phase byte sampled in menu_update_detour is now the only writer.
        if (g_v3_native.parent_vtable == 0 && v3_heap_ptr(g_v3_native.parent))
        {
            uint64_t pvt = 0;
            if (v3_read64(g_v3_native.parent, pvt) && pvt != 0)
            {
                g_v3_native.parent_vtable = pvt; // first frame it is genuinely alive
                g_v3_native.parent_movie = v3_movie_of(g_v3_native.parent);
            }
        }
        if (g_v3_native.layer != layer)
        {
            // Same shared parent hosts every layer's markers: a layer switch
            // is pure show/hide of the right rows - re-snapshot immediately.
            // SAY SO in the log. Nothing else records the displayed layer (the two lines that
            // print one are build-time only), so a report from the DLC map was indistinguishable
            // from one from the Lands Between - which is exactly the wrong thing to be blind to
            // while chasing a DLC-only crash. 0 = overworld, 1 = underground, 2 = Shadow Realm.
            spdlog::info("[v3native] displayed layer {} -> {} (tracked {} markers)",
                         g_v3_native.layer, layer, g_v3_native.objects.size());
            g_v3_native.layer = layer;
            g_v3_native.next_refresh_ms = 0;
        }

        mark(g_v3_perf.head_rest_qpc);
        {
            const int64_t d = t_mark - t_head0;
            g_v3_perf.head_qpc += d;
            if (d > g_v3_perf.head_max_qpc)
                g_v3_perf.head_max_qpc = d;
        }

        // ── Location emphasis, from here on ─────────────────────────────────────────────
        // EVERYTHING that touches a child for the emphasis lives BELOW the retarget branch
        // above, and that placement is load-bearing. It used to sit at the top of the tick,
        // where it ran on the frame the map re-anchored to a NEW movie - i.e. over the
        // previous generation's children, whose blocks the engine had already freed. The
        // matrix write survived that on its vtable guard; the colour write does not, because
        // it hands a stale render entry to an ENGINE function that walks and writes through
        // it. Log of the crash: RETARGET, then "3867 of 9454 markers resized", then an
        // access violation inside GetWritableData. Below the branch, the manager is proven
        // to be the live generation.

        // Learn where the colour transform lives, from a child that exists and is untinted
        // by definition - it was built from our own frame and nothing has touched its
        // colour. Retried for a few frames because a freshly created child's render entry
        // is not necessarily hooked up the instant it appears; after that, give up loudly
        // rather than scan every frame forever.
        if (!g_v3_cx_searched && !g_v3_native.objects.empty() &&
            v3_heap_ptr(g_v3_native.objects.front().child))
        {
            const uintptr_t c = g_v3_native.objects.front().child;
            if (v3_node_data_ro(c) || ++g_v3_native.cx_attempts >= 60)
            {
                v3_find_cx_offset(c);
                if (goblin::config::debugLogging) v3_dump_child_layout(c);
            }
        }

        // Where the player stands, resampled every map frame. The character cannot walk
        // while the map is up, so in practice this changes between opens - but a fast
        // travel closes the map from a different place than it opened in, and a marker
        // that kept the old answer would be emphasising the wrong dungeon. Reading it is
        // three derefs; deciding what to DO about it costs nothing unless it changed.
        // The two scales and the on/off ride the same signature so a toggle flipped in
        // the menu lands on the OPEN map, exactly like a category toggle does.
        {
            uint32_t pm = 0;
            if (!goblin::collected::read_player_map_id(pm)) pm = 0;
            uint64_t sig = 0;
            if (goblin::config::locationEmphasis)
            {
                // Every input the answer depends on, mixed into one compare - the player's
                // map AND all four tuning values, so editing any of them in the ini lands
                // on the OPEN map instead of waiting for the next build.
                const float vals[4] = {goblin::config::locationEmphasisOwnScale,
                                       goblin::config::locationEmphasisOtherScale,
                                       goblin::config::locationEmphasisOtherFade,
                                       goblin::config::locationEmphasisOtherCool};
                sig = 0xcbf29ce484222325ull ^ pm;
                for (float v : vals)
                {
                    uint32_t bits = 0;
                    memcpy(&bits, &v, 4);
                    sig = (sig ^ bits) * 0x100000001b3ull;
                }
                sig |= 1;  // never 0 while the feature is on (0 means "off")
            }
            if (sig != g_v3_native.emph_sig)
            {
                g_v3_native.emph_sig = sig;
                g_v3_native.player_map = pm;
                g_v3_native.emph_dirty = true;
                g_v3_native.emph_cursor = 0;
                spdlog::info("[v3native] location emphasis: player map "
                             "m{:02d}_{:02d}_{:02d}_{:02d}, re-deciding {} markers",
                             (pm >> 24) & 0xFF, (pm >> 16) & 0xFF, (pm >> 8) & 0xFF,
                             pm & 0xFF, g_v3_native.objects.size());
            }
        }

        // Re-apply a SLICE per frame. Touching all ~9500 at once froze the game for
        // seconds: a colour write is not a bare store but GetWritableData, which
        // copy-on-writes the node data and registers a change with the render context, so
        // thousands in one frame is thousands of allocations and change records. Only
        // objects whose factor actually changed are written, and the budget counts those.
        if (g_v3_native.emph_dirty)
        {
            const int64_t t_e0 = v3_perf_now();
            constexpr size_t V3_EMPH_WRITES_PER_FRAME = 192;
            size_t written = 0;
            while (g_v3_native.emph_cursor < g_v3_native.objects.size() &&
                   written < V3_EMPH_WRITES_PER_FRAME)
            {
                V3NativeObject &obj = g_v3_native.objects[g_v3_native.emph_cursor++];
                const float emph = v3_emphasis(obj.area, obj.gx, obj.gz);
                const float want_fade = obj.is_ring ? 1.0f : v3_fade(obj.area, obj.gx, obj.gz);
                const float want_cool = obj.is_ring ? 0.0f : v3_cool(obj.area, obj.gx, obj.gz);
                if (emph == obj.emph && obj.fade == want_fade && obj.cool == want_cool)
                    continue;
                obj.emph = emph;
                v3_native_reapply(obj);
                ++written;
            }
            if (g_v3_native.emph_cursor >= g_v3_native.objects.size())
            {
                g_v3_native.emph_dirty = false;
                g_v3_native.emph_cursor = 0;
            }
            g_v3_perf.emph_qpc += v3_perf_now() - t_e0;
            g_v3_perf.emph_writes += static_cast<uint32_t>(written);
        }

        if (!g_v3_native.seeded)
        {
            // NEVER commit the deferred seed to a parent outside the live tree. Every bad
            // layer-2 run of the 2.1.1 series seeded right here: the picker had anchored
            // onto the OLD view's 105-child pin list at a layer switch, the teardown then
            // detached it (pParent 0, depth -1 - the rejected children's "lv 1" up-walks),
            // and a detached list's count is frozen, so "stable for 8 frames" is a
            // property it satisfies PERFECTLY. The inline seed never fired on those runs
            // (measured: good runs get a seed pulse 10-21 ms after the retarget, bad runs
            // never do), so this path was the one that committed, with no validation at
            // all. The live parent (81 children) was refused by floors alone; liveness,
            // not size, is what tells the two apart. Drop the dead anchor so the next
            // burst can re-anchor - holding it is what made the breakage session-long.
            if (v3_node_detached(parent) || v3_movie_of(parent) == 0)
            {
                v3_drop_dead_anchor("tick seed", parent);
                return;
            }
            // Let the game's small remaining vanilla/ERR WorldMapItem burst
            // settle before our count changes the same parent.
            if (live_count != g_v3_native.observed_count)
            {
                g_v3_native.observed_count = live_count;
                g_v3_native.stable_frames = 0;
                return;
            }
            if (++g_v3_native.stable_frames < 8)
                return;
            g_v3_native.seeded = true;
            g_v3_native.build_started_ms = GetTickCount64();
            g_v3_native.next_refresh_ms = g_v3_native.build_started_ms + 200;
            g_v3_native.last_progress_ms = g_v3_native.build_started_ms;
            v3_native_merge_snapshot(goblin::native_marker_snapshot(layer, true), true);
        }

        // Visibility/category/collection is re-read live. Existing objects are
        // moved offscreen/on-map; newly enabled rows are appended to the queue.
        const uint64_t now_ms = GetTickCount64();

        // Queue watchdog: RM2 factory pulses exist only during a build burst.
        // If queued rows never build (this session's 180-parent incident), the
        // outstanding queue would gate the 500ms refresh forever and kill live
        // hide/show - drop it (rows stay in `queued`, skip-for-session).
        // [v3pump] Not while the pump owns the queue: it is the pulse source, and it stops only
        // with the map tick (our own screen over the map is enough), so the first tick after
        // such a pause would otherwise find "no progress for 5 s" and drop what it was building.
        // A pump that fails turns itself off, which re-arms this.
        if (g_v3_native.pending_index < g_v3_native.pending.size() &&
            !g_v3_native.pump_capped &&
            g_v3_native.last_progress_ms != 0 &&
            now_ms - g_v3_native.last_progress_ms > 5000)
        {
            // Say WHY there were no pulses. A pulse needs all of: the icon sprite injected, a live
            // timeline ctx on sprite 171, and an open map. A player report (Linux, DLC map) hit
            // this with 1331 markers dropped, and the message alone could not tell which gate was
            // shut - so print the three of them plus how many children were rejected.
            v3_seed_trace("at queue drop");
            // Which side lost the pulses: the engine's burst or our gate. Logged HERE because
            // this line is the one a starved build always writes.
            {
                uint64_t rt = 0, rs = 0, rd = 0, ri = 0;
                goblin::gfx_probe::rm2_stats(rt, rs, rd, ri);
                spdlog::warn("[v3native] RM2 traffic this open: {} tags executed, {} were "
                             "sprite-171, refused {} no-screen / {} no-icons; "
                             "failures so far: project={} charId={} noNode={} "
                             "notMaterialized={} attach={}",
                             rt, rs, rd, ri, g_v3_native.failed_project,
                             g_v3_native.failed_charid, g_v3_native.failed_nonode,
                             g_v3_native.failed_material, g_v3_native.failed_attach);
            }
            spdlog::warn("[v3native] {} queued markers never built; dropping the queue to keep "
                         "live refresh alive. gates: qmark_injected={} map_open={} "
                         "rejected_so_far={} attached={}; pulses seen={} unseeded={} "
                         // budget_regranted is NOT a block: it counts the pulses that arrived with
                         // an empty batch and were handed a fresh one. Reading it as a gate cost a
                         // whole diagnostic pass once.
                         "blocked(driver/sprite/busy/hbwait)={}/{}/{}/{} budget_regranted={}",
                         g_v3_native.pending.size() - g_v3_native.pending_index,
                         goblin::gfx_probe::icons_injected(),
                         // The engine's phase byte, not map_dialog(): this field is read as
                         // "was the map up", and the pointer answers "has our hover hook run",
                         // which is a different question and was misleading in two rounds.
                         goblin::stall_probe::map_screen_alive(), g_v3_native.failed,
                         g_v3_native.objects.size(),
                         g_v3_pulse_seen.load(std::memory_order_relaxed),
                         g_v3_pulse_unseeded.load(std::memory_order_relaxed),
                         g_v3_consume_nodriver.load(std::memory_order_relaxed),
                         g_v3_consume_nosprite.load(std::memory_order_relaxed),
                         g_v3_consume_busy.load(std::memory_order_relaxed),
                         g_v3_consume_hbwait.load(std::memory_order_relaxed),
                         g_v3_consume_nobudget.load(std::memory_order_relaxed));
            g_v3_native.pending_index = g_v3_native.pending.size();
        }
        // A visibility toggle re-reads visibility NOW, whatever the build queue is doing.
        // The periodic refresh below is gated on a SPENT queue on purpose (it must not
        // interleave with a build), but a master or category toggle only ever re-decides
        // objects that already exist - and making it wait cost the icons outright:
        // measured 2026-08-05, master OFF before the map opens seeds every row invisible,
        // the reconcile then keeps all of them DETACHED (out of window is what invisible
        // means to it), and if the burst ran out of pulses the queue never spends, so
        // turning the switch back on over the open map changed nothing. The close-time
        // self-detach named it exactly: "matched=0 removed=0 tracked=5975" against
        // "matched=908" on a healthy open. Only a map reopen brought them back.
        mark(g_v3_perf.emphsec_qpc);
        const uint32_t vis_epoch = goblin::visibility_epoch();
        if (vis_epoch != g_v3_seen_vis_epoch)
        {
            g_v3_seen_vis_epoch = vis_epoch;
            g_v3_native.next_refresh_ms = now_ms + V3_IDLE_REFRESH_MS;
            const int64_t t_m0 = v3_perf_now();
            v3_native_merge_snapshot(goblin::native_marker_snapshot(layer, true), false);
            g_v3_perf.merge_qpc += v3_perf_now() - t_m0;
            ++g_v3_perf.merges;
            g_v3_native.pump_merged = true;
        }
        else if ((g_v3_native.pending_index >= g_v3_native.pending.size() ||
                  g_v3_native.pump_capped) &&
                 now_ms >= g_v3_native.next_refresh_ms)
        {
            g_v3_native.next_refresh_ms = now_ms + V3_IDLE_REFRESH_MS;
            const int64_t t_m0 = v3_perf_now();
            v3_native_merge_snapshot(goblin::native_marker_snapshot(layer, true), false);
            g_v3_perf.merge_qpc += v3_perf_now() - t_m0;
            ++g_v3_perf.merges;
            g_v3_native.pump_merged = true;
        }
        mark(g_v3_perf.mergesec_qpc);

        // Counter-zoom pass: our children ride the marker layer's parent
        // transform 1:1 (nothing engine-side updates non-widget children).
        // Primary source: the live 2x2 sampled from a real native pin node -
        // IF the engine writes its adaptation there (first session showed
        // identity at the level we sampled; the [v3probe] dump pins the truth).
        // Until a non-identity sample appears, use the measured power law:
        // vs a fully map-locked icon the natives were ~2x smaller at ~3x
        // zoom-in and ~2x bigger at ~3x zoom-out, pivot at the default open
        // zoom -> factor = (PIVOT/zoom)^(ln2/ln3), applied per frame.
        constexpr float V3_ZOOM_PIVOT = 0.70f;
        constexpr float V3_ZOOM_EXP = 0.6309f; // ln(2)/ln(3)
        constexpr float V3_SIZE_TRIM = 0.92f;  // user calibration: a touch smaller
        float fx = 0.0f, fy = 0.0f;
        goblin::mapproject::MapView view{};
        const bool have_view =
            goblin::mapproject::read_view(view) && view.zoom > 0.01f;
        if (have_view)
        {
            const float zd = view.zoom > g_v3_native.last_dump_zoom
                                 ? view.zoom - g_v3_native.last_dump_zoom
                                 : g_v3_native.last_dump_zoom - view.zoom;
            if (zd > 0.02f * view.zoom)
            {
                g_v3_native.last_dump_zoom = view.zoom;
                v3_native_dump_probe(view.zoom);
            }
        }
        if (v3_native_sample_widget(fx, fy) &&
            (fx < 0.98f || fx > 1.02f || fy < 0.98f || fy > 1.02f))
        {
            if (!g_v3_native.sample_logged)
            {
                g_v3_native.sample_logged = true;
                spdlog::info("[v3native] non-identity widget sample fx={:.4f} "
                             "fy={:.4f} zoom={:.4f} - using engine curve",
                             fx, fy, have_view ? view.zoom : 0.0f);
            }
        }
        else if (have_view)
        {
            fx = fy = V3_SIZE_TRIM * powf(V3_ZOOM_PIVOT / view.zoom, V3_ZOOM_EXP);
        }
        else
        {
            fx = fy = 0.0f;
        }
        if (fx > 0.0f && fy > 0.0f)
        {
            fx = std::clamp(fx, 0.01f, 100.0f);
            fy = std::clamp(fy, 0.01f, 100.0f);
            const float dx = fx > g_v3_native.cur_fx ? fx - g_v3_native.cur_fx
                                                     : g_v3_native.cur_fx - fx;
            const float dy = fy > g_v3_native.cur_fy ? fy - g_v3_native.cur_fy
                                                     : g_v3_native.cur_fy - fy;
            if (dx > 0.003f * g_v3_native.cur_fx || dy > 0.003f * g_v3_native.cur_fy)
            {
                const int64_t t_z0 = v3_perf_now();
                g_v3_native.cur_fx = fx;
                g_v3_native.cur_fy = fy;
                // Visible children only. A hidden child is parked offscreen, so its stale
                // scale shows nowhere, and v3_native_set_visible writes the transform with
                // the CURRENT v3_obj_fx/fy on show - it catches up the moment it matters.
                // Writing all of them made every pass pay for the whole population
                // (report 27: ~2.1 ms x 23-24 passes/s during a smooth zoom, mostly
                // hidden/off-tab children), and each write is an engine change record,
                // not a bare store.
                for (auto &obj : g_v3_native.objects)
                    if (obj.visible)
                        v3_native_reapply(obj);
                g_v3_perf.zoom_qpc += v3_perf_now() - t_z0;
                ++g_v3_perf.zoom_passes;
            }
        }

        // Once both the stock build and our category queue have stopped growing,
        // report the actual parent composition.  Subtracting our known bitmap
        // children gives the remaining heavyweight WorldMapItem population.
        if (g_v3_native.pending_index >= g_v3_native.pending.size() &&
            g_v3_factory_active == 0)
        {
            if (live_count != g_v3_native.settle_count)
            {
                g_v3_native.settle_count = live_count;
                g_v3_native.settle_frames = 0;
            }
            else if (!g_v3_native.completion_reported &&
                     ++g_v3_native.settle_frames >= 30)
            {
                const uint64_t lightweight = g_v3_native.objects.size();
                const uint64_t heavy = live_count >= lightweight ? live_count - lightweight : 0;
                // wrongCtx belongs next to failed: both answer "why are markers missing", and the
                // counter was being incremented on every root-changed child while nothing read it.
                uint64_t rt = 0, rs = 0, rd = 0, ri = 0;
                goblin::gfx_probe::rm2_stats(rt, rs, rd, ri);
                // The RM2 traffic goes on the HEALTHY line too, not only on the starved one: a
                // number with nothing to compare it against says nothing, and the whole question
                // is why one open gets 7877 sprite-171 executions and the next gets 10.
                // Where the projection failures are, once per distinct set: the tiles the
                // engine's converter refuses are a property of the profile's data, and this
                // line is what inputs/unprojectable_tiles.json is maintained from.
                if (!g_v3_native.failed_project_tiles.empty())
                {
                    std::string tiles;
                    for (const auto &[key, n] : g_v3_native.failed_project_tiles)
                    {
                        char buf[48];
                        snprintf(buf, sizeof buf, "%sm%02u_%02u_%02u=%u", tiles.empty() ? "" : " ",
                                 key >> 16, (key >> 8) & 0xFF, key & 0xFF, n);
                        tiles += buf;
                    }
                    static std::string s_last_tiles;
                    if (tiles != s_last_tiles)
                    {
                        s_last_tiles = tiles;
                        spdlog::info("[v3native] projection refused by the engine, by tile: {}", tiles);
                    }
                }
                spdlog::info("[v3native] CATEGORIES READY: layer={} "
                             "created={} failed={} (project={} [declined={} stale={} "
                             "noView={} raised={}] fromOrigin={} charId={} noNode={} "
                             "notMaterialized={} attach={}) wrongCtx={} "
                             "parentCount={} inferredHeavy={}; "
                             "RM2 this open: {} tags, {} sprite-171, refused {} no-screen / "
                             "{} no-icons",
                             g_v3_native.layer, lightweight, g_v3_native.failed,
                             g_v3_native.failed_project, g_v3_native.failed_project_declined,
                             g_v3_native.failed_project_stale, g_v3_native.failed_project_noview,
                             g_v3_native.failed_project_faulted, g_v3_native.placed_from_origin,
                             g_v3_native.failed_charid,
                             g_v3_native.failed_nonode, g_v3_native.failed_material,
                             g_v3_native.failed_attach,
                             g_v3_native.wrong_contexts, live_count, heavy, rt, rs, rd, ri);
                // What THIS open actually cost us, in one line: the number that has to be watched
                // when a profile freezes on open, and the only form in which "each open is worse
                // than the last" can be confirmed or refuted.
                {
                    const int64_t f = v3_perf_freq();
                    uint64_t sc = 0, si = 0, sw = 0, sl = 0;
                    goblin::gfx_probe::scan_stats(sc, si, sw, sl);
                    spdlog::info("[v3perf] open cost: factory {} ms over {} pulses; our frames "
                                 "{} ms over {} frames; stages: project {} ms, create {} ms, "
                                 "materialize {} ms, attach {} ms; node search: {} walks, {} "
                                 "nodes total, worst {}, longest list {}",
                                 g_v3_open.pulse_qpc * 1000 / f, g_v3_open.pulses,
                                 g_v3_open.frame_qpc * 1000 / f, g_v3_open.frames,
                                 g_v3_open.proj_qpc * 1000 / f, g_v3_open.create_qpc * 1000 / f,
                                 g_v3_open.mat_qpc * 1000 / f, g_v3_open.attach_qpc * 1000 / f,
                                 sc, si, sw, sl);
                }
                // [v3pump] The other half of the open: "factory" above is the burst alone.
                if (g_v3_open.capped)
                    spdlog::info("[v3pump] this open: burst built {} of a cap of {}, the pump {} in "
                                 "{} frames / {} calls / {} pulses, {} us in all, worst frame {} "
                                 "us, budget now {}; generator + nested proof {} us ({} pulses), "
                                 "its children at most {}; {} rows dropped by a stop",
                                 g_v3_open.pump_frames ? g_v3_open.burst_built : lightweight,
                                 g_v3_open.burst_cap, g_v3_open.pump_built, g_v3_open.pump_frames,
                                 g_v3_open.pump_calls, g_v3_open.pump_pulses,
                                 park_us(g_v3_open.pump_qpc), park_us(g_v3_open.pump_max_qpc),
                                 g_v3_pump.budget, park_us(g_v3_open.gen_qpc),
                                 g_v3_open.proof_pulses, g_v3_open.gen_kids_max,
                                 g_v3_open.pump_dropped);
                v3_seed_trace("at READY");
                if (goblin::config::debugLogging) // per-open diagnostics, not shipping chatter
                {
                    const uint64_t dlg = g_open_dialog_ms.load(std::memory_order_relaxed);
                    const uint64_t t0 = goblin::map_timing::open_burst_t0();
                    uint64_t site = 0, skipped = 0, other = 0;
                    goblin::map_timing::open_counters(site, skipped, other);
                    const uint64_t now_ms = GetTickCount64();
                    spdlog::info("[opentime] dialog->burst {} ms, dialog->ready {} ms; map-site "
                                 "relayout calls {} (skipped {}), other-site since burst {}: {}",
                                 (dlg && t0 >= dlg) ? t0 - dlg : 0,
                                 dlg ? now_ms - dlg : 0, site, skipped, other,
                                 goblin::map_timing::open_other_sites());
                }
                g_v3_native.completion_reported = true;
                // The generation is fully built, so the PREVIOUS one's movie is certainly gone by
                // now: this is the moment its sampled children answer whether they outlived it.
                goblin::crashdiag::note_map_open_completed(
                    g_v3_native.layer, static_cast<uint32_t>(lightweight), g_v3_native.failed,
                    static_cast<uint32_t>(g_v3_native.objects.size()));
                goblin::crashdiag::memory("open", static_cast<uint32_t>(g_v3_native.objects.size()));
                goblin::crashdiag::arena("open");
                goblin::crashdiag::probe_survivors();
            }
        }

        v3_pump_tick();

        // A live RM2::Execute callback later in this frame consumes this budget.
        // The timeline ctx is never retained here. 96, i.e. two batches: raising it in step with
        // a 1024 batch was part of the reverted 2026-08-05 experiment (see V3_FACTORY_BATCH).
        g_v3_native.frame_budget = 96;
        mark(g_v3_perf.tail_qpc);
    }

    // Dev-only (debug_logging) one-shot: prove a SOLID-color fill renders with no
    // GPU texture. Routes ONE empty MovieClip (DefineSprite 171, flags1=0x00) through
    // the SAME create -> materialize -> stage -> attach pipeline the icon markers use
    // (the o_v3_place detour stages the materialized child into slot[0]; a raw live_ctx
    // node is already parented and faults on re-attach), then draws an opaque magenta
    // 500px square into its DrawingContext via beginFill (NOT beginBitmapFill) and
    // anchors it at map (0,0). If a filled square appears, route (c) - a native panel
    // drawn from our own display objects (backgrounds/bars/frames) - is viable.
    // Fires ONCE per build (latched on entry) BEFORE the marker loop re-arms slot[0].

    // ── MENU-MOVIE graphics probe (does a MENU movie rasterize solid fills?) ──
    // The worldmap movie does NOT render DrawingContext shape-meshes; this tests
    // whether a system/menu movie DOES. Hooks CSMenuMan::updateTask (the menu UI
    // thread), reads the active menu (*(this+0x80)), scans its fields for reachable
    // GFx display objects (a non-null code ptr at vtable+0x2a0 = the DrawingContext
    // getter present only on Sprite/MovieClip), and draws a big solid rect into each
    // candidate's graphics (SEH-guarded per candidate). If a rect renders on any
    // NON-worldmap menu, route (c) is viable on a menu surface = custom visual
    // controls become possible. Dev-only (debug_logging); this updateTask hook is
    // also the UI-thread beachhead the future native announce/dialogs need.
    using MenuUpdateFn = void(void *menuman, void *dt);
    MenuUpdateFn *o_menu_update = nullptr;
    // (a g_menu_last_active atomic sat here, left from the retired solid-fill spike's MENU-MOVIE
    //  block; it was declared and never touched again)


    // ── Job-push machinery ──
    // Push a MenuJob onto a live menu's job stack, the same way the game's own MessageBox does
    // (reference impl FUN_14080fbf0): ref-move (FUN_1407a7b60) then push (FUN_1407edfa0). Must run
    // on the menu UI thread with a menu active - so it is driven from the updateTask detour. This
    // is how open_screen() gets our key-binding screen up over plain gameplay.
    //
    // THE F11 SETTINGS-MENU PROTOTYPE THAT LIVED HERE IS GONE (2026-07-31). It opened the game's own
    // OptionSettingTopDialog with our tab spliced into it, through a third primitive p_build_job
    // (FUN_1408087e0, movie 02_040_OptionSetting). Nothing triggered it: g_our_f11_open was declared
    // and read once, never assigned anything but 0, and no F11 poll existed anywhere in the file -
    // yet setup() still scanned p_build_job and armed five hooks for it on every launch. The whole
    // chain hung off that one flag: no flag -> no suppress -> our tab never appended -> our category
    // id never reached the page dispatch -> the populate swap never armed. The native menu went the
    // other way (one screen per page off the key-binding movie, open_screen below), which is what
    // ships.
    using RefMoveFn = void *(void *src, void *dst);                  // FUN_1407a7b60
    using PushJobFn = void *(void *menu, void *out2, void *ctxOut, void *jobPtr, void *tmp); // FUN_1407edfa0
    RefMoveFn *p_refmove = nullptr;
    PushJobFn *p_push_job = nullptr;
    void **p_worldchrman_slot = nullptr; // *slot != 0 => a game world is loaded (in-game)
    // The persistent base/root menu (the in-game HUD menu = *(CSMenuMan+0x80) while NO
    // full-screen menu is up). Pushing settings onto the BASE stacks it as an overlay
    // (like it stacks over gameplay); pushing onto the map instead REPLACES the map view.
    std::atomic<uintptr_t> g_base_menu{0};
    // Whatever menu is active this frame (see menu_update_detour) - the generic host.
    std::atomic<uintptr_t> g_active_menu{0};
    // CSMenuMan itself, for the read-only window scan when no window host is found.

    // g_form_pushed ("was the screen pushed rather than slotted - decides how the close is
    // noticed") and g_form_job stood here. Both were declared and never touched again: the screen
    // stack in g_screens carries `pushed` per level, and the close is noticed by heartbeat.
    // Liveness by ACTIVITY, not by inspecting the dialog: while our screen is up its row-draw
    // hook runs every frame, so a gap means it is gone. Reading the dialog's vtable instead
    // reported "closed" on a live screen, which dropped the one-screen guard and let presses
    // stack layers.
    // A g_form_pushed_host stood here, described as where liveness is read from. Nothing wrote or
    // read it; liveness is the per-screen heartbeat in g_screens (see beat_fresh).
    // TRUE per-frame liveness: the dialog's own update (FUN_14093F540, its vt+0x10) runs every
    // frame while the dialog exists. Row draws do not - they only happen when the view refreshes,
    // which is why timing them declared a static screen closed half a second after it opened.
    // The host's sequence slot was the next guess and it never reported empty, so the screen is
    // evidently not held there; this needs no slot to be guessed at all.
    // THE FULL SIGNATURE, and it matters. FUN_14093F540 is (this, float dt, uint8_t *consumed) -
    // the same shape as the window input dispatcher it tail-calls: rcx = dialog, xmm1 = dt,
    // r8 = the shared "input still available" byte, which it reads at 0x14093F5D1
    // (`movzx eax, byte [rdi]`, rdi = r8).
    // Declaring the detour with two parameters let the compiler use r8/xmm1 as scratch, so the
    // game got whatever was left there. It survived on luck until this function grew a little and
    // r8 came through as 0 - an access violation at that instruction with RDI=0 (dump 15:27),
    // which looked exactly like "the host we pushed onto is bad" and was not.
    using FormUpdateFn = void(void *dlg, float dt, void *consumed);
    FormUpdateFn *o_form_update = nullptr;
    // (A g_form_last_update timestamp was stamped here every frame and never read - the live
    //  liveness signal is form_dialog_ticked() below, keyed on g_form_update_watch.)

    // Set from the row-build hook once our dialog is known; the heartbeat compares against it
    // rather than the later-declared g_form_dialog.
    std::atomic<uintptr_t> g_form_update_watch{0};
    void log_form_command_table(uintptr_t dlg); // defined with the command helpers below
    void arm_action_log_on_escape();            // defined with the input-action probe below

    // Defined with the screen stack: stamps the heartbeat of whichever of our screens this is.
    void form_dialog_ticked(uintptr_t dlg);

    // Defined with the icon code: re-applies the row icon strips. Called AFTER the dialog's own
    // update, which is the only moment in the frame that is later than the timeline advance.
    void repaint_row_icons_after_advance(uintptr_t dlg);

    // ── which way is left/right physically held? ─────────────────────────────────────
    // Keyboard through GetAsyncKeyState, pad through the overlay's XInput cache (a direct
    // XInputGetState would read back the buttons our own hook injects). 0x0004/0x0008 =
    // XINPUT_GAMEPAD_DPAD_LEFT/RIGHT.
    int arrow_dir_held()
    {
        const uint16_t pad = goblin::overlay::gamepad_buttons();
        const bool left = (GetAsyncKeyState(VK_LEFT) & 0x8000) || (pad & 0x0004);
        const bool right = (GetAsyncKeyState(VK_RIGHT) & 0x8000) || (pad & 0x0008);
        return left == right ? 0 : (right ? 1 : -1);
    }

    // ── why there is no cursor guard here ────────────────────────────────────────────
    // A slider row and the grid both want left/right, and TWO attempts to take the press
    // away from the grid failed in game on 2026-08-04:
    //   1. snap an odd (right-column) cursor back to the left cell - never fired: the grid
    //      SKIPS the unfocusable empty filler and lands on the neighbouring ROW, so the
    //      landing index is even;
    //   2. remember {cursor +0xD4, top row +0x348} before this update and write the pair
    //      back after it - made it worse. The dialog's update repaints the highlight from
    //      the moved cursor BEFORE we restore the field, so the two desynced: the next
    //      up/down then appeared to skip a row (measured, reported).
    // The fix is not a better guard, it is a layout where the press has nowhere to go: a
    // slider is edited on its OWN screen holding exactly one row (build_value), so the
    // grid's move is refused by its own bounds. FUN_14093F540 itself never touches the
    // cursor - it calls the dialog's vt[8] and the command dispatcher - so there is no
    // earlier point in this detour to intercept anyway.

    void shift_section_after_advance(uintptr_t dlg); // defined with the layout helpers below

    void form_update_detour(void *dlg, float dt, void *consumed)
    {
        const uintptr_t d = reinterpret_cast<uintptr_t>(dlg);
        form_dialog_ticked(d);
        o_form_update(dlg, dt, consumed);
        // Menu centring, per INSTANCE: only a dialog of ours is moved, the player's own screen on
        // the same movie is not (the movie-level shift that did this for every instance is gone).
        shift_section_after_advance(d);
        // Order inside the frame is what decides whether an icon is visible at all. Writing the
        // strip position from our tick puts it BEFORE the timeline advance, and the advance
        // re-applies the authored matrix (DisplayList::MoveDisplayObject) - so the engine always
        // wrote last and the row showed the blank cell. Measured: the same object, the right x,
        // written every frame, still invisible; and scrolling fixed it because that write comes
        // from the render path. This is the same write, moved to after the update.
        repaint_row_icons_after_advance(d);
    }

    // ── Task #9 CAPTURE: log every menu-job push (FUN_1407edfa0) ──
    // Goal: learn the PUSH TARGET the settings sub-page open (tab 6/7/8 from our
    // map-hosted dialog) uses - that push lands the sub-movie OVER the live map with
    // a clean lifecycle. Once we know the target menu + call-site RA, we open OUR
    // top settings dialog through the same push (F11-direct, no memo hijack).
    // Read-only pass-through; dev-gated on debug_logging. Registered AFTER the
    // p_push_job scan (MinHook rewrites the prologue the AOB matches).
    // Task #9 CAPTURE 2: the GENERIC settings movie-job builder FUN_140808630
    // (out, owner, DESC{id,...}, flag). EVERY movie-based settings sub-open funnels
    // through it (Graphic tab, Brightness, and F11's own OptionSetting is its sibling
    // FUN_1408087e0). The pushJob capture came back empty for sub-pages, so this is
    // the funnel to watch: RA = the tab-select/decide site that opens the sub-page
    // over the map; owner = the target the sub-page attaches to. Read-only pass-through.
    using BuilderFn = void *(void *out, void *owner, void *desc, uint8_t flag,
                             void *p5, void *p6);
    BuilderFn *o_builder = nullptr;
    void *builder_detour(void *out, void *owner, void *desc, uint8_t flag,
                         void *p5, void *p6)
    {
        if (goblin::config::debugLogging)
        {
            const uintptr_t ra = reinterpret_cast<uintptr_t>(_ReturnAddress());
            const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            uint64_t ownerVt = 0;
            uint32_t descId = 0;
            __try
            {
                if (owner)
                    ownerVt = *reinterpret_cast<uint64_t *>(owner);
                if (desc)
                    descId = *reinterpret_cast<uint32_t *>(desc);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            spdlog::info("[subopen] builder owner=0x{:X} ownerVt=exe+0x{:X} descId=0x{:X} "
                         "flag={} RA=exe+0x{:X} (map=0x{:X})",
                         reinterpret_cast<uintptr_t>(owner),
                         ownerVt >= base ? ownerVt - base : ownerVt, descId, flag,
                         ra - base, reinterpret_cast<uintptr_t>(goblin::maphover::map_dialog()));
        }
        return o_builder(out, owner, desc, flag, p5, p6);
    }

    PushJobFn *o_push_job = nullptr;
    void *pushjob_detour(void *menu, void *out2, void *ctxOut, void *jobPtr, void *tmp)
    {
        if (goblin::config::debugLogging)
        {
            const uintptr_t ra = reinterpret_cast<uintptr_t>(_ReturnAddress());
            const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            uint64_t menuVt = 0;
            __try
            {
                if (menu)
                    menuVt = *reinterpret_cast<uint64_t *>(menu);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            spdlog::info("[pushjob] menu=0x{:X} vt=exe+0x{:X} job=0x{:X} RA=exe+0x{:X} "
                         "(base=0x{:X} map=0x{:X})",
                         reinterpret_cast<uintptr_t>(menu),
                         menuVt >= base ? menuVt - base : menuVt,
                         reinterpret_cast<uintptr_t>(jobPtr), ra - base,
                         g_base_menu.load(std::memory_order_relaxed),
                         reinterpret_cast<uintptr_t>(goblin::maphover::map_dialog()));
        }
        return o_push_job(menu, out2, ctxOut, jobPtr, tmp);
    }


    // ── OVER-MAP settings via the WorldMapDialog's sub-dialog JOB slot (v24) ──
    // The map runs its sub-dialogs (memo/warp) as a CS::MenuWindowJob held at
    // WorldMapDialog+0xA28 (found by host-scan; vt exe+0x2AA97E8). F11's build-job
    // (FUN_1408087e0) produces exactly a MenuWindowJob for the settings movie. So build
    // a settings MenuWindowJob and STORE it at WMD+0xA28 when the slot is empty; the WMD's
    // own update steps it -> settings movie + dialog + render OVER the live map. Heavily
    // logged so a crash's last line shows how far it got. Dev-only.
    // ── F11-DIRECT settings over the map (v29): create the 02_040 movie ourselves + build
    // the OptionSettingTopDialog against its scene, DEFERRED until the movie async-loads. No
    // memo/marker involvement. The created 02_040 movie renders over the map on its own (the
    // gfx mgr ticks it); we only need to construct the dialog once its scene root is populated
    // (root!=0), else the ctor faults on a null root (the v2/v3 crash). base+RVA calls (dev).
    // g_settings_movie (our own 02_040, async-loading), g_settings_dialog (the
    // OptionSettingTopDialog built against its scene) and g_form_rows_armed stood here. None was
    // ever written. g_settings_dialog was READ though, as the third host fallback in open_screen -
    // a branch that could therefore never be taken, and whose "our settings menu" label could never
    // be printed. Same defect the audit recorded for g_form_host, fifteen lines below it in the
    // same function.
    // The form's dialog only exists a few frames after its job is stored, so the page
    // title is stamped by the first row draw rather than at open time.
    std::atomic<bool> g_form_title_pending{false};
    // The live CS::KeyConfigDialog, recovered from the row-vector builder's argument
    // (the dialog's own list IS dlg+0x1268) and validated against the class vtable.
    std::atomic<uintptr_t> g_form_dialog{0};
    void set_form_captions(uintptr_t base, uintptr_t dlg, const wchar_t *title,
                           const wchar_t *hint);
    void prepare_form_layout(uintptr_t base, uintptr_t dlg);
    void probe_form_commands(uintptr_t base, uintptr_t dlg);
    // One-shot: dump the form's registered input commands (see probe_form_commands).
    std::atomic<bool> g_cmd_probe_done{false};
    // Live-apply for our native menu rows: snapshot of every row's bool at menu
    // open; the per-frame tick diffs and calls reapply_live_settings() on change
    // (same as the overlay does), close_settings() persists the ini once if
    // anything changed during the session.
    std::vector<uint8_t> g_menu_cfg_snapshot;
    bool g_menu_cfg_dirty = false;

    void menu_cfg_snapshot_take()
    {
        size_t n = 0;
        const goblin::NativeMenuRowDef *rows = goblin::native_menu_rows(&n);
        g_menu_cfg_snapshot.assign(n, 0);
        for (size_t i = 0; i < n; ++i)
            g_menu_cfg_snapshot[i] = *rows[i].value ? 1 : 0;
        g_menu_cfg_dirty = false;
    }

    void menu_cfg_apply_if_changed()
    {
        size_t n = 0;
        const goblin::NativeMenuRowDef *rows = goblin::native_menu_rows(&n);
        if (g_menu_cfg_snapshot.size() != n)
            return;
        bool changed = false;
        for (size_t i = 0; i < n; ++i)
        {
            const uint8_t cur = *rows[i].value ? 1 : 0;
            if (cur != g_menu_cfg_snapshot[i])
            {
                g_menu_cfg_snapshot[i] = cur;
                changed = true;
            }
        }
        if (changed)
        {
            g_menu_cfg_dirty = true;
            goblin::reapply_live_settings(); // live-apply, same as the overlay's change path
            spdlog::info("[nmenu] setting applied live");
        }
    }
    // g_settings_job and g_settings_scene stood here (the F11 build-job and its SceneObjProxy
    // buffer), with prose for a close_settings() and a per-frame dialog builder that are not in
    // this tree. All of it belonged to the F11 prototype removed above.

// The CommandList child prototype is KEPT but NOT BUILT (MFG_CMDLIST_PROTO, default 0). It is the
// working half of the CommandList RE session and the entry point cmdlist_notes.md points at for the
// unfinished native-menu work, so deleting it would throw away the result of that session - but it
// has no callers, and "do not delete" is not the same as "must ship". Behind the guard the source
// stays available and the DLL carries none of it: no hand-made game-ABI functors, no registrar call.
#if MFG_CMDLIST_PROTO
    // ── CommandList child prototype (RE cmdlist_notes.md, session 2026-07-23 night) ──
    // FUN_140747450(win, out, in) = open the native CommandList screen (movie
    // 02_045_PC_CommandList / 01_070_CommandList, dialog CS::CommandSelectDialog) as a
    // CHILD of any CS::MenuWindow: it collects the window's registered input commands
    // via vt+0x30 (base impl FUN_140744f30), builds the MenuJob (generic movie-sub-job
    // FUN_1407acb00) and stores it in the window's child-job holder @win+0xA28; the
    // window's own tick steps/renders it, Back pops it (v27-proven over the map).
    // ── Phase B: hand-made game-ABI std::function objects (MSVC _Func_impl vtable:
    // [0] copy(this,buf)->ptr [1] move [2] invoke [3] target_type [4] destroy(this,
    // dealloc)). The registrar FUN_140744540 and the entry copy-ctor FUN_140741820
    // deep-copy commands by cloning each holder's object via vt[0] into a 0x38
    // inline buffer, so our payload must stay POD and <= 0x30 bytes. Command entry
    // (0x140) layout (cmdlist_reg_re*.txt):
    //   +0x000 fn input-trigger char(byte* consumed) - REQUIRED (dispatcher derefs)
    //   +0x040 fn name/key pack provider             - REQUIRED (collect derefs):
    //          builds pack {DLString name @+0x8, byte @+0x40, DLString key @+0x48};
    //          collect FUN_140744f30 skips the row unless BOTH strings non-empty
    //   +0x080 fn extra (nullable)
    //   +0x0C0 fn action - row decide, DLRefPtr<MenuJob>* invoke(this, out)
    //   +0x100 fn visible-predicate char()           - REQUIRED (collect derefs)
    struct GameFnVt
    {
        void *copy;
        void *move;
        void *invoke;
        void *type;
        void *destroy;
    };
    struct GameFn
    {
        const GameFnVt *vt;
        uintptr_t a;
        uintptr_t b;
    };
    struct GameFnHolder
    {
        uint8_t buf[0x38];
        void *ptr; // null = empty std::function slot
    };
    static void *gamefn_copy(GameFn *self, void *buf)
    {
        auto *d = static_cast<GameFn *>(buf);
        *d = *self;
        return d;
    }
    static void gamefn_destroy(GameFn *, char) {}
    static void *gamefn_type(GameFn *)
    {
        static int s_type_tag;
        return &s_type_tag;
    }
    static char gamefn_trigger_never(GameFn *, void *) { return 0; }
    static char gamefn_pred_true(GameFn *) { return 1; }
    static void *gamefn_action_log(GameFn *self, void **out)
    {
        *out = nullptr; // no sub-job yet - just prove the decide path reaches us
        spdlog::info("[cmdlist] our row (labelId={}) activated", self->a);
        return out;
    }
    // Build the name/key pack: DLString ctor from GR_MenuText (FUN_140760970) with
    // our injected label ids (a = row label = name, b = tab label = key stand-in).
    static void *gamefn_namekey_pack(GameFn *self, uint8_t *out)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        using TextCtorFn = void(void *, uint32_t);
        auto p_text = reinterpret_cast<TextCtorFn *>(goblin::anchors::at(goblin::AnchorId::caption_text_ctor));
        std::memset(out, 0, 0x88);
        p_text(out + 0x8, static_cast<uint32_t>(self->a));
        p_text(out + 0x48, static_cast<uint32_t>(self->b));
        return out;
    }
    static void gamefn_init(GameFnHolder &h, const GameFnVt *vt, uintptr_t a, uintptr_t b)
    {
        auto *f = reinterpret_cast<GameFn *>(h.buf);
        f->vt = vt;
        f->a = a;
        f->b = b;
        h.ptr = h.buf;
    }

    // Register our commands on the dialog via FUN_140744540(win, inputSpec,
    // actionFn, predFn); inputSpec = 3 contiguous holders (trigger/pack/extra).
    // Everything is deep-copied by the registrar, so stack lifetime is fine.
    //
    // NOT CONNECTED TO ANYTHING (verified 2026-07-30): this function has no callers, and with it the
    // whole hand-made game-ABI std::function cluster above is unused. It is KEPT DELIBERATELY - it is
    // the working half of the CommandList RE session and the entry point that
    // scratch/endgame_research/cmdlist_notes.md points at for the unfinished native-menu work. Do not
    // remove it as dead code without settling that work first; do not assume it is wired up either.
    void register_our_cmdlist_rows(uintptr_t dlg)
    {
        if (goblin::g_menutext_tab_id <= 0 || goblin::g_menutext_row_ids.empty())
        {
            spdlog::info("[cmdlist] no injected GR_MenuText labels - cannot register rows");
            return;
        }
        static const GameFnVt s_trig_vt{reinterpret_cast<void *>(&gamefn_copy),
                                        reinterpret_cast<void *>(&gamefn_copy),
                                        reinterpret_cast<void *>(&gamefn_trigger_never),
                                        reinterpret_cast<void *>(&gamefn_type),
                                        reinterpret_cast<void *>(&gamefn_destroy)};
        static const GameFnVt s_pack_vt{reinterpret_cast<void *>(&gamefn_copy),
                                        reinterpret_cast<void *>(&gamefn_copy),
                                        reinterpret_cast<void *>(&gamefn_namekey_pack),
                                        reinterpret_cast<void *>(&gamefn_type),
                                        reinterpret_cast<void *>(&gamefn_destroy)};
        static const GameFnVt s_action_vt{reinterpret_cast<void *>(&gamefn_copy),
                                          reinterpret_cast<void *>(&gamefn_copy),
                                          reinterpret_cast<void *>(&gamefn_action_log),
                                          reinterpret_cast<void *>(&gamefn_type),
                                          reinterpret_cast<void *>(&gamefn_destroy)};
        static const GameFnVt s_pred_vt{reinterpret_cast<void *>(&gamefn_copy),
                                        reinterpret_cast<void *>(&gamefn_copy),
                                        reinterpret_cast<void *>(&gamefn_pred_true),
                                        reinterpret_cast<void *>(&gamefn_type),
                                        reinterpret_cast<void *>(&gamefn_destroy)};
        using RegisterFn = void(void *, void *, void *, void *);
        auto p_reg = reinterpret_cast<RegisterFn *>(goblin::anchors::at(goblin::AnchorId::caption_register));
        struct InputSpec
        {
            GameFnHolder trig;
            GameFnHolder pack;
            GameFnHolder extra;
        };
        size_t added = 0;
        for (int label_id : goblin::g_menutext_row_ids)
        {
            if (label_id <= 0)
                continue;
            InputSpec spec{};
            gamefn_init(spec.trig, &s_trig_vt, 0, 0);
            gamefn_init(spec.pack, &s_pack_vt, static_cast<uintptr_t>(label_id),
                        static_cast<uintptr_t>(goblin::g_menutext_tab_id));
            spec.extra.ptr = nullptr;
            GameFnHolder action{}, pred{};
            gamefn_init(action, &s_action_vt, static_cast<uintptr_t>(label_id), 0);
            gamefn_init(pred, &s_pred_vt, 0, 0);
            p_reg(reinterpret_cast<void *>(dlg), &spec, &action, &pred);
            ++added;
        }
        spdlog::info("[cmdlist] registered {} of our commands on dlg=0x{:X}", added, dlg);
    }
#endif // MFG_CMDLIST_PROTO

    // ── Our rows ON the native keybinding form (02_160) ──────────────────────────
    // RE (keysetting_*_re.txt): KeyConfigDialog = GenericItemSelectDialog<CSMenuKeySetting>
    // over movie clips "KeySetting/ItemList" (+ ScrollBarV/V2 = native scroll). Its rows
    // are a vector of CSMenuKeySetting items (stride 0x50) built by
    // FUN_140868590(itemVec, list) -> mode 0 FUN_140868990 / mode 1 FUN_1408687d0, which
    // walk a PARAM table (FD4ParamHeaderConstAccessor, row stride 0x18) and per row call
    //   FUN_140866f80(item, list+8, deviceKind, paramRow)  - normal row
    //   FUN_140867010(item, deviceKind, paramRow)          - header row (item+0x18 = 0)
    // then append with FUN_140868fe0(&vec.alloc, item) and finish each with
    // FUN_140867820(item, list+8).
    // Item fields that drive rendering (vt+0x8 = FUN_1408674e0):
    //   +0x08 u32 deviceKind, +0x10 paramRow ptr, +0x18 bind-slot ptr (0 => HEADER row),
    //   +0x20 assigning flag, +0x30/+0x31 conflict flags.
    // paramRow layout used by the renderer: [0] u32 = FMG id in GR_MenuText (row NAME -
    // exactly the bank we already inject our localized labels into), [1] s32 = action
    // index (-1 => header; also the index into the 54-slot bind table via
    // FUN_140866ca0), byte +9 / byte +10 = enabled per device kind.
    // Headers additionally switch the row clip to frame "PadCategory"/"KeyCategory".
    // So OUR rows need nothing exotic: a small array of fake param rows pointing at our
    // injected FMG ids. The value column (Text_1) comes from FUN_140867de0(item, out) -
    // hooked below to hand back our localized On/Off instead of a key name.
    struct FakeParamRow
    {
        int32_t text_id;   // [0] GR_MenuText id - only a fallback (we draw rows ourselves),
                           //     but must stay VALID in case the draw hook is missing.
        int32_t action_ix; // [1] bind-table index; >= 0 keeps the row selectable
        int32_t pad2;
        int32_t pad3;      // bytes +8/+9/+10 = per-device enabled flags
        int32_t pad4;
    };
    static_assert(sizeof(FakeParamRow) == 0x14, "param row stride");

    // Bridge between the native item list and the menu MODEL (goblin_native_menu.*).
    // The native item ctor stores a pointer to the fake param row, and those items can
    // outlive a row rebuild - so the mapping must NOT depend on the array's address:
    //   * the pool is allocated once and never reallocates (a dangling pointer would
    //     otherwise be read by the draw hook), and
    //   * identity travels IN the param row itself: a magic tag plus a generation
    //     counter and the model index, so a stale item from a previous build is
    //     recognised and ignored instead of drawing the wrong row.
    struct FormItem
    {
        FakeParamRow param;
    };
    // How many items the last build actually put in the view list. The grid's own count runs
    // ONE build behind (measured: page 0 built 30 items with count=0, page 1 built 86 with
    // count=30), and with 2 columns a count of 30 caps the list at 15 rows - which is exactly
    // what a 43-row page showed. So the count is restated from this, AFTER the view refresh.
    std::atomic<uint32_t> g_form_landed{0};
    // before<<32 | after, so one log line shows whether the write to the grid's count survives.
    // Defined with the grid layout further down; both the first build and every refresh use it.
    void sync_grid_extent(uintptr_t dlg, uint32_t items);
    constexpr uint32_t kFormMagic = 0x4D464752; // 'MFGR'
    constexpr size_t kFormPoolMax = 512;        // hard cap: 97 ini entries + extras
    // ONE POOL PER NESTING LEVEL. A native item keeps a POINTER to its fake param row, so with a
    // single shared pool a child screen's build overwrote the rows the parent's items still point
    // at - in game the parent came back with its icons gone and needed a second confirm press.
    // Each level therefore owns its rows for as long as that screen lives.
    constexpr size_t kMaxNest = 6;
    std::vector<FormItem> g_form_pools[kMaxNest];
    size_t g_form_depth = 0;

    std::vector<FormItem> &form_pool()
    {
        return g_form_pools[g_form_depth < kMaxNest ? g_form_depth : kMaxNest - 1];
    }
    // One generation per pool: a rebuild on level 2 must not invalidate the items level 0 still
    // holds. A single counter did, and the parent came back with dead rows.
    uint32_t g_form_generation[kMaxNest] = {};

    // Which pool does this param row live in? The pools are reserved once and never move, so
    // their address ranges identify the level an item belongs to - no tag needed, and an item
    // from a level that is not the live one is simply left alone.
    int pool_level_of(const void *param)
    {
        const uintptr_t a = reinterpret_cast<uintptr_t>(param);
        for (size_t l = 0; l < kMaxNest; ++l)
        {
            const std::vector<FormItem> &p = g_form_pools[l];
            if (p.empty())
                continue;
            const uintptr_t b = reinterpret_cast<uintptr_t>(p.data());
            if (a >= b && a < b + p.size() * sizeof(FormItem))
                return static_cast<int>(l);
        }
        return -1;
    }

    // ── ONE NATIVE SCREEN PER PAGE ───────────────────────────────────────────────────
    // Every page is a real screen, and EVERY screen - the root as much as a sub-page - is
    // stored in its host window's SEQUENCE slot (+0x10). That slot is not a detail: it is what
    // the engine's own input gate reads. The window input dispatcher FUN_140745570 runs a
    // window's commands only when
    //     job not done (win+0x1E8) AND sequence slot EMPTY (FUN_1407A9230 on win+0x10)
    //     AND vt[0x50](win) == 0   (that one is `*(u64*)(win+0x98) > 1`, a counter - it does
    //                               NOT look at the child-job holder +0xA28)
    // so a screen parked anywhere else leaves its host still eating the same input. Measured at
    // the title screen: pushed onto the menu's job stack, our screen took input AND the main
    // menu behind it took the same presses and opened its own pages under ours.
    // Vanilla agrees - its "Settings -> Key Assignments" opener FUN_14094FD40 ends in
    // `p_seq(window + 0x10, job)`, the same call we make. (It stores a SEQUENCE of three jobs:
    // ours plus two lambdas that handle the page transition. We store the screen alone, which is
    // why the host stays visible behind our screen instead of being swapped out.)
    //
    // Everything hangs off ONE stack. Each entry point that carries a dialog pointer (row build,
    // decide, the dialog's own per-frame update) resolves it to a level, so "which page is live"
    // is read from the engine rather than tracked in parallel with it. That was the flaw in the
    // first attempt: an arm flag, a screen->page map and three close-detection schemes drifting
    // apart.
    //
    // Cursor + first visible row of a list, saved per screen (both are plain grid fields; the
    // offsets and how they were found are documented with the grid layout below).
    struct ListPos
    {
        uint32_t cursor;
        int32_t top_row;
    };

    struct Screen
    {
        uintptr_t dlg = 0;  // its dialog - known once its first row build arrives
        int32_t page = 0;   // the model page this screen shows
        uintptr_t host = 0; // the window whose sequence slot holds it
        uintptr_t job = 0;
        ListPos pos{0, 0};  // saved while a child screen is over it
        uint64_t beat = 0;  // last time its own update ran (0 = not seen yet)
        uint64_t opened = 0;
        uint64_t shown = 0; // when its dialog first appeared
        bool hidden = false; // stepped aside while a child screen is over it
        bool shifted = false;  // the centring shift has been applied to this screen
        float shift_tx = 0.0f; // the tx we want the row section to sit at (twips)
        // Frames the centring shift has been asserted for since the screen was shown (or shown
        // again): the intro re-places the section for its first frames, so the shift is written
        // from the dialog's own update until that has run its course, then left alone.
        uint32_t shift_frames = 0;
    };

    // How fresh a screen's heartbeat must be before we may call INTO the engine on its dialog.
    // kBeatStaleMs (further down) is a PRUNING threshold - how long we wait before declaring a
    // screen gone - and it is far too loose for this: measured on the Deck, a dialog's memory is
    // already scrambled about two frames after its last tick (the debug scroll telemetry read
    // count=3161501440 at +32 ms). Engine calls want a few frames, not half a second.
    constexpr uint64_t kEngineCallFreshMs = 100;
    inline bool safe_to_call_engine(const Screen &s, uint64_t now)
    {
        return s.beat != 0 && now - s.beat < kEngineCallFreshMs;
    }

    // UI thread only: every writer (row build, decide, dialog update, the menu tick that polls
    // F8) runs on the menu thread.
    std::vector<Screen> g_screens;

    // ── why a brand-new screen must ignore input for a moment ────────────────────────
    // A window's input state is its own (the dispatcher refreshes win+0x120 each frame), so a
    // window that did not exist yet when a button went down sees that button as newly pressed on
    // its first frame. The window it was opened FROM does not: it has been watching the same
    // button all along, which is why Q closing a child does not also close the parent.
    // Vanilla never notices because its sub-screens arrive with a movie load from disk - by then
    // the button is long released. Ours comes up in ONE frame (the movie is already resident, it
    // is the same one the parent uses), so the confirm that opened a page immediately confirmed
    // that page's first row. Measured: parent decide 10:59:07.303 -> child dialog .322 -> child
    // decide on row 0 at .323.
    // Until a new window's input snapshot can be seeded from its parent, a fresh screen simply
    // does not act on input for this long.
    constexpr uint64_t kNewScreenInputGraceMs = 250;

    int screen_level(uintptr_t dlg)
    {
        if (!dlg)
            return -1;
        for (size_t i = 0; i < g_screens.size(); ++i)
            if (g_screens[i].dlg == dlg)
                return static_cast<int>(i);
        return -1;
    }


    // ── HUD-after-close diagnostic ───────────────────────────────────────────────────────────────
    // Opening our screen over GAMEPLAY pushes a job onto the base HUD menu (*(CSMenuMan+0x80)); after
    // our screen closes the game's HUD stays hidden until some native menu opens. The engine therefore
    // keeps a piece of state that our close never puts back. Rather than guess which, snapshot the base
    // menu (and CSMenuMan) when we push, and again once every screen of ours is gone, then log the
    // offsets that differ and stayed different - that names the field. Debug-logging only.
    constexpr size_t kHudSnapBytes = 0x400;
    uint8_t g_hud_snap_menu[kHudSnapBytes];
    uint8_t g_hud_snap_man[0x200]; // covers the whole menu-state array at +0x90..+0xD6
    std::atomic<uintptr_t> g_hud_snap_base{0};
    std::atomic<int> g_hud_snap_state{0}; // 1 = taken at push, 2 = compared after close

    void *g_hud_orig_job = nullptr; // the HUD menu's own sequence-slot job, kept alive across our screen
    std::atomic<uint64_t> g_hudwatch_until{0}; // keep dumping the HUD state window until this tick count
    uint8_t g_hudwatch_last[64] = {};
    std::atomic<uint64_t> g_hudwatch_next{0};

    // ── HUD mode: restore what our over-gameplay screen leaves behind ────────────────────────────
    // ROOT CAUSE, established 2026-07-28 by reading and then poking the LIVE process:
    // `CS::CSFeManImp+0x78` is the HUD's own visibility mode - 3 = HUD on in gameplay, 1 = hidden
    // under a menu, 0 = what is left after our pushed screen closes. In the stuck state the engine
    // genuinely believes the HUD is off (mode 0, the 31 per-widget flags cleared, menu ids 7 and 26
    // demoted to registered-only); writing 3 back brought the HUD up instantly AND STUCK, which also
    // proves the mode is edge-driven rather than recomputed per frame. The engine reaches 3 again only
    // through its own 0 -> 1 -> 3 transition, which is why opening and closing any native menu fixes it.
    //
    // So the fix is symmetric cleanup, not a policy: capture the mode before we push and put that
    // exact value back once every screen of ours is gone. Two guards keep it honest - we only write
    // when the mode is still 0 (so a legitimate "menu on top" state is never overridden), and the
    // singleton is only trusted when the CSFeAutoHideCtrl vtable is where we expect it at +0x4E70,
    // so on a game update that moves the layout this quietly does nothing instead of corrupting.
    void **g_feman_slot = nullptr;   // resolved by AOB in the setup below
    uint8_t g_hud_mode_at_push = 0;  // 0 = nothing captured

    uintptr_t feman_checked()
    {
        if (!g_feman_slot)
            return 0;
        __try
        {
            const uintptr_t f = reinterpret_cast<uintptr_t>(*g_feman_slot);
            if (!v3_heap_ptr(f))
                return 0;
            const uintptr_t vt_ctrl = goblin::anchors::vtable_of(kVtAutoHideCtrl);
            if (!vt_ctrl || *reinterpret_cast<uintptr_t *>(f + 0x4E70) != vt_ctrl)
                return 0; // layout moved - do not touch anything
            return f;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    void hud_mode_capture()
    {
        g_hud_mode_at_push = 0;
        const uintptr_t f = feman_checked();
        if (!f)
            return;
        __try
        {
            g_hud_mode_at_push = *reinterpret_cast<uint8_t *>(f + 0x78);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    void hud_mode_restore(const char *via)
    {
        const uint8_t want = g_hud_mode_at_push;
        g_hud_mode_at_push = 0;
        if (!want) // nothing captured, or the HUD was already off before us - leave it alone
            return;
        const uintptr_t f = feman_checked();
        if (!f)
            return;
        __try
        {
            uint8_t *mode = reinterpret_cast<uint8_t *>(f + 0x78);
            if (*mode != 0)
                return; // the engine is in a state of its own now; ours is not the last word
            *mode = want;
            spdlog::info("[hud] mode was left at 0 by our screen; restored the value from before it "
                         "({}) via {}",
                         want, via);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    void log_autohide(const char *when); // defined below, next to the snapshot pair
    void log_menuman_map(const char *when, bool with_objects);

    // ── CSMenuMan, read as a map instead of a hex dump ───────────────────────────────────────────
    // Two facts turn this manager from a wall of bytes into something readable.
    //
    // 1. The menu-state array at CSMenuMan+0x90 is indexed by the MENU ID ITSELF. The engine's
    //    accessor `0x140767DF0(scratch, id)` is three instructions - `mov word [rcx], dx; mov rax, rcx;
    //    ret` - so it only wraps the id; every reader then does `state = [id + this + 0x90]` after a
    //    `cmp id, 0x47` bound check. The state is THREE bits (`and al, 7`, and `or byte[..], 7` exists
    //    at 0x14074571A), with bit 0 = registered, bit 1 = visible; the engine's own hide is
    //    `and byte [id + this + 0x90], 1` at 0x1408D4447, i.e. it KEEPS the registration.
    // 2. The exe ships RTTI, so any pointer in the object can be named offline from its vtable RVA
    //    (scratch/rtti_map.py).
    //
    // So this logs the ids that are registered/visible, and every field that looks like an object,
    // with its vtable RVA. Diffing two of these by hand names both the leftovers and the participants.
    void log_menuman_map(const char *when, bool with_objects)
    {
        if (!goblin::config::debugLogging)
            return;
        const uintptr_t man = g_menuman.load(std::memory_order_relaxed);
        if (!v3_heap_ptr(man))
            return;
        __try
        {
            const uintptr_t exe_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            char vis[220], reg[220];
            int vn = 0, rn = 0;
            for (int id = 0; id < 0x47; ++id)
            {
                const uint8_t st = *reinterpret_cast<uint8_t *>(man + 0x90 + id);
                if (!st)
                    continue;
                if ((st & 2) && vn < (int)sizeof(vis) - 12)
                    vn += _snprintf_s(vis + vn, sizeof(vis) - vn, _TRUNCATE, "%d(%u) ", id, st);
                else if (!(st & 2) && rn < (int)sizeof(reg) - 12)
                    rn += _snprintf_s(reg + rn, sizeof(reg) - rn, _TRUNCATE, "%d(%u) ", id, st);
            }
            vis[vn] = 0;
            reg[rn] = 0;
            spdlog::info("[menumap] {}: VISIBLE ids [{}] | registered-only ids [{}]", when, vis, reg);
            // Object fields. 0x600 bytes covers the state array and well past it.
            if (!with_objects)
                return;
            for (unsigned off = 0; off < 0x600; off += 8)
            {
                const uintptr_t v = *reinterpret_cast<uintptr_t *>(man + off);
                if (!v3_heap_ptr(v))
                    continue;
                const uintptr_t vt = *reinterpret_cast<uintptr_t *>(v);
                if (vt <= exe_base || vt - exe_base >= 0x10000000)
                    continue;
                spdlog::info("[menumap]   +0x{:X} -> 0x{:X} (vt exe+0x{:X})", off, v, vt - exe_base);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    // Sampled from the menu-manager update while the watch is armed: prints the state window only when it
    // CHANGES, so the log shows the exact transition that brings the HUD back instead of a wall of dumps.
    void hud_watch_sample(void *menuman)
    {
        const uint64_t until = g_hudwatch_until.load(std::memory_order_acquire);
        if (!until || !menuman)
            return;
        const uint64_t now = GetTickCount64();
        if (now > until)
        {
            g_hudwatch_until.store(0, std::memory_order_release);
            spdlog::info("[hudprobe] watch window closed");
            return;
        }
        if (now < g_hudwatch_next.load(std::memory_order_relaxed))
            return;
        g_hudwatch_next.store(now + 250, std::memory_order_relaxed);
        __try
        {
            const uint8_t *b = reinterpret_cast<const uint8_t *>(menuman) + 0x80;
            if (memcmp(b, g_hudwatch_last, sizeof(g_hudwatch_last)) == 0)
                return;
            char hex[3 * 64 + 1];
            int n = 0;
            for (int i = 0; i < 64 && n < (int)sizeof(hex) - 4; ++i)
                n += _snprintf_s(hex + n, sizeof(hex) - n, _TRUNCATE, "%02X ", b[i]);
            // show which offsets moved since the previous sample - that is the useful part
            char diff[256];
            int dn = 0;
            for (int i = 0; i < 64 && dn < (int)sizeof(diff) - 24; ++i)
                if (b[i] != g_hudwatch_last[i])
                    dn += _snprintf_s(diff + dn, sizeof(diff) - dn, _TRUNCATE, "+0x%02X:%02X->%02X ",
                                      0x80 + i, g_hudwatch_last[i], b[i]);
            diff[dn] = 0;
            memcpy(g_hudwatch_last, b, sizeof(g_hudwatch_last));
            spdlog::info("[hudprobe] state changed while the HUD was hidden: {}", diff);
            log_autohide("while the HUD was hidden");
            log_menuman_map("while the HUD was hidden", false);
            spdlog::info("[hudprobe]   full window now: {}", hex);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    // The job stack our over-gameplay screen is pushed onto is a DLFixedVector (capacity 8) at
    // menu+0xD0: the push is 0x1407AA400, which bails with "out of memory." past 8 and keeps its element
    // COUNT at vector+0x48, i.e. menu+0x118, with the elements packed 8 bytes each from align8(vector).
    // The user's reading of the defect is that we never take our job back out - the HUD hides normally
    // under any screen and simply never learns that ours is gone. This prints the count and the slots so
    // that is a measured fact rather than a theory.
    // One line per DLFixedVector: its count and every element, each named by the RVA of its vtable.
    // The exe ships full MSVC RTTI, so scratch/rtti_map.py turns those RVAs into class names offline
    // (`py scratch/rtti_map.py 0x2a93a60` -> CS::MenuWindow), which is how a bare heap address in this
    // log becomes an identified object.
    void log_one_vector(uintptr_t vec, const char *label, uintptr_t exe_base)
    {
        __try
        {
            const uint64_t count = *reinterpret_cast<uint64_t *>(vec + 0x48);
            char slots[420];
            int n = 0;
            for (uint64_t i = 0; i < count && i < 8 && n < (int)sizeof(slots) - 48; ++i)
            {
                const uintptr_t obj = *reinterpret_cast<uintptr_t *>(vec + i * 8);
                unsigned long long vt = 0;
                if (v3_heap_ptr(obj))
                {
                    const uintptr_t vtp = *reinterpret_cast<uintptr_t *>(obj);
                    if (vtp > exe_base && vtp - exe_base < 0x10000000)
                        vt = (unsigned long long)(vtp - exe_base);
                }
                n += _snprintf_s(slots + n, sizeof(slots) - n, _TRUNCATE, "0x%llX(vt exe+0x%llX) ",
                                 (unsigned long long)obj, vt);
            }
            slots[n] = 0;
            spdlog::info("[jobstack]   {}: count={} [{}]", label, count, slots);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    // The job stack our over-gameplay screen is pushed onto is a DLFixedVector (capacity 8) at
    // menu+0xD0: the push is 0x1407AA400, which bails with "out of memory." past 8 and keeps its element
    // COUNT at vector+0x48, i.e. menu+0x118, with the elements packed 8 bytes each from align8(vector).
    // There is a SECOND such vector at menu+0x120 (count menu+0x168): 0x140733D70 is the engine's
    // remove-by-value for this container type and its asserts name DLFixedVector.inl, with the element
    // storage starting at align8(base) and the count at base+0x48, so +0x168 is that second vector's
    // count. Measured over an F8 cycle: the +0xD0 vector reads 0 both before and after, while +0x168
    // goes 0 -> 1 and stays, so something of ours settles in the second one. Naming the leftover object
    // is the point of this probe: writing that count back was already tried and did not restore the HUD
    // (docs/research_hud_after_close.md, excluded hypothesis 2), so the leftover matters as an identity,
    // not as a number.
    void log_job_stack(uintptr_t menu, const char *when)
    {
        if (!goblin::config::debugLogging || !v3_heap_ptr(menu))
            return;
        __try
        {
            const uintptr_t exe_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            unsigned long long menu_vt = 0;
            const uintptr_t mvt = *reinterpret_cast<uintptr_t *>(menu);
            if (mvt > exe_base && mvt - exe_base < 0x10000000)
                menu_vt = (unsigned long long)(mvt - exe_base);
            const uintptr_t seq = *reinterpret_cast<uintptr_t *>(menu + 0x10);
            unsigned long long seq_vt = 0;
            if (v3_heap_ptr(seq))
            {
                const uintptr_t svt = *reinterpret_cast<uintptr_t *>(seq);
                if (svt > exe_base && svt - exe_base < 0x10000000)
                    seq_vt = (unsigned long long)(svt - exe_base);
            }
            spdlog::info("[jobstack] {}: menu 0x{:X} (vt exe+0x{:X}), slot +0x10 = 0x{:X} (vt exe+0x{:X})",
                         when, menu, menu_vt, seq, seq_vt);
            log_one_vector(menu + 0xD0, "vector +0xD0", exe_base);
            log_one_vector(menu + 0x120, "vector +0x120", exe_base);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    // ── CS::CSFeAutoHideCtrl, the engine's own HUD hide/fade controller ──────────────────────────
    // Found by name, not by guessing: the exe ships RTTI, so scratch/rtti_map.py lists CS::CSFeAutoHideCtrl,
    // and its vtable (exe+0x2A9CC50) is stored at +0x4E70 of CS::CSFeManImp - the HUD manager - whose
    // singleton pointer the init at exe+0x766068 writes to exe+0x3D6B880 (object size 0x8420).
    //
    // Layout from the controller's own constructor (exe+0x767E60):
    //   +0x08  byte   mode, built as 2
    //   +0x0C  float  6.0 - the idle delay
    //   +0x68  42 slots of 0x40 bytes: dword state at +0x00, byte at +0x2C, dword at +0x30 = -1
    //   +0xAF0 dword, +0xAF4 byte, +0xAF8 byte, +0xAFC dword, +0xB00 float -1.0
    // The FeMan update (exe+0x775170) picks one of three paths by a mode byte: tick (exe+0x768320),
    // plain update (exe+0x767F70) or RESET (exe+0x768240, also the ctor's tail).
    //
    // This is a read-only probe. If the HUD stays hidden because this controller is left in a hidden
    // state that nothing re-evaluates until a menu transition, the fields below will say so, and the fix
    // is then to call the engine's own reset rather than to write any of this memory.
    void log_autohide(const char *when)
    {
        if (!goblin::config::debugLogging)
            return;
        __try
        {
            // Through the AOB-resolved slot, not the literal exe+0x3D6B880 this used to read:
            // that address is a .data slot and it MOVES between exe builds (2.2.3 keeps the
            // same singleton at +0x20 from here), so on any other build the literal read a
            // neighbouring pointer. The scan that owns this slot already runs in setup.
            if (!g_feman_slot)
                return;
            const uintptr_t feman = reinterpret_cast<uintptr_t>(*g_feman_slot);
            if (!v3_heap_ptr(feman))
                return;
            // THE HUD MODE. A hardware write watch on menu id 7's state byte caught the hide in the act
            // at exe+0x7754B6 and named its caller chain; the setter that owns that write is
            // exe+0x775320, and it opens with:
            //     eax = byte [feman + 0x82C1]      ; signed override
            //     esi = byte [feman + 0x78]        ; mode
            //     if (al >= 0) esi = eax           ; cmovns - the override wins when not negative
            //     switch (esi) { ... show/hide the menu set for this mode ... }
            // and the per-id writes are `if (registered) { show ? state |= 3 : state &= 1 }`. So the
            // state array is RECOMPUTED from this mode every frame, which is exactly why writing 3 back
            // into the array never brought the HUD back. Same idiom at exe+0x7752DB and exe+0x76E030.
            // The override has only three references in the whole exe and all three are reads, so
            // whoever sets it does so through a computed pointer - i.e. it can only be caught at runtime.
            // THE ACTUAL HUD VISIBILITY, measured rather than assumed. The mode-1 branch of the setter
            // (exe+0x775369..exe+0x775436) clears exactly these per-widget flag bytes, so they ARE the
            // "is this HUD element drawn" state:
            //   singles +0xC8, +0x2B8, +0x4A8, +0x5F8, +0x748, +0xF20, +0x1200, +0x1208,
            //           +0x3648, +0x4D58, +0x4E58, +0x6568
            //   arrays  +0x9E0 stride 0x150 x4, +0x1250 stride 0x128 x8, +0x1F08 stride 0x128 x7
            // Every previous round of this investigation correlated against the PHRASE "while the HUD was
            // hidden", which was a fixed log label and not a measurement - and both candidate switches
            // (the menu-state array and the mode byte) were seen back at their gameplay values while the
            // HUD was believed hidden. This digest ends that: it says what the engine thinks is drawn.
            {
                static const unsigned singles[] = {0xC8,   0x2B8,  0x4A8,  0x5F8,  0x748,  0xF20,
                                                   0x1200, 0x1208, 0x3648, 0x4D58, 0x4E58, 0x6568};
                char d[220];
                int n = 0;
                int on = 0, total = 0;
                for (unsigned off : singles)
                {
                    const uint8_t v = *reinterpret_cast<uint8_t *>(feman + off);
                    ++total;
                    if (v)
                        ++on;
                    if (n < (int)sizeof(d) - 16)
                        n += _snprintf_s(d + n, sizeof(d) - n, _TRUNCATE, "%X:%u ", off, v);
                }
                struct Arr { unsigned base, stride, count; };
                static const Arr arrays[] = {{0x9E0, 0x150, 4}, {0x1250, 0x128, 8}, {0x1F08, 0x128, 7}};
                for (const Arr &a : arrays)
                {
                    if (n < (int)sizeof(d) - 20)
                        n += _snprintf_s(d + n, sizeof(d) - n, _TRUNCATE, "| %X[", a.base);
                    for (unsigned i = 0; i < a.count; ++i)
                    {
                        const uint8_t v = *reinterpret_cast<uint8_t *>(feman + a.base + i * a.stride);
                        ++total;
                        if (v)
                            ++on;
                        if (n < (int)sizeof(d) - 6)
                            n += _snprintf_s(d + n, sizeof(d) - n, _TRUNCATE, "%u", v);
                    }
                    if (n < (int)sizeof(d) - 4)
                        n += _snprintf_s(d + n, sizeof(d) - n, _TRUNCATE, "] ");
                }
                d[n] = 0;
                spdlog::info("[hudflags] {}: {}/{} widget flags set - {}", when, on, total, d);
            }
            spdlog::info("[feman] {}: mode +0x78={} override +0x82C1={} (effective {})", when,
                         *reinterpret_cast<uint8_t *>(feman + 0x78),
                         *reinterpret_cast<int8_t *>(feman + 0x82C1),
                         *reinterpret_cast<int8_t *>(feman + 0x82C1) >= 0
                             ? *reinterpret_cast<int8_t *>(feman + 0x82C1)
                             : *reinterpret_cast<int8_t *>(feman + 0x78));
            const uintptr_t c = feman + 0x4E70;
            // Trust nothing: the vtable must still be the class we identified, or the offsets moved.
            // The address is a .rdata vtable, which no byte anchor can follow, so it is found by
            // the class's own RTTI name instead - that works on every build, where a baked address
            // matched on exactly one.
            const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            const uintptr_t want = goblin::anchors::vtable_of(kVtAutoHideCtrl);
            const uintptr_t vt = *reinterpret_cast<uintptr_t *>(c);
            if (!want || vt != want)
            {
                spdlog::info("[autohide] {}: ctrl vt is exe+0x{:X}, expected exe+0x{:X} - layout moved",
                             when, vt > base ? vt - base : 0, want > base ? want - base : 0);
                return;
            }
            int busy = 0;
            char first[120];
            int n = 0;
            for (int i = 0; i < 42; ++i)
            {
                const uintptr_t slot = c + 0x68 + i * 0x40;
                const uint32_t st = *reinterpret_cast<uint32_t *>(slot);
                const int32_t id = *reinterpret_cast<int32_t *>(slot + 0x30);
                if (st || id != -1)
                {
                    ++busy;
                    if (n < (int)sizeof(first) - 24)
                        n += _snprintf_s(first + n, sizeof(first) - n, _TRUNCATE, "[%d]st=%u id=%d ", i,
                                         st, id);
                }
            }
            first[n] = 0;
            spdlog::info("[autohide] {}: mode={} delay={:.2f} +0xAF0={} +0xAF4={} +0xAF8={} +0xAFC={} "
                         "+0xB00={:.2f} busy={}/42 {}",
                         when, *reinterpret_cast<uint8_t *>(c + 8),
                         *reinterpret_cast<float *>(c + 0xC),
                         *reinterpret_cast<uint32_t *>(c + 0xAF0),
                         *reinterpret_cast<uint8_t *>(c + 0xAF4),
                         *reinterpret_cast<uint8_t *>(c + 0xAF8),
                         *reinterpret_cast<uint32_t *>(c + 0xAFC),
                         *reinterpret_cast<float *>(c + 0xB00), busy, first);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    void hud_snapshot_take(uintptr_t base_menu)
    {
#if !MFG_STALL_PROFILER
        // MUST leave shipping builds together with hud_snapshot_compare, which is where its
        // matching RELEASE and slot restore live. Retiring only the compare half on 2026-08-01 left
        // this one taking a reference on the base menu's sequence-slot job and never giving it back,
        // and never restoring the slot either - an addref with no release, on an object the engine
        // owns. It is gated on debug_logging, which is why it bit the Steam Deck (true there) and
        // not the PC (false), and the crash it produced is the engine calling a virtual on a dead
        // object through a menu window's +0x118. Take and compare are one mechanism; gate them as one.
        (void)base_menu;
        return;
#else
        if (!goblin::config::debugLogging || !v3_heap_ptr(base_menu))
            return;
        __try
        {
            // The strongest remaining lead: base menu +0x10 is the SEQUENCE SLOT (the same field we put
            // our own screens into), and after our screen closes it holds a different object than before -
            // the HUD menu's own content was displaced and never put back. Keep a reference to the
            // original so it cannot die while our screen is up, and try restoring it on close.
            const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            log_job_stack(base_menu, "before our screen is pushed");
            log_autohide("before our screen is pushed");
            log_menuman_map("before our screen is pushed", true);
            // Catch the HIDE in the act. Measured, reproducibly: in gameplay the visible menu ids are
            // {5, 7, 8, 19, 23, 26}, and after our screen closes 7 and 26 are down to "registered only"
            // and stay there until a native menu transition. Writing 3 back into those bytes does NOT
            // bring the HUD back (tried 2026-07-28), so the array is a reflection of state held
            // elsewhere - which means the useful question is who performs the hide, not what the byte
            // says. A hardware write watch on id 7's byte answers exactly that, and the handler now also
            // prints stack values that look like return addresses so the CALLER is named too.
#if MFG_STALL_PROFILER
            const uintptr_t man_now = g_menuman.load(std::memory_order_relaxed);
            if (v3_heap_ptr(man_now))
                goblin::watch::request(man_now + 0x90 + 7, GetCurrentThreadId());
#endif
            g_hud_orig_job = *reinterpret_cast<void **>(base_menu + 0x10);
            if (v3_heap_ptr(reinterpret_cast<uintptr_t>(g_hud_orig_job)))
                reinterpret_cast<void (*)(void *)>(goblin::anchors::at(goblin::AnchorId::refcount_addref))(
                    reinterpret_cast<uint8_t *>(g_hud_orig_job) + 8);
            else
                g_hud_orig_job = nullptr;
            memcpy(g_hud_snap_menu, reinterpret_cast<void *>(base_menu), kHudSnapBytes);
            const uintptr_t man = g_menuman.load(std::memory_order_relaxed);
            if (v3_heap_ptr(man))
                memcpy(g_hud_snap_man, reinterpret_cast<void *>(man), sizeof(g_hud_snap_man));
            g_hud_snap_base.store(base_menu, std::memory_order_release);
            g_hud_snap_state.store(1, std::memory_order_release);
            spdlog::info("[hud] snapshot taken of the base menu 0x{:X} before our over-gameplay screen",
                         base_menu);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_hud_snap_state.store(0, std::memory_order_release);
        }
#endif // !MFG_STALL_PROFILER
    }

    void hud_snapshot_compare()
    {
#if !MFG_STALL_PROFILER
        // RETIRED FROM SHIPPING BUILDS 2026-08-01. This is the HUD-after-close investigation
        // harness, and its question was answered long ago - the fix is the CSFeManImp+0x78 restore
        // on job_finished, which lives elsewhere and is untouched by this. What it still did was
        // walk the base menu, the job stack, the autohide block and the whole CSMenuMan map - THE
        // MOMENT OUR SCREEN CLOSED, i.e. exactly while the engine is tearing those structures down.
        // On the Steam Deck (debug_logging = true) that walk faulted inside the game's own menu
        // enumeration: menu_update_detour -> tick_native_menu -> prune_screens ->
        // hud_snapshot_compare -> AV at exe+0x7A8A8B, on the reporter's "close the menu over
        // gameplay, then open the map" path. A diagnostic must not be able to do that.
        // Rebuild with -DMFG_STALL_PROFILER=1 to get it back for an investigation.
        g_hud_snap_state.store(0, std::memory_order_release);
        return;
#else
        if (g_hud_snap_state.load(std::memory_order_acquire) != 1)
            return;
        g_hud_snap_state.store(2, std::memory_order_release);
        const uintptr_t base_menu = g_hud_snap_base.load(std::memory_order_acquire);
        if (!v3_heap_ptr(base_menu))
            return;
        __try
        {
            int shown = 0;
            for (size_t off = 0; off + 8 <= kHudSnapBytes && shown < 24; off += 8)
            {
                uint64_t before = 0, after = 0;
                memcpy(&before, g_hud_snap_menu + off, 8);
                memcpy(&after, reinterpret_cast<uint8_t *>(base_menu) + off, 8);
                if (before != after)
                {
                    ++shown;
                    spdlog::info("[hud] base menu +0x{:X}: 0x{:X} -> 0x{:X}", off, before, after);
                }
            }
            const uintptr_t man = g_menuman.load(std::memory_order_relaxed);
            if (v3_heap_ptr(man))
            {
                shown = 0;
                for (size_t off = 0; off + 8 <= sizeof(g_hud_snap_man) && shown < 16; off += 8)
                {
                    uint64_t before = 0, after = 0;
                    memcpy(&before, g_hud_snap_man + off, 8);
                    memcpy(&after, reinterpret_cast<uint8_t *>(man) + off, 8);
                    if (before != after)
                    {
                        ++shown;
                        spdlog::info("[hud] CSMenuMan +0x{:X}: 0x{:X} -> 0x{:X}", off, before, after);
                    }
                }
            }
            log_job_stack(base_menu, "after our screen closed");
            log_autohide("after our screen closed");
            log_menuman_map("after our screen closed", true);
            spdlog::info("[hud] compare done - the field that keeps the HUD hidden is among the above");
            if (v3_heap_ptr(man))
            {
                char hex[3 * 64 + 1];
                int n = 0;
                const uint8_t *b = reinterpret_cast<const uint8_t *>(man) + 0x80;
                for (int i = 0; i < 64 && n < (int)sizeof(hex) - 4; ++i)
                    n += _snprintf_s(hex + n, sizeof(hex) - n, _TRUNCATE, "%02X ", b[i]);
                spdlog::info("[hudprobe] HUD IS NOW HIDDEN (our screen just closed); CSMenuMan+0x80: {}",
                             hex);
                // Keep watching: the HUD comes back on its own once any native menu is opened, and the
                // bytes that change AT THAT MOMENT are the answer. The active-menu dump alone missed it
                // (the pointer at +0x80 does not always change), so sample on a timer for the next
                // minute - every second, only when the window actually differs from the last dump.
                memcpy(g_hudwatch_last, reinterpret_cast<const uint8_t *>(man) + 0x80,
                       sizeof(g_hudwatch_last));
                g_hudwatch_next.store(0, std::memory_order_relaxed);
                g_hudwatch_until.store(GetTickCount64() + 60000, std::memory_order_release);
                // The watch is now armed at PUSH time instead of here, on the byte of menu id 7 - see
                // hud_snapshot_take. Watching after the close was aimed at the restore side, but the
                // measured defect is on the HIDE side: id 7 and id 26 lose their visible bit while our
                // screen is up and never get it back.
            }
            // The two CSMenuMan bytes (+0x97, +0xAA) that go 3 -> 1 across our screen are NOT the gate:
            // writing 3 back was tried on 2026-07-28, both writes fired, and the HUD stayed hidden.
            // Next candidate: the counter at base menu +0x168, which goes up by one per open and never
            // comes down. Same shape of experiment - only act on exactly the delta we recorded.
            // THE MENU-STATE ARRAY IS RULED OUT (2026-07-28, measured three ways). A hardware write watch
            // named the writer (exe+0x734438) and its code gives the semantics: the array at
            // singleton+0x90 is 71 one-byte entries, bit 0 = registered, bit 1 = visible, so 3 = shown,
            // 1 = hidden, 0 = unregistered (`and eax,1` gates it, `or cl,3` shows, `and cl,1` hides).
            // Then: (a) writing 3 back to the two entries that flipped did not restore the HUD;
            // (b) restoring EVERY entry across the whole 71-byte array reported "no entry went from 3 to
            // 1", so our screen does not hide the HUD through this array at all; and (c) when the game
            // itself restores the HUD it ZEROES entries (unregisters them) rather than setting them to 3 -
            // that is cleanup of what our screens left, not the switch.
            // What remains unexplored is the HUD's own objects: it is a set of menus/widgets of its own,
            // and the switch is on that side, not in the menu-state table. Do not spend more runs writing
            // into this array - it has been excluded.
            // TRIED AND REVERTED 2026-07-28. The watch showed the game restoring the HUD by ZEROING the
            // entries our screens left in this window (`+0x94:03->00 +0x97:01->00 +0x98:03->00
            // +0x9B..+0x9E:01->00`), so we did the same for exactly the bytes that were zero before us.
            // It did not bring the HUD back AND it made things worse: pressing ESC right after closing our
            // menu hung the game and crashed it. These entries are live state the menu machinery reads and
            // re-registers, so writing them from our thread races it - no matter which bytes are "right".
            // Conclusion: the HUD must be restored by making the GAME do it (call whatever the menu
            // open/close path calls), not by writing this memory. That is the next line of work, and it
            // needs the function, not another byte.
            // Neither the two CSMenuMan bytes (+0x97/+0xAA, 3 -> 1) nor the counter at +0x168 is the gate:
            // both were written back on 2026-07-28, both writes fired, and the HUD stayed hidden.
            // EXPERIMENT: restore the HUD menu's own sequence-slot job, which the diff shows is a
            // DIFFERENT object after our screen than before. Uses the engine's own slot setter (0x7A9250,
            // which takes its own reference) and then drops the reference we held.
            void *now_job = *reinterpret_cast<void **>(base_menu + 0x10);
            if (g_hud_orig_job)
            {
                void *held = g_hud_orig_job;
                reinterpret_cast<void (*)(void *, void *)>(
                    goblin::anchors::at(goblin::AnchorId::job_holder_store_seq))(
                    reinterpret_cast<void *>(base_menu + 0x10), &held);
                spdlog::info("[hud] restored the HUD menu's sequence-slot job 0x{:X} (was 0x{:X}) - "
                             "experiment", (uint64_t)g_hud_orig_job, (uint64_t)now_job);
                // NO release here. The setter above already CONSUMED our reference - decompiled
                // 2026-08-01: 0x7A9250 addrefs into the holder, then unrefs *src and nulls it, so
                // `held` is spent by the time it returns. The inline unref that used to follow was
                // therefore a second release of a reference we no longer owned, and in the case
                // this experiment exists for - where our captured +1 was the object's last one - it
                // destroyed the job the engine had just been handed, leaving its holder pointing at
                // freed memory. Dormant (profiler builds only) but a guaranteed use-after-free in an
                // engine structure, and a plausible source of the exe+0x7A8A8B fault this harness
                // was itself blamed for.
                g_hud_orig_job = nullptr;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
#endif // !MFG_STALL_PROFILER
    }

    Screen *top_screen() { return g_screens.empty() ? nullptr : &g_screens.back(); }

    bool menu_open() { return !g_screens.empty(); }

    // The heartbeat, stamped from the dialog's own per-frame update: it runs for every screen
    // that exists, including one sitting under a child (the input gate stops a parent's
    // COMMANDS, not its update).
    void form_dialog_ticked(uintptr_t dlg)
    {
        const uint64_t now = GetTickCount64();
        const int level = screen_level(dlg);
        if (level >= 0)
            g_screens[static_cast<size_t>(level)].beat = now;
    }

    // Defined further down, next to the primitives they use.
    void open_screen(int32_t page, uintptr_t nest_parent);
    void resume_level(size_t level);
    void close_screen(uintptr_t dlg);
    bool seq_slot_empty(uintptr_t win);

    void build_form_rows()
    {
        goblin::nmenu::rebuild();
        size_t count = 0;
        goblin::nmenu::rows(&count);
        if (count > kFormPoolMax / 2)
            count = kFormPoolMax / 2; // two entries per visual row (left + right column)
        if (form_pool().capacity() < kFormPoolMax)
            form_pool().reserve(kFormPoolMax); // once - the address must stay put
        const size_t level = g_form_depth < kMaxNest ? g_form_depth : kMaxNest - 1;
        ++g_form_generation[level];
        form_pool().clear();
        const int fallback_id = goblin::g_menutext_tab_id;
        for (size_t i = 0; i < count; ++i)
        {
            FormItem it{};
            it.param.text_id = fallback_id;
            it.param.action_ix = static_cast<int32_t>(i % 0x36);
            // Field layout matters: native code that we do NOT replace reads per-device
            // "enabled" bytes out of this row - the mode-0 path tests the byte at +0x10 and
            // mode 1 the byte at +0x11 (verify_nesting.md 2.2), and the conflict pass reads
            // +9/+0xA/+0xB. So keep all of those NON-ZERO and carry our identity in the
            // remaining bits: magic in pad2 (its bytes +9/+0xA/+0xB are 'G','F','M' - all
            // non-zero), generation in pad3, and the row index in the HIGH half of pad4
            // whose low bytes stay 0x01.
            it.param.pad2 = static_cast<int32_t>(kFormMagic);
            it.param.pad3 = static_cast<int32_t>(g_form_generation[level]);
            it.param.pad4 = static_cast<int32_t>((static_cast<uint32_t>(i & 0xFFFF) << 16) |
                                                 0x0101u);
            form_pool().push_back(it);
            // The right column is NOT fed through the grid: real items there scroll together
            // with the left list and read as (disabled) buttons. We paint those clips
            // directly instead - see paint_right_panel().
            form_pool().push_back(FormItem{});
        }
    }

    // Which model row does this native item belong to? Reads the identity out of the
    // param row (magic + generation + index), so stale items simply fail the check.
    // Validate-then-read (goblin_safemem.hpp), NOT a bare __try: this runs inside the
    // engine's row-render path for EVERY item on a native form, ours or not, and an item
    // whose param row has been freed is routine there. The __try version raised a steady
    // ~10 Hz first-chance AV at the magic compare whenever such an item was on screen
    // (ERSS-FG run 2026-08-05: 53 logged AVs at MapForGoblins+0x94c3a, each one walked
    // through its whole exception filter) - the exact process-wide-event class the header
    // documents.
    const goblin::nmenu::Row *model_row_of(uintptr_t item, size_t *out_index)
    {
        if (form_pool().empty())
            return nullptr;
        uint64_t rowp = 0;
        if (!goblin::safemem::copy(&rowp, reinterpret_cast<const void *>(item + 0x10),
                                   sizeof(rowp)) ||
            !rowp)
            return nullptr;
        FakeParamRow local{};
        if (!goblin::safemem::copy(&local, reinterpret_cast<const void *>(rowp), sizeof(local)))
            return nullptr;
        if (static_cast<uint32_t>(local.pad2) != kFormMagic)
            return nullptr;
        const FakeParamRow *row = reinterpret_cast<const FakeParamRow *>(rowp);
        // Which screen does this item belong to? Only the LIVE one may be drawn or acted on
        // from the model - a parent's items keep their own pool and their last-drawn text, and
        // rendering them against the child's page would show the wrong row.
        // (pool_level_of is address arithmetic only; once the row is inside one of OUR pools
        // the identity fields below are our own live memory - read via the validated copy.)
        const int level = pool_level_of(row);
        if (level < 0 || static_cast<size_t>(level) != g_form_depth)
            return nullptr;
        if (static_cast<uint32_t>(local.pad3) != g_form_generation[level])
            return nullptr; // an item left over from a previous build
        const uint32_t tag = static_cast<uint32_t>(local.pad4);
        const size_t ix = tag >> 16;
        if (out_index)
            *out_index = ix;
        if ((tag & 0x8000u) != 0)
            return goblin::nmenu::right_row(ix); // right-column preview line
        size_t count = 0;
        const goblin::nmenu::Row *rows = goblin::nmenu::rows(&count);
        if (!rows || ix >= count)
            return nullptr;
        return &rows[ix];
    }

    bool our_form_item(uintptr_t item) { return model_row_of(item, nullptr) != nullptr; }

    using BuildItemsFn = void(void *itemVec, void *list);
    BuildItemsFn *o_build_items = nullptr;
    using RowRenderFn = void *(void *item, void *rowProxy);
    RowRenderFn *o_row_render = nullptr;

    // ── Drawing a row ourselves (hook of the item's vt+0x8, FUN_1408674e0) ───────────
    // Everything the native renderer does is available as small clip-proxy helpers, and
    // crucially FUN_14074a000 takes a RAW wchar_t* - so our rows are not limited to FMG
    // strings and can show numbers, names, anything:
    //   FUN_14074a2f0(parentProxy, outBuf, "Child/Path") - resolve a child clip; the
    //       result is used as (out + 8) for text and destroyed via FUN_140D7F850(out+0x28)
    //   FUN_14074a000(resolved + 8, wchar_t*)            - set the text field's text
    //   FUN_140733340(proxy, visible) / FUN_1407331E0(proxy, grayed)
    //   FUN_1407499E0(proxy + 0x18, "FrameName")         - gotoAndStop by frame label
    //       ("PadCategory"/"KeyCategory" = the section-header look)
    //   FUN_140733150(proxy)                             - is the clip actually there
    // Types are declared here so the SEH body below stays POD-only (MSVC C2712).
    using ResolveFn = void *(void *parent, void *out, const char *path);
    using SetTextFn = void(void *field, const wchar_t *text);
    using SetVisibleFn = void(void *proxy, char on);
    using SetGrayFn = void(void *proxy, char grayed);
    using GotoFrameFn = void(void *proxy, const char *frame);
    using ProxyValidFn = char(void *proxy);
    using ProxyDtorFn = void(void *proxy);

    // One-shot probe: log which child clips a row actually has, so the next step (icons,
    // bars, extra columns) works from facts instead of guesses.
    std::atomic<bool> g_row_probe_done{false};

    const char *const kRowClipNames[] = {"Text_0",   "Text_1",    "Text_2", "ItemIcon",
                                         "Conflict", "Assigning", "Icon",   "Bar",
                                         "HitArea",  "Cursor",    "Lock",   "CursorLock"};
    constexpr size_t kRowClipCount = sizeof(kRowClipNames) / sizeof(kRowClipNames[0]);

    // POD-only: fill found[i] with "does the row have this child clip".
    void probe_row_clips_raw(uintptr_t base, void *rowProxy, char *found)
    {
        auto p_resolve = reinterpret_cast<ResolveFn *>(goblin::anchors::at(goblin::AnchorId::clip_resolve_child));
        auto p_valid = reinterpret_cast<ProxyValidFn *>(goblin::anchors::at(goblin::AnchorId::clip_is_valid));
        auto p_dtor = reinterpret_cast<ProxyDtorFn *>(goblin::anchors::at(goblin::AnchorId::clip_proxy_dtor));
        __try
        {
            for (size_t i = 0; i < kRowClipCount; ++i)
            {
                uint8_t buf[0x60] = {};
                void *r = p_resolve(rowProxy, buf, kRowClipNames[i]);
                found[i] = p_valid(r);
                p_dtor(buf + 0x28);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    void probe_row_clips(uintptr_t base, void *rowProxy)
    {
        char found[kRowClipCount] = {};
        probe_row_clips_raw(base, rowProxy, found);
        std::string list;
        for (size_t i = 0; i < kRowClipCount; ++i)
        {
            list += kRowClipNames[i];
            list += found[i] ? "=yes " : "=no ";
        }
        spdlog::info("[form] row clips: {}", list);
    }

    // POD-only row draw. Mirrors the native renderer's call order (frame style -> visible
    // -> Text_0 -> Text_1 -> hide Conflict -> destroy the row proxy).
    //
    // The row clip (sprite cid 189 in 02_160) has FOUR labelled style frames, and they
    // differ in which text fields exist (gfx_02_160_dump.txt):
    //   "Normal"      (frame 1)  Text_0 left 532px + Text_1 right 260px  <- value rows
    //   "Grayout"     (frame 10) same, dimmed                            <- read-only rows
    //   "PadCategory" (frame 19) ONE wide Text_0 (610px), NO Text_1      <- section header
    //   "KeyCategory" (frame 28) second header variant
    // So a row that needs to show a value must NOT use a category frame, or the value
    // field is simply not there. Both text fields are html=1 EditText (MenuFont_01 24pt),
    // which is why passing markup is worth testing.
    using SetScaleFn = void(void *proxy, float sx, float sy);

    // Optional graphic progress bar: the row's Conflict sprite is the only spare graphic
    // in the 02_160 row clip, so show it, squash it to the fraction and tint it
    // (recon_draw_primitives.md: setScale 0x733280, setColor 0x74A1D0).
    // The experimental clip progress bar (scale the row own Conflict clip to the fraction) lived
    // here until 2026-07-29. Measured in game: it draws, but reads no better than the text bar, so
    // the text bar is the only one now. The RVA note stays in tools/rva_anchors.py.

    void draw_our_row(uintptr_t base, void *rowProxy, const char *styleFrame,
                      const wchar_t *label, const wchar_t *value, bool plate)
    {
        auto p_resolve = reinterpret_cast<ResolveFn *>(goblin::anchors::at(goblin::AnchorId::clip_resolve_child));
        auto p_settext = reinterpret_cast<SetTextFn *>(goblin::anchors::at(goblin::AnchorId::clip_set_text_html));
        auto p_visible = reinterpret_cast<SetVisibleFn *>(goblin::anchors::at(goblin::AnchorId::clip_set_visible));
        auto p_frame = reinterpret_cast<GotoFrameFn *>(goblin::anchors::at(goblin::AnchorId::clip_goto_frame_name));
        auto p_valid = reinterpret_cast<ProxyValidFn *>(goblin::anchors::at(goblin::AnchorId::clip_is_valid));
        auto p_dtor = reinterpret_cast<ProxyDtorFn *>(goblin::anchors::at(goblin::AnchorId::clip_proxy_dtor));
        __try
        {
            p_visible(rowProxy, 1);
            if (styleFrame)
                p_frame(reinterpret_cast<uint8_t *>(rowProxy) + 0x18, styleFrame);
            {
                uint8_t buf[0x60] = {};
                void *r = p_resolve(rowProxy, buf, "Text_0");
                if (p_valid(r))
                    p_settext(reinterpret_cast<uint8_t *>(r) + 8, label);
                p_dtor(buf + 0x28);
            }
            {
                // ALWAYS write the value field, even when empty: the row clips are reused
                // across pages, so skipping the write leaves the PREVIOUS row's value on
                // screen (seen in-game as values from the parent page bleeding through).
                uint8_t buf[0x60] = {};
                void *r = p_resolve(rowProxy, buf, "Text_1");
                if (p_valid(r)) // absent on the category frames
                    p_settext(reinterpret_cast<uint8_t *>(r) + 8, value ? value : L"");
                p_dtor(buf + 0x28);
            }
            {
                uint8_t buf[0x60] = {};
                void *r = p_resolve(rowProxy, buf, "Conflict");
                if (p_valid(r))
                    p_visible(r, plate ? 1 : 0);
                p_dtor(buf + 0x28);
            }
            p_dtor(reinterpret_cast<uint8_t *>(rowProxy) + 0x28);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    // POD-only worker (an SEH frame must not hold objects with destructors - MSVC C2712).
    // Mirrors FUN_140868990's shape: clear the vector, then append items. The form's grid
    // is TWO columns wide (left clip cid 189 has Text_0+Text_1, the right one cid 182 has
    // only Text_1) and the native builder appends items in PAIRS - a real item plus an
    // EMPTY filler (FUN_1408686c0) that keeps the right column blank. We do the same, so
    // all our rows stay in the left column and the native scrollbar handles overflow.
    // No finisher pass (FUN_140867820): it only computes key-conflict flags, which would
    // light the Conflict marker on rows that bind nothing.
    bool build_our_form_items(uintptr_t obj, void *list, const FormItem *rows, size_t count)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        void *vec = reinterpret_cast<void *>(obj + 8); // &allocator, as the game passes
        __try
        {
            using ClearFn = void(void *, void *, uintptr_t, uintptr_t);
            using AppendFn = void(void *, void *);
            using NormalCtorFn = void *(void *out, void *listCtx, uint32_t kind, const void *row);
            using EmptyCtorFn = void *(void *out);
            auto p_clear = reinterpret_cast<ClearFn *>(goblin::anchors::at(goblin::AnchorId::menu_row_vec_clear));
            auto p_append = reinterpret_cast<AppendFn *>(goblin::anchors::at(goblin::AnchorId::menu_row_vec_append));
            auto p_normal = reinterpret_cast<NormalCtorFn *>(goblin::anchors::at(goblin::AnchorId::menu_row_item_ctor));
            auto p_empty = reinterpret_cast<EmptyCtorFn *>(goblin::anchors::at(goblin::AnchorId::menu_row_item_empty));
            uint8_t scratch[8] = {};
            p_clear(vec, scratch, *reinterpret_cast<uintptr_t *>(obj + 0x10),
                    *reinterpret_cast<uintptr_t *>(obj + 0x18));
            *reinterpret_cast<uintptr_t *>(obj + 0x18) =
                *reinterpret_cast<uintptr_t *>(obj + 0x10); // end = begin (emptied)
            uint8_t item[0x50];
            for (size_t i = 0; i < count; ++i)
            {
                std::memset(item, 0, sizeof(item));
                p_normal(item, list, 0, &rows[i].param); // param row is the first member
                p_append(vec, item);
                // The pool alternates left/right; a right entry with our magic is a real
                // preview line, a zeroed one means "keep this half blank".
                const bool have_right = (i + 1) < count &&
                                        static_cast<uint32_t>(rows[i + 1].param.pad2) == kFormMagic;
                std::memset(item, 0, sizeof(item));
                if (have_right)
                    p_normal(item, list, 0, &rows[i + 1].param);
                else
                    p_empty(item); // right-column filler
                p_append(vec, item);
                ++i; // the right entry was consumed with this pair
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // POD-only: remember the dialog if this list really is dlg+0x1268 (the refresh path
    // also builds into temporary stack lists).
    // A g_movie_open_settled latch stood here. It was raised when the screen's dialog first
    // appeared and its only job was to disarm the movie interception for the rest of the session.
    // That bracket is gone (goblin_own_movie.hpp says why: 02_160 is parsed once at startup, so
    // there is nothing to arm around), and with it the latch.

    // The dialog behind a row-item list, or 0 when this list belongs to something else. The
    // class is CHECKED (vtable), never assumed: the refresh path also builds into temporary
    // stack lists, and the player's own key-binding screen uses the very same builder.
    uintptr_t keyconfig_dialog_of(uintptr_t listObj)
    {
        const uintptr_t want = goblin::anchors::vtable_of(kVtKeyConfigDialog);
        if (!want)
            return 0;
        __try
        {
            const uintptr_t cand = listObj - 0x1268;
            if (*reinterpret_cast<uintptr_t *>(cand) == want)
                return cand;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return 0;
    }

    // ── a child screen must REPLACE the page it came from ────────────────────────────
    // Vanilla gets this from the two extra jobs it wraps around the screen (FUN_14094FD40 stores
    // a sequence of three: a lambda, the screen, a lambda) - those handle the page transition.
    // We store the screen alone, so both movies render and the child appeared on top of its
    // parent. Hiding is cheaper than reproducing the sequence and needs nothing new: the root
    // timeline of 02_160 places exactly these named clips (movie dump, "=== ROOT timeline ==="),
    // and one unnamed one at depth 342 (cid 165, the MENU_TitleBase plate) that can only be
    // reached through the movie root itself - so the root is tried first and the named list is
    // the fallback that is known to work (it is how the stray column captions are blanked).
    const char *const kRootClips[] = {"BG",         "StatusBar", "Help",     "SelectKey",
                                      "ActionHelp", "KeySetting", "MenuTitle"};

    // ── when a page may come back ────────────────────────────────────────────────────
    // Vanilla never faces this: it does not hide the page at all. Its opener stores a SEQUENCE of
    // three jobs (FUN_14094FD40) - a transition lambda, the screen, a transition lambda - and the
    // second lambda is what hands the page back, at the moment the engine knows the transition is
    // over. We store the screen alone and hide the page ourselves, so the "when" is ours to
    // answer, and the obvious answer is wrong: the parent's sequence slot empties while the child
    // is still playing its closing animation, which is the white frame that showed up.
    // A fixed wait was only a stand-in. The real signal is the child's own DEATH: its dialog
    // stamps a heartbeat every frame from its update (form_update_detour), and when that stops the
    // movie is gone - there is nothing left to flash. So the page comes back on the first frames
    // after the child stops updating, not on a timer.
    // The root timeline's settled state, between its intro and its close animation (movie dump:
    // f2 'FadeIn', f9 'Loop', f12 'FadeOut').
    constexpr const char *kSettledLabel = "Loop";

    // Turn off the dark rectangle behind the rows. It is the 'BG' child of the row section
    // ('KeySetting' -> 'BG', sprite 169 at depth 1 inside sprite 198). Removing that placement from the
    // movie instead made the screen hang and crash - the dialog needs the child to exist - so we leave it
    // in place and only set it invisible, with the same primitive that shows and hides whole screens.
    // Called ONCE per screen, from the point where the dialog identifies itself: the movie is live there
    // (its rows are being built) and nothing is tearing down, which is what made the earlier per-tick
    // version crash on close.
    void hide_row_panel(uintptr_t dlg)
    {
        if (!dlg)
            return;
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        auto p_resolve = reinterpret_cast<ResolveFn *>(goblin::anchors::at(goblin::AnchorId::clip_resolve_child));
        auto p_valid = reinterpret_cast<ProxyValidFn *>(goblin::anchors::at(goblin::AnchorId::clip_is_valid));
        auto p_visible = reinterpret_cast<SetVisibleFn *>(goblin::anchors::at(goblin::AnchorId::clip_set_visible));
        auto p_dtor = reinterpret_cast<ProxyDtorFn *>(goblin::anchors::at(goblin::AnchorId::clip_proxy_dtor));
        __try
        {
            void *root = reinterpret_cast<void *>(dlg + 0x120);
            if (!p_valid(root))
                return;
            uint8_t sect_buf[0x60] = {};
            void *sect = p_resolve(root, sect_buf, "KeySetting");
            if (sect && p_valid(sect))
            {
                uint8_t bg_buf[0x60] = {};
                void *bg = p_resolve(sect, bg_buf, "BG");
                if (bg && p_valid(bg))
                {
                    p_visible(bg, 0);
                    spdlog::info("[form] row panel hidden (KeySetting/BG) on dlg 0x{:X}", dlg);
                }
                else
                    spdlog::info("[form] row panel: 'BG' did not resolve under 'KeySetting'");
                p_dtor(bg_buf + 0x28);
            }
            else
                spdlog::info("[form] row panel: 'KeySetting' did not resolve on dlg 0x{:X}", dlg);
            p_dtor(sect_buf + 0x28);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            spdlog::warn("[form] row panel: SEH while hiding it");
        }
    }

    void set_screen_visible(uintptr_t dlg, bool on)
    {
        // Shown again = the intro plays again and re-places the section: re-assert the shift.
        if (on)
        {
            const int lvl = screen_level(dlg);
            if (lvl >= 0)
                g_screens[static_cast<size_t>(lvl)].shift_frames = 0;
        }
        if (!dlg)
            return;
        auto p_resolve = reinterpret_cast<ResolveFn *>(goblin::anchors::at(goblin::AnchorId::clip_resolve_child));
        auto p_visible = reinterpret_cast<SetVisibleFn *>(goblin::anchors::at(goblin::AnchorId::clip_set_visible));
        auto p_valid = reinterpret_cast<ProxyValidFn *>(goblin::anchors::at(goblin::AnchorId::clip_is_valid));
        auto p_dtor = reinterpret_cast<ProxyDtorFn *>(goblin::anchors::at(goblin::AnchorId::clip_proxy_dtor));
        void *root = reinterpret_cast<void *>(dlg + 0x120);
        bool root_done = false;
        __try
        {
            if (p_valid(root))
            {
                p_visible(root, on ? 1 : 0);
                root_done = true;
                // EXPERIMENT (2026-07-28): shift the whole screen right. The key-binding host lays out in
                // the LEFT half (two-column screen), the graphics host is centred. The previous attempt
                // targeted the per-row clip inside draw_row_icon_own, which never runs on this path (it
                // logged nothing), so this moves the MOVIE ROOT instead - the same proxy setVisible above
                // works on. Set kScreenShiftX to 0 to switch it off; a value that sticks means centring is
                // one constant, a value that snaps back means the engine re-lays the root every frame.
                // (the shift experiment lives in the tick now - this path is not reached for a screen
                //  that is never hidden, which is why two attempts here logged nothing)
                // Coming back, PLAY THE INTRO instead of snapping on. The first F8 open looks
                // smooth for exactly one reason: the movie is created playing and runs its own
                // intro (the root timeline has FrameLabel 'FadeIn' at frame 2, and frames 2+
                // re-place every top-level clip). A page we hid ourselves has no such moment, so
                // it appeared at full brightness in one frame - which is what read as a flash
                // when it landed on top of the closing screen, and as an abrupt pop when it
                // landed after it.
                // Vanilla never needs this: its page-aside/page-back is a method on the SETTINGS
                // dialog (the transition lambdas call *(win+0x1DA0)->vt[0x10](bool) and
                // win->vt[0xA8]()), and our parent is a second copy of the key-binding screen,
                // whose vtable ends at +0x98 - there is no such method to call.
                // The root timeline is a THREE-part state machine, not an intro:
                //     f2 'FadeIn'  ->  f9 'Loop'  ->  f12 'FadeOut'  (to f19)
                // The engine plays FadeIn when a screen opens, leaves it on Loop while it is up
                // and plays FadeOut when it closes. Jumping to FadeIn was tried and it ran
                // straight THROUGH Loop into FadeOut, because nothing stopped it - in game the
                // page appeared for a frame or two and then faded itself away, leaving only the
                // backdrop.
                // So go to the settled state and STOP there: the page cannot wander into its own
                // close animation, and the screen on its way out still covers the moment it
                // appears. Its own FadeOut later still works - the engine starts that itself.
                using GotoAndStopFn = void(void *proxy, const char *label);
                if (on)
                    reinterpret_cast<GotoAndStopFn *>(goblin::anchors::at(goblin::AnchorId::clip_goto_frame_name))(
                        reinterpret_cast<uint8_t *>(root) + 0x18, kSettledLabel);
            }
            // ONLY if the root would not take it. Walking the named clips instead is not
            // equivalent: prepare_form_layout deliberately HIDES two of them (Help and
            // SelectKey - the latter is the "press a key" overlay), so turning the whole list
            // back on flashed that overlay for a frame on every return from a sub-page. The root
            // carries one flag and leaves every per-clip decision the layout made intact.
            for (const char *name : kRootClips)
            {
                if (root_done)
                    break;
                if (on && (std::strcmp(name, "Help") == 0 || std::strcmp(name, "SelectKey") == 0))
                    continue; // the layout wants these off - never turn them back on
                uint8_t buf[0x60] = {};
                void *r = p_resolve(root, buf, name);
                if (p_valid(r))
                    p_visible(r, on ? 1 : 0);
                p_dtor(buf + 0x28);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            spdlog::warn("[form] SEH turning screen 0x{:X} {}", dlg, on ? "back on" : "off");
            return;
        }
        static std::atomic<bool> s_logged{false};
        if (!s_logged.exchange(true, std::memory_order_acq_rel))
            spdlog::info("[form] screen visibility: movie root {}",
                         root_done ? "accepted setVisible (per-clip state untouched)"
                                   : "did not validate - falling back to named clips");
    }

    // Forget every screen above `level`. Called when a level that is not the top builds or takes
    // input again, which can only mean the screens over it are gone.
    void pop_above(size_t level)
    {
        while (g_screens.size() > level + 1)
        {
            // A screen we have just asked for is not a corpse: the parent often rebuilds its own
            // rows while its child's movie is still coming up, and dropping the entry there would
            // leave the child with no level to land on.
            if (!g_screens.back().dlg && GetTickCount64() - g_screens.back().opened < 3000)
                return;
            spdlog::info("[form] level {} (page {}) closed", g_screens.size() - 1,
                         g_screens.back().page);
            g_screens.pop_back();
            if (g_screens.empty())
                hud_snapshot_compare(); // names the state our close leaves behind (HUD stays hidden)
        }
    }

    void build_items_detour(void *itemVec, void *list)
    {
        const uintptr_t obj = reinterpret_cast<uintptr_t>(itemVec);
        const uintptr_t dlg = menu_open() ? keyconfig_dialog_of(obj) : 0;
        int level = dlg ? screen_level(dlg) : -1;
        if (dlg && level < 0)
        {
            // Not a screen we know. It is OURS only if we have just asked for one and its dialog
            // has not identified itself yet; anything else is the game's own screen and must be
            // left completely alone.
            Screen *pending = top_screen();
            if (pending && !pending->dlg)
            {
                pending->dlg = dlg;
                pending->beat = GetTickCount64();
                pending->shown = pending->beat;
                level = static_cast<int>(g_screens.size()) - 1;
                log_form_command_table(dlg);
                spdlog::info("[form] level {} is dialog 0x{:X} (page {})", level, dlg,
                             pending->page);
                hide_row_panel(dlg); // the dark rectangle behind our rows
                // The page it came from steps aside NOW - the child's movie exists as of this
                // build, so there is no frame with neither of them on screen.
                if (level > 0)
                {
                    Screen &below = g_screens[static_cast<size_t>(level) - 1];
                    set_screen_visible(below.dlg, false);
                    below.hidden = true;
                }
            }
        }
        else if (dlg && static_cast<size_t>(level) + 1 < g_screens.size())
            pop_above(static_cast<size_t>(level)); // a parent rebuilding = its child has gone
        // A list that is not a dialog's own: the refresh path also builds into TEMPORARY stack
        // lists. Those belong to whichever screen is live and must carry ITS rows - handing them
        // to the game would fill them with the real key binds. Their build must NOT rebuild the
        // model though: doing that bumped the pool generation while the screen's REAL items were
        // still pointing at the old one, which made every row unrecognisable a moment after the
        // screen was drawn (in game: a parent that came back with dead rows).
        const bool temp_list = !dlg && menu_open();
        if (temp_list)
            level = screen_level(g_form_dialog.load(std::memory_order_acquire));
        if (level < 0)
        {
            static std::atomic<int> s_passthrough{0};
            if (s_passthrough.fetch_add(1) < 3)
                spdlog::info("[form] row build passed through to the game (menu {}) - this is "
                             "what leaves the real keybinding list untouched",
                             menu_open() ? "open" : "closed");
            o_build_items(itemVec, list);
            return;
        }
        // From here the model, the pool and the view all describe THIS screen.
        g_form_depth = static_cast<size_t>(level) < kMaxNest ? static_cast<size_t>(level)
                                                             : kMaxNest - 1;
        if (dlg)
        {
            g_form_dialog.store(dlg, std::memory_order_release);
            g_form_update_watch.store(dlg, std::memory_order_release);
        }
        if (!temp_list)
        {
            goblin::nmenu::set_page(g_screens[static_cast<size_t>(level)].page);
            build_form_rows(); // the pool must match the page this screen was just put on
        }
        if (build_our_form_items(obj, list, form_pool().data(), form_pool().size()))
        {
            // How many items actually LANDED, versus how many we appended. The progress page
            // builds 86 items (43 visual rows) but only ~15 rows were reachable in game, so
            // either the item vector truncates or the grid's extent disagrees. The vector is
            // {begin @+0x10, end @+0x18} with stride 0x50; the grid's own view of the list is
            // count/columns/rows at +0xD0/+0xD8/+0xDC.
            size_t landed = 0;
            uint32_t g_count = 0, g_cols = 0, g_rows = 0;
            __try
            {
                const uintptr_t b = *reinterpret_cast<uintptr_t *>(obj + 0x10);
                const uintptr_t e = *reinterpret_cast<uintptr_t *>(obj + 0x18);
                if (e >= b)
                    landed = static_cast<size_t>((e - b) / 0x50);
                const uintptr_t dlg = g_form_dialog.load(std::memory_order_acquire);
                if (dlg)
                {
                    const uintptr_t grid = dlg + 0xA38;
                    g_count = *reinterpret_cast<uint32_t *>(grid + 0xD0);
                    g_cols = *reinterpret_cast<uint32_t *>(grid + 0xD8);
                    g_rows = *reinterpret_cast<uint32_t *>(grid + 0xDC);
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            g_form_landed = static_cast<uint32_t>(landed);
            if (goblin::config::debugLogging) // one line per menu-page build: debug chatter
                spdlog::info("[form] rows built: {} (page {}, list=0x{:X}) - landed {} items, "
                             "grid count={} cols={} rows={}",
                             form_pool().size(), goblin::nmenu::current_page(), obj, landed,
                             g_count, g_cols, g_rows);
        }
        else
        {
            spdlog::warn("[form] SEH building our rows - falling back to native");
            o_build_items(itemVec, list);
        }
    }

    // ── Independent icon path: RETIRED, this is its tombstone ───────────────────────
    // Three implementations of "draw our own pixels into a clip the row already has" lived below
    // this banner and all three are gone (icon_resource_for, draw_row_icon_own,
    // draw_row_icon_direct). The menu icons ship as characters spliced into the movie on the parse
    // instead - goblin_own_movie.cpp. What is left here is the leftover state of that route.
    // The spliced route below needs the movie's bytes patched at load. This one does not
    // touch any movie: we build a Scaleform image resource from our own RGBA (RawImage +
    // ScaleformImageResource), create our OWN child sprite in the row via
    // CreateEmptyMovieClip, and fill its drawing context with the resource. Works in any
    // movie, so overhaul mods and game updates cannot collide with it.
    std::atomic<bool> g_icon_diag_done{false};
    std::atomic<bool> g_icon_host_logged{false};
    std::atomic<bool> g_icon_path_probed{false};
    std::atomic<int> g_prehide_report{-1};
    constexpr const char *kOwnIconClip = "MfgIconDraw";
    // -1 means "append": the AS2 implementation turns it into (largest depth in use + 1) and
    // the AS3 one into GetNumChildren(). A positive number is NOT portable - under AS3 it is
    // a child INDEX, and a row clip has only a handful of children, so any index past that
    // range makes AddChildAt fail and the whole call return false.
    constexpr int32_t kOwnIconDepth = -1;
    constexpr float kOwnIconPx = 28.f;

    std::atomic<int> g_own_icon_state{0}; // 0 = untried, 1 = working, -1 = unavailable
    // Direct-draw route (native_menu_icons = 3): whether anything has been drawn yet, and a
    // resource to clear with. Both are UI-thread only, like every other draw here.
    bool g_direct_icon_seen = false;
    void *g_blank_resource = nullptr;
    // A g_row_slot / g_row_column pair was captured here from the engine's row-path helper and
    // never read: the icon route explicitly rejected that capture ("stale by the time we draw") and
    // takes the slot from slot_of_row() instead. row_path_detour stays - it still feeds the
    // row-count log below, which is what tells us how many rows the engine is willing to show.

    // icon_resource_for() built (and cached in g_icon_resources) a Scaleform resource per iconId by
    // inflating the shared lossless tag. Its two consumers were the own-icon draw routes, both
    // permanently disabled - the row icons are spliced into the menu movie instead - so it and the
    // cache it owned had no callers left.

    // OFF. Creating display objects inside someone else's live movie is a dead end, and this
    // is the third crash from it: after the screen closed, Scaleform faulted on poisoned memory
    // (0xC0000005 at exe+0x112CE1A with RCX = 0xCCCCCCCCCC000A56) while tearing the dialog
    // down. The clip and the resolve both worked - the drawing itself was still refused - but
    // the object we added to the row outlives the screen and the engine expects to own
    // everything in that display list. The replacement is the load-time movie transform: the
    // icon becomes an ordinary timeline child that the engine creates and destroys itself, and
    // all we do at runtime is resolve it by name and pick its frame - which is exactly what we
    // already do safely for Text_0 and friends. The code stays as the record of what the live
    // route can and cannot do.

    // Returns true if the icon was drawn by the independent path.
    // draw_row_icon_own() lived here until 2026-07-29, permanently disabled by
    // kEnableOwnIconPath = false. Its state reporting is what the retired "Row icons" row read.

    // ── Icons with NO movie edited at all (native_menu_icons = 3) ───────────────────
    // The two strip variants need the screen's movie rebuilt at load, which means our icon
    // art has to match that movie's row clip byte for byte - a game patch or an overhaul
    // shipping its own 02_160 disables them. This route needs none of that: the pixels
    // become a Scaleform image resource of ours and are drawn straight into the drawing
    // context of a clip the row ALREADY has, so nothing is created, registered or patched.
    //
    // HitArea is the host: it spans the row, the menu never draws anything in it, and unlike
    // Conflict it carries no art of its own that would show through. Drawing must happen
    // AFTER the row's style frame is applied - a gotoAndStop rebuilds the row's display list
    // and takes the drawing with it.
    //
    // The row is reached BY PATH from the movie root, not through the handle the renderer
    // passes: draw_our_row finishes with the native renderer's own `dtor(rowProxy + 0x28)`,
    // so by the time we draw, that handle is dead - the first run said exactly that
    // ("clip=false" for a row whose HitArea the earlier probe had found). The slot to address
    // comes from the engine's own row-path helper, which is already hooked for the strip
    // variants.
    // draw_row_icon_direct() (the former native_menu_icons = 3: our own pixels straight into the
    // row clip, editing no movie) lived here until 2026-07-29. It rode on the own-clip path that
    // kEnableOwnIconPath had switched off, so it was never confirmed to draw in game.

    // Show the category icon in a row, if the movie carries our spliced icon sprite.
    // The sprite has one frame per icon; the frame number for an ini key comes from the
    // generated table. Rows without an icon just hide the clip.
    // POD-only (SEH frames may not hold objects with destructors): which of these paths the
    // engine can resolve from the movie root.
    void probe_icon_paths_raw(uintptr_t base, const char *const *paths, size_t count, char *found)
    {
        auto p_resolve = reinterpret_cast<ResolveFn *>(goblin::anchors::at(goblin::AnchorId::clip_resolve_child));
        auto p_valid = reinterpret_cast<ProxyValidFn *>(goblin::anchors::at(goblin::AnchorId::clip_is_valid));
        auto p_dtor = reinterpret_cast<ProxyDtorFn *>(goblin::anchors::at(goblin::AnchorId::clip_proxy_dtor));
        const uintptr_t dlg = g_form_dialog.load(std::memory_order_acquire);
        if (!dlg)
            return;
        __try
        {
            void *root = reinterpret_cast<void *>(dlg + 0x120);
            for (size_t i = 0; i < count; ++i)
            {
                uint8_t pb[0x60] = {};
                void *pr = p_resolve(root, pb, paths[i]);
                found[i] = p_valid(pr);
                p_dtor(pb + 0x28);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    // probe_icon_owner_raw() stood here (SEH-guarded reads of a row proxy by handle and by path,
    // plus the grid fields). Its only caller was probe_icon_owner, removed just below.

    // probe_icon_owner() logged how a row proxy resolves by handle and by path. Removed with its
    // raw helper: both lost their callers when the own-icon route was retired.

    void probe_icon_paths(uintptr_t base)
    {
        const char *const paths[] = {"KeySetting/ItemList/Item_0_0",
                                     "KeySetting/ItemList/Item_0_0/Text_0",
                                     "KeySetting/ItemList/Item_0_0/Conflict",
                                     "KeySetting/ItemList/Item_0_0/MfgIcon"};
        constexpr size_t kCount = sizeof(paths) / sizeof(paths[0]);
        char found[kCount] = {};
        probe_icon_paths_raw(base, paths, kCount, found);
        std::string report;
        for (size_t i = 0; i < kCount; ++i)
        {
            report += paths[i];
            report += found[i] ? "=yes " : "=no ";
        }
        spdlog::info("[menuicons] from movie root: {}", report);
    }

    // The row handle cannot see a child we added to the movie (its value carries type 0, so the
    // path walker never reaches GetMember on it), but the SAME row reached by an explicit path
    // from the movie root resolves it - both were measured side by side, with identical pdata.
    // So icons are addressed by path, and the slot number comes from the engine itself: this is
    // the helper that formats "Item_<slot>_<column>" for the list's own row lookups, so the pair
    // it is called with is exactly the slot about to be drawn.
    struct RowSlotPair
    {
        int32_t slot;
        int32_t column;
    };
    using RowPathFn = void *(void *list, void *out, const RowSlotPair *pair);
    RowPathFn *o_row_path = nullptr;

    std::atomic<int32_t> g_row_slot_max{-1};

    void *row_path_detour(void *list, void *out, const RowSlotPair *pair)
    {
        if (pair)
        {
            // The engine formats the row name itself, so the highest slot it ever asks for IS the
            // number of rows it is willing to show. That is the one fact that decides whether adding
            // clips to the movie buys anything, so it goes in the log the first time it grows.
            if (pair->column == 0 && pair->slot > g_row_slot_max.load(std::memory_order_relaxed))
            {
                g_row_slot_max.store(pair->slot, std::memory_order_relaxed);
                spdlog::info("[form] engine asked for row slot {} (movie now has {} left-column "
                             "clips)", pair->slot, goblin::own_movie::kRowSlots);
            }
        }
        return o_row_path(list, out, pair);
    }

    // ── which grid cell is this row? ─────────────────────────────────────────────────
    // Icons have to be addressed BY PATH from the movie root (the row handle the renderer
    // passes resolves the movie's own declared children but not one we added), so the cell
    // index has to be right. Taking it from the engine's row-path helper did NOT work: that
    // capture is stale by the time we draw, and in game the shift always landed on the same
    // clip - one icon, stuck in the topmost visible row, changing only when the list scrolled.
    //
    // The row handle and the clip reached by path are the SAME display object (both carry the
    // identical pdata at +0x50, measured side by side), so the cell can be identified instead
    // of guessed: resolve each cell once, remember its object, and match. The eleven clips
    // belong to the movie and live as long as it does, so the table is built once.
    // Sized from the movie, not repeated here: goblin_own_movie adds row clips, and a slot table
    // that stopped at eleven would leave the added rows without icons.
    constexpr int kGridSlots = goblin::own_movie::kRowSlots;
    uint64_t g_slot_obj[kGridSlots] = {};
    bool g_slot_table_ready = false;

    void build_slot_table(uintptr_t base, uintptr_t dlg)
    {
        auto p_resolve = reinterpret_cast<ResolveFn *>(goblin::anchors::at(goblin::AnchorId::clip_resolve_child));
        auto p_valid = reinterpret_cast<ProxyValidFn *>(goblin::anchors::at(goblin::AnchorId::clip_is_valid));
        auto p_dtor = reinterpret_cast<ProxyDtorFn *>(goblin::anchors::at(goblin::AnchorId::clip_proxy_dtor));
        int found = 0;
        __try
        {
            void *root = reinterpret_cast<void *>(dlg + 0x120);
            for (int s = 0; s < kGridSlots; ++s)
            {
                char path[64];
                _snprintf_s(path, sizeof(path), _TRUNCATE, "KeySetting/ItemList/Item_%d_0", s);
                uint8_t buf[0x60] = {};
                void *r = p_resolve(root, buf, path);
                g_slot_obj[s] = p_valid(r)
                                    ? *reinterpret_cast<uint64_t *>(
                                          reinterpret_cast<uint8_t *>(r) + 0x50)
                                    : 0;
                if (g_slot_obj[s])
                    ++found;
                p_dtor(buf + 0x28);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        g_slot_table_ready = found > 0;
        spdlog::info("[menuicons] grid cell table: {} of {} cells identified", found, kGridSlots);
    }

    int slot_of_row(uintptr_t base, uintptr_t dlg, void *rowProxy)
    {
        uint64_t want = 0;
        __try
        {
            want = *reinterpret_cast<uint64_t *>(reinterpret_cast<uint8_t *>(rowProxy) + 0x50);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
        if (!want)
            return -1;
        for (int pass = 0; pass < 2; ++pass)
        {
            if (!g_slot_table_ready)
                build_slot_table(base, dlg);
            for (int s = 0; s < kGridSlots; ++s)
                if (g_slot_obj[s] == want)
                    return s;
            // A miss means the clips were rebuilt (a fresh open of the screen) - re-measure
            // once, then give up rather than spin.
            g_slot_table_ready = false;
        }
        return -1;
    }

    // rowProxy: the LIVE row handle when the renderer gave us one, else nullptr (the
    // repaint pass has no handle and falls back to the path from the movie root).
    void draw_row_icon(uintptr_t base, int32_t slot, const char *ini_key,
                      void *rowProxy = nullptr)
    {
        auto p_resolve = reinterpret_cast<ResolveFn *>(goblin::anchors::at(goblin::AnchorId::clip_resolve_child));
        auto p_visible = reinterpret_cast<SetVisibleFn *>(goblin::anchors::at(goblin::AnchorId::clip_set_visible));
        auto p_valid = reinterpret_cast<ProxyValidFn *>(goblin::anchors::at(goblin::AnchorId::clip_is_valid));
        auto p_dtor = reinterpret_cast<ProxyDtorFn *>(goblin::anchors::at(goblin::AnchorId::clip_proxy_dtor));
        using SetFrameNumFn = void(void *proxy, int frame);
        auto p_framenum = reinterpret_cast<SetFrameNumFn *>(goblin::anchors::at(goblin::AnchorId::clip_goto_frame_num));
        // One-shot: report whether a brand-new spliced clip name resolves at all - that
        // answers the long-standing question and tells us if icons can work this way.
        static std::atomic<int> s_reported{0};
        // The child renders in every row, so it IS in the movie - only the name lookup fails.
        // GFx resolves a name on a display object by property first and then by child name
        // (AS3_GFxValueImpl::GetMember falls back to GetAS3ChildByName), so a timeline child
        // with an instance name should be findable. Bracket the failure once from the movie
        // root: if an explicit path finds it, the row handle is what cannot see it; if even the
        // explicit path fails while its siblings resolve, it is the name itself.
        if (!g_icon_path_probed.exchange(true, std::memory_order_acq_rel))
            probe_icon_paths(base);
        int frame = 0;
        if (ini_key)
            for (const auto &kf : goblin::menu_icon_tags::ICON_FRAME_OF_KEY)
                if (std::strcmp(kf.key, ini_key) == 0)
                {
                    frame = kf.frame;
                    break;
                }
        const uintptr_t dlg = g_form_dialog.load(std::memory_order_acquire);
        if (!dlg || slot < 0)
        {
            static uint64_t s_last = 0;
            const uint64_t now = GetTickCount64();
            if (now - s_last > 1000)
            {
                s_last = now;
                spdlog::info("[menuicons] no icon drawn: row clip not matched to a grid cell "
                             "(slot {} dlg 0x{:X})", slot, dlg);
            }
            return;
        }
        // Column 0, ALWAYS. Every row of ours is the left half of a pair, and only the left
        // clip (cid 189) carries our icon child - the right one (cid 182) has just a value
        // field. The captured column is whichever path the engine formatted last, and it was
        // observed as 0 in one run and 1 in another: on the 1 runs the resolve targets a clip
        // that has no MfgIcon, so the left column's strip never gets shifted and every row
        // shows the same cell.
        const int32_t column = 0;
        // Icons stop appearing after a row is toggled and come back only on a fresh open, yet
        // neither of the two obvious faults reports itself: the slot resolves and the MfgIcon
        // child is found. So trace what is ACTUALLY applied for a short burst after every view
        // refresh - slot, the ini key it matched, the cell and the x it lands on. A cell of 0 is
        // the blank one, which would look exactly like "the icon vanished".
        auto note = [](const char *what, int32_t s, int32_t f) {
            static uint64_t last = 0;
            const uint64_t now = GetTickCount64();
            if (now - last <= 1000)
                return;
            last = now;
            spdlog::info("[menuicons] no icon drawn: {} (slot {} cell {})", what, s, f);
        };
        // Which cell each slot currently shows, so variant 2 can put the previous one away
        // again. 16 slots x 2 columns covers the list with room to spare.
        static int8_t s_shown[16][2] = {};
        char path[96];
        void *root = reinterpret_cast<void *>(dlg + 0x120);
        // (proxy, int x, int y) - the helper converts to float itself (cvtdq2ps), so passing
        // floats would put the values in the wrong registers entirely.
        using SetPosFn = void(void *proxy, int32_t x, int32_t y);
        __try
        {
            {
                // One named child holding the strip, masked by a sibling: a single resolve and a
                // single move. The placement already sits at (ICON_X, ICON_Y); the position we set is
                // absolute within the row, so the cell offset is folded into x.
                // WHERE the child is resolved from matters. By path from the movie root the shift
                // measurably lands on something that is not what the row draws: after a toggle
                // the trace showed the right cell applied to all eleven slots three times over
                // and the icons stayed blank, while the row's own text - written through the row
                // handle - was correct. So when the renderer hands us the live row handle, ask
                // THAT for its child; the path stays as the route for the repaint pass, which has
                // no handle. The two are compared once per refresh below.
                uint8_t buf[0x60] = {};
                void *r = nullptr;
                if (rowProxy)
                {
                    r = p_resolve(rowProxy, buf, "MfgIcon");
                    if (!p_valid(r))
                    {
                        p_dtor(buf + 0x28);
                        std::memset(buf, 0, sizeof(buf));
                        r = nullptr;
                    }
                }
                if (!r)
                {
                    _snprintf_s(path, sizeof(path), _TRUNCATE,
                                "KeySetting/ItemList/Item_%d_%d/MfgIcon", slot, column);
                    r = p_resolve(root, buf, path);
                }
                const bool ok = p_valid(r) != 0;
                if (ok)
                    // From the origin now: the strip's placement carries no matrix (so nothing
                    // resets it) and the row offset ICON_X/ICON_Y is baked into the strip itself.
                    reinterpret_cast<SetPosFn *>(goblin::anchors::at(goblin::AnchorId::clip_set_pos))(
                        r, -frame * goblin::menu_icon_tags::ICON_CELL_PX, 0);
                else
                    note("MfgIcon did not resolve in the row", slot, frame);
                // The identity trace that settled this (row handle vs path, both +0x50) is gone
                // now that the answer is known and in the tag: same object, right cell, and the
                // matrix in the placement was what undid it. The two failure notes above stay -
                // they are rate-limited and they are what would speak up if it ever regresses.
                g_prehide_report.store(ok ? 1 : 0, std::memory_order_release);
                p_dtor(buf + 0x28);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        // Once per run: which construction is live, plus how many of the icon children the
        // pre-hide pass could actually reach - "resolved 0 of 64" and "hidden but still drawn"
        // are different faults and must not look alike in the log.
        if (s_reported.load() == 0)
            spdlog::info("[menuicons] strip resolved: {} (slot {} column {}, cell {}, "
                         "x {})",
                         g_prehide_report.load(std::memory_order_acquire) == 1 ? "yes" : "no",
                         slot, column, frame,
                         -frame * goblin::menu_icon_tags::ICON_CELL_PX);
        if (s_reported.exchange(1) == 0)
            spdlog::info("[menuicons] icons drawn (slot {} column {})", slot, column);
    }

    // POD-only body for draw_row_slider (SEH + spdlog temporaries may not share a frame).
    // Returns -1 resolve failed, else the cell applied.
    int draw_row_slider_raw(uintptr_t base, uintptr_t dlg, int32_t slot, int cell,
                            void *rowProxy)
    {
        auto p_resolve = reinterpret_cast<ResolveFn *>(goblin::anchors::at(goblin::AnchorId::clip_resolve_child));
        auto p_valid = reinterpret_cast<ProxyValidFn *>(goblin::anchors::at(goblin::AnchorId::clip_is_valid));
        auto p_dtor = reinterpret_cast<ProxyDtorFn *>(goblin::anchors::at(goblin::AnchorId::clip_proxy_dtor));
        using SetPosFn = void(void *proxy, int32_t x, int32_t y);
        char path[96];
        void *root = reinterpret_cast<void *>(dlg + 0x120);
        int applied = -1;
        __try
        {
            uint8_t buf[0x60] = {};
            void *r = nullptr;
            // Same two routes as the icon strip: the live row handle when the renderer gave
            // us one, the path from the movie root for the repaint pass.
            if (rowProxy)
            {
                r = p_resolve(rowProxy, buf, "MfgSlider");
                if (!p_valid(r))
                {
                    p_dtor(buf + 0x28);
                    std::memset(buf, 0, sizeof(buf));
                    r = nullptr;
                }
            }
            if (!r)
            {
                _snprintf_s(path, sizeof(path), _TRUNCATE,
                            "KeySetting/ItemList/Item_%d_0/MfgSlider", slot);
                r = p_resolve(root, buf, path);
            }
            if (p_valid(r))
            {
                reinterpret_cast<SetPosFn *>(goblin::anchors::at(goblin::AnchorId::clip_set_pos))(
                    r, -cell * goblin::menu_icon_tags::SLIDER_CELL_PITCH_PX, 0);
                applied = cell;
            }
            p_dtor(buf + 0x28);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return applied;
    }

    // The native-look slider bar, driven exactly like the icon strip: one named child
    // ("MfgSlider", placed with NO matrix), shifted to the cell for the row's value.
    // Cell 0 is empty, so every non-slider row parks the strip there - which is also what
    // an untouched instance (the game's own key-binding screen) shows.
    void draw_row_slider(uintptr_t base, int32_t slot, const goblin::nmenu::Row *row,
                         void *rowProxy = nullptr)
    {
        const uintptr_t dlg = g_form_dialog.load(std::memory_order_acquire);
        if (!dlg || slot < 0)
            return;
        int cell = 0;
        if (row && row->kind == goblin::nmenu::RowKind::Slider)
        {
            cell = 1 + static_cast<int>(row->slider_frac *
                                        (goblin::menu_icon_tags::SLIDER_CELLS - 1) + 0.5f);
            if (cell < 1)
                cell = 1;
            if (cell > goblin::menu_icon_tags::SLIDER_CELLS)
                cell = goblin::menu_icon_tags::SLIDER_CELLS;
        }
        const int applied = draw_row_slider_raw(base, dlg, slot, cell, rowProxy);
        // One-shot, and only for a REAL slider cell: whether the second spliced name
        // resolves at all. The measured history (icons, 2026-07) is that exactly one added
        // named child ever resolved, so this line IS the experiment's readout.
        static std::atomic<int> s_logged{0};
        if (cell > 0 && s_logged.exchange(1) == 0)
            spdlog::info("[menuslider] 'MfgSlider' resolves: {} (slot {}, cell {})",
                         applied >= 0 ? "yes" : "NO", slot, cell);
    }

    void *row_render_detour(void *item, void *rowProxy)
    {
        const uintptr_t it = reinterpret_cast<uintptr_t>(item);
        const goblin::nmenu::Row *row = menu_open() ? model_row_of(it, nullptr) : nullptr;
        if (!row)
        {
            // One of OUR items that is not on the live screen (a parent's row, redrawn while a
            // child is over it): leave it exactly as it was drawn. Handing it to the native
            // renderer would fill it with key-binding data read out of our fake param row.
            uintptr_t param = 0;
            __try
            {
                param = *reinterpret_cast<uintptr_t *>(it + 0x10);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                param = 0;
            }
            if (param && pool_level_of(reinterpret_cast<void *>(param)) >= 0)
                return item;
            return o_row_render(item, rowProxy);
        }
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (!g_row_probe_done.exchange(true, std::memory_order_acq_rel))
            probe_row_clips(base, rowProxy);
        // First draw after opening: the dialog now exists, so stamp the page title.
        if (g_form_title_pending.exchange(false, std::memory_order_acq_rel))
        {
            const uintptr_t dlg = g_form_dialog.load(std::memory_order_acquire);
            if (dlg)
            {
                prepare_form_layout(base, dlg);
                set_form_captions(base, dlg, goblin::nmenu::page_title(), nullptr);
                // First page of a fresh open: the grid was seeded from the bind list.
                sync_grid_extent(dlg, g_form_landed.load(std::memory_order_acquire));
                if (!g_cmd_probe_done.exchange(true, std::memory_order_acq_rel))
                    probe_form_commands(base, dlg);
            }
        }
        // Style per row kind: value rows keep the normal two-field layout, read-only rows
        // use the dimmed frame, and pure separators use the wide category frame.
        const char *style = "Normal";
        switch (row->kind)
        {
        case goblin::nmenu::RowKind::Info:
            style = (row->value && *row->value) ? "Grayout" : "PadCategory";
            break;
        case goblin::nmenu::RowKind::Back:
        case goblin::nmenu::RowKind::SubPage:
        case goblin::nmenu::RowKind::Toggle:
        case goblin::nmenu::RowKind::Slider:
        case goblin::nmenu::RowKind::Enum:
        case goblin::nmenu::RowKind::ValueOption:
        case goblin::nmenu::RowKind::Rebind:
        case goblin::nmenu::RowKind::Action:
        case goblin::nmenu::RowKind::Progress:
            style = "Normal";
            break;
        }
        // The live-drawing route needs its host clip BEFORE the style frame is applied (those
        // clips differ per frame); the spliced icon child is the opposite - it must be updated
        // AFTER, because a gotoAndStop rebuilds the row's display list and would hand back a
        // fresh child, visible and playing from frame 1, undoing whatever we just set.
        // Identify the grid cell while the row handle is still alive: draw_our_row ends with
        // the native renderer's own proxy dtor, and both icon routes run after it.
        const uintptr_t icon_dlg = g_form_dialog.load(std::memory_order_acquire);
        const int32_t icon_slot = icon_dlg ? slot_of_row(base, icon_dlg, rowProxy) : -1;
        // Keep the caption clear of the icon: the label field starts at x=26.8 and a 32px
        // icon reaches x=34, so a row with a picture gets a small text indent.
        const wchar_t *label = row->label;
        wchar_t indented[512];
        if (row->icon_id >= 0 && label && label[0])
        {
            // Six spaces, not four: at 22 px the four the row started with left the caption touching
            // the icon once the icons moved right for their own left margin.
            _snwprintf_s(indented, _TRUNCATE, L"      %s", label);
            label = indented;
        }
        draw_our_row(base, rowProxy, style, label, row->value, row->plate);
        draw_row_icon(base, icon_slot, row->ini_key, rowProxy);
        draw_row_slider(base, icon_slot, row, rowProxy);
        return item;
    }

    // Is this a pointer INTO THE GAME'S OWN IMAGE? Used to sanity-check a value that is about to
    // become a CALL TARGET. A vtable and the function it points at both live in the exe, so a
    // value outside that range is not one, whatever else it may be.
    bool exe_image_ptr(uintptr_t p)
    {
        static uintptr_t s_base = 0, s_end = 0;
        if (!s_end)
        {
            const uintptr_t b = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(b);
            const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(b + dos->e_lfanew);
            s_base = b;
            s_end = b + nt->OptionalHeader.SizeOfImage;
        }
        return p >= s_base && p < s_end;
    }

    // The selected model row index, resolved exactly like the form's own handler does:
    //   index = FUN_140739e20(dlg + 0xa38)               (GridControl cursor)
    //   item  = viewList_vt[+0x28](dlg + 0x1268, index)   (MenuViewItemList)
    bool selected_model_index(uintptr_t dlg, size_t *out_index, bool *out_is_preview)
    {
        bool ok = false;
        if (out_is_preview)
            *out_is_preview = false;
        __try
        {
            using CursorFn = uint32_t(void *grid);
            using ItemAtFn = void *(void *viewList, uint32_t index);
            const uint32_t index = reinterpret_cast<CursorFn *>(goblin::anchors::at(goblin::AnchorId::grid_cursor_get))(
                reinterpret_cast<void *>(dlg + 0xa38));
            void *viewList = reinterpret_cast<void *>(dlg + 0x1268);
            const uintptr_t vvt = *reinterpret_cast<uintptr_t *>(viewList);
            // CHECKED BEFORE IT IS CALLED. A dialog that is being torn down under us hands back
            // rubbish here - measured on the Deck 2026-08-02, where it read -1 and produced two
            // AVs with fault=0xFFFFFFFFFFFFFFFF. The __except below caught those, but only by
            // luck: -1 is unmapped, so the call faulted instead of landing somewhere. A garbage
            // value that happened to be mapped and executable would have been EXECUTED, and no
            // exception filter can help with that. So the vtable and the slot we take out of it
            // must both point into the game's own image before either becomes a call target.
            if (!exe_image_ptr(vvt))
                return false;
            const uintptr_t item_at = *reinterpret_cast<uintptr_t *>(vvt + 0x28);
            if (!exe_image_ptr(item_at))
                return false;
            // AND the index must be inside the list AS IT IS RIGHT NOW. itemAt does not check the
            // index at all (begin + index*0x50, disassembled 2026-09-23), so an index left over from
            // before a rebuild comes back as a mapped but stale slot; nothing faults at the call, and
            // the crash surfaces one frame deeper when that dead item's scene proxy is copied
            // (measured: exe+0x74A7F5, six times in a second while the player toggled a row). Decompiled 2026-08-02: the list at dlg+0x1268
            // holds a {begin,end,cap} vector at +0x10/+0x18/+0x20 with a 0x50 element stride, and
            // the rebuild destructs every element in place and may relocate the buffer entirely.
            // There is no generation counter and no rebuild-in-progress flag anywhere to ask
            // instead - this bounds test IS the liveness test.
            const uintptr_t list_begin = *reinterpret_cast<uintptr_t *>(dlg + 0x1278);
            const uintptr_t list_end = *reinterpret_cast<uintptr_t *>(dlg + 0x1280);
            if (!v3_heap_ptr(list_begin) || !v3_heap_ptr(list_end) || list_end < list_begin)
                return false;
            const uintptr_t span = list_end - list_begin;
            if (span % 0x50 != 0)
                return false;  // not the layout we believe it is - never guess past this point
            if (index >= span / 0x50)
                return false;  // a cursor from before the last rebuild
            void *item = reinterpret_cast<ItemAtFn *>(item_at)(viewList, index);
            const goblin::nmenu::Row *row =
                item ? model_row_of(reinterpret_cast<uintptr_t>(item), out_index) : nullptr;
            if (row)
            {
                ok = true;
                if (out_is_preview)
                    *out_is_preview = row->right_column;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // Silent until 2026-08-01, which is why 12 faults a session went unnoticed. MEASURED
            // that day, so nobody re-opens this: exe+0x739E20 is a two-instruction leaf,
            // `mov eax,[rcx+0xd4]; ret`, so this reads the FIXED address dlg+0xB0C - no pointer
            // chase, nothing uninitialised to blame inside a grid. It faults about four times per
            // freshly opened screen and then works for the rest of that screen's life, which is an
            // initialisation race, not a freed dialog. The caller's screen_level() check does NOT
            // catch it (measured: screenLevel=0 screens=1 while faulting) and could not, because
            // g_form_dialog and g_screens[i].dlg are filled from the same source - that test is
            // circular for this failure. The __except IS the handling: skip the frame, repaint next
            // one. Debug level on purpose - it is benign and costs a few unpainted help lines.
            static uintptr_t s_last = 0;
            if (dlg != s_last)
            {
                s_last = dlg;
                spdlog::debug("[nmenu] cursor read faulted: dlg=0x{:X} screenLevel={} screens={}",
                              dlg, screen_level(dlg), g_screens.size());
            }
            return false;
        }
        return ok;
    }

    using DecideFn = void(void *dlg);
    DecideFn *o_form_decide = nullptr;

    // Rebuild + redraw the rows in place. dlg+0x1268 = the MenuViewItemList the rows live
    // in, dlg+0x1290 = CSMenuKeyConfig (the bind manager the item ctors take).
    // FUN_140942690 is the game's own view refresh; it READS the grid cursor (grid+0xd4)
    // and never moves it, so the selection survives a rebuild.
    // Write the current page title (and a hint line) into the form's own caption clips.
    // dlg+0x120 is the movie's clip-root proxy - the ctor resolves "MenuTitle/Text",
    // "ActionHelp/Text_0" and friends from it (FUN_14093d290), so we can retarget them.
    void set_form_captions(uintptr_t base, uintptr_t dlg, const wchar_t *title,
                           const wchar_t *hint)
    {
        auto p_resolve = reinterpret_cast<ResolveFn *>(goblin::anchors::at(goblin::AnchorId::clip_resolve_child));
        auto p_settext = reinterpret_cast<SetTextFn *>(goblin::anchors::at(goblin::AnchorId::clip_set_text_html));
        auto p_valid = reinterpret_cast<ProxyValidFn *>(goblin::anchors::at(goblin::AnchorId::clip_is_valid));
        auto p_dtor = reinterpret_cast<ProxyDtorFn *>(goblin::anchors::at(goblin::AnchorId::clip_proxy_dtor));
        void *root = reinterpret_cast<void *>(dlg + 0x120);
        __try
        {
            if (title)
            {
                uint8_t buf[0x60] = {};
                void *r = p_resolve(root, buf, "MenuTitle/Text");
                if (p_valid(r))
                    p_settext(reinterpret_cast<uint8_t *>(r) + 8, title);
                p_dtor(buf + 0x28);
            }
            if (hint)
            {
                uint8_t buf[0x60] = {};
                void *r = p_resolve(root, buf, "ActionHelp/Text_0");
                if (p_valid(r))
                    p_settext(reinterpret_cast<uint8_t *>(r) + 8, hint);
                p_dtor(buf + 0x28);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    // Make the screen look like a settings screen rather than a keybinding screen:
    //  * KeySetting/BG frame 2 = the WIDE two-column panel (frame 1 is the narrow one
    //    that leaves room for the gamepad art). The ctor itself does
    //    FUN_140749980(bgProxy+0x18, mode + 1), so frame 2 is what device mode 1 uses.
    //  * hide the device preview clips (SelectKey/*, Help/*) - a settings list has no
    //    button to point at.
    // recon_02_160_structure.md, section "visual element we want".
    uint32_t g_caption_hits = 0;

    // ── menu centring, per instance ───────────────────────────────────────────────
    // The key-binding screen lays its rows out to the LEFT of the 'KeySetting' section origin (960,
    // 480 px in the root): our single-column page reads as "left half". Moving the section by +472 px
    // centres it (the derivation is with kRowSectionShiftPx in goblin_own_movie.cpp). Done here, on
    // the dialog's OWN update, for OUR screens only - the 2026-07 runtime attempts wrote from the
    // menu tick and reached dying screens; the movie-level edit that replaced them moved every
    // instance of the one parse, the player's own Key Assignments screen included. The timeline
    // re-places the section during the intro (frames 2+, and again whenever a hidden page is shown
    // again), so the position is re-asserted for the first kShiftFrames frames after each show and
    // then left alone. SetPos is absolute, in px, in the parent's space.
    constexpr int32_t kSectionShiftPx = 472;
    constexpr int32_t kSectionAuthoredX = 960;
    constexpr int32_t kSectionAuthoredY = 480;
    constexpr uint32_t kShiftFrames = 180; // 3 s at 60 fps: comfortably past the FadeIn frames

    // POD-only body (SEH): resolve the section from the movie root and place it. 1 = placed.
    int shift_section_raw(uintptr_t dlg)
    {
        auto p_resolve = reinterpret_cast<ResolveFn *>(goblin::anchors::at(goblin::AnchorId::clip_resolve_child));
        auto p_valid = reinterpret_cast<ProxyValidFn *>(goblin::anchors::at(goblin::AnchorId::clip_is_valid));
        auto p_dtor = reinterpret_cast<ProxyDtorFn *>(goblin::anchors::at(goblin::AnchorId::clip_proxy_dtor));
        using SetPosFn = void(void *proxy, int32_t x, int32_t y);
        auto p_setpos = reinterpret_cast<SetPosFn *>(goblin::anchors::at(goblin::AnchorId::clip_set_pos));
        if (!p_resolve || !p_valid || !p_dtor || !p_setpos)
            return 0;
        void *root = reinterpret_cast<void *>(dlg + 0x120);
        int placed = 0;
        __try
        {
            uint8_t buf[0x60] = {};
            void *r = p_resolve(root, buf, "KeySetting");
            if (p_valid(r))
            {
                p_setpos(r, kSectionAuthoredX + kSectionShiftPx, kSectionAuthoredY);
                placed = 1;
            }
            p_dtor(buf + 0x28);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return placed;
    }

    void shift_section_after_advance(uintptr_t dlg)
    {
        // With our own movie definition the shift is authored into that movie (goblin_own_movie,
        // kRowSectionShiftPx) and reaches no other screen; this runtime write is the fallback for
        // an exe where the opener route is dead and the shared parse is what our screens run on.
        if (kSectionShiftPx == 0 || goblin::own_movie::separate_movie_armed())
            return;
        const int level = screen_level(dlg);
        if (level < 0)
            return; // not one of ours: the player's screen keeps its authored layout
        Screen &s = g_screens[static_cast<size_t>(level)];
        if (s.shift_frames >= kShiftFrames)
            return;
        ++s.shift_frames;
        const int placed = shift_section_raw(dlg);
        static std::atomic<int> s_logged{0};
        if (s.shift_frames == 1 && s_logged.fetch_add(1) < 3)
            spdlog::info("[form] section shift (+{} px) on level {}: {}", kSectionShiftPx, level,
                         placed ? "placed" : "'KeySetting' did not resolve");
    }

    void prepare_form_layout(uintptr_t base, uintptr_t dlg)
    {
        auto p_resolve = reinterpret_cast<ResolveFn *>(goblin::anchors::at(goblin::AnchorId::clip_resolve_child));
        auto p_visible = reinterpret_cast<SetVisibleFn *>(goblin::anchors::at(goblin::AnchorId::clip_set_visible));
        auto p_valid = reinterpret_cast<ProxyValidFn *>(goblin::anchors::at(goblin::AnchorId::clip_is_valid));
        auto p_dtor = reinterpret_cast<ProxyDtorFn *>(goblin::anchors::at(goblin::AnchorId::clip_proxy_dtor));
        using SetFrameNumFn = void(void *proxy, int frame);
        auto p_framenum = reinterpret_cast<SetFrameNumFn *>(goblin::anchors::at(goblin::AnchorId::clip_goto_frame_num));
        void *root = reinterpret_cast<void *>(dlg + 0x120);
        static const char *const kHide[] = {
            "SelectKey/Win64", "SelectKey/PS4",  "SelectKey/PS5",     "SelectKey/XboxOne",
            "SelectKey/XboxSeries", "Help/Win64", "Help/PS4",         "Help/PS5",
            "Help/XboxOne",   "Help/XboxSeries", "SelectKey",         "Help"};
        __try
        {
            {
                uint8_t buf[0x60] = {};
                void *r = p_resolve(root, buf, "KeySetting/BG");
                if (p_valid(r))
                    p_framenum(reinterpret_cast<uint8_t *>(r) + 0x18, 2);
                p_dtor(buf + 0x28);
            }
            for (const char *name : kHide)
            {
                uint8_t buf[0x60] = {};
                void *r = p_resolve(root, buf, name);
                if (p_valid(r))
                    p_visible(r, 0);
                p_dtor(buf + 0x28);
            }
            static const char *const kCaptions[] = {
                "KeySetting/StaticText_280005",          "KeySetting/StaticText_280006",
                "KeySetting/ItemList/StaticText_280005", "KeySetting/ItemList/StaticText_280006",
                "KeySetting/BG/StaticText_280005",       "KeySetting/BG/StaticText_280006",
                "StaticText_280005",                     "StaticText_280006"};
            // The RIGHT column - the "second bind" half the gamepad screen does not even have.
            // Our rows are all left-column, so those eleven clips are dead furniture under the
            // caption we just removed. Unlike the captions they ARE named, so a plain hide works.
            for (int r = 0; r < goblin::own_movie::kRowSlots; ++r) // the added rows have partners too
            {
                char rp[64];
                _snprintf_s(rp, sizeof(rp), _TRUNCATE, "KeySetting/ItemList/Item_%d_1", r);
                uint8_t rb[0x60] = {};
                void *rr = p_resolve(root, rb, rp);
                if (p_valid(rr))
                    p_visible(rr, 0);
                p_dtor(rb + 0x28);
            }
            for (size_t c = 0; c < sizeof(kCaptions) / sizeof(kCaptions[0]); ++c)
            {
                uint8_t buf[0x60] = {};
                void *r = p_resolve(root, buf, kCaptions[c]);
                if (p_valid(r))
                {
                    p_visible(r, 0);
                    g_caption_hits |= (1u << c);
                }
                p_dtor(buf + 0x28);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        static bool s_caption_logged = false;
        if (!s_caption_logged)
        {
            s_caption_logged = true;
            spdlog::info("[form] stray column captions blanked: mask 0x{:X}", g_caption_hits);
        }
    }

    // One-shot: log the input commands the form has registered (vector win+0x1F8..+0x200,
    // stride 0x140; the name/key pack lives at entry+0x40 and FUN_140745170 hands it to
    // us). Knowing the real command set is the prerequisite for taking over Back (so it
    // walks our page stack instead of closing) and for adding left/right on value rows -
    // recon_screen_hierarchy.md section 6.
    size_t count_form_commands(uintptr_t dlg, uintptr_t *first, uintptr_t *last)
    {
        __try
        {
            *first = *reinterpret_cast<uintptr_t *>(dlg + 0x1F8);
            *last = *reinterpret_cast<uintptr_t *>(dlg + 0x200);
            if (!*first || *last < *first)
                return 0;
            return static_cast<size_t>((*last - *first) / 0x140);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    // POD-only: pull one command's display name into caller-provided storage.
    bool form_command_name(uintptr_t base, uintptr_t entry, wchar_t *out, size_t cap)
    {
        out[0] = 0;
        __try
        {
            using PackFn = void *(uintptr_t entry, void *outPack);
            uint8_t pack[0x90] = {};
            void *p = reinterpret_cast<PackFn *>(goblin::anchors::at(goblin::AnchorId::caption_pack))(entry, pack);
            if (!p)
                return false;
            // pack layout (cmdlist RE): DLString name @+0x8 - inline buffer unless long.
            const uint8_t *str = reinterpret_cast<const uint8_t *>(p) + 8;
            const wchar_t *chars = *reinterpret_cast<const wchar_t *const *>(str);
            const uint64_t len = *reinterpret_cast<const uint64_t *>(str + 0x28);
            if (!chars || len == 0 || len > 256)
                chars = reinterpret_cast<const wchar_t *>(str + 0x10); // inline
            for (size_t i = 0; i + 1 < cap && chars[i]; ++i)
            {
                out[i] = chars[i];
                out[i + 1] = 0;
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void probe_form_commands(uintptr_t base, uintptr_t dlg)
    {
        uintptr_t first = 0, last = 0;
        const size_t n = count_form_commands(dlg, &first, &last);
        spdlog::info("[form] registered commands: {}", n);
        for (size_t i = 0; i < n && i < 16; ++i)
        {
            const uintptr_t entry = first + i * 0x140;
            wchar_t name[64] = {};
            const bool ok = form_command_name(base, entry, name, 64);
            char utf8[128] = {};
            if (ok)
                WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof(utf8), nullptr, nullptr);
            spdlog::info("[form]   cmd[{}] entry=0x{:X} action=0x{:X} name='{}'", i, entry,
                         *reinterpret_cast<uintptr_t *>(entry + 0xF8), utf8);
        }
    }

    // Paint the preview into the right-hand column by writing the fixed row clips
    // ("KeySetting/ItemList/Item_<n>_1") straight from the movie root. Those slots are not
    // part of the grid's data, so the panel stays put while the left list scrolls, and it
    // never behaves like a row the cursor can land on.
    void paint_right_panel(uintptr_t base, uintptr_t dlg)
    {
        auto p_resolve = reinterpret_cast<ResolveFn *>(goblin::anchors::at(goblin::AnchorId::clip_resolve_child));
        auto p_settext = reinterpret_cast<SetTextFn *>(goblin::anchors::at(goblin::AnchorId::clip_set_text_html));
        auto p_visible = reinterpret_cast<SetVisibleFn *>(goblin::anchors::at(goblin::AnchorId::clip_set_visible));
        auto p_valid = reinterpret_cast<ProxyValidFn *>(goblin::anchors::at(goblin::AnchorId::clip_is_valid));
        auto p_dtor = reinterpret_cast<ProxyDtorFn *>(goblin::anchors::at(goblin::AnchorId::clip_proxy_dtor));
        void *root = reinterpret_cast<void *>(dlg + 0x120);
        for (int n = 0; n < goblin::own_movie::kRowSlots; ++n) // rows past the model hide their clip
        {
            const goblin::nmenu::Row *row = goblin::nmenu::right_row(static_cast<size_t>(n));
            char path[64];
            _snprintf_s(path, sizeof(path), _TRUNCATE, "KeySetting/ItemList/Item_%d_1", n);
            __try
            {
                uint8_t buf[0x60] = {};
                void *slot = p_resolve(root, buf, path);
                if (p_valid(slot))
                {
                    p_visible(slot, row ? 1 : 0);
                    if (row)
                    {
                        // Strip the row furniture so it reads as text, not as a button.
                        for (const char *child : {"Cursor", "CursorLock", "Conflict", "HitArea"})
                        {
                            uint8_t cbuf[0x60] = {};
                            void *c = p_resolve(slot, cbuf, child);
                            if (p_valid(c))
                                p_visible(c, 0);
                            p_dtor(cbuf + 0x28);
                        }
                        // cid 182 (the right column) has ONE text field, so the label and
                        // the value share it, laid out with HTML rather than two clips.
                        wchar_t line[192];
                        if (row->value && row->value[0])
                            _snwprintf_s(line, _TRUNCATE,
                                         L"<p align=\"left\">%s   <font color=\"#C8B48C\">%s"
                                         L"</font></p>",
                                         row->label ? row->label : L"", row->value);
                        else
                            _snwprintf_s(line, _TRUNCATE, L"<p align=\"left\">%s</p>",
                                         row->label ? row->label : L"");
                        uint8_t tbuf[0x60] = {};
                        void *t = p_resolve(slot, tbuf, "Text_1");
                        if (p_valid(t))
                            p_settext(reinterpret_cast<uint8_t *>(t) + 8, line);
                        p_dtor(tbuf + 0x28);
                    }
                }
                p_dtor(buf + 0x28);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                break;
            }
        }
    }

    // The screen's help line is rewritten by the game every frame from an FMG id (which for
    // our rows resolves to the placeholder "GR_LineHelp(...)"), so we overwrite it AFTER the
    // engine's update with the highlighted row's own description - the text the overlay used
    // to show as a tooltip.
    // (A stray forward declaration of paint_right_panel stood here, 62 lines BELOW the definition
    //  and wedged between the doc comment of paint_help_line and kPaintRightPanel, so it read as
    //  documentation of the wrong function. One namespace, so it declared nothing new.)

    // Off for now. Painting the preview into the right-hand row clips works, but those clips
    // carry the list's own button plate (an unnamed timeline child, so it cannot be hidden by
    // name, and cid 182 has no plate-less frame), and the result reads as a column of dead
    // buttons. The replacement is to draw that half ourselves once the image path is proven;
    // the code stays so the experiment is one flag away.
    constexpr bool kPaintRightPanel = false;

    void paint_help_line()
    {
        const uintptr_t dlg = g_form_dialog.load(std::memory_order_acquire);
        // Tighter than the old `menu_open()`, which only said SOME screen exists while
        // g_form_dialog is a raw pointer cleared solely at teardown. It does NOT fix the
        // cursor-read fault this was written for - that one is an init race, see the __except in
        // selected_model_index - and it cannot, since both fields come from the same store. Kept
        // because it is still the right question to ask before handing a dialog to the game.
        if (!dlg || screen_level(dlg) < 0)
            return;
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        size_t ix = 0;
        bool preview = false;
        if (!selected_model_index(dlg, &ix, &preview) || preview)
            return;
        size_t count = 0;
        const goblin::nmenu::Row *rows = goblin::nmenu::rows(&count);
        if (!rows || ix >= count)
            return;
        // With no description of its own a row would blank the block, which reads as a
        // glitch next to the rows that do have one. Fall back to what we know: the row's own
        // caption and its current value.
        wchar_t fallback[256];
        const wchar_t *tip = rows[ix].help;
        if (!tip || !tip[0])
        {
            const wchar_t *label = rows[ix].label ? rows[ix].label : L"";
            const wchar_t *value = rows[ix].value ? rows[ix].value : L"";
            if (value[0])
                _snwprintf_s(fallback, _TRUNCATE, L"%s: %s", label, value);
            else
                _snwprintf_s(fallback, _TRUNCATE, L"%s", label);
            tip = fallback;
        }
        auto p_resolve = reinterpret_cast<ResolveFn *>(goblin::anchors::at(goblin::AnchorId::clip_resolve_child));
        auto p_settext = reinterpret_cast<SetTextFn *>(goblin::anchors::at(goblin::AnchorId::clip_set_text_html));
        auto p_valid = reinterpret_cast<ProxyValidFn *>(goblin::anchors::at(goblin::AnchorId::clip_is_valid));
        auto p_dtor = reinterpret_cast<ProxyDtorFn *>(goblin::anchors::at(goblin::AnchorId::clip_proxy_dtor));
        void *root = reinterpret_cast<void *>(dlg + 0x120);
        __try
        {
            // The descriptions come from the ini schema, where the line breaks are AUTHORED so the
            // generated file wraps nicely. In the menu they are HARD breaks, which is what read as
            // "wraps by character count, left of centre" - our newline, not the field's word wrap.
            // Collapse them into spaces and let the field decide where a line ends.
            wchar_t flat[1024];
            size_t fi = 0;
            for (const wchar_t *c = tip; *c && fi + 1 < _countof(flat); ++c)
                flat[fi++] = (*c == 10 || *c == 13) ? L' ' : *c;
            flat[fi] = 0;
            uint8_t buf[0x60] = {};
            void *r = p_resolve(root, buf, "ActionHelp/Text_0");
            if (p_valid(r))
                p_settext(reinterpret_cast<uint8_t *>(r) + 8, flat);
            p_dtor(buf + 0x28);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        if (kPaintRightPanel)
            paint_right_panel(base, dlg);
    }

    // ── the strips have to be re-applied AFTER the engine's own refresh ──────────────
    // Measured (13:19 run): after a toggle our draw runs for all eleven slots and applies the
    // right cell for each - and the icons are still blank. The engine's refresh only marks the
    // view dirty (FUN_140942690 sets dlg+0x11FD); the render that follows re-applies each row's
    // style frame, which rebuilds the row's display list and hands the spliced icon child back at
    // its authored position - the blank cell. So our shift is undone on exactly those refreshes
    // that keep the same rows, and comes back the moment the list is scrolled, because a scroll
    // renders rows through the path where our draw runs last. That is what happens today, but not
    // through the counter this note proposed: the repaint moved to the tail of form_update_detour,
    // and the g_icon_repaint_left it describes was declared here and never read or written.

    // POD: the item the view list has bound to a visible slot, or nullptr.
    void *view_item_at(uintptr_t dlg, uint32_t index)
    {
        __try
        {
            // The same checks selected_model_index makes. itemAt (vt+0x28, exe+0x8695A0 on 1.17.1)
            // is `begin + index*0x50` with NO bounds check, and the per-frame icon repaint walks all
            // kGridSlots slots: on a page with fewer rows, the slots below the last row asked for
            // items past the end of the vector and read whatever lay there - ERR 2026-09-23, page 0
            // (11 rows): 4 refused reads every frame while the settings screen was up. The vtable
            // and its slot must point into the game's image before either becomes a call target.
            const uintptr_t list_begin = *reinterpret_cast<uintptr_t *>(dlg + 0x1278);
            const uintptr_t list_end = *reinterpret_cast<uintptr_t *>(dlg + 0x1280);
            if (!v3_heap_ptr(list_begin) || !v3_heap_ptr(list_end) || list_end < list_begin ||
                (list_end - list_begin) % 0x50 != 0 || index >= (list_end - list_begin) / 0x50)
                return nullptr;
            using ItemAtFn = void *(void *viewList, uint32_t index);
            void *viewList = reinterpret_cast<void *>(dlg + 0x1268);
            const uintptr_t vvt = *reinterpret_cast<uintptr_t *>(viewList);
            if (!exe_image_ptr(vvt))
                return nullptr;
            const uintptr_t item_at = *reinterpret_cast<uintptr_t *>(vvt + 0x28);
            if (!exe_image_ptr(item_at))
                return nullptr;
            return reinterpret_cast<ItemAtFn *>(item_at)(viewList, index);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    // POD: the grid fields that map a visible slot to a data index.
    bool read_grid_window(uintptr_t dlg, int32_t *top, uint32_t *cols, int32_t *flat_base)
    {
        __try
        {
            // +0x348 first visible row, +0xD8 columns, +0xE0 flat base - the same fields the
            // engine's own slot-to-index helper reads (documented with the grid layout below).
            const uintptr_t grid = dlg + 0xA38;
            *top = *reinterpret_cast<int32_t *>(grid + 0x348);
            *cols = *reinterpret_cast<uint32_t *>(grid + 0xD8);
            *flat_base = *reinterpret_cast<int32_t *>(grid + 0xE0);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void repaint_row_icons(uintptr_t dlg)
    {
        if (!dlg)
            return;
        int32_t top = 0, flat_base = 0;
        uint32_t cols = 1;
        if (!read_grid_window(dlg, &top, &cols, &flat_base))
            return;
        if (!cols)
            cols = 1;
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        for (int slot = 0; slot < kGridSlots; ++slot)
        {
            // index = (slotRow + firstVisible) * columns + slotCol + flatBase, the engine's own
            // slot-to-data mapping (FUN_140739F70); our rows are always column 0.
            const int32_t index = (slot + top) * static_cast<int32_t>(cols) + flat_base;
            if (index < 0)
                continue;
            void *item = view_item_at(dlg, static_cast<uint32_t>(index));
            const goblin::nmenu::Row *row =
                item ? model_row_of(reinterpret_cast<uintptr_t>(item), nullptr) : nullptr;
            if (!row)
                continue;
            draw_row_icon(base, slot, row->ini_key);
            draw_row_slider(base, slot, row);
        }
    }

    void repaint_row_icons_after_advance(uintptr_t dlg)
    {
        if (!menu_open() || dlg != g_form_dialog.load(std::memory_order_acquire))
            return;
        repaint_row_icons(dlg);
    }

    void refresh_form_view(uintptr_t dlg)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        set_form_captions(base, dlg, goblin::nmenu::page_title(), nullptr);
        __try
        {
            using RebuildFn = void(void *viewList, void *mgr);
            using RefreshFn = void(void *dlg);
            // The HOOKED builder, so our rows are the ones rebuilt.
            reinterpret_cast<RebuildFn *>(goblin::anchors::at(goblin::AnchorId::menu_row_build_dispatch))(reinterpret_cast<void *>(dlg + 0x1268),
                                                          reinterpret_cast<void *>(dlg + 0x1290));
            reinterpret_cast<RefreshFn *>(goblin::anchors::at(goblin::AnchorId::menu_view_refresh))(reinterpret_cast<void *>(dlg));
            // The refresh leaves the grid's item count at the PREVIOUS page's value, so the
            // list is only walkable up to that many items. Restate it from what the rebuild
            // actually landed - after the refresh, never before: writing it first is what let
            // the cursor run past the real end (rows there are never rendered, so our icon
            // strips fell back to their empty cell and the pictures vanished).
            sync_grid_extent(dlg, g_form_landed.load(std::memory_order_acquire));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            spdlog::warn("[form] SEH refreshing rows after a row action");
        }
    }

    // ── the list position belongs to the PAGE, not to the screen ─────────────────────
    // The grid keeps one cursor for the whole screen, so entering a short sub-page from a
    // scrolled-down long one left the view parked below its last row - the player had to
    // scroll up to see anything. The cursor is a plain field (grid+0xD4, which the game's own
    // reader FUN_140739E20 just returns), so each level's position is remembered on the way
    // down and put back on the way out.
    // GridControl layout, established by disassembling its own helpers:
    //   +0xD0 item COUNT   - FUN_14073A0A0 takes (count - 1) as the highest index
    //   +0xD4 cursor       - a FLAT index over all items (FUN_140739E20 returns it)
    //   +0xD8 columns      - FUN_140739F10 splits a flat index by it
    //   +0xDC visible rows - FUN_14073A1C0 returns rows * columns = page capacity
    // Writing the cursor alone did NOT move the view, and the count is why: the game never
    // needs to change it (a key-binding list has a fixed length), so nothing refreshes it when
    // we swap the rows for a shorter page - the grid keeps paging a list that is no longer
    // there. Both fields are set together.
    constexpr size_t kGridCountOff = 0xD0;
    constexpr size_t kGridCursorOff = 0xD4;
    // THE SCROLL WINDOW, found by following how a slot becomes a data index. FUN_140736F20
    // gets the cursor's row clip as `slot = cursor - FUN_1407374A0(grid, 0)`, and that resolves
    // through FUN_140739F70, which computes
    //     index = (slotRow + *(int*)(grid + 0x348)) * columns + slotCol + *(int*)(grid + 0xE0)
    // So +0x348 is the FIRST VISIBLE ROW and +0xE0 a flat base offset. Neither the cursor nor
    // the item count takes part - which is exactly why setting those two moved nothing.
    constexpr size_t kGridTopRowOff = 0x348;
    // TOTAL ROWS, and the field the clamp actually consults. Found by scanning the grid for a
    // stale value: with 86 items in the list this still read 15 - the row count of the page we
    // replaced - which is why the list stopped at 15 rows no matter what was written to +0xD0.
    // That one is only a mirror. The pair {first visible @+0x348, total @+0x34C} sits together.
    constexpr size_t kGridTotalRowsOff = 0x34C;
    constexpr size_t kGridBaseOff = 0xE0;

    // State the extent of the list we just built. Called AFTER the game's own refresh, which
    // leaves both fields describing the previous page.
    void sync_grid_extent(uintptr_t dlg, uint32_t items)
    {
        if (!items)
            return;
        __try
        {
            const uintptr_t grid = dlg + 0xA38;
            uint32_t cols = *reinterpret_cast<uint32_t *>(grid + 0xD8);
            if (cols == 0)
                cols = 1;
            const uint32_t rows = (items + cols - 1) / cols;
            *reinterpret_cast<uint32_t *>(grid + kGridCountOff) = items;
            *reinterpret_cast<uint32_t *>(grid + kGridTotalRowsOff) = rows;
            // A page that shrank can leave the window past its end, where nothing renders.
            const uint32_t visible = *reinterpret_cast<uint32_t *>(grid + 0xDC);
            int32_t *top = reinterpret_cast<int32_t *>(grid + kGridTopRowOff);
            const int32_t max_top =
                rows > visible ? static_cast<int32_t>(rows - visible) : 0;
            if (*top > max_top)
                *top = max_top;
            uint32_t *cur = reinterpret_cast<uint32_t *>(grid + kGridCursorOff);
            if (*cur >= items)
                *cur = items ? items - 1 : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    ListPos read_list_pos(uintptr_t dlg)
    {
        ListPos p{0, 0};
        __try
        {
            const uintptr_t grid = dlg + 0xA38;
            p.cursor = *reinterpret_cast<uint32_t *>(grid + kGridCursorOff);
            p.top_row = *reinterpret_cast<int32_t *>(grid + kGridTopRowOff);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return p;
    }

    void write_list_pos(uintptr_t dlg, ListPos p)
    {
        __try
        {
            const uintptr_t grid = dlg + 0xA38;
            const uint32_t count = *reinterpret_cast<uint32_t *>(grid + kGridCountOff);
            if (count && p.cursor >= count)
                p.cursor = count - 1;
            if (p.top_row < 0)
                p.top_row = 0;
            *reinterpret_cast<uint32_t *>(grid + kGridCursorOff) = p.cursor;
            *reinterpret_cast<int32_t *>(grid + kGridTopRowOff) = p.top_row;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    // With one screen per page the list position needs no stack of its own: each screen keeps its
    // own grid, and a resuming parent is restored from the entry that describes it.

    // ── The screen's own Back button walks OUR page stack ────────────────────────────
    // The window dispatches input by walking its command vector and running the first
    // entry whose trigger accepts the input and whose predicate allows it. Disassembled
    // at 0x140745662, the action call is `MOV RCX,[entry+0xF8]` then `CALL [RAX+0x10]` -
    // ONE argument, and the return value is dropped. So taking Back over is a single
    // pointer write: swap the action holder's impl for one of ours that pops a page and
    // only delegates to the original once we are back at the root.
    //
    // Which entry is Back comes from the game, not from a guess: every trigger is a
    // _Func_impl built by FUN_14075E8B0, which stores {u32 actionId, u8 flags} right
    // after the vptr, and the factory that carries the "Close" caption (GR_KeyGuide
    // 110001, the twin of 110000 "Back") registers action id 0x18. Both the vtable and
    // the id are checked, so an entry of any other shape is skipped rather than misread.
    constexpr uint32_t kInputActionBack = 0x18;
    constexpr uintptr_t kInputTriggerVtable = 0x2A99768;
    constexpr size_t kTriggerActionIdOff = 8;
    constexpr size_t kCmdTriggerOff = 0x38;
    constexpr size_t kCmdActionOff = 0xF8;

    // POD-only: the action id behind one command entry, or false if this is not a trigger
    // of the shape we know.
    bool command_action_id(uintptr_t base, uintptr_t entry, uint32_t *out)
    {
        __try
        {
            const uintptr_t impl = *reinterpret_cast<uintptr_t *>(entry + kCmdTriggerOff);
            if (!impl)
                return false;
            if (*reinterpret_cast<uintptr_t *>(impl) != base + kInputTriggerVtable)
                return false;
            *out = *reinterpret_cast<uint32_t *>(impl + kTriggerActionIdOff);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // ── which input action does ESC actually produce? ────────────────────────────────────────────
    // The command table proved that our screen HAS a command accepting two actions (0x25 or 0x35,
    // guard allows) besides Back (0x18), so if ESC mapped to one of those the engine would already be
    // closing us. It does not - therefore either ESC produces some other action id, or nothing at all
    // reaches the screen. exe+0x758500 is the engine's own "is this action present in this input?"
    // predicate, called by every matcher lambda with (InputData*, actionId, capturedState*). Logging
    // what it is ASKED and what it ANSWERS while ESC is physically down settles it.
    //
    // This is a HOT path, so the detour is inert unless armed: one of our screens must be open, the
    // ESC key must have gone down within the last second, and at most kActionLogCap lines are printed
    // per arming. Everything else is a straight tail-call to the original.
    using ActionTestFn = char(void *input, uint32_t action, void *state);
    ActionTestFn *o_action_test = nullptr;
    std::atomic<uint64_t> g_action_log_until{0};
    std::atomic<int> g_action_log_left{0};
    constexpr int kActionLogCap = 60;

    // The search page is typing (cursor on its text row): set by poll_search every tick. While it
    // is, Backspace and Q are text keys, not "one step back" - the predicate below answers "no
    // Back here" for them. Escape keeps closing the screen, so nobody is trapped on the page.
    std::atomic<bool> g_search_typing{false};

    // While typing, a press of a TYPEABLE key (letters, digits, space, punctuation, Backspace) is
    // text, whatever action the game maps it to - Q/Backspace = Back, E = confirm, W/S = move.
    // A navigation key (Escape, Enter, Tab, arrows) held at the same time means the player is
    // driving the menu, and every action passes. Pad input is untouched (no key is down).
    // The game snapshots its input once per frame and asks the predicate later, so a quick tap can
    // be UP again by the time the question comes - and the letter then walked the cursor or backed
    // out (the "typing suddenly stops" report, 2026-09-02). A press is therefore remembered for
    // kTypedLatchMs by whoever sees it (this predicate or the tick's key scan).
    constexpr uint64_t kTypedLatchMs = 250;
    std::atomic<uint64_t> g_last_typed_ms{0};

    bool typed_key_held()
    {
        static const int nav[] = {VK_ESCAPE, VK_RETURN, VK_TAB, VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT};
        for (int vk : nav)
            if (GetAsyncKeyState(vk) & 0x8000) return false;
        const uint64_t now = GetTickCount64();
        bool down = (GetAsyncKeyState(VK_BACK) & 0x8000) != 0;
        for (int vk = VK_SPACE; !down && vk <= VK_OEM_102; ++vk)
            if (goblin::overlay::text_key(vk) && (GetAsyncKeyState(vk) & 0x8000)) down = true;
        if (down)
        {
            g_last_typed_ms.store(now, std::memory_order_relaxed);
            return true;
        }
        return now - g_last_typed_ms.load(std::memory_order_relaxed) <= kTypedLatchMs;
    }
    std::atomic<int> g_typing_log_left{0}; // diagnostic: a few predicate queries per typing session

    char action_test_detour(void *input, uint32_t action, void *state)
    {
        if (g_search_typing.load(std::memory_order_relaxed) && typed_key_held())
        {
            const char orig = o_action_test ? o_action_test(input, action, state) : 0;
            if (orig && g_typing_log_left.fetch_sub(1, std::memory_order_relaxed) > 0)
                spdlog::info("[action] typing: action 0x{:X} would fire on a typed key - denied", action);
            return 0;
        }
        const char r = o_action_test ? o_action_test(input, action, state) : 0;
        if (g_action_log_until.load(std::memory_order_relaxed) &&
            GetTickCount64() <= g_action_log_until.load(std::memory_order_relaxed) &&
            g_action_log_left.fetch_sub(1, std::memory_order_relaxed) > 0)
            spdlog::info("[action] asked 0x{:X} -> {}", action, r ? "YES" : "no");
        return r;
    }

    // Called from the per-frame upkeep: arms the log for one second on an ESC down-edge.
    void arm_action_log_on_escape()
    {
        static bool was_down = false;
        const bool down = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        if (down && !was_down && goblin::config::debugLogging)
        {
            g_action_log_left.store(kActionLogCap, std::memory_order_relaxed);
            g_action_log_until.store(GetTickCount64() + 1000, std::memory_order_relaxed);
            spdlog::info("[action] ESC went down - logging the next {} action queries", kActionLogCap);
        }
        was_down = down;
    }

    // ── what the screen's command table actually contains ────────────────────────────────────────
    // The dispatcher (0x140745623..0x14074567A) runs a command only when its MATCHER (+0x38) accepts
    // the current input AND its GUARD (+0x138) allows it, then calls the ACTION (+0xF8). Our
    // command_action_id() only recognises ONE matcher lambda (vtable exe+0x2A99768,
    // std::function<bool(const CS::InputData&)>), so any entry built by a different lambda has been
    // invisible to us - which is why we only ever found Back (0x18).
    //
    // The player's report reframes the problem: Q is "one step back", ESC is "close all the way to
    // gameplay", and every native screen closes on ESC - including the very screen we borrow. So the
    // ESC entry should be IN this table; either its matcher is a kind we do not recognise, or its
    // guard refuses in our context. This dump answers which, by listing every entry with its matcher
    // vtable (nameable offline with scratch/rtti_map.py), its guard's verdict and its label.
    bool command_guard_ok(uintptr_t entry, bool *known)
    {
        *known = false;
        __try
        {
            const uintptr_t guard = *reinterpret_cast<uintptr_t *>(entry + 0x138);
            if (!guard)
                return false;
            auto fn = *reinterpret_cast<char (**)(void *)>(*reinterpret_cast<uintptr_t *>(guard) + 0x10);
            if (!fn)
                return false;
            *known = true;
            return fn(reinterpret_cast<void *>(guard)) != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // A SECOND matcher shape, decoded from its _Do_call at exe+0x75D2F0 (vtable exe+0x2A997D8): it
    // captures TWO action ids, at +0x8 and +0xC, and matches if EITHER is present in the InputData
    //     edx = [self+8];  if (test(input, edx, self+0x10)) return true;
    //     edx = [self+0xC]; return test(input, edx, self+0x10);
    // That is the shape a "Q or Escape" command would have, and command_action_id() cannot read it
    // because it insists on the single-id lambda's vtable. Returns the count of ids read (0, 1 or 2).
    constexpr uintptr_t kInputTrigger2Vtable = 0x2A997D8;

    int command_action_ids(uintptr_t base, uintptr_t entry, uint32_t *a, uint32_t *b)
    {
        __try
        {
            const uintptr_t impl = *reinterpret_cast<uintptr_t *>(entry + kCmdTriggerOff);
            if (!impl)
                return 0;
            const uintptr_t vt = *reinterpret_cast<uintptr_t *>(impl);
            if (vt == base + kInputTriggerVtable)
            {
                *a = *reinterpret_cast<uint32_t *>(impl + 8);
                return 1;
            }
            if (vt == base + kInputTrigger2Vtable)
            {
                *a = *reinterpret_cast<uint32_t *>(impl + 8);
                *b = *reinterpret_cast<uint32_t *>(impl + 0xC);
                return 2;
            }
            return 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    // POD-only, so it may hold the __try: MSVC refuses __try in a function that also needs object
    // unwinding (C2712), and the logging below unavoidably builds temporaries.
    unsigned long long command_matcher_vt(uintptr_t entry, uintptr_t base)
    {
        __try
        {
            const uintptr_t m = *reinterpret_cast<uintptr_t *>(entry + kCmdTriggerOff);
            if (!m)
                return 0;
            const uintptr_t v = *reinterpret_cast<uintptr_t *>(m);
            if (v > base && v - base < 0x10000000)
                return (unsigned long long)(v - base);
            return 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    void log_form_command_table(uintptr_t dlg)
    {
        if (!goblin::config::debugLogging || !dlg)
            return;
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        uintptr_t first = 0, last = 0;
        const size_t n = count_form_commands(dlg, &first, &last);
        spdlog::info("[cmdtable] screen 0x{:X} has {} command(s)", dlg, n);
        for (size_t i = 0; i < n && i < 32; ++i)
        {
            const uintptr_t entry = first + i * 0x140;
            uint32_t id = 0, id2 = 0;
            const int nids = command_action_ids(base, entry, &id, &id2);
            const unsigned long long mvt = command_matcher_vt(entry, base);
            bool known = false;
            const bool ok = command_guard_ok(entry, &known);
            wchar_t name[64] = {};
            form_command_name(base, entry, name, 64);
            char utf8[128] = {};
            WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof(utf8), nullptr, nullptr);
            char idbuf[24];
            if (nids == 2)
                _snprintf_s(idbuf, sizeof(idbuf), _TRUNCATE, "0x%X or 0x%X", id, id2);
            else if (nids == 1)
                _snprintf_s(idbuf, sizeof(idbuf), _TRUNCATE, "0x%X", id);
            else
                _snprintf_s(idbuf, sizeof(idbuf), _TRUNCATE, "?");
            spdlog::info("[cmdtable]   [{}] matcher vt exe+0x{:X} actionId={} guard={} label='{}'", i,
                         mvt, idbuf, known ? (ok ? "allows" : "REFUSES") : "unreadable", utf8);
        }
    }

    // The Back button is NOT hooked any more. With one real screen per page the engine's own
    // cancel does exactly the right thing (close this screen, hand control to the one beneath),
    // so the action-pointer swap that used to walk our own page stack - and the GetAsyncKeyState
    // trick that tried to tell Esc from Back - are both gone. What is left of that work is
    // invoke_cancel() below, which RUNS the engine's cancel when a row has finished its job.

    // Close a screen of ours the way its own Back button does: run the CANCEL command the window
    // registered (action id 0x18). The dispatcher calls it with one argument and drops the result,
    // so calling it directly is what the player pressing Q does - and it leaves the whole
    // close/unwind path to the engine.
    // Deferred by one frame by its callers: a screen must not be torn down from inside its own
    // confirm handler.
    bool invoke_cancel(uintptr_t dlg)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        uintptr_t first = 0, last = 0;
        const size_t n = count_form_commands(dlg, &first, &last);
        for (size_t i = 0; i < n && i < 32; ++i)
        {
            const uintptr_t entry = first + i * 0x140;
            uint32_t id = 0;
            if (!command_action_id(base, entry, &id) || id != kInputActionBack)
                continue;
            __try
            {
                const uintptr_t act = *reinterpret_cast<uintptr_t *>(entry + kCmdActionOff);
                if (!act)
                    return false;
                using InvokeFn = void *(void *self, void *out);
                auto fn = *reinterpret_cast<InvokeFn **>(*reinterpret_cast<uintptr_t *>(act) + 0x10);
                if (!fn)
                    return false;
                void *out = nullptr;
                fn(reinterpret_cast<void *>(act), &out);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
        return false;
    }

    // A screen asked to be closed from its own handler; done on the next tick.
    std::atomic<uintptr_t> g_close_request{0};
    // A g_cmd_ids_logged one-shot latch lived here: reset on teardown, never read, never set true.

    void form_decide_detour(void *dlg)
    {
        const uintptr_t d = reinterpret_cast<uintptr_t>(dlg);
        const int level = menu_open() ? screen_level(d) : -1;
        if (level < 0)
        {
            o_form_decide(dlg);
            return;
        }
        // A parent taking input again means its child is gone; put the model back on its page
        // before the row under the cursor is resolved.
        if (static_cast<size_t>(level) + 1 < g_screens.size())
        {
            pop_above(static_cast<size_t>(level));
            resume_level(static_cast<size_t>(level));
        }
        // A screen that has only just appeared must not act on the press that opened it (see
        // kNewScreenInputGraceMs).
        const Screen &me = g_screens[static_cast<size_t>(level)];
        if (me.shown && GetTickCount64() - me.shown < kNewScreenInputGraceMs)
        {
            spdlog::info("[form] level {} ignored a confirm {} ms after appearing (the press that "
                         "opened it)", level, GetTickCount64() - me.shown);
            return;
        }
        // The pool this screen's items live in - stated here rather than assumed, because the row
        // lookup below rejects an item that belongs to any other level.
        g_form_depth = static_cast<size_t>(level) < kMaxNest ? static_cast<size_t>(level)
                                                             : kMaxNest - 1;
        size_t ix = 0;
        bool preview = false;
        if (!selected_model_index(d, &ix, &preview))
        {
            o_form_decide(dlg);
            return;
        }
        if (preview)
            return; // a right-column preview line is display-only
        {
            size_t count = 0;
            const goblin::nmenu::Row *rows = goblin::nmenu::rows(&count);
            spdlog::info("[form] decide row {} of {} (kind {}, key {}) on page {} (level {})", ix,
                         count, ix < count ? static_cast<int>(rows[ix].kind) : -1,
                         ix < count && rows[ix].ini_key ? rows[ix].ini_key : "-",
                         goblin::nmenu::current_page(), level);
        }
        // The model toggles / applies / runs the action and reports whether the rows changed. It
        // never navigates on its own here (screen-per-page mode): a row that leads somewhere asks
        // for a CHILD SCREEN, and a row that finishes an edit asks for this screen to close.
        // The native "assign a key" chain must never run for our rows.
        const bool changed = goblin::nmenu::activate(ix);
        const int32_t child = goblin::nmenu::take_child_page();
        if (child >= 0)
        {
            g_screens[static_cast<size_t>(level)].pos = read_list_pos(d);
            open_screen(child, d);
            return;
        }
        if (goblin::nmenu::take_close_request())
        {
            g_close_request.store(d, std::memory_order_release);
            return;
        }
        if (!changed)
            return;
        build_form_rows();
        refresh_form_view(d);
    }

    // build_cmdlist_job() stood here: it opened the game's CommandList popup with a forced
    // gamepad layout by taking PAGE_EXECUTE_READWRITE on the exe, repointing a rip-relative
    // lea at the other movie-name literal and putting it back. It had no callers anywhere in
    // src/, so a dead patcher of the game's own code was compiled into every shipped DLL - the
    // worst kind of dead weight, since that is also exactly the shape an antivirus heuristic
    // looks for. The RE it rests on (the builder stages 01_070 and then unconditionally
    // overwrites it with 02_045, so there is no runtime branch to flip) is in cmdlist_notes.md.
    // The live analogue that DOES ship is patch_form_desc_kind, which touches one descriptor
    // byte of our own screen and restores it in the same call.

    // Release one DLRefCountObj reference (Unref + vt[0] dtor at rc==1), the pattern
    // used across the job/holder machinery.
    void release_job_ref(uintptr_t base, void *obj)
    {
        if (!obj)
            return;
        auto p_unref = reinterpret_cast<int (*)(void *)>(goblin::anchors::at(goblin::AnchorId::refcount_unref));
        if (p_unref(reinterpret_cast<uint8_t *>(obj) + 8) == 1)
            (*reinterpret_cast<void (**)(void *)>(*reinterpret_cast<void **>(obj)))(obj);
    }

    // The live CS::WorldMapDialog, derived from the CS::WorldMapArea the hover hook
    // captures: wmd_update passes r8 = dialogBase + 0x27D8, so the dialog sits 0x27D8
    // below the area (same relation that gave us the map-layer field). Verified by its
    // vtable before use. WorldMapDialog shares the MenuWindow base with our settings
    // dialog (vt+0x28/0x30/0x38/0x40 are the same MenuWindow slots), so it has the same
    // child-job holder at +0xA28 - we can hang the keybinding form straight off the MAP
    // and skip the intermediate F11 menu layer.
    // host_fields() and usable_host() stood here: a POD-only read of a window's vtable and its
    // child-job holder at +0xA28, and the check built on it. They formed a closed pair with no
    // outside caller. open_screen validates a host with window_like() instead, and the +0xA28
    // route was tried and reverted on 2026-07-28 - that field is not a child holder on the
    // object we reach, which is exactly what window_like() was right to reject.

    uintptr_t map_menu_window()
    {
        void *area = goblin::maphover::map_dialog();
        if (!area)
            return 0;
        const uintptr_t want = goblin::anchors::vtable_of(kVtWorldMapDialog);
        const uintptr_t cand = reinterpret_cast<uintptr_t>(area) - 0x27D8;
        uintptr_t vt = 0;
        __try
        {
            vt = *reinterpret_cast<uintptr_t *>(cand);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
        return (want && vt == want) ? cand : 0;
    }


    // Set while the form hangs off the MAP (no F11 menu in the stack): nothing steps our
    // own job then, so the per-frame tick must live-apply and the ini must be saved when
    // the form closes (its child-job holder empties).
    // g_form_host lived here: set nowhere, cleared on teardown, compared once. Removed 2026-07-30.

    // F8 (dev): open the game's real keybinding form (movie 02_160_KeyConfiguration,
    // dialog CS::KeyConfigDialog) as a CHILD WINDOW, with our rows swapped in.
    // Host preference = the MAP itself (WorldMapDialog shares the MenuWindow base, so it
    // has the same child-job holder @+0xA28): that keeps the stack at map + form instead
    // of map + F11 menu + form. Falls back to our F11 menu when the map is not up.
    // Build path mirrors the game's own opener FUN_14094fd40: FUN_1408078f0(outRef,
    // win+0x50, modeByte) -> ref conversions FUN_1407a7e30 / FUN_1407a7b60 -> store in
    // the holder (FUN_1407a9460) with the AddRef/Unref dance, releasing every temp slot.
    // (The native opener FUN_140747450 is NOT usable: its KeyGuide input gate can only
    // pass from the game's own per-tick input dispatch - see cmdlist_gate_re3.txt.)
    // Two ways a window can host another screen (both stepped by the host's own tick):
    //   holder +0xA28 = CHILD, drawn OVER the host (what we use over the live map)
    //   holder +0x10  = SEQUENCE, the screen REPLACES the host and Back returns to it,
    //                   exactly as the game's settings page opens Key Assignments
    //                   (FUN_14094fd40 / FUN_1409411a0 both store into +0x10)
    // Store call differs too: +0xA28 takes FUN_1407a9460(holder, &old, &ref), +0x10 takes
    // FUN_1407a9250(holder, ref).
    // ── Second host: 02_042_PC_GraphicSetting, the screen with REAL widgets ──────────
    // The key-binding form gives us an unlimited scrolling LIST, but its rows are two text
    // fields - a slider or a combo box has to be faked. The graphics screen is the opposite:
    // its page clip carries 15 row slots plus a ScrollBarV, and its rows ARE the engine's own
    // Slider / ComboBox / Button / TextInput widgets.
    //
    // Opening it is the same trick as the key-binding form, with two differences found by
    // decompiling both openers side by side:
    //   * FUN_1408079E0(out, owner, flag) builds an OptionSettingDialog job through the
    //     generic builder FUN_140808630 (movie block {8, 1, L"02_042_PC_GraphicSetting"},
    //     factory FUN_140807B10), where the key-binding form builds a MenuWindow job through
    //     FUN_1407ACB00. `owner` is the SAME host + 0x50 (read off the game's own call site
    //     at 0x14094FB02).
    //   * its tail is FUN_1407418D0 = a DLRefPtr assign, so `out` already holds an owned
    //     reference - the two fd40 conversions the other opener needs are not wanted here.
    // The game itself puts this screen in the SEQUENCE slot (it replaces the settings page);
    // over the live map we want it drawn on top, so it goes in the child slot instead.
    //
    // Its rows come from populate FUN_14095A5D0, and the page it populates is built by the
    // very same page factory (FUN_14095EB90) our settings tab already borrows - with clip
    // index 0xD = "GraphicOption" instead of 3 = "CameraSetting", which is exactly why that
    // one caps out at 12 rows with no scrollbar and this one does not.
    // The F6 graphics-screen host's own state (its populate hook type and pointer, plus the armed /
    // populated / host trio) stood here. The host was removed on 2026-07-28 once it was measured in
    // game - its list caps at sixteen rows and does not scroll - so nothing assigned or read any of
    // it. See docs/research_f6_graphics_host_retired.md.
    // (A build_our_rows() forward declaration stood here for the F11 settings tab's page populate.
    //  Both it and its definition went with that prototype.)



    // The populate only runs while the page is built, so disarm as soon as the screen is up -
    // and clear the host once the holder empties, so a second press can open it again.

    // ── the byte that decides who owns the input ─────────────────────────────────────
    // Both screen openers call the same core FUN_1407ACB00(out, owner, desc, factory) and differ
    // only in the DESCRIPTOR they hand it:
    //     F11's settings (input works):  { u32 8, u8 2, L"02_040_OptionSetting" }
    //     our form       (input dead):   { u32 8, u8 1, L"02_160_KeyConfiguration" }
    // and that byte is not a movie version - FUN_1408087E0 sets it to 1 only for the trial movie.
    // It is written by a single instruction with an immediate:
    //     0x140807967  MOV byte ptr [RSP+0x34], 0x1     -> immediate at RVA 0x80796B
    // so the experiment is one byte, flipped around our own call and put straight back. The same
    // trick the CommandList work already used on a rip-relative operand.
    // Kept as an OFFSET INTO the anchored function, not as an absolute RVA: the byte lives
    // 0x7B into keyconfig_form_build, and that anchor is the thing that gets re-found on a
    // different exe build. Writing a byte at an absolute address that moved would patch a
    // random instruction. Verified: 0x80796B - 0x8078F0 = 0x7B.
    constexpr goblin::AnchorId kFormDescKindFn = goblin::AnchorId::keyconfig_form_build; // anchor: keyconfig_form_build
    constexpr uintptr_t kFormDescKindInFn = 0x7B;

    // ── the separate movie definition: re-point the job at OUR movie name ──────────────────
    // The game's builder (keyconfig_form_build) copies its 16-byte movie descriptor into the job it
    // makes: {u32 8, u8 kind, pad, wchar_t *name} at job+0x58 (the MOVUPS in FUN_1407ACB00, 2026-09-06
    // RE). The job's load step later resolves that name through CSMenuMan's name-keyed definition
    // cache and, on a miss, the file opener - which goblin_own_movie aliases onto the game's file
    // for our name. So a job that carries our name gets its own definition, and the game's own Key
    // Assignments job (same builder, untouched job) keeps the shared one. POD-only body (SEH).
    // Shape-checked before the write: the u32 8 and the game's own name must be exactly where the
    // layout says, or the job is left alone and the screen simply shares the game's definition.
    constexpr uintptr_t kJobDescOff = 0x58;
    constexpr uintptr_t kJobDescNameOff = 0x60;
    constexpr const wchar_t *kGameMovieNameW = L"02_160_KeyConfiguration";

    int repoint_job_movie(uintptr_t job)
    {
        if (!job || !goblin::own_movie::separate_movie_armed())
            return 0;
        int done = 0;
        __try
        {
            const uint32_t tag = *reinterpret_cast<const uint32_t *>(job + kJobDescOff);
            const wchar_t *name = *reinterpret_cast<const wchar_t *const *>(job + kJobDescNameOff);
            if (tag == 8 && name && wcscmp(name, kGameMovieNameW) == 0)
            {
                *reinterpret_cast<const wchar_t **>(job + kJobDescNameOff) =
                    goblin::own_movie::menu_movie_name();
                done = 1;
            }
            else
                done = -1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            done = -2;
        }
        // (The game's own 02_160 file is requested in goblin_own_movie's name_to_def detour, not
        //  here: this function has no CSMenuMan of its own, and the one the menu-update detour
        //  caches is not the object that call reads its list from - asking with it FAULTED on every
        //  open of report 42's run.)
        return done;
    }

    // Returns the previous value, or 0 if the patch could not be applied.
    uint8_t patch_form_desc_kind(uint8_t want)
    {
        const uintptr_t fn = goblin::anchors::at(kFormDescKindFn);
        if (!fn)
            return 0; // helper not located on this exe: no byte patch, no guesswork
        // The instruction itself, not just the function, must be where we think it is:
        // `C6 44 24 34 imm8` = MOV byte [rsp+0x34], imm8. Verified present at fn+0x77 on
        // 2.6.2 / 2.6.0 / 2.2.3 / 2.2.0; a build that moves it inside the function gets no
        // patch instead of a byte written into the middle of something else.
        {
            const auto *op = reinterpret_cast<const uint8_t *>(fn + kFormDescKindInFn - 4);
            bool shape_ok = false;
            __try
            {
                shape_ok = op[0] == 0xC6 && op[1] == 0x44 && op[2] == 0x24 && op[3] == 0x34;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                shape_ok = false;
            }
            if (!shape_ok)
                return 0;
        }
        const uintptr_t at = fn + kFormDescKindInFn;
        DWORD old_prot = 0;
        if (!VirtualProtect(reinterpret_cast<void *>(at), 1, PAGE_EXECUTE_READWRITE, &old_prot))
            return 0;
        uint8_t prev = 0;
        __try
        {
            prev = *reinterpret_cast<uint8_t *>(at);
            *reinterpret_cast<uint8_t *>(at) = want;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            prev = 0;
        }
        DWORD tmp = 0;
        VirtualProtect(reinterpret_cast<void *>(at), 1, old_prot, &tmp);
        return prev;
    }

    // ── OPENING A SCREEN ─────────────────────────────────────────────────────────────
    // The job goes into the host window's SEQUENCE slot (+0x10) - see the note on the screen
    // stack for why that slot and no other. modeByte (dlg+0x1260) does more than swap glyphs:
    // the ctor runs setFrameNumber(KeySetting/BG + 0x18, modeByte + 1), so
    //   mode 0 = NARROW 941px panel (leaves room for the gamepad art, which the right column
    //            then lands on top of), device pictures shown, two scrollbars
    //   mode 1 = WIDE 1298px panel, device pictures hidden, single scrollbar moved right
    // A settings list wants mode 1 (recon_02_160_structure.md), and its per-frame device-panel
    // refresh is mode-0 only, so mode 1 also keeps our rows from being second-guessed.
    constexpr uint8_t kFormMode = 1;

    // ── is this thing a WINDOW we may hand a job to? ─────────────────────────────────
    // Not every "menu" is. *(CSMenuMan+0x80) - what we had been calling the active menu - is not
    // of this family at all: in game its vtable slots read back as string data (measured
    // 0x2AB7958: +0x38 = 65704F6D21435F3A), and storing a job into its +0x10 faulted, which is
    // the "[form] SEH opening the screen" that kept F8 from opening over gameplay. Worse, the
    // failed store left that offset reading non-zero, so every later press answered "already
    // holds a screen".
    // The family marker is the shared slot-0 destructor 0x7342B0 (138 vtables carry it) plus the
    // per-window command notifier 0x745BD0 at +0x38 - both present on WorldMapDialog and
    // KeyConfigDialog, the two windows we know host jobs correctly.
    bool window_like(uintptr_t win)
    {
        if (!v3_heap_ptr(win))
            return false;
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        __try
        {
            const uintptr_t vt = *reinterpret_cast<uintptr_t *>(win);
            if (!v3_heap_ptr(vt) && vt < base)
                return false;
            if (*reinterpret_cast<uintptr_t *>(vt) != goblin::anchors::at(goblin::AnchorId::trigger_vt_slot0_fn))
                return false;
            if (*reinterpret_cast<uintptr_t *>(vt + 0x38) != goblin::anchors::at(goblin::AnchorId::trigger_vt_slot7_fn))
                return false;
            // The sequence holder must read as a DLRefPtr: empty, or something on the heap.
            const uint64_t held = *reinterpret_cast<uint64_t *>(win + 0x10);
            return held == 0 || v3_heap_ptr(static_cast<uintptr_t>(held));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Read-only diagnostic, logged once per open that finds no window: which pointers inside
    // CSMenuMan DO look like windows. The answer is what a root screen can be hosted on when the
    // map is closed, and it is cheaper to read it out of the live game than to guess.
    void log_window_candidates(uintptr_t menuman)
    {
        if (!menuman)
            return;
        int found = 0;
        std::string list;
        for (size_t off = 0; off < 0x400; off += 8)
        {
            uint64_t raw = 0;
            if (!v3_read64(menuman + off, raw)) // POD reader: SEH cannot live in this function
                break;
            const uintptr_t p = static_cast<uintptr_t>(raw);
            if (!window_like(p))
                continue;
            char buf[64];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, " +0x%zX=0x%llX", off,
                        static_cast<unsigned long long>(p));
            list += buf;
            if (++found >= 12)
                break;
        }
        spdlog::info("[form] window-like pointers in CSMenuMan 0x{:X}:{}", menuman,
                     found ? list : std::string(" none"));
    }

    // A menu open that arrived while the map's own job chain still held the sequence slot. Retried
    // from tick_native_menu() until the slot frees; -1 = nothing waiting. Map UI thread only.
    int32_t g_pending_open_page = -1;
    uint64_t g_pending_open_since = 0;
    // Long enough for the Deck's slowest measured occupancy (about 12 s from the map opening, and
    // the chains that follow our own screen closing), short enough that a key press cannot appear
    // to act half a minute later.
    constexpr uint64_t kPendingOpenTimeoutMs = 15000;

    void open_screen(int32_t page, uintptr_t nest_parent)
    {
        if (!nest_parent && menu_open())
        {
            spdlog::info("[form] ignored: the menu is already open ({} screen(s))",
                         g_screens.size());
            return;
        }
        if (g_screens.size() >= kMaxNest)
        {
            spdlog::info("[form] ignored: {} screens deep already", g_screens.size());
            return;
        }
        if (Screen *pending = top_screen())
            if (!pending->dlg)
            {
                spdlog::info("[form] ignored: the screen opened last has not come up yet");
                return;
            }

        // Where does it hang? A sub-page always goes into the page above it; a root screen onto
        // the window that owns the screen the player is looking at.
        uintptr_t host = nest_parent;
        const char *what = "the page above it";
        if (!host)
        {
            host = map_menu_window();
            if (host)
                what = "the map";
        }
        // (A third fallback tried g_settings_dialog here - the F11 prototype's own dialog. It was
        //  never constructed, so this branch could not be taken; removed with that prototype.)
        if (host && !window_like(host))
        {
            spdlog::info("[form] host 0x{:X} ({}) is not a window - not storing a job in it", host,
                         what);
            host = 0;
        }
        // TRIED AND REVERTED 2026-07-28: hanging our screen off the HUD menu's +0xA28 holder (the
        // structural theory - we displace the HUD screen instead of parenting to it). The holder read as
        // empty and accepted the job, but the screen never came up over gameplay, so that field is not a
        // child holder on this object; window_like() was right to reject it. The HUD defect therefore is
        // NOT about which host we choose, and the next step is to find the engine's own HUD-visibility
        // code rather than to keep trying placements.
        // No window - plain gameplay. PUSH the job onto the active menu instead of storing it in a
        // slot (that needs descriptor byte 2 to receive input at all). window_like() is NOT
        // required here, and the crash that looked like it proved otherwise was ours: the dialog's
        // update hook had the wrong arity, so the game read a byte through a register we had
        // clobbered. Pushing onto this object is what worked before, on this very vtable
        // (exe+0x2AB7958 - it is a menu, not a window, and only the SLOT paths need a window).
        bool pushed = false;
        if (!host && p_refmove && p_push_job)
        {
            host = g_active_menu.load(std::memory_order_acquire);
            if (host)
            {
                pushed = true;
                what = "the active menu (pushed)";
                // Capture the HUD mode BEFORE we disturb anything; hud_mode_restore() puts it back
                // once our screens are gone. See the note on hud_mode_capture.
                hud_mode_capture();
                // Over gameplay this menu IS the HUD menu; snapshot it so the close-side compare can
                // name whatever keeps the HUD hidden afterwards.
                hud_snapshot_take(host);
            }
        }
        if (!host)
        {
            spdlog::info("[form] ignored: there is nothing to host the screen");
            log_window_candidates(g_menuman.load(std::memory_order_acquire));
            return;
        }
        // TRIED AND REVERTED 2026-08-02: forcing the push route here for every open over the map,
        // to keep our screen out of the map's own menu sequence (the sequence whose teardown the
        // crash below happens in). It made the menu STOP OPENING over the map entirely - the Deck
        // logged "SEH opening the screen" on all eleven opens of the run. The premise was wrong:
        // pushing is NOT a working alternative over the map. Re-reading the crash run says so
        // plainly - of its two pushes, one threw the same SEH and the other logged "pushed onto"
        // and then "level 0 never came up - dropped" 3 s later. The ONLY route that has ever
        // brought the screen up over the map is the slot store below. So the slot store stays,
        // and the crash is dealt with where it actually happens: on the way out (see the slot
        // hand-back in close_screen / the map dtor).
        // Is the slot free? Asked with the engine's OWN test - the same one its input gate uses,
        // so "the host holds a screen" and "the host is not reading input" can never disagree.
        // A pushed job is in no slot, so there is nothing to ask.
        if (!pushed && !seq_slot_empty(host))
        {
            // A TAKEN SLOT IS NOT A REFUSAL, AND IT IS NOT A REASON TO PUSH EITHER. What sits in
            // the map window's slot is the engine's own CS::FixOrderJobSequence (vt exe+0x2AA8D78,
            // RTTI-confirmed) - a legitimate occupant, put there by the map's own open/close job
            // chains. It clears by itself, and the ONLY thing that differs between the two
            // machines is how long that takes: measured across every log we have, the PC's slot
            // was empty on all 7 opens over the map, while the Deck's was busy on 37 of 54 - for
            // SECONDS at a time (the same occupant answered five presses across 2.9 s), which is
            // why no frame-rate story explains it and why every Deck-only symptom traces here.
            //
            // Pushing was the old answer to a busy slot and it is DEAD: exe+0x1EBA1C0 is
            // DLReferenceCountObject::AddRef, three instructions, and the fault is its LOCK XADD
            // at +5. The push functions (FUN_1407edfa0/FUN_1407f0b50) are CS::CSPopupMenu methods
            // whose first host access dereferences *(host+0xB0) as a job pointer. Over gameplay
            // the host IS a CSPopupMenu and it works; the map host is a CS::WorldMapDialog, a
            // different class with a different layout, so the push is a this-pointer type
            // confusion that cannot be fixed by any argument or offset. It scored 0 successes in
            // 36 attempts across the logs. (It also invalidated the note that used to stand here
            // claiming "the dialog's push vector +0xD0 reads count=0": execution never reaches
            // +0xD0, and that reading was taken on the wrong class.)
            //
            // So: WAIT for the slot instead. The request is remembered and tick_native_menu()
            // retries it once the engine's chain has finished, which is the same route the PC
            // takes on the first try - both machines end up in the identical code path, one just
            // waits a beat.
            log_job_stack(host, "the slot host that is still busy");
            if (g_pending_open_page >= 0)
            {
                // Pressed again while we were waiting. The key is a TOGGLE, so the second press
                // means "never mind" - otherwise the screen would spring open by itself seconds
                // after the player had given up on it.
                spdlog::info("[form] the wait for the map's slot was cancelled by a second press");
                g_pending_open_page = -1;
                g_pending_open_since = 0;
                return;
            }
            g_pending_open_since = GetTickCount64();
            g_pending_open_page = page;
            spdlog::info("[form] host 0x{:X} ({}) is still running its own job chain - waiting for "
                         "the slot instead of forcing the screen in", host, what);
            return;  // nothing pushed onto g_screens yet - that happens further down
        }
        spdlog::info("[form] opening page {} on {} (0x{:X})", page, what, host);

        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        // (No movie-interception arming here. The movie this screen runs on was parsed - and
        //  transformed - during the startup preload, long before any open; see goblin_own_movie.hpp.)
        g_own_icon_state.store(0, std::memory_order_release);
        g_icon_diag_done.store(false, std::memory_order_release);
        g_icon_host_logged.store(false, std::memory_order_release);
        // The model serves one page per screen from here on and never navigates by itself.
        goblin::nmenu::set_nested(true);

        Screen s{};
        s.page = page;
        s.host = host;
        s.opened = GetTickCount64();
        g_screens.push_back(s);
        // The root open takes the baseline for the live-apply diff.
        if (g_screens.size() == 1)
            menu_cfg_snapshot_take();

        bool ok = false;
        __try
        {
            void *slotA = nullptr, *slotB = nullptr, *slotC = nullptr;
            auto p_keycfg =
                reinterpret_cast<void *(*)(void **, void *, uint8_t)>(goblin::anchors::at(goblin::AnchorId::keyconfig_form_build));
            auto p_conv1 = reinterpret_cast<void *(*)(void *, void **)>(goblin::anchors::at(goblin::AnchorId::job_ref_convert_a));
            auto p_conv2 = reinterpret_cast<void *(*)(void *, void **)>(goblin::anchors::at(goblin::AnchorId::job_ref_convert_b));
            // OWNER: win+0x50, what the game's own key-binding opener passes; the pushed path
            // uses the menu's own +0x10 the way F11's settings screen does.
            void *owner = reinterpret_cast<void *>(host + (pushed ? 0x10 : 0x50));
            // THE byte that decides who owns the input, but only for a pushed screen: parked in
            // a menu's job stack our screen rendered and received nothing until the descriptor
            // said 2 (what 02_040 uses). A screen in a window's sequence slot takes input with
            // the 1 the game itself uses for this movie.
            const uint8_t prev_kind = pushed ? patch_form_desc_kind(2) : 0;
            void *r = p_keycfg(&slotA, owner, kFormMode);
            if (prev_kind)
                patch_form_desc_kind(prev_kind); // restore at once - one call, one patch
            // Our own movie definition for this screen (see repoint_job_movie). Logged once per
            // outcome so a layout change shows up as "-1" rather than as a silently shared movie.
            {
                const int rp = repoint_job_movie(reinterpret_cast<uintptr_t>(slotA));
                static std::atomic<int> s_logged{0};
                if (rp != 0 && s_logged.fetch_add(1) < 3)
                    spdlog::info("[form] job movie name: {} (job 0x{:X})",
                                 rp == 1 ? "re-pointed at our own definition"
                                 : rp == -1 ? "descriptor shape unexpected - sharing the game's"
                                            : "unreadable - sharing the game's",
                                 reinterpret_cast<uintptr_t>(slotA));
            }
            r = p_conv1(r, &slotB);
            r = p_conv2(r, &slotC);
            if (!slotC)
            {
                spdlog::warn("[form] no job built (slotA=0x{:X} slotB=0x{:X})",
                             reinterpret_cast<uintptr_t>(slotA),
                             reinterpret_cast<uintptr_t>(slotB));
                release_job_ref(base, slotB);
                release_job_ref(base, slotA);
            }
            else
            {
                // The sequence slot MOVES the reference out of the DLRefPtr it is handed (the
                // conversion chain returns the address of the last slot), so slotC reads back as
                // 0 afterwards - which is also why the release below is a no-op rather than a
                // double free. Record the job BEFORE the store, or the log shows 0x0.
                auto p_addref = reinterpret_cast<void (*)(void *)>(goblin::anchors::at(goblin::AnchorId::refcount_addref));
                g_screens.back().job = reinterpret_cast<uintptr_t>(slotC);
                if (pushed)
                {
                    void *out = nullptr, *ctx = nullptr, *jobCopy = nullptr;
                    uint8_t tmp[64] = {};
                    void *job = p_refmove(r, &jobCopy);
                    p_push_job(reinterpret_cast<void *>(host), &out, &ctx, job, tmp);
                    // Third sample of the pair taken by hud_snapshot_take/compare. The push targets
                    // the +0xD0 vector, so if our job is in +0x120 by the very next instruction the
                    // engine moved it there synchronously; if +0xD0 holds it here and +0x120 gets it
                    // later, the move happens on a menu tick. Both readings change where the missing
                    // cleanup belongs, and one line of log separates them.
                    if (goblin::config::debugLogging)
                    {
                        spdlog::info("[jobstack] our job is 0x{:X} (screen 0x{:X}, pushed onto 0x{:X})",
                                     reinterpret_cast<uintptr_t>(job),
                                     reinterpret_cast<uintptr_t>(slotC), host);
                        log_job_stack(host, "immediately after our push");
                    }
                }
                else
                {
                    // The slot setter is SELF-BALANCED - decompiled 2026-08-01: 0x7A9250 addrefs the
                    // object into the holder, then unrefs *src and nulls it. So it needs no help
                    // from us, and the extra addref that used to stand here belonged to nobody: it
                    // left the object at one reference above zero forever, so the engine's own
                    // teardown unref never reached the destructor and one job leaked per open.
                    // That is the same shape of imbalance that produced the Deck crash from
                    // hud_snapshot_take - an unowned +1 on an engine-owned object - and it is worth
                    // fixing even though this branch is rarely taken now (over the map the slot is
                    // usually held by the map's own job sequence, so we push instead).
                    auto p_seq = reinterpret_cast<void (*)(void *, void *)>(goblin::anchors::at(goblin::AnchorId::job_holder_store_seq));
                    p_seq(reinterpret_cast<void *>(host + 0x10), r);
                }
                release_job_ref(base, slotC);
                release_job_ref(base, slotB);
                release_job_ref(base, slotA);
                ok = true;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            spdlog::warn("[form] SEH opening the screen");
        }
        if (!ok)
        {
            g_screens.pop_back();
            if (g_screens.empty())
                goblin::nmenu::set_nested(false);
            return;
        }
        g_form_title_pending.store(true, std::memory_order_release);
        spdlog::info("[form] level {} job 0x{:X} {} 0x{:X}", g_screens.size() - 1,
                     g_screens.back().job,
                     pushed ? "pushed onto" : "placed in the sequence slot of", host);
    }

    // Bring a level back to life after the screen above it went away. THE MODEL ONLY: point it
    // at this screen's page and refill this level's pool, and touch nothing that belongs to the
    // engine.
    // The first version also ran the game's own rebuild+refresh on the resuming dialog (and wrote
    // its grid fields). In game the parent then died about a second later - its update stopped,
    // its memory read back as garbage - and the whole menu tore down. The child's teardown is not
    // finished at the moment its slot reports empty, so refreshing the parent right then reaches
    // into a window that is still unwinding. It is not needed either: the engine refreshes the
    // resuming screen by itself (measured - a row build for the parent arrives ~1 s later), and
    // that build lands in our hook with the right page and pool already set.
    void resume_level(size_t level)
    {
        if (level >= g_screens.size())
            return;
        Screen &s = g_screens[level];
        if (!s.dlg)
            return;
        g_form_depth = level < kMaxNest ? level : kMaxNest - 1;
        g_form_dialog.store(s.dlg, std::memory_order_release);
        g_form_update_watch.store(s.dlg, std::memory_order_release);
        goblin::nmenu::set_page(s.page);
        build_form_rows();
        // Normally the page is already back by now - reveal_page_under_closing_top() brings it
        // back as soon as the screen over it starts closing, so there is no frame with neither of
        // them drawn. This is the fallback for the paths that skip that (a screen that died
        // without closing, a level dropped by the heartbeat).
        if (s.hidden)
        {
            s.hidden = false;
            set_screen_visible(s.dlg, true);
        }
        spdlog::info("[form] level {} resumed (page {}) - model only, the engine redraws it",
                     level, s.page);
    }

    void close_screen(uintptr_t dlg)
    {
        if (!invoke_cancel(dlg))
        {
            spdlog::warn("[form] screen 0x{:X} has no cancel command to close it with", dlg);
            return;
        }
        // NO HUD restore here, deliberately. invoke_cancel() reports success as soon as it FINDS and
        // calls the Back command's action, but the engine only ever runs a command when the command's
        // own matcher (+0x38) matches the input AND its guard (+0x138) allows it - both of which we
        // skip. Measured 2026-07-28: ESC logged a successful cancel, the screen stayed up, and the
        // only visible effect was the HUD coming back OVER a still-open menu. So the restore stays on
        // job_finished(), which cannot be true unless the screen really is going away.
    }
    // Per-frame upkeep for a MAP-hosted form: live-apply toggles, and when the form is
    // gone (the map's child-job holder emptied) persist the ini and disarm the row swap.
    // ── "press a key" capture for the rebind page ────────────────────────────────────
    // The screen's own input goes through the window's command vector, which knows nothing
    // about arbitrary keys, so the capture is done here - the same way the overlay does it.
    // The page is entered BY a key press, so nothing is read until every key is up again;
    // otherwise the confirm key would instantly rebind the entry to itself.
    bool g_rebind_armed = false;
    uint16_t g_pad_peak = 0; // the widest set of buttons held during THIS capture

    // A combo is captured on RELEASE, not on press: the player pressing Y+R3 goes through a frame
    // where only Y is down, and taking the first non-empty read would bind Y alone. So accumulate
    // while anything is held and commit the peak once everything is up again.
    // The state comes from overlay::gamepad_buttons, NOT from XInputGetState: we hook that
    // function, so a direct call reads back the buttons we inject ourselves and, while our own
    // menu is open, reports no pad at all - this page would have captured nothing.
    bool poll_rebind_pad()
    {
        const uint16_t held = goblin::overlay::gamepad_buttons();
        if (!g_rebind_armed)
        {
            if (!held)
                g_rebind_armed = true; // the press that opened this page has been let go
            return false;
        }
        if (held)
        {
            g_pad_peak |= held;
            return false;
        }
        if (!g_pad_peak)
            return false;
        const uint16_t captured = g_pad_peak;
        g_pad_peak = 0;
        g_rebind_armed = false;
        goblin::nmenu::rebind_apply_pad(captured);
        return true;
    }

    void poll_rebind()
    {
        if (!goblin::nmenu::rebind_pending())
        {
            g_rebind_armed = false;
            g_pad_peak = 0;
            return;
        }
        if (goblin::nmenu::rebind_is_pad())
        {
            if (poll_rebind_pad())
            {
                const uintptr_t dlg = g_form_dialog.load(std::memory_order_acquire);
                if (dlg)
                    build_form_rows();
            }
            return;
        }
        bool any_down = false;
        int pressed = 0;
        for (int vk = 0x07; vk <= 0xFE; ++vk)
        {
            if (!(GetAsyncKeyState(vk) & 0x8000))
                continue;
            any_down = true;
            if (!pressed)
                pressed = vk;
        }
        if (!g_rebind_armed)
        {
            if (!any_down)
                g_rebind_armed = true; // everything released - now we may listen
            return;
        }
        if (!pressed)
            return;
        g_rebind_armed = false;
        // Escape means "leave it alone", which rebind_apply spells as 0.
        goblin::nmenu::rebind_apply(pressed == VK_ESCAPE ? 0u : static_cast<uint32_t>(pressed));
        const uintptr_t dlg = g_form_dialog.load(std::memory_order_acquire);
        // The MODEL is rebuilt unconditionally - it is ours and touches nothing of the engine's.
        // The engine-side refresh is not: refresh_form_view drives the game's own row rebuild on
        // this dialog, and g_form_dialog is only dropped after the heartbeat has been stale for
        // 600 ms, so without a freshness test this is the widest window we have for calling into a
        // dialog the engine may already have freed. A live screen ticks every frame, so the gate
        // is always open in practice; when it is not, the engine's next row build repaints anyway.
        if (dlg)
        {
            build_form_rows();
            const int lvl = screen_level(dlg);
            if (lvl >= 0 && safe_to_call_engine(g_screens[static_cast<size_t>(lvl)],
                                                GetTickCount64()))
                refresh_form_view(dlg);
        }
    }

    // ── typed text for the search page ───────────────────────────────────────────────
    // Same shape as the rebind capture: the screen's command vector knows nothing about
    // letters, so they are read physically, translated through this thread's (the game's)
    // keyboard layout and handed to the model. ONLY while the list cursor sits on the page's
    // text row (visual row 0 = flat cursor 0/1): there the typeable keys are muted for the game
    // through the overlay's raw-input hook (set_text_capture), so "e" types an e instead of
    // confirming; one row down they are the game's menu keys again. Held = repeat, like a
    // text field: 400 ms, then every 40 ms.
    uint64_t g_search_next[256] = {}; // per key: 0 = up, else when the held key fires again

    void poll_search(const Screen &top)
    {
        bool typing = goblin::nmenu::search_active() && !goblin::nmenu::rebind_pending();
        if (typing)
        {
            const uintptr_t dlg = g_form_dialog.load(std::memory_order_acquire);
            typing = dlg != 0 && read_list_pos(dlg).cursor < 2;
        }
        if (typing)
        {
            // Only while the GAME is the foreground window: the key reads are global, and an
            // alt-tabbed player typing elsewhere must not type in here.
            DWORD fg_pid = 0;
            if (HWND fg = GetForegroundWindow()) GetWindowThreadProcessId(fg, &fg_pid);
            typing = fg_pid == GetCurrentProcessId();
        }
        goblin::overlay::set_text_capture(typing);
        g_search_typing.store(typing, std::memory_order_relaxed); // action_test_detour: typed keys are text
        // The game's window never handles the OS layout switch (both threads stayed 0x04090409
        // after Alt+Shift, 2026-09-02), so the layout is OUR pick: the overlay module cycles it on
        // the usual hotkeys and both text feeds translate with it.
        const bool layout_switched = typing && goblin::overlay::text_layout_poll();
        const HKL layout = reinterpret_cast<HKL>(goblin::overlay::text_layout());
        static bool s_was_typing = false;
        // Armed only once every typeable key is up: the page is opened BY a key press (E), and
        // that press must not become the first letter - the same rule the rebind page uses.
        static bool s_armed = false;
        if (typing && !s_was_typing)
        {
            s_armed = false;
            g_typing_log_left.store(12, std::memory_order_relaxed);
            spdlog::info("[search] typing on (native page), layout=0x{:X}",
                         reinterpret_cast<uintptr_t>(layout));
        }
        s_was_typing = typing;
        if (!typing)
        {
            for (auto &t : g_search_next) t = 0;
            return;
        }
        if (!s_armed)
        {
            bool any = (GetAsyncKeyState(VK_BACK) & 0x8000) != 0;
            for (int vk = VK_SPACE; !any && vk <= VK_OEM_102; ++vk)
                if (goblin::overlay::text_key(vk) && (GetAsyncKeyState(vk) & 0x8000)) any = true;
            if (any) return;
            s_armed = true;
        }
        const uint64_t now = GetTickCount64();
        BYTE ks[256] = {};
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) ks[VK_SHIFT] = 0x80;
        if (GetAsyncKeyState(VK_RMENU) & 0x8000) { ks[VK_CONTROL] = 0x80; ks[VK_MENU] = 0x80; ks[VK_RMENU] = 0x80; }
        if (GetKeyState(VK_CAPITAL) & 1) ks[VK_CAPITAL] = 1;
        const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        bool changed = layout_switched; // the text row shows the layout tag - redraw it at once
        for (int vk = VK_BACK; vk <= VK_OEM_102; ++vk)
        {
            if (vk != VK_BACK && !goblin::overlay::text_key(vk)) continue;
            if (!(GetAsyncKeyState(vk) & 0x8000)) { g_search_next[vk] = 0; continue; }
            g_last_typed_ms.store(now, std::memory_order_relaxed); // the latch typed_key_held reads
            if (g_search_next[vk] == 0) g_search_next[vk] = now + 400;
            else if (now < g_search_next[vk]) continue;
            else g_search_next[vk] = now + 40;
            if (vk == VK_BACK)
            {
                changed |= goblin::nmenu::search_backspace();
                continue;
            }
            if (ctrl) continue; // a shortcut, not a character
            wchar_t out[8] = {};
            const UINT sc = MapVirtualKeyExW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC, layout);
            const int n = ToUnicodeEx(static_cast<UINT>(vk), sc, ks, out, 8, 1u << 2, layout);
            if (g_typing_log_left.fetch_sub(1, std::memory_order_relaxed) > 0)
                spdlog::info("[search] key 0x{:X} sc 0x{:X} hkl 0x{:X} -> n={} U+{:04X}", vk, sc,
                             reinterpret_cast<uintptr_t>(layout), n, n > 0 ? static_cast<unsigned>(out[0]) : 0u);
            for (int i = 0; i < n; ++i)
                if (out[i] >= 0x20)
                {
                    goblin::nmenu::search_type(out[i]);
                    changed = true;
                }
        }
        if (!changed) return;
        const uintptr_t dlg = g_form_dialog.load(std::memory_order_acquire);
        if (dlg)
        {
            // Same freshness rule as the rebind poller: the model is ours, the engine-side row
            // rebuild only while the dialog is still ticking.
            build_form_rows();
            if (safe_to_call_engine(top, GetTickCount64()))
                refresh_form_view(dlg);
        }
    }

    // ── left/right stepping for Slider rows ──────────────────────────────────────────
    // The screen's own input cannot deliver this: up/down is the grid's internal logic (not
    // a window command) and no command with a D-pad Left/Right action id is registered on
    // this screen (measured - cmdlist_notes). So the press is read physically through
    // arrow_dir_held() - the same reading the cursor guard in form_update_detour uses, which
    // is what keeps "step the value" and "undo the grid's row jump" perfectly in sync.
    // Held = repeat, like the game's own sliders: one step on the press, then after
    // kSliderRepeatDelayMs a step every kSliderRepeatMs.
    constexpr uint64_t kSliderRepeatDelayMs = 400;
    constexpr uint64_t kSliderRepeatMs = 90;

    int g_slider_dir = 0;           // direction currently held (0 = none)
    uint64_t g_slider_next = 0;     // when the held direction fires again
    bool g_slider_stepped = false;  // something moved during this hold

    void poll_slider(const Screen &top)
    {
        if (goblin::nmenu::rebind_pending())
        {
            g_slider_dir = 0;
            g_slider_stepped = false;
            return; // the rebind page owns raw input
        }
        const int dir = arrow_dir_held(); // the same reading the cursor guard uses
        const uint64_t now = GetTickCount64();
        if (dir == 0)
        {
            // Release ends the hold. The steps themselves skip reapply_live_settings() (every
            // current slider value is read live per frame by its consumer), so one call here
            // settles anything heavier a future slider key may gate - and one log line covers
            // the whole hold instead of one per repeat.
            if (g_slider_stepped)
            {
                goblin::reapply_live_settings();
                spdlog::info("[form] slider hold released - settings re-applied");
            }
            g_slider_dir = 0;
            g_slider_stepped = false;
            return;
        }
        if (dir != g_slider_dir)
        {
            g_slider_dir = dir;
            g_slider_next = now + kSliderRepeatDelayMs; // fire this press now, repeat later
        }
        else if (now < g_slider_next)
            return;
        else
            g_slider_next = now + kSliderRepeatMs;
        size_t ix = 0;
        bool preview = false;
        if (!selected_model_index(top.dlg, &ix, &preview) || preview)
            return;
        size_t count = 0;
        const goblin::nmenu::Row *rows = goblin::nmenu::rows(&count);
        if (!rows || ix >= count || rows[ix].kind != goblin::nmenu::RowKind::Slider)
            return;
        if (!goblin::nmenu::slider_step(ix, dir))
            return; // already at the end the press points at
        g_slider_stepped = true;
        build_form_rows();
        // Same engine gate the rebind path uses: FUN_140942690 reads the grid cursor and never
        // moves it, so the selection survives every repaint of the hold.
        if (safe_to_call_engine(top, now))
            refresh_form_view(top.dlg);
    }

    // ESC does not reach our screen's Back command: the host is the key-binding screen, whose own
    // command table registers action 0x18 for Q (and for the pad's circle), not for Escape, so the
    // engine simply has nothing bound to it there. The screen therefore ignored ESC entirely. Read the
    // key ourselves - the same way poll_rebind() does, because the screen consumes routed input but
    // GetAsyncKeyState reports the physical key - and raise the normal close request, so the close
    // still runs through the engine's own cancel path rather than anything of ours.
    // Guards: not while the rebind page is waiting for a key (there ESC means "leave the binding
    // alone" and poll_rebind owns it), and only on the down-edge so holding ESC cannot close a
    // parent screen right after a child.

    // ESC-CLOSE, RETIRED 2026-07-28. A poll_escape_close(dlg) stood here: it read the physical ESC
    // key and raised a close request. That cannot work, and the reasoning is worth keeping:
    // close_screen -> invoke_cancel calls the Back command's ACTION directly, and the engine only
    // runs an action when that command's matcher AND guard agree (section 2a of
    // docs/research_native_menu_screens.md), so the call reported success while the screen stayed up -
    // the only visible effect was the HUD returning over a still-open menu. Measured afterwards: ESC
    // produces NONE of the nine actions this screen listens for, and in plain gameplay it never
    // reaches the action predicate at all, because the system menu it opens is handled outside the
    // per-screen command tables. So ESC-close is not a defect of ours to repair; it is engine
    // integration we do not have yet, and it belongs with the native-registration work.
    // The function itself was left in place with an unconditional `return` at the top, which meant a
    // per-frame call into a body that could never run. Both are gone; the reasoning stays.

    // FUN_1407A9230(win + 0x10) - the game's OWN "is the sequence slot free?" test, the one its
    // input dispatcher gates a window's commands on (it stops feeding a window input while the
    // slot holds a screen). That makes it the exact liveness test for a screen we put THERE:
    // non-empty = the child is up, empty = it is gone.
    bool seq_slot_empty(uintptr_t win)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        __try
        {
            return reinterpret_cast<char (*)(void *)>(goblin::anchors::at(goblin::AnchorId::job_holder_test_seq))(
                       reinterpret_cast<void *>(win + 0x10)) != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Everything is down: persist, forget the screens, and let the next F8 start clean.
    void menu_torn_down()
    {
        hud_mode_restore("teardown (fallback)"); // in case no normal close was seen
        g_screens.clear();
        hud_snapshot_compare(); // our screens can also leave here, not only through the per-level close
        g_form_depth = 0;
        g_form_dialog.store(0, std::memory_order_release);
        g_form_update_watch.store(0, std::memory_order_release);
        g_close_request.store(0, std::memory_order_release);
        g_slider_dir = 0; // a hold must not carry into the next open
        g_slider_stepped = false;
        for (std::vector<FormItem> &p : g_form_pools)
            p.clear();
        goblin::nmenu::set_nested(false);
        if (goblin::nmenu::dirty())
        {
            goblin::nmenu::clear_dirty();
            g_menu_cfg_dirty = true; // fold into the single save below
        }
        if (g_menu_cfg_dirty)
        {
            g_menu_cfg_dirty = false;
            goblin::save_config(goblin::g_ini_path);
            spdlog::info("[form] menu closed - settings saved to ini");
        }
        else
            spdlog::info("[form] menu closed (no changes)");
    }

    // ── WHO IS STILL THERE ───────────────────────────────────────────────────────────
    // Two signals, and neither one reads memory that may already be freed:
    //   * HEARTBEAT - a dialog's own per-frame update (its vt+0x10) runs for every screen that
    //     exists, including one sitting under a child (the input gate stops a parent's COMMANDS,
    //     not its update). A screen whose heartbeat stopped is gone. This is the only signal used
    //     for the bottom of the stack, and it needs no host to be inspected at all.
    //   * PARENT'S SEQUENCE SLOT - read only on a parent whose heartbeat is fresh THIS frame, so
    //     the object is known to be alive. Empty slot = the child above it has closed, known one
    //     frame after it happens instead of after a timeout.
    // Inspecting the dialog's vtable is deliberately NOT among them: that read reported "closed"
    // on a live screen and let presses stack layers.
    constexpr uint64_t kBeatStaleMs = 600;
    // How long a screen that has been asked for may take to come up (async movie load).
    constexpr uint64_t kOpenGraceMs = 3000;

    bool beat_fresh(const Screen &s, uint64_t now)
    {
        return s.beat != 0 && now - s.beat < kBeatStaleMs;
    }

    // FUN_1407A9200(&win[0x1E8]) - the engine's own "is this window's job finished?" test, the
    // first of the three gates its input dispatcher checks (0x1407455D5..0x1407455EB). It goes
    // true when the screen starts CLOSING, which is earlier than its slot emptying and earlier
    // than its dialog dying - so it is the moment the page underneath must come back if there is
    // to be no frame with neither of them drawn.
    bool job_finished(uintptr_t win)
    {
        __try
        {
            uint64_t held = *reinterpret_cast<uint64_t *>(win + 0x1E8);
            return reinterpret_cast<char (*)(void *)>(goblin::anchors::at(goblin::AnchorId::job_holder_test_child))(&held) != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Bring the page under the top screen back while that screen is still fading out.
    void reveal_page_under_closing_top()
    {
        if (g_screens.size() < 2)
            return;
        Screen &top = g_screens.back();
        Screen &below = g_screens[g_screens.size() - 2];
        if (!top.dlg || !below.dlg || !below.hidden)
            return;
        if (!job_finished(top.dlg))
            return;
        below.hidden = false;
        set_screen_visible(below.dlg, true);
        // Rebuild it, do not just show it. Each page is its own SCREEN, so the parent still holds the
        // rows it was built with - which is why a filter set on a child page only showed up on the
        // parent after the whole menu was reopened, and why a rebound key read as "not saved" there.
        // The rebuild goes through the hooked builder, which takes the page from this screen's record.
        goblin::nmenu::set_page(below.page);
        goblin::nmenu::rebuild(); // unconditional: set_page is a no-op when the ids already match
        build_form_rows();
        refresh_form_view(below.dlg);
        spdlog::info("[form] page 0x{:X} back on screen (rows rebuilt) while the screen over it closes",
                     below.dlg);
    }

    // The HUD has to start coming back WITH the close animation, not after it. The engine does that
    // for its own screens; we used to wait for prune_screens() to notice the missing heartbeat, which
    // is a ~600 ms timeout ("level 0 gone (no update for 609 ms)") and read as a visible lag. So use
    // the same event the page-reveal above uses: job_finished() is the engine's own "this screen's job
    // is done" test, true while the screen is still fading out. Restoring here is safe even if the
    // screen somehow lingers, because hud_mode_restore() only writes when the mode is still 0 and
    // clears its captured value, so the later call from menu_torn_down() becomes a no-op.
    // How late is the restore, in milliseconds, measured rather than guessed? The close is started by
    // a key press we do not hook (the engine routes it to the screen), but GetAsyncKeyState reports the
    // physical key regardless of routing, so stamping the down-edge of the two keys that close a screen
    // is enough to turn "sometimes a small delay" into a number. Debug-logging only; it changes nothing.
    uint64_t g_back_key_ms = 0;
    bool g_back_key_was_down = false;

    void note_back_key()
    {
        if (!goblin::config::debugLogging)
            return;
        const bool down = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0 ||
                          (GetAsyncKeyState('Q') & 0x8000) != 0;
        if (down && !g_back_key_was_down)
            g_back_key_ms = GetTickCount64();
        g_back_key_was_down = down;
    }

    void restore_hud_when_close_starts()
    {
        if (g_screens.size() != 1 || !g_hud_mode_at_push)
            return; // only the root screen going away ends our over-gameplay session
        const Screen &top = g_screens.back();
        if (!top.dlg || !job_finished(top.dlg))
            return;
        if (goblin::config::debugLogging && g_back_key_ms)
            spdlog::info("[hud] the close signal arrived {} ms after the back key went down",
                         GetTickCount64() - g_back_key_ms);
        hud_mode_restore("the close animation");
    }

    void prune_screens()
    {
        const uint64_t now = GetTickCount64();
        if (g_screens.empty())
            return;
        note_back_key();
        restore_hud_when_close_starts();
        // Top down: a screen with no heartbeat is gone. One that has not come up yet has no
        // heartbeat either, so it is judged by its open grace instead.
        while (!g_screens.empty())
        {
            const Screen &top = g_screens.back();
            if (!top.dlg)
            {
                if (now - top.opened < kOpenGraceMs)
                    break;
                spdlog::warn("[form] level {} never came up - dropped", g_screens.size() - 1);
            }
            else
            {
                if (beat_fresh(top, now))
                    break;
                spdlog::info("[form] level {} gone (no update for {} ms)", g_screens.size() - 1,
                             now - top.beat);
            }
            g_screens.pop_back();
            if (!g_screens.empty())
                resume_level(g_screens.size() - 1);
        }
        if (g_screens.empty())
        {
            hud_snapshot_compare(); // the usual exit for an over-gameplay screen
            menu_torn_down();
            return;
        }
        // EXPERIMENT (2026-07-28): shift the screen right to see whether the key-binding host's
        // left-half layout can be centred. Applied HERE, per screen, because the two earlier attempts
        // sat in code paths that never ran for a lone screen and logged nothing. Set to 0 to disable.
        // CENTRING IS NOT DONE AT RUNTIME - see goblin_own_movie.cpp. Three runtime attempts failed and
        // each taught something: the delta must be added to the current position (SetPos is absolute);
        // proxy -> value+0x28 is an AS object handle, not a DisplayObject (calling vt+0x10/+0x18 on it
        // crashed); and even with the authored coordinates, the movie's own timeline RE-PLACES the section
        // a few frames in and wins, while re-applying every tick reaches a dying screen during teardown
        // and crashed on close. The position the engine keeps restoring is the one authored in the MOVIE,
        // so that is where it has to be changed - which costs no runtime writes at all.

        // A live parent whose sequence slot has emptied has lost its child - the fast path, and
        // the reason a return does not wait for a timeout.
        for (size_t i = 1; i < g_screens.size(); ++i)
        {
            const Screen &parent = g_screens[i - 1];
            if (!parent.dlg || !beat_fresh(parent, now))
                break; // cannot ask a parent we are not sure about
            if (!g_screens[i].dlg && now - g_screens[i].opened < kOpenGraceMs)
                break; // its child is still coming up
            if (!seq_slot_empty(parent.dlg))
                continue; // the parent still holds it
            spdlog::info("[form] level {} closed (page {}), {} level(s) dropped", i,
                         g_screens[i].page, g_screens.size() - i);
            g_screens.resize(i);
            resume_level(i - 1);
            break;
        }
    }

    // Per-frame upkeep for the whole menu, whatever it is hosted on.
    void tick_native_menu()
    {
        // A screen that asked to be closed from inside its own confirm handler: doing it here
        // means the handler has long returned before the dialog is torn down.
        const uintptr_t want_close = g_close_request.exchange(0, std::memory_order_acq_rel);
        if (want_close)
        {
            // Only if that screen is STILL BEING TICKED. close_screen -> invoke_cancel reads the
            // dialog's command table and CALLS A FUNCTION POINTER out of it, and an indirect call
            // through freed memory is the one shape __try cannot save us from - if the stale bytes
            // happen to land in mapped executable memory the game runs them instead of faulting.
            // The request is stamped by a decide handler, when the dialog was alive, and consumed
            // on the next tick, so the window is one frame - narrow, but the worst-shaped hazard
            // we have. screen_level() alone cannot cover it: it is filled from the same source as
            // the pointer it is meant to vet, which makes that test circular (see the note on
            // g_form_dialog).
            const int lvl = screen_level(want_close);
            if (lvl >= 0 && safe_to_call_engine(g_screens[static_cast<size_t>(lvl)], GetTickCount64()))
                close_screen(want_close);
        }
        // Before pruning: a screen that has begun closing means the page under it should already
        // be visible, so the fade-out plays over it instead of over nothing.
        reveal_page_under_closing_top();
        prune_screens();
        // An open that had to wait for the map's job chain to let go of the sequence slot. Retried
        // here, on the map UI thread, exactly as if the player had pressed the key at this moment.
        if (g_pending_open_page >= 0)
        {
            const int32_t page = g_pending_open_page;
            const uintptr_t win = map_menu_window();
            const bool expired = GetTickCount64() - g_pending_open_since > kPendingOpenTimeoutMs;
            if (menu_open() || !win || expired)
            {
                // Gone, or waited too long, or the player got a menu another way in the meantime.
                if (expired && win && !menu_open())
                    spdlog::info("[form] gave up waiting for the map's sequence slot after {} ms",
                                 GetTickCount64() - g_pending_open_since);
                g_pending_open_page = -1;
                g_pending_open_since = 0;
            }
            else if (seq_slot_empty(win))
            {
                const uint64_t waited = GetTickCount64() - g_pending_open_since;
                g_pending_open_page = -1;
                g_pending_open_since = 0;
                spdlog::info("[form] the map's sequence slot came free after {} ms - opening now",
                             waited);
                open_screen(page, 0);
            }
        }
        if (!menu_open())
        {
            goblin::overlay::set_text_capture(false); // the search page cannot be up any more
            g_search_typing.store(false, std::memory_order_relaxed);
            return;
        }
        const Screen &top = g_screens.back();
        if (!top.dlg)
            return; // still coming up
        menu_cfg_apply_if_changed();
        // ONLY while the engine is still ticking this dialog. paint_help_line reaches into the
        // dialog's own row list, and the engine frees that list's items - with their scene proxies
        // - when the rows are rebuilt or the screen goes away. Measured on the Deck 2026-08-02: the
        // dialog stopped ticking 610 ms before we noticed (g_form_dialog is only dropped once the
        // heartbeat has been stale for kBeatStaleMs = 600), and we kept calling into it for that
        // whole window: six access violations in one second, each one dragging an SEH unwind
        // through the engine's own destructors. kBeatStaleMs is a PRUNING threshold and far too
        // loose to authorise an engine call; three frames is not.
        if (safe_to_call_engine(top, GetTickCount64()))
            paint_help_line();
        poll_rebind();
        poll_search(top);
        poll_slider(top);
        arm_action_log_on_escape();
        // ── the strips, EVERY frame ──────────────────────────────────────────────────
        // The SDK says why they cannot be written once: DisplayList::MoveDisplayObject
        // (GFx_DisplayList.cpp:363) re-applies the tag's MATRIX whenever the timeline places an
        // object again, and a script-set transform only survives if the object rejects anim
        // moves - which the movie-wide continueAnimation flag undoes for it
        // (GFx_DisplayObject.cpp:1175). Position and scale are both matrix, so both are fair game
        // for the engine; the playhead is NOT in that list, which is the long-term fix (one frame
        // per icon instead of a shifted strip).
        // NOT from here: our tick runs before the timeline advance, so the engine's re-placement
        // of the authored matrix always landed after it. The repaint moved to the tail of
        // form_update_detour, the one point in the frame that is later than the advance.
        // Only while the dialog is provably alive: for ~600 ms after its last update this read freed
        // memory (ERR 2026-09-23: top 0 / 1440 and count 3196666339 alternating after the close).
        if (goblin::config::debugLogging && safe_to_call_engine(top, GetTickCount64()))
        {
            // Scroll telemetry: a top that stops at (totalRows - visible) is a clamp, a top that
            // keeps climbing is the data mapping running out.
            static int32_t s_last_top = -1;
            static uint32_t s_last_cur = 0xFFFFFFFFu;
            int32_t t = 0;
            uint32_t cur = 0, cnt = 0;
            __try
            {
                const uintptr_t grid = top.dlg + 0xA38;
                t = *reinterpret_cast<int32_t *>(grid + 0x348);
                cur = *reinterpret_cast<uint32_t *>(grid + 0xD4);
                cnt = *reinterpret_cast<uint32_t *>(grid + 0xD0);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            if (t != s_last_top || cur != s_last_cur)
            {
                s_last_top = t;
                s_last_cur = cur;
                spdlog::info("[form] scroll: top={} cursor={} count={} (level {})", t, cur, cnt,
                             g_screens.size() - 1);
            }
        }
    }
    void menu_update_detour(void *menuman, void *dt)
    {
        const int64_t entry_qpc = v3_perf_now(); // [closeframes]: this frame's stamp (a POD, no unwinding)
        // Arm the action log from HERE too, not only from the form upkeep: the control measurement is
        // ESC pressed in PLAIN GAMEPLAY, where no screen of ours exists and the upkeep does not run.
        // Whatever id answers YES there is the action ESC really produces - and the first run proved
        // it is none of the nine our screen listens for.
        arm_action_log_on_escape();
        // F11 (dev-only): open the native settings menu on this UI thread. GATED to
        // IN-GAME with the map open - the settings dialog reads gameplay-state
        // singletons that are NULL at the title screen (opening it from the main menu
        // crashes in the async job). The open map also guarantees a valid active menu
        // to push onto.
        // Cache the persistent base/root menu: while in-game with NO full-screen menu
        // up (map closed), *(CSMenuMan+0x80) is the base HUD menu. Settings pushed onto
        // THIS stacks as an overlay (so it can later overlay the map instead of replacing it).
        // What *(CSMenuMan+0x80) is depends on the state, and it is NOT always a window: in game
        // its vtable slots read back as string data, so a job may only be PUSHED onto it, never
        // stored in a slot of it (window_like() is the gate for that).
        g_menuman.store(reinterpret_cast<uintptr_t>(menuman), std::memory_order_release);
        // The map phase, sampled once per frame on the game UI thread. This is CSMenuMan::updateTask,
        // so it runs whether or not any menu is up - which is exactly what makes it able to see the
        // close edge. Nothing else in the mod reads the byte directly.
        {
            // Resolve the world map's menu id from the live dialog rather than hardcoding it:
            // wmd_update passes r8 = dialog + 0x27D8, and the id is a u16 at dialog+0x180. Until it
            // resolves, 0x3D stands (WorldMapDialog::create builds it as 3 + 0x3a).
            if (!g_map_menu_id_resolved.load(std::memory_order_relaxed))
            {
                if (void *area = goblin::maphover::map_dialog())
                {
                    uint32_t id = 0;
                    const uintptr_t dialog = reinterpret_cast<uintptr_t>(area) - 0x27D8;
                    if (v3_read32(dialog + 0x180, id) && (id & 0xFFFF) < 0x47)
                    {
                        g_map_menu_id.store(static_cast<uint16_t>(id & 0xFFFF),
                                            std::memory_order_relaxed);
                        g_map_menu_id_resolved.store(true, std::memory_order_relaxed);
                        spdlog::info("[mapphase] world map menu id resolved: 0x{:X}", id & 0xFFFF);
                    }
                }
            }
            const uint8_t raw = v3_map_phase_sample();
            const uint8_t prev = g_map_phase.exchange(raw, std::memory_order_release);
            if (raw != 0xFF && prev != raw)
                spdlog::info("[mapphase] {} -> {} ({})", prev, raw,
                             raw == 0 ? "gone"
                                      : (raw == 1 ? "idle/closing"
                                                  : ((prev == 0 || prev == 0xFF) ? "open" : "focus")));
            // The FIRST valid phase read: the menu manager answers, so the Scaleform system it
            // sits on exists too. Start the park shortlist scan here, in the main menu, minutes
            // before the first map - the open edge below stays as the second launch point.
            if (prev == 0xFF && raw != 0xFF)
                v3_park_build_slots();
            // The OPEN edge, and the only place g_v3_map_closed is ever cleared. One writer per
            // direction: the engine's byte going non-zero means a dialog exists again.
            if (prev == 0 && raw != 0 && raw != 0xFF)
            {
                g_v3_map_closed.store(false, std::memory_order_release);
                g_v3_open = V3OpenCost{}; // per-open cost accounting starts here
                g_open_dialog_ms.store(GetTickCount64(), std::memory_order_relaxed);
                // Second launch point for the park shortlist scan (the first is the phase read
                // above): a no-op once the list exists, a retry if the early scan found nothing.
                v3_park_build_slots();
            }
            // Before the walk, so the walk's time falls into the "next" interval of [closeframes].
            close_frames_note(entry_qpc, prev != 0 && prev != 0xFF && raw == 0);
            // While the map is closed is exactly when the parked entry exists and the engine is
            // counting it down. One bounded walk per frame, read-only until the exact match.
            if (raw == 0)
                v3_park_expire_tick();
            if (raw == 0)
            {
                g_map_screen_gone.store(true, std::memory_order_release);
                // The EARLY close edge: the engine zeroes this byte two steps before the destructor
                // our current close hook waits for. on_map_close() is idempotent.
                if (prev != 0 && prev != 0xFF)
                    goblin::stall_probe::on_map_close();
            }
        }
        hud_watch_sample(menuman); // armed right after our screen closes; prints only on change
        // ONE-SHOT: locate the STATIC slot that holds this singleton. With its RVA we can x-ref the code
        // that touches CSMenuMan+0x90.. offline and find the function the game itself uses to clear those
        // entries (writing them ourselves does not restore the HUD and races the menu machinery). Scans
        // the module's own image for a qword equal to the live pointer.
        if (goblin::config::debugLogging && menuman)
        {
            static std::atomic<int> once{0};
            if (once.exchange(1) == 0)
            {
                __try
                {
                    const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
                    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(exe);
                    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(exe + dos->e_lfanew);
                    const uintptr_t want = reinterpret_cast<uintptr_t>(menuman);
                    int found = 0;
                    const auto *sec = IMAGE_FIRST_SECTION(nt);
                    for (unsigned k = 0; k < nt->FileHeader.NumberOfSections && found < 6; ++k, ++sec)
                    {
                        if ((sec->Characteristics & IMAGE_SCN_CNT_INITIALIZED_DATA) == 0)
                            continue;
                        const uintptr_t from = exe + sec->VirtualAddress;
                        const uintptr_t to = from + sec->Misc.VirtualSize;
                        for (uintptr_t a = from; a + 8 <= to; a += 8)
                        {
                            if (*reinterpret_cast<const uintptr_t *>(a) != want)
                                continue;
                            char nm[9] = {};
                            memcpy(nm, sec->Name, 8);
                            spdlog::info("[hudprobe] CSMenuMan pointer 0x{:X} is stored at exe+0x{:X} "
                                         "(section {})", want, a - exe, nm);
                            ++found;
                        }
                    }
                    if (!found)
                        spdlog::info("[hudprobe] CSMenuMan pointer 0x{:X} not found in any data section",
                                     want);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                }
            }
        }
        // HUD PROBE (2026-07-28): the state bytes that change across our screen live in an ARRAY around
        // CSMenuMan+0x90 (no instruction stores 3 at a fixed +0x97, so they are indexed entries), and
        // writing them back did not bring the HUD back. So stop guessing which byte and let the GAME show
        // us: the user's own observation is that opening any native menu restores the HUD. Dump this
        // window whenever the active menu changes, and once more right after our screen closes. The diff
        // between "HUD hidden after our close" and "HUD back after a native menu" is the answer.
        if (goblin::config::debugLogging && menuman)
        {
            static uintptr_t s_last_active = 0;
            __try
            {
                const uintptr_t act = *reinterpret_cast<uintptr_t *>(
                    reinterpret_cast<uintptr_t>(menuman) + 0x80);
                if (act != s_last_active)
                {
                    s_last_active = act;
                    char hex[3 * 64 + 1];
                    int n = 0;
                    const uint8_t *b = reinterpret_cast<const uint8_t *>(menuman) + 0x80;
                    for (int i = 0; i < 64 && n < (int)sizeof(hex) - 4; ++i)
                        n += _snprintf_s(hex + n, sizeof(hex) - n, _TRUNCATE, "%02X ", b[i]);
                    spdlog::info("[hudprobe] active menu -> 0x{:X}; CSMenuMan+0x80: {}", act, hex);
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
        if (menuman)
        {
            __try
            {
                const uintptr_t a = *reinterpret_cast<uintptr_t *>(
                    reinterpret_cast<uintptr_t>(menuman) + 0x80);
                // (Two further terms excluded nothing and are gone: g_form_host, which open_screen
                //  never wrote - the map-hosted path stores its job in the window's sequence slot -
                //  and g_our_dialog, which only the retired F11 prototype ever set.)
                if (a)
                    g_active_menu.store(a, std::memory_order_release);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }

        if (menuman && p_worldchrman_slot && *p_worldchrman_slot && !goblin::maphover::map_dialog())
        {
            __try
            {
                const uintptr_t a = *reinterpret_cast<uintptr_t *>(
                    reinterpret_cast<uintptr_t>(menuman) + 0x80);
                // (This used to skip caching while our own F11 settings dialog was alive. That
                //  dialog was never constructed - see the F11 note at the top of the file.)
                if (a)
                    g_base_menu.store(a, std::memory_order_release);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }

        if (goblin::config::native_menu_enabled())
        {
            // F8: open OUR menu - the game's keybinding screen carrying our rows, on the
            // map when it is up and over whatever else is on screen otherwise. F8 only OPENS:
            // closing belongs to the screen's own Back/Esc, and a press while it is up is
            // ignored (making it a toggle stacked a second screen on the first, because the
            // close is not instantaneous).
            // F8 is the DEVELOPER's door, so it only exists in `dev` - there the player's key opens
            // the overlay and this one opens the in-game menu, which is the whole point of that mode.
            // In `native` the player's own toggle_key opens this menu (below), and F8 answering as well
            // was a leftover of the arrangement where the two menus had separate keys.
            if (goblin::config::menu_mode() == goblin::config::MenuMode::Dev)
            {
                static bool s_f8_down = false;
                const bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
                // ...unless this press is the one that just got bound to something (key_swallowed).
                if (f8 && !s_f8_down && !goblin::nmenu::key_swallowed(VK_F8))
                    open_screen(goblin::nmenu::kPageRoot, 0);
                s_f8_down = f8;
            }

            // In `native` mode the PLAYER'S key opens this menu - F8 is only the developer's door.
            // The overlay owns that key in the other two modes (it is the overlay that opens there),
            // so this must not also run in `dev`, or one press would open both menus.
            if (!goblin::config::overlay_menu_enabled() && goblin::config::menuEnabled)
            {
                static bool s_key_down = false;
                const int vk = static_cast<int>(goblin::config::toggleInjectionKey);
                const uint16_t mask = goblin::config::toggleGamepadMask;
                const bool down = (vk && (GetAsyncKeyState(vk) & 0x8000) != 0) ||
                                  (mask && goblin::overlay::gamepad_mask_down(mask));
                if (down && !s_key_down)
                {
                    // "The menu does not open over the map" came in from the Steam Deck with a
                    // debug log that contained NO attempt at all - every push it recorded had
                    // map=0x0 - so the open either never reached open_screen or died silently.
                    // One line per PRESS separates those: if this prints and no "[form] opening
                    // page" follows, the refusal is inside open_screen; if it never prints, the
                    // press never got here (input path), and the map state says whether the map
                    // was up when it happened.
                    const bool swallowed = goblin::nmenu::key_swallowed(static_cast<uint32_t>(vk));
                    spdlog::debug("[form] toggle pressed: swallowed={} menuOpen={} mapDialog=0x{:X}",
                                  swallowed, menu_open(),
                                  reinterpret_cast<uintptr_t>(goblin::maphover::map_dialog()));
                    if (!swallowed)
                        open_screen(goblin::nmenu::kPageRoot, 0);
                }
                s_key_down = down;
            }
            // F6 (the graphics screen as a second host) was REMOVED 2026-07-28: measured in game, its
            // list caps at the same sixteen items and does not scroll, so it bought visible rows and
            // nothing else. Its reverse engineering, and the two things worth harvesting from it
            // (centring, and its slider/combo widgets), are in
            // docs/research_f6_graphics_host_retired.md.
            tick_native_menu(); // liveness, live-apply, help line, key capture, save
        }

        // The menu-graphics probe that used to run here has been removed: under the log key it
        // called the engine's drawing primitives to fill an opaque square into whatever menu had
        // just become active. That is a visible mutation of the game's UI, not a diagnostic - and
        // its question (can we draw our own filled shapes into a native screen?) is answered in
        // docs/research_retired_native_ui_experiments.md.
        o_menu_update(menuman, dt);
    }

    // ── NATIVE SETTINGS-MENU tab injection, Proto 0 (read-only observe) ──
    // ── OUR TAB AND OUR PAGE IN THE GAME'S OWN SETTINGS MENU: REMOVED 2026-07-31 ──
    // Roughly 220 lines lived here and none of it could run. The chain, in order:
    //   opttop_ctor_detour   consumed g_our_f11_open to decide "this ctor is OUR open"
    //   append_tab_detour    on that flag, skipped every game tab and called...
    //   inject_our_tab       which built a MenuOptionCategory, relabelled it from our injected
    //                        GR_MenuText id and appended it with category id 64
    //   show_page_detour     matched that id 64 and borrowed page 3, arming...
    //   populate_page_detour which swapped the game's populate for...
    //   build_our_rows       which appended native ON/OFF combo rows bound to our config bools
    //   opttop_dtor_detour   cleared g_our_dialog so the toggle could open again
    // g_our_f11_open was never set to anything but 0 and no F11 poll existed, so the first link
    // was never made and nothing downstream of it ever ran. Five hooks and two AOB scans were
    // nevertheless armed at every launch for it, and two of its log literals ("our tab added",
    // "[optmenu] our page populated") sat in the shipped DLL - readable text with no code path,
    // which is exactly what the AV heuristics weigh.
    //
    // The RE it rests on is NOT lost and is worth keeping: the options dialog builds its tabs as
    // CS::MenuViewItemList<CS::MenuOptionCategory> (cap 10) at dialog+0x1208, count at +0x1760,
    // stride 0x88 with the category id at +0x40; the page-open dispatch FUN_14093c590 switches on
    // that id and its page cache is a fixed 10-slot array at pageCtl+0x68 indexed UNCHECKED; the
    // row primitives are MenuTextCtor 0x760970, help 0x760790, ComboItemList 0x9543B0, append-combo
    // 0x948FA0 and pack dtor 0x742C90. That is written up with the rest of the native-menu work in
    // docs/research_native_menu_screens.md.
    //
    // What ships instead is open_screen() below: one native screen per page, hosted on the
    // key-binding movie. It needs none of this.

    // A diagnostic for the map sub-dialog job runner (FUN_1407ad1c0) lived here: it dumped a live
    // job's scene/movie/factory/dialog fields so a Path-B settings job could be reproduced on the
    // map's own runner. It was fully written and NEVER HOOKED - setup() installed nothing for it,
    // so o_jobstep stayed null and the detour that dereferenced it was unreachable. That is not the
    // usual AOB-miss degradation; there was no scan at all.

    // opttop_ctor_detour stood here. Besides consuming g_our_f11_open (see the note above) it
    // logged the dialog's tab count under debug_logging; with the flag permanently 0 that log was
    // the only thing it did, on a hook installed for every settings-menu open in the game.

    // The Task #5 "memo dialog" prototype lived here until 2026-07-30. It was never connected:
    // o_memo_show and o_memo_ctor were declared and never assigned, so no detour existed; the
    // scan helpers had no callers, and by the end the block was ~50 lines of comments describing
    // functions that had already been deleted. Its startup cost was real, though - setup() kept
    // resolving three AOB patterns (memo view-couple, game allocator, allocator global) whose
    // only use was printing them in one log line. What was learned about WorldMapMemoSelectDialog
    // is in docs/research_retired_native_ui_experiments.md; the working native path is the
    // key-binding screen (open_screen), not this one.



    void v3_native_factory_consume(void *live_ctx, uint32_t frame)
    {
        if (!g_v3_native.seeded || g_v3_native.in_factory)
        {
            g_v3_consume_busy.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // Fully SYNCHRONOUS pipeline: queue a batch of records, run the
        // engine's own materialization driver on them, transfer the children,
        // neutralize the records - all inside this one pulse. No cross-pulse
        // state; leftovers can only mean a previous pulse aborted mid-way.
        if (g_v3_factory_active != 0)
            v3_factory_clear_request(true);
        // Counted separately: a spent queue is the NORMAL end of a build and must not be
        // reported as a blocked gate, while a missing driver or a spent budget is exactly
        // what the watchdog needs to name.
        if (!g_v3_mat_driver)
        {
            g_v3_consume_nodriver.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (g_v3_native.pending_index >= g_v3_native.pending.size())
            return;   // spent queue: the normal end of a build, not a blocked gate
        if (g_v3_native.frame_budget == 0)
        {
            // [v3pump] A capped burst stops here: the rest is the pump's, and the self-grant below
            // would quietly build it all in this frame again. Inside a pump stroke the budget is
            // simply spent.
            if (g_v3_native.pump_capped || t_v3_pump.pumping)
                return;
            // The budget is a PER-FRAME cap and only the map-frame tick refills it (to 96). When
            // that tick does not run - our own screen sitting on top of the map is enough to stop
            // it - the budget stays 0 forever and every pulse bounces off it while the queue still
            // has work. Measured on the Deck: "pulses seen=27428 ... blocked(driver/budget/sprite/
            // busy)=0/24372/0/0" and CATEGORIES READY created=0, i.e. the icons vanished with a
            // plentiful supply of pulses and nothing wrong anywhere else. Starving forever is worse
            // than the stall the cap exists to prevent, so a pulse may grant itself ONE batch when
            // the queue is not empty. That keeps the per-pulse work bounded exactly as before -
            // the loop below still stops at V3_FACTORY_BATCH - it only stops the build from
            // deadlocking when no frame boundary is coming.
            // ...but ONLY when no frame boundary is actually coming. That condition used to be
            // assumed; it is now measurable, because on_map_frame stamps a heartbeat. Pulses
            // arrive far faster than frames on a big profile (Convergence: 30789 pulses against
            // ~40 frames in the same second), so an unconditional self-grant let every pulse
            // hand itself a batch and the per-frame cap stopped capping anything: the whole
            // 8424-marker build landed inside one second, 293 ms of it in the factory, and the
            // map ran that second at 14 fps instead of 55. Measured 2026-08-05, and it is the
            // slowdown the player feels on open - not a stall (nothing is blocked long enough
            // for [stallcap]), just a second of half-rate.
            //
            // With a live heartbeat the budget does its job again and the build spreads over the
            // frames it was always meant to span. The starvation case the self-grant exists for
            // is unchanged: if frames genuinely stop arriving - our own screen sitting over the
            // map is enough - the heartbeat goes stale and the grant resumes.
            const uint64_t hb = g_map_hb_ms.load(std::memory_order_relaxed);
            const uint64_t hb_age = hb ? GetTickCount64() - hb : UINT64_MAX;
            if (hb_age < 100)
            {
                // Frames are running; wait for the tick to refill the budget. Counted so that
                // if a build ever starves in exactly this state (budget stuck at 0 with a live
                // heartbeat), the queue-drop warning can name this gate.
                g_v3_consume_hbwait.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            g_v3_consume_nobudget.fetch_add(1, std::memory_order_relaxed);
            g_v3_native.frame_budget = V3_FACTORY_BATCH;
        }
        uint64_t sprite = 0;
        if (!v3_read64(reinterpret_cast<uintptr_t>(live_ctx) + 0x58, sprite) ||
            !v3_heap_ptr(sprite))
        {
            g_v3_consume_nosprite.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // Anchor movie identity, revalidated on every pulse BEFORE anything is issued
        // from the queue. The live ctx's sprite is by construction inside the movie the
        // engine is executing right now, so its movie view is the ground truth to hold
        // the anchor against. If the anchor is detached, or lives in a different movie
        // view, no child created this pulse can ever attach to it - the 2.1.1 layer-2
        // runs proved that one child at a time, 14272 rejections and created=0.
        // Detecting it here instead keeps pending_index untouched and the burst's
        // remaining pulses alive: the drop re-arms discovery, a later attach in this
        // same burst re-anchors, and the fresh seed rebuilds the queue and builds on
        // the pulses that are left.
        const uintptr_t live_movie = v3_movie_of(static_cast<uintptr_t>(sprite));
        const uintptr_t target_movie = v3_movie_of(g_v3_native.parent);
        if (v3_node_detached(g_v3_native.parent) || target_movie == 0 ||
            (live_movie != 0 && target_movie != live_movie))
        {
            v3_drop_dead_anchor("factory pulse", g_v3_native.parent);
            return;
        }
        if (!g_v3_native.pump_tried && goblin::variants::kFramePump && g_v3_goto_frame &&
            !g_v3_pump.off && g_v3_native.burst_end < g_v3_native.pending.size())
            v3_pump_make_generator(live_ctx, frame, static_cast<uintptr_t>(sprite), target_movie);
        g_v3_native.in_factory = true;

        // Per-item synchronous pipeline: queue ONE record, run the engine's
        // materialization driver, transfer the child, neutralize the record -
        // then the next item, up to BATCH per pulse. Strictly one in flight
        // keeps the per-icon SHARED tag legal: its depth is re-patched only
        // after the previous record was decoded and neutralized. (The 16-wide
        // variant needed a fresh tag per request - ~9.4k extra movie-heap
        // allocations that all came back as close-teardown frees.)
        uint32_t attempts = 0;
        while (attempts < V3_FACTORY_BATCH &&
               g_v3_native.frame_budget != 0 &&
               g_v3_native.pending_index < g_v3_native.pending.size())
        {
            ++attempts;
            const auto point = g_v3_native.pending[g_v3_native.pending_index++];
            --g_v3_native.frame_budget;
            float mx = 0.0f, mz = 0.0f;
            const int64_t t_proj0 = v3_perf_now();
            goblin::worldmap_probe::ProjFail proj_why = goblin::worldmap_probe::ProjFail::none;
            bool from_origin = false;
            const bool proj_ok = goblin::mapproject::to_map(point.area, point.gx, point.gz,
                                                            point.px, point.pz, mx, mz, &proj_why,
                                                            &from_origin);
            if (!t_v3_pump.pumping) // stages of the BURST, like "factory"
                g_v3_open.proj_qpc += v3_perf_now() - t_proj0;
            if (proj_ok && from_origin)
                ++g_v3_native.placed_from_origin;
            if (!proj_ok)
            {
                ++g_v3_native.failed;
                ++g_v3_native.failed_project;
                using goblin::worldmap_probe::ProjFail;
                if (proj_why == ProjFail::declined)
                {
                    ++g_v3_native.failed_project_declined;
                    // Only the converter's own refusals go into the by-tile line: that line is
                    // what inputs/unprojectable_tiles.json is maintained from, and a tile we
                    // never asked about says nothing about the profile's data.
                    ++g_v3_native.failed_project_tiles[
                        (static_cast<uint32_t>(point.area) << 16) |
                        (static_cast<uint32_t>(point.gx & 0xFF) << 8) |
                        static_cast<uint32_t>(point.gz & 0xFF)];
                }
                else if (proj_why == ProjFail::stale)
                    ++g_v3_native.failed_project_stale;
                else if (proj_why == ProjFail::no_view)
                    ++g_v3_native.failed_project_noview;
                else
                    ++g_v3_native.failed_project_faulted;
                // NOT OUR ARITHMETIC - the ENGINE's own converter declined this tile. Identified
                // 2026-08-05 after `failed=1 (project=1)` showed up on 47 of 47 opens: it is
                // always vanilla row 6000197, a Stake of Marika at area 42 grid (1,0), the only
                // area-42 row in the profile using gridX=1 (every other one is 0, 2 or 3). We
                // read the row live from WorldMapPointParam and hand the tile to the game's
                // converter, which answers "cannot convert" - so the game's own data references a
                // tile its map has no converter for. Skipping is the correct outcome: the only
                // alternative would be to invent an offset and draw the icon in the wrong place,
                // which is worse than one missing marker. Kept as a warning, not silenced, so a
                // profile where this count is not 1 is still noticed.
                static uint32_t s_proj_logged = 0;
                if (s_proj_logged++ < 3)
                    spdlog::warn("[v3native] {}, marker skipped:"
                                 " row={} area={} grid=({},{}) pos=({:.1f},{:.1f}) ring={}",
                                 proj_why == ProjFail::declined
                                     ? "engine converter declined this tile"
                                 : proj_why == ProjFail::stale
                                     ? "converter not asked (its last own call is over 300 ms old)"
                                 : proj_why == ProjFail::no_view
                                     ? "converter not asked (no map view yet)"
                                     : "converter call raised and was caught",
                                 point.original_row_id & ~goblin::NATIVE_CLEARED_KEY_BIT,
                                 point.area, point.gx, point.gz, point.px, point.pz,
                                 (point.original_row_id & goblin::NATIVE_HIGHLIGHT_KEY_BIT) != 0);
                continue;
            }
            const uint32_t char_id =
                goblin::gfx_probe::native_character_id(point.source_icon_id);
            if (char_id == 0)
            {
                ++g_v3_native.failed;
                ++g_v3_native.failed_charid;
                continue;
            }
            // Rings from their own high band; markers from the normal one.
            const bool is_ring =
                (point.original_row_id & goblin::NATIVE_HIGHLIGHT_KEY_BIT) != 0;
            const uint16_t depth =
                is_ring ? g_v3_native.next_ring_depth++ : g_v3_native.next_depth++;
            // Arm the slot BEFORE issuing: capture can fire synchronously
            // inside the Execute call.
            auto &s = g_v3_factory_slots[0];
            s = V3FactorySlot{};
            s.depth = depth;
            s.char_id = char_id;
            s.point = point;
            s.map_x = mx;
            s.map_z = mz;
            g_v3_factory_active = 1;
            const int64_t t_create0 = v3_perf_now();
            const uintptr_t node = goblin::gfx_probe::create_native_icon_instance(
                point.source_icon_id, depth, live_ctx, frame);
            if (!t_v3_pump.pumping) // stages of the BURST, like "factory"
                g_v3_open.create_qpc += v3_perf_now() - t_create0;
            bool ok = false;
            // Which of the three post-request failures happened, for the counters at the end of
            // the iteration: the logs are capped, the counters are not.
            enum class FailWhy { None, NoNode, NotMaterialized, Attach };
            FailWhy why = FailWhy::None;
            if (!v3_heap_ptr(node))
            {
                why = FailWhy::NoNode;
                static uint32_t s_no_node_logged = 0;
                if (s_no_node_logged++ < 4)
                    spdlog::warn("[v3native] request produced no timeline node "
                                 "(row={} depth={})",
                                 point.original_row_id, depth);
            }
            else
            {
                s.node = node;
                if (!s.held || !v3_heap_ptr(s.child))
                {
                    const int64_t t_mat0 = v3_perf_now();
                    const uint32_t mat_exc = v3_guarded_materialize(
                        live_ctx, static_cast<uintptr_t>(sprite));
                    if (!t_v3_pump.pumping) // stages of the BURST, like "factory"
                        g_v3_open.mat_qpc += v3_perf_now() - t_mat0;
                    if (mat_exc)
                    {
                        static bool s_mat_exc_logged = false;
                        if (!s_mat_exc_logged)
                        {
                            s_mat_exc_logged = true;
                            spdlog::warn("[v3native] materialize driver seh=0x{:08X}",
                                         mat_exc);
                        }
                    }
                }
                if (!s.held || !v3_heap_ptr(s.child))
                {
                    why = FailWhy::NotMaterialized;
                    static uint32_t s_uncaptured_logged = 0;
                    if (s_uncaptured_logged++ < 4)
                        spdlog::warn("[v3native] record not materialized by driver "
                                     "(row={} depth={})",
                                     s.point.original_row_id, s.depth);
                }
                else if (s.root != target_movie)
                {
                    // s.root now carries the CHILD's movie view (captured in the place
                    // detour), and target_movie is nonzero - the pulse-top validation
                    // already returned otherwise. A mismatch on a single item with a
                    // healthy anchor is genuinely a wrong context, not a dead anchor.
                    ++g_v3_native.wrong_contexts;
                    spdlog::warn("[v3native] child movie changed: childMovie=0x{:X} "
                                 "targetMovie=0x{:X}",
                                 s.root, target_movie);
                }
                else
                {
                    if (s.point.original_row_id & goblin::NATIVE_CLEARED_KEY_BIT)
                    {
                        for (float &m : s.basis)
                            m *= V3_BADGE_SCALE;
                        s.base_tx = s.base_tx * V3_BADGE_SCALE + V3_BADGE_OFF_X;
                        s.base_ty = s.base_ty * V3_BADGE_SCALE + V3_BADGE_OFF_Y;
                    }
                    const int64_t t_att0 = v3_perf_now();
                    const uint32_t exc = v3_guarded_attach(g_v3_native.wrapper, s.child);
                    uint64_t child_parent = 0;
                    v3_read64(s.child + 0x38, child_parent);
                    const bool attached = exc == 0 && child_parent == g_v3_native.parent;
                    // Born with its emphasis already applied: a marker that only got it on
                    // the next merge would pop a frame after appearing. (The colour offset
                    // is learned in the tick, not here - a child's render entry is not
                    // necessarily hooked up yet at the instant it is created, and the first
                    // markers get their fade from the merge a moment later.)
                    const float emph = v3_emphasis(s.point.area, s.point.gx, s.point.gz);
                    // A ring takes the emphasis SIZE but never its dimming - and this is the site
                    // that has to know it too. Missing it here wrote the dim colour straight into
                    // the child at creation while the object recorded 1.0, so the budgeted pass
                    // compared equal and never corrected it: rings came out with mismatched
                    // brightness that no later pass could touch.
                    const bool staged_ring =
                        (s.point.original_row_id & goblin::NATIVE_HIGHLIGHT_KEY_BIT) != 0;
                    const float fade =
                        staged_ring ? 1.0f : v3_fade(s.point.area, s.point.gx, s.point.gz);
                    const float cool =
                        staged_ring ? 0.0f : v3_cool(s.point.area, s.point.gx, s.point.gz);
                    if (fade != 1.0f || cool != 0.0f)
                        v3_write_cxform(s.child, fade, cool);
                    const bool positioned = attached &&
                        v3_position_child(s.child,
                                          s.point.visible ? s.map_x : V3_HIDDEN_MAP_POS,
                                          s.point.visible ? s.map_z : V3_HIDDEN_MAP_POS,
                                          s.base_tx, s.base_ty, s.basis,
                                          g_v3_native.cur_fx * emph,
                                          g_v3_native.cur_fy * emph);
                    if (!t_v3_pump.pumping) // stages of the BURST, like "factory"
                        g_v3_open.attach_qpc += v3_perf_now() - t_att0;
                    if (!positioned)
                    {
                        why = FailWhy::Attach;
                        spdlog::warn("[v3native] attach failed: seh=0x{:08X} "
                                     "parent=0x{:X} expected=0x{:X}",
                                     exc, child_parent, g_v3_native.parent);
                    }
                    else
                        ok = true;
                }
            }
            if (s.held && v3_heap_ptr(s.child))
            {
                if (goblin::variants::kViewportWindow && ok)
                {
                    // Lever B: KEEP our reference so the child survives while it is
                    // DETACHED (out of view). Without it, detaching (which drops the
                    // parent's reference) would free the child. The manager (obj.child)
                    // now owns this reference; it is released only by movie teardown.
                    s.held = false;
                }
                else
                {
                    uint32_t rb = 0, ra = 0;
                    v3_drop_held_ref(s.child, rb, ra);
                    s.held = false;
                }
            }
            if (ok)
            {
                V3NativeObject obj{};
                obj.child = s.child;
                obj.map_x = s.map_x;
                obj.map_z = s.map_z;
                obj.base_tx = s.base_tx;
                obj.base_ty = s.base_ty;
                memcpy(obj.base_m, s.basis, sizeof(obj.base_m));
                obj.area = s.point.area;
                obj.gx = s.point.gx;
                obj.gz = s.point.gz;
                obj.is_ring =
                    (s.point.original_row_id & goblin::NATIVE_HIGHLIGHT_KEY_BIT) != 0;
                obj.emph = v3_emphasis(obj.area, obj.gx, obj.gz);
                obj.fade = obj.is_ring ? 1.0f : v3_fade(obj.area, obj.gx, obj.gz);
                obj.cool = obj.is_ring ? 0.0f : v3_cool(obj.area, obj.gx, obj.gz);
                obj.visible = s.point.visible;
                // We kept the build reference above iff B was on -> this child may be
                // safely detached/re-attached by the viewport reconcile.
                obj.ref_held = goblin::variants::kViewportWindow;
                g_v3_native.last_progress_ms = GetTickCount64();
                g_v3_native.by_row[s.point.original_row_id] =
                    g_v3_native.objects.size();
                g_v3_native.objects.push_back(obj);
                goblin::map_api::record_depth(s.point.original_row_id, s.depth);
                const size_t created = g_v3_native.objects.size();
                // The COMPLETION line always prints (one per rebuild, the field-report
                // anchor for "markers vanished mid-session"); the start/progress ticks
                // only under debug_logging - a highlight-toggling session emitted 114
                // start lines and they all said the same thing (2026-08-31).
                if (g_v3_native.pending_index >= g_v3_native.pending.size() ||
                    ((created == 1 || created % 256 == 0) && goblin::config::debugLogging))
                    spdlog::info("[v3native] category progress: created={}/{} "
                                 "failed={} lastRow={}{} srcIconId={} "
                                 "map=({:.1f},{:.1f})",
                                 created, g_v3_native.pending.size(),
                                 g_v3_native.failed,
                                 s.point.original_row_id &
                                     ~goblin::NATIVE_CLEARED_KEY_BIT,
                                 (s.point.original_row_id &
                                  goblin::NATIVE_CLEARED_KEY_BIT)
                                     ? "(badge)"
                                     : "",
                                 s.point.source_icon_id, s.map_x, s.map_z);
            }
            else
            {
                ++g_v3_native.failed;
                switch (why)
                {
                case FailWhy::NoNode: ++g_v3_native.failed_nonode; break;
                case FailWhy::NotMaterialized: ++g_v3_native.failed_material; break;
                case FailWhy::Attach: ++g_v3_native.failed_attach; break;
                default: break;
                }
            }
            if (v3_heap_ptr(node))
                goblin::gfx_probe::remove_native_icon_record(depth, live_ctx, frame);
            s = V3FactorySlot{};
            g_v3_factory_active = 0;
        }
        g_v3_native.in_factory = false;
    }

    // Lever B (variants::kViewportWindow): keep out-of-view markers DETACHED so they carry
    // no TreeCacheNode; attach only those within the visible map rect (+ hysteresis).
    // Runs once per map frame on the map UI thread; bounded ops/frame so a big pan
    // reconciles across a few frames instead of one spike. Detaching happens while the
    // movie is alive and rendering, so the freed render nodes recycle on normal frames
    // (async) - unlike the close-time lever C, whose nodes never get a reconcile pass.
    // A g_v3_vp_cursor "reserved for round-robin if needed" was declared here and never touched.
    // Calls vs completed passes. The function has eight early returns, so a silent straggler log
    // is ambiguous - it means either "nothing to report" or "we never got that far". These two
    // counters tell those apart, and the tick reports a reconcile that stops completing while the
    // map is open (which would freeze every detached marker in place, invisible).
    uint64_t g_recon_calls = 0;
    uint64_t g_recon_done = 0;

    void v3_viewport_reconcile()
    {
        // The config-level gate does not count as a call: with kViewportWindow off
        // (MFG_ATTACH_ALL_MARKERS=1) or the remove-at AOB unresolved, reconcile is not supposed
        // to run at all, and counting those entries would make the stall reporter warn every
        // 2 s about a pass that by construction never completes.
        if (!goblin::variants::kViewportWindow || !g_v3_remove_at)
            return;
        ++g_recon_calls;
        if (!g_v3_native.seeded || g_v3_native.objects.empty() ||
            g_v3_map_closed.load(std::memory_order_relaxed))
            return;
        // Stricter than the tick's gate: reconcile drives engine attach and remove-at, so it needs
        // the dialog enabled and not closing, not merely existing. Leaves o.attached alone when
        // shut, so the next drivable frame resumes instead of rebuilding.
        if (!v3_map_drivable())
            return;
        const uintptr_t wrapper = g_v3_native.wrapper;
        const uintptr_t parent = g_v3_native.parent;
        if (!v3_heap_ptr(wrapper) || !v3_heap_ptr(parent))
            return;
        // Parent-liveness gate. A freed and zeroed parent still passes v3_heap_ptr, and even
        // wrapper+0x18 == parent still holds (that field lives in the wrapper, not in the freed
        // block), so the structural checks alone would let us relink a DEAD container and corrupt
        // the heap. The parent's own vtable slot is the reliable death signal: it reads 0 once
        // freed. Require it non-zero and matching this generation before any engine detach or
        // attach touches the container.
        // ... and that vtable test is NOT enough on its own. It catches a freed block (slot reads 0)
        // but not a freed and REUSED one, where the new owner's vtable sits in the slot. Attaching
        // into such a container is a WRITE into somebody else's object, which is how the heap gets
        // corrupted with no fault at the scene: report 19's four minidumps all fault later, inside
        // Scaleform's display-list and pool code, one of them on an object reading the freed-memory
        // poison 0x5555555555555550. So the movie the parent belonged to when we seeded must still
        // be the movie it reports now: that identity is written once in the node's ctor and is not
        // cleared on detach, so a reused or detached block cannot fake it. Same test the anchor
        // paths already use - reconcile was the one per-frame engine mutation left outside it.
        {
            // Gate BEFORE the vtable read, not after: v3_node_detached below does gate, but this
            // read got there first and is the same per-frame dead-anchor case as the tick's.
            uint64_t pvt = 0;
            if (v3_node_unusable(parent) || !v3_read64(parent, pvt) || pvt == 0 ||
                (g_v3_native.parent_vtable != 0 && pvt != g_v3_native.parent_vtable))
                return;
            if (v3_node_detached(parent))
                return;
            const uintptr_t movie = v3_movie_of(parent);
            if (movie == 0 ||
                (g_v3_native.parent_movie != 0 && movie != g_v3_native.parent_movie))
                return;
        }

        goblin::mapproject::MapView view{};
        if (!goblin::mapproject::read_view(view) || !(view.zoom > 0.01f))
            return;

        // Visible map-space rect: screen_x in [0,w] <=> |map_x - cU| <= (1920/2)/zoom
        // (the client-size factor cancels, see mapproject::project). MARGIN (virtual px)
        // is hysteresis so pan jitter near an edge does not thrash attach/detach.
        const float cU = (view.panX + view.snapMidX) / view.zoom;
        const float cV = (view.panZ + view.snapMidZ) / view.zoom;
        constexpr float MARGIN = 400.0f;
        const float halfX = (960.0f + MARGIN) / view.zoom;
        const float halfZ = (540.0f + MARGIN) / view.zoom;

        constexpr size_t DETACH_BUDGET = 1024;
        constexpr size_t ATTACH_BUDGET = 1024;

        auto in_window = [&](const V3NativeObject &o) {
            return o.visible && std::fabs(o.map_x - cU) <= halfX &&
                   std::fabs(o.map_z - cV) <= halfZ;
        };

        // Phase 1: collect out-of-view (or hidden) attached children, batch-detach them
        // via the same engine remove-at leaves lever C uses (one parent scan, descending
        // removal). Their retained reference (kept at build) keeps them alive detached.
        static std::vector<uintptr_t> det_ptrs;
        static std::vector<size_t> det_objs;
        det_ptrs.clear();
        det_objs.clear();
        for (size_t i = 0; i < g_v3_native.objects.size() &&
                           det_objs.size() < DETACH_BUDGET; ++i)
        {
            V3NativeObject &o = g_v3_native.objects[i];
            if (o.ref_held && o.attached && v3_heap_ptr(o.child) && !in_window(o))
            {
                det_ptrs.push_back(o.child);
                det_objs.push_back(i);
            }
        }
        if (!det_ptrs.empty())
        {
            std::sort(det_ptrs.begin(), det_ptrs.end());
            static std::vector<uint32_t> idxbuf;
            idxbuf.assign(det_ptrs.size(), 0);
            uint32_t faulted = 0;
            const uint32_t found = v3_detach_scan(parent, det_ptrs.data(),
                                                  static_cast<uint32_t>(det_ptrs.size()),
                                                  idxbuf.data(),
                                                  static_cast<uint32_t>(idxbuf.size()), &faulted);
            if (faulted)
                spdlog::warn("[v3native] viewport detach: child-vector scan faulted, removing nothing "
                             "(the vector moved under us; {} markers stay attached this pass).",
                             det_objs.size());
            uint32_t skipped = 0;
            v3_detach_remove(wrapper, parent, idxbuf.data(), found, det_ptrs.data(),
                             static_cast<uint32_t>(det_ptrs.size()), &skipped);
            if (skipped)
                spdlog::warn("[v3native] viewport detach: {} of {} indices no longer pointed at our "
                             "children and were SKIPPED (the child vector moved between scan and "
                             "removal; removing them would have freed engine-owned objects).",
                             skipped, found);
            for (size_t j : det_objs)
                g_v3_native.objects[j].attached = false;
        }

        // Phase 2: attach in-view children that are currently detached (append = on top).
        size_t attached_now = 0;
        for (size_t i = 0; i < g_v3_native.objects.size() &&
                           attached_now < ATTACH_BUDGET; ++i)
        {
            V3NativeObject &o = g_v3_native.objects[i];
            if (!o.ref_held || o.attached || !v3_heap_ptr(o.child) || !in_window(o))
                continue;
            const uint32_t exc = v3_guarded_attach(wrapper, o.child);
            uint64_t child_parent = 0;
            v3_read64(o.child + 0x38, child_parent);
            if (exc == 0 && child_parent == parent)
            {
                v3_position_child(o.child, o.map_x, o.map_z, o.base_tx, o.base_ty,
                                  o.base_m, v3_obj_fx(o), v3_obj_fy(o));
                o.attached = true;
                o.ever_attached = true;
                ++attached_now;
            }
        }

        // STRAGGLERS: markers the snapshot says are visible AND inside the window, yet not linked
        // into the parent - i.e. exactly the reported symptom, "some icons draw, some do not, but
        // the popup over the missing ones still works" (the popup reads the marker ROW, which is
        // fine either way, so it proves nothing about the child). A straggler is normal for a
        // frame or two while the budgets catch up; one that persists is the defect. Reported at
        // most once a second, and only while non-zero, so a healthy map stays silent.
        ++g_recon_done;
        // DO WE STILL OWN WHAT WE THINK WE OWN? `attached` is written once, at the moment the
        // engine accepted the child, and never re-checked. If the engine later evicts a child -
        // and `InsertChildAtDepth` DOES evict whatever already sits at the depth being written -
        // our record keeps saying "attached", the reconcile therefore skips it, and the marker is
        // gone from the screen while its row still answers the hover popup. That is the reported
        // symptom exactly, and it should hit Convergence hardest: its own container holds ~5620
        // children against vanilla's ~1000, so there is far more to collide with. A rotating
        // slice of 512 per audit keeps the cost flat; a full rotation takes obj_n/512 audits
        // at 1 Hz (~17 s on Convergence's 8424), NOT one second - read the sampled counts
        // accordingly. The cursor advances once per AUDIT, after all three sampled loops, so
        // the eviction, matrix and render-node numbers in one report describe one slice.
        // THE SAMPLING LOOPS BELOW ARE 1 Hz, NOT PER FRAME. They read engine memory and one of
        // them calls the engine's own matrix getter; running them every pass while reporting
        // once a second meant doing the work sixty times to print it once. Measured cost of that
        // mistake on Convergence: our idle per-frame time went from ~7 ms/s to 27-64 ms/s with
        // 18-23 ms single-frame spikes - a diagnostic that was itself the slowdown being
        // diagnosed.
        //
        // The straggler scan further down is gated too, and the first attempt at this got that
        // wrong: it was left per-pass on the reasoning that it "drives a decision", when in fact
        // its only consumer is a warning. It is a third full walk of all 8424 objects every
        // frame, and leaving it in put the idle cost UP - 53-88 ms/s against the 27-64 it was
        // meant to cure, even though the per-frame spikes did fall. Its two-second persistence
        // rule works just as well on one sample a second.
        // DEBUG ONLY since 2026-08-07. Everything this flag gates is diagnostics - three sampling
        // loops of 512 objects each (one of them calling GetMatrix through the child's vtable), a
        // walk of ALL tracked objects for the straggler counts, and a walk of the parent's child
        // array of up to 16384 entries at TWO guarded reads apiece. Nothing downstream acts on any
        // of it; the counters feed spdlog and nothing else, and the pass that does the real work
        // (detach, then attach) has already run above.
        //
        // It was running in shipping builds, once a second, for the whole time the map was open,
        // and it is the stutter two players reported (34, 35) and the tester reproduced: hold W+D
        // over the open map and a hitch lands once per second of movement. Their three
        // observations pin it - the rate does not follow the zoom, does not follow how many icons
        // are on screen (these loops walk every tracked object and the engine's whole child list,
        // not the visible ones), and it needs the map to be MOVING. That last one fits the
        // parent-list walk in particular: `break` on the first unreadable entry makes an idle,
        // settled list cheap and a churning one expensive, because while scrolling the entries
        // stay valid all the way to the end. Observed list sizes in those logs: 1011 to 7903.
        const uint64_t audit_now = GetTickCount64();
        static uint64_t s_next_audit = 0;
        const bool audit = goblin::config::debugLogging && audit_now >= s_next_audit;
        if (audit)
            s_next_audit = audit_now + 1000;

        static size_t s_verify_cursor = 0;
        size_t evicted = 0, checked = 0;
        const size_t obj_n = g_v3_native.objects.size();
        for (size_t n = 0; audit && obj_n && n < 512 && n < obj_n; ++n)
        {
            const V3NativeObject &o = g_v3_native.objects[(s_verify_cursor + n) % obj_n];
            if (!o.attached || !v3_heap_ptr(o.child))
                continue;
            ++checked;
            uint64_t cp = 0;
            if (!v3_read64(o.child + 0x38, cp) || cp != parent)
                ++evicted;
        }

        // ── THE RENDER CONTAINER, which is NOT the logical child list ────────────────────
        // Everything we have checked so far - `attached`, the parent back-pointer, the straggler
        // count - reads the LOGICAL list at parent+0xd8. Drawing walks a different structure: a
        // child's render node lives at child+0x48 and knows its container at node+0x30. A child
        // can sit correctly in the logical list, satisfy every check we have, and still never be
        // drawn. Two independent audits landed here from opposite directions (2026-08-05): the
        // render index is computed by scanning siblings BACKWARD, so it degrades with sibling
        // count - Convergence has ~5620 engine children against vanilla's ~1000 - and the attach
        // path writes child+0x2c = -1, which makes the engine give every one of our children the
        // SAME render slot with only an incrementing sub-index to tell them apart.
        //
        // So: sample the child side (does a render node exist, does it know a container), and
        // walk the parent's array for the shape of the slot assignment (the longest run of one
        // repeated render slot, and the highest sub-index reached). Both bounded, once a second.
        // ── READ THE TRANSFORM BACK ──────────────────────────────────────────────────────
        // Everything structural has now been refuted by measurement: the child is in the logical
        // list, under our parent, with a render node that knows its container. What has never
        // been checked is the one thing that decides whether a correctly-parented child puts
        // pixels on screen - the matrix actually sitting in it. We write it and trust the write.
        //
        // Read it back through the same vtable getter `v3_position_child` uses (slot +0x10) and
        // classify: a collapsed 2x2 (m0/m5 ~ 0) draws nothing at any position, and a translation
        // near the park coordinate means the child is still sitting where a hidden marker is put
        // (V3_HIDDEN_MAP_POS * 20 twips) although our bookkeeping calls it visible. Both are
        // silent today. Sampled with the same rotating cursor, so the cost stays flat.
        size_t zero_scale = 0, parked_pos = 0, matrix_unreadable = 0;
        {
            using GetMatrixFn = const float *(void *);
            for (size_t n = 0; audit && obj_n && n < 512 && n < obj_n; ++n)
            {
                const V3NativeObject &o = g_v3_native.objects[(s_verify_cursor + n) % obj_n];
                if (!o.visible || !o.attached || !v3_heap_ptr(o.child))
                    continue;
                uint64_t vt = 0, get_addr = 0;
                // Same dead-generation guard v3_position_child and v3_write_cxform carry: a child
                // our own reference kept alive across a close, whose block the engine freed and
                // reused, passes every heap check - and a get_addr harvested from such a block is
                // not a function. The signature is already adopted by the time anything is
                // attached, so a mismatch here is a dead generation, never a first child.
                if (!v3_read64(o.child, vt) || vt == 0 ||
                    (g_v3_native.child_vtable != 0 && vt != g_v3_native.child_vtable) ||
                    !v3_read64(static_cast<uintptr_t>(vt) + 0x10, get_addr) ||
                    !v3_heap_ptr(get_addr))
                {
                    ++matrix_unreadable;
                    continue;
                }
                float m[8]{};
                if (!v3_matrix_readback(reinterpret_cast<void *>(o.child), get_addr, m))
                {
                    ++matrix_unreadable;
                    continue;
                }
                const float sx = m[0] < 0 ? -m[0] : m[0];
                const float sy = m[5] < 0 ? -m[5] : m[5];
                if (sx < 1e-4f && sy < 1e-4f)
                    ++zero_scale;
                // The park position is V3_HIDDEN_MAP_POS in map units, times the 20 twips the
                // transform uses; anything within a screen of it is parked, not merely far away.
                const float parked = V3_HIDDEN_MAP_POS * 20.0f;
                if (m[3] < parked + 100000.0f && m[3] > parked - 100000.0f)
                    ++parked_pos;
            }
        }

        size_t no_render_node = 0, node_no_container = 0;
        for (size_t n = 0; audit && obj_n && n < 512 && n < obj_n; ++n)
        {
            const V3NativeObject &o = g_v3_native.objects[(s_verify_cursor + n) % obj_n];
            if (!o.attached || !v3_heap_ptr(o.child))
                continue;
            uint64_t rnode = 0;
            if (!v3_read64(o.child + 0x48, rnode) || !v3_heap_ptr(rnode))
            {
                ++no_render_node;
                continue;
            }
            uint64_t cont = 0;
            if (!v3_read64(static_cast<uintptr_t>(rnode) + 0x30, cont) || !v3_heap_ptr(cont))
                ++node_no_container;
        }
        // Rotate the slice once per audit, only now that every sampled loop has read it. The
        // first version advanced here-and-earlier on EVERY reconcile pass (60 Hz against the
        // loops' 1 Hz), so consecutive audits sampled pseudo-random slices instead of rotating,
        // and the matrix/render loops ran one slice ahead of the eviction loop whose `checked`
        // they were reported against.
        if (audit && obj_n)
            s_verify_cursor = (s_verify_cursor + 512) % obj_n;

        size_t stragglers = 0, want = 0, lost = 0, unreachable = 0;
        for (size_t i = 0; audit && i < g_v3_native.objects.size(); ++i)
        {
            const V3NativeObject &o = g_v3_native.objects[i];
            if (in_window(o))
            {
                ++want;
                if (!o.attached)
                {
                    ++stragglers;
                    if (o.ever_attached)
                        ++lost;          // we DID attach it; something took it back out
                    if (!o.ref_held || !v3_heap_ptr(o.child))
                        ++unreachable;   // phase 2 skips these, so they can never come back
                }
            }
        }
        // ONLY report a set that STAYS stuck. The first version logged the first non-zero count
        // each second and so reported `4098 of 5122` over and over - which turned out to be
        // 5122 - 1024, i.e. the FIRST pass after every seed, with the map being reopened every
        // 2-3 seconds. That is the budget doing its job, not a defect, and the rate limit hid
        // the passes that follow. A straggler set is normal for a few frames after a build; only
        // one that survives two continuous seconds means the markers are never coming back.
        static uint64_t s_straggler_since = 0;
        static uint64_t s_next_view_log = 0;
        const uint64_t now_view = GetTickCount64();

        // Slot-assignment shape, once a second: one pass over the parent's array (stride 0x10,
        // slot+8 = render slot, slot+0xc = sub-index). No pointer matching needed - if all our
        // children really do land in one render slot, it shows up as a run of thousands of
        // identical slot values with the sub-index climbing alongside.
        {
            // Same 1 Hz gate as the sampling above, so the numbers reported here and the work
            // done to produce them always belong to the same pass.
            if (audit)
            {
                uint64_t base = 0, cnt = 0;
                if (v3_read64(parent + 0xd8, base) && v3_read64(parent + 0xe0, cnt) &&
                    v3_heap_ptr(base) && cnt && cnt < 32768)
                {
                    uint32_t prev_slot = 0xFFFFFFFFu, run = 0, best_run = 0, max_sub = 0;
                    const uint64_t walk = cnt < 16384 ? cnt : 16384;
                    for (uint64_t i = 0; i < walk; ++i)
                    {
                        uint32_t slot = 0, sub = 0;
                        const uintptr_t e = static_cast<uintptr_t>(base) + i * 0x10;
                        if (!v3_read32(e + 8, slot) || !v3_read32(e + 0xc, sub))
                            break;
                        run = (slot == prev_slot) ? run + 1 : 1;
                        prev_slot = slot;
                        if (run > best_run)
                            best_run = run;
                        // Unassigned entries carry -1 as the sub-index; counting them pins the
                        // metric at 4294967295 and hides every real value.
                        if (sub != 0xFFFFFFFFu && sub > max_sub)
                            max_sub = sub;
                    }
                    spdlog::info("[v3render] parent list {} entries; longest run of one render "
                                 "slot = {}; highest sub-index = {}; of {} sampled children {} "
                                 "have no render node, {} have one with no container; of the "
                                 "VISIBLE sampled: {} collapsed to zero scale, {} still parked "
                                 "offscreen, {} unreadable",
                                 cnt, best_run, max_sub, checked, no_render_node,
                                 node_no_container, zero_scale, parked_pos, matrix_unreadable);
                }
            }
        }
        // Report an eviction the moment it is seen: unlike a straggler set, this never resolves
        // on its own - nothing re-checks these children, so they stay off screen until the map
        // is rebuilt. Rate-limited only so a large sweep does not flood.
        if (evicted)
        {
            static uint64_t s_next_evict_log = 0;
            if (now_view >= s_next_evict_log)
            {
                s_next_evict_log = now_view + 2000;
                spdlog::warn("[v3view] {} of {} sampled children we RECORD as attached are no "
                             "longer under our parent - evicted after we linked them, and "
                             "nothing re-checks them",
                             evicted, checked);
            }
        }
        // Only meaningful on an audit pass - off one, the counts above are all zero by design.
        if (audit && !stragglers)
            s_straggler_since = 0;
        else if (audit && !s_straggler_since)
            s_straggler_since = now_view;
        if (audit && stragglers && s_straggler_since &&
            now_view - s_straggler_since >= 2000 && now_view >= s_next_view_log)
        {
            s_next_view_log = now_view + 2000;
            spdlog::warn("[v3view] {} of {} in-window markers have been unattached for {} ms "
                         "({} were attached before and came back out, {} unreachable by phase 2);"
                         " detached {} / attached {} this pass, budgets {}/{}",
                         stragglers, want, now_view - s_straggler_since, lost, unreachable,
                         det_objs.size(), attached_now, DETACH_BUDGET, ATTACH_BUDGET);
        }
    }
    // v3_try_matrix_batch had no callers anywhere in src/ - its only entry condition was set by the retired
    // custom-capture path above. Removed 2026-07-30.

    // v3_try_visual_move had no callers anywhere in src/ - its only entry condition was set by the retired
    // custom-capture path above. Removed 2026-07-30.


#if MFG_STALL_PROFILER
    // The recorder behind the insert-core probe. Its only caller is v3_add_detour just below, which
    // is profiler-only, and the counters it feeds are declared under the same guard at the top of
    // the file - so it belongs inside it too.
    void v3_note_add(void *vector_ptr, void *parent_ptr, void *index_arg,
                     void *child_ptr, uintptr_t ret, uint64_t before_count)
    {
        const uintptr_t vector = reinterpret_cast<uintptr_t>(vector_ptr);
        const uintptr_t parent = reinterpret_cast<uintptr_t>(parent_ptr);
        uint64_t after_count = 0;
        if (!v3_heap_ptr(vector) || !v3_read64(vector + 8, after_count) || after_count > 100000)
            return;

        g_v3_add_calls.fetch_add(1, std::memory_order_relaxed);
        if (after_count > before_count)
            g_v3_add_grew.fetch_add(1, std::memory_order_relaxed);
        else if (after_count == before_count)
            g_v3_add_same.fetch_add(1, std::memory_order_relaxed);

        const bool owner_match = v3_heap_ptr(parent) && vector == parent + 0xd8;
        if (owner_match)
            g_v3_add_owner_match.fetch_add(1, std::memory_order_relaxed);

        if (after_count <= g_v3_add_max_count.load(std::memory_order_relaxed) ||
            g_v3_add_lock.test_and_set(std::memory_order_acquire))
            return;
        if (after_count > g_v3_add_max_count.load(std::memory_order_relaxed))
        {
            V3AddSnapshot s{};
            s.vector = vector;
            s.parent = parent;
            s.child = reinterpret_cast<uintptr_t>(child_ptr);
            s.ret = ret;
            s.before_count = before_count;
            s.after_count = after_count;
            s.insert_index = reinterpret_cast<uintptr_t>(index_arg);
            s.owner_match = owner_match;
            v3_read64(s.child, s.child_vt);
            v3_read32(s.child + 0x2c, s.child_depth);
            g_v3_add = s;
            g_v3_add_max_count.store(after_count, std::memory_order_release);
        }
        g_v3_add_lock.clear(std::memory_order_release);
    }

    // Profiler-only: every branch of this body is gated on the capture flag, and it sits on the
    // engine's hottest path (every display-list add). Not built unless the profiler is.
    void v3_add_detour(void *vector, void *parent, void *index_arg, void *child)
    {
        // The map's native children are inserted before the first per-marker refresh call starts a
        // sampling window, so this feeds the stall profiler and is built only with it: it takes a
        // spinlock on one of the engine's hottest paths (every display-list add), which is exactly the
        // kind of cost that must not be present in a run whose timings we trust.
        const bool capture = goblin::variants::kStallProfiler && goblin::config::debugLogging;
        uint64_t before_count = 0;
        if (capture && v3_heap_ptr(reinterpret_cast<uintptr_t>(vector)))
            v3_read64(reinterpret_cast<uintptr_t>(vector) + 8, before_count);
        const uintptr_t ret = capture ? reinterpret_cast<uintptr_t>(_ReturnAddress()) : 0;
        o_v3_add(vector, parent, index_arg, child);
        if (capture)
            v3_note_add(vector, parent, index_arg, child, ret, before_count);
    }
#endif // MFG_STALL_PROFILER

    void v3_place_detour(void *vector_ptr, void *parent_ptr, void *placement_ptr,
                         void *child_ptr, uint64_t replace)
    {
        v3_note_thread("v3_place_detour (engine AddDisplayObject)");
        // Does the ENGINE add children to the very container we remove from by index, and from which
        // thread? That decides whether the scan-then-remove-by-index pass can be handed an index that
        // has since moved (which would free an engine-owned child and leave a hole where a map tile
        // was). Rate-limited to a handful of lines; reports the child count so we can also see whether
        // this container is the one that carries the map's own clips alongside our markers.
        if (parent_ptr && reinterpret_cast<uintptr_t>(parent_ptr) == g_v3_native.parent)
        {
            static std::atomic<int> shared_logs{0};
            const uint32_t tid = GetCurrentThreadId();
            const uint32_t owner = g_v3_owner_tid.load(std::memory_order_relaxed);
            if (shared_logs.fetch_add(1, std::memory_order_relaxed) < 6)
            {
                uint64_t count = 0;
                v3_read64(reinterpret_cast<uintptr_t>(parent_ptr) + 0xe0, count);
                spdlog::info("[v3native] engine adds into OUR marker parent 0x{:X}: tid {} (manager "
                             "owner tid {}), child count now {}{}",
                             reinterpret_cast<uintptr_t>(parent_ptr), tid, owner,
                             count & 0xFFFFFFFF,
                             (owner && tid != owner) ? " - CROSS-THREAD, index surgery is unsafe" : "");
            }
        }
        uint32_t char_id = 0;
        uint32_t depth = 0;
        const uintptr_t placement = reinterpret_cast<uintptr_t>(placement_ptr);
        // A `const bool custom = false;` switch stood here with a `before_count` and a `ret` it
        // gated, feeding the eight-cell discovery grid's capture tail. The grid is retired and the
        // tail was removed on 2026-07-30; the switch and its two locals outlived it, which is the
        // same compile-time-false-constant-inside-a-.cpp shape the audit was removing elsewhere.
        // Everything below this point is the PRODUCTION factory match, and it is unconditional.

        // PlaceObject::Execute only queues a 0x78 timeline record. The real
        // DisplayObject arrives here later through Sprite::AddDisplayObject,
        // where placement+0x4c/+0x50 are the decoded depth/character id. Match
        // it against the outstanding factory batch by its unique depth.
        V3FactorySlot *slot = nullptr;
        if (g_v3_factory_active != 0 &&
            v3_read32(placement + 0x4c, depth) &&
            v3_read32(placement + 0x50, char_id) && char_id != 0)
        {
            // Match by depth+charId only: materialization is normally
            // SYNCHRONOUS (fires inside create_native_icon_instance's Execute,
            // before the issuer stores the returned node), so s.node may still
            // be 0 at capture time. Default slots (depth=UINT32_MAX, charId=0)
            // can never match a real placement, and charId 0 is never ours: our
            // ids are chosen per world-map parse and 0 means "none yet" (the
            // factory skips a marker whose native_character_id() is 0).
            for (auto &s : g_v3_factory_slots)
                if (!s.child && s.depth == depth && s.char_id == char_id)
                {
                    slot = &s;
                    break;
                }
        }
        o_v3_place(vector_ptr, parent_ptr, placement_ptr, child_ptr, replace);
        if (slot)
        {
            const uintptr_t child = reinterpret_cast<uintptr_t>(child_ptr);
            // Same-movie test by MOVIE IDENTITY (v3_movie_of), not by the +0x38 up-walk
            // the old v3_parent_root did. The walk answered wrong in exactly the two
            // cases that mattered: a DETACHED target's walk terminates at itself (the
            // 2.1.1 layer-2 storm's "lv 1" roots, 14272 children rejected), and a tree
            // deeper than the 16-level cap returns a mid-chain node (a Linux player
            // report rejected every child on two "different" roots that way). pASRoot
            // survives both, so this compare is stable for attached and detached nodes
            // alike.
            const uintptr_t child_movie = v3_movie_of(child);
            const uintptr_t target_parent = g_v3_target_parent.load(std::memory_order_acquire);
            const uintptr_t target_movie =
                v3_heap_ptr(target_parent) ? v3_movie_of(target_parent) : 0;
            uint32_t refs_before = 0, refs_held = 0;
            float base_tx = 0.0f, base_ty = 0.0f;
            float basis[4] = {};
            const bool same_movie = child_movie != 0 && child_movie == target_movie;
            const bool held = same_movie && v3_hold_ref(child, refs_before, refs_held);
            const bool staged = held && v3_stage_source_child(child, base_tx, base_ty, basis);
            if (staged)
            {
                slot->child = child;
                slot->root = child_movie;
                slot->base_tx = base_tx;
                slot->base_ty = base_ty;
                memcpy(slot->basis, basis, sizeof(basis));
                slot->held = true;
            }
            else
            {
                if (held)
                {
                    uint32_t before_drop = 0, after_drop = 0;
                    v3_drop_held_ref(child, before_drop, after_drop);
                }
                ++g_v3_native.failed;
                // A zero movie means the resolve failed (unreadable node, or no pASRoot),
                // which is a different failure from two RESOLVED movies disagreeing - print
                // both raw values so the log keeps that distinction. (The old walk-root form
                // of this line printed walk levels; "lv 1" there is what proved the target
                // was detached in the 2.1.1 layer-2 storm.)
                spdlog::warn("[v3native] deferred child rejected: child=0x{:X} "
                             "charId={} depth={} movies 0x{:X}/0x{:X} held={} "
                             "staged={}",
                             child, char_id, depth, child_movie, target_movie,
                             held, staged);
                *slot = V3FactorySlot{};
                if (g_v3_factory_active)
                    --g_v3_factory_active;
            }
            return;
        }
        // The custom-capture tail stood here: it staged the factory child, filled a V3CustomSnapshot
        // and published a matrix slot. It was gated on a `custom` constant that was hard-coded false
        // (the eight-cell discovery grid is retired), so none of it could run - and with no writer,
        // the [v3custom] dump and the matrix slots it fed were dead too. Removed 2026-07-30; the
        // constant and the two locals it gated followed on 2026-07-31.
    }

    void v3_attach_detour(void *wrapper_ptr, void *child_ptr, uint32_t index)
    {
        const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
        o_v3_attach(wrapper_ptr, child_ptr, index);
        // This detour serves TWO purposes and they must not share a gate. v3_note_movie_attach is the
        // ONLY writer of the marker anchor (g_v3_target_wrapper/parent/layer/count, and the same-parent
        // clear of g_v3_map_closed), which the whole native-marker path reads - seed, tick and viewport
        // reconcile all bail when the anchor is 0. It used to sit behind debug_logging, which was
        // survivable only while native markers were themselves dev-gated: with the mechanism now
        // compiled in as the shipping default, that gate meant a normal build anchored nothing and drew
        // NO markers at all. The other two calls really are diagnostics and stay behind the log key.
        if (!goblin::variants::kNativeMarkers && !goblin::config::debugLogging)
            return;

        const uintptr_t wrapper = reinterpret_cast<uintptr_t>(wrapper_ptr);
        uint64_t parent = 0, count = 0;
        if (!v3_heap_ptr(wrapper) || !v3_read64(wrapper + 0x18, parent) ||
            !v3_heap_ptr(parent) || !v3_read64(static_cast<uintptr_t>(parent) + 0xe0, count))
            return;
        if (goblin::variants::kNativeMarkers)
            v3_note_movie_attach(wrapper, static_cast<uintptr_t>(parent), count);
        if (goblin::variants::kStallProfiler && goblin::config::debugLogging)
        {
            // Thousands of calls per map open - profiler-only, not general logging.
            v3_note_build_attach(wrapper, static_cast<uintptr_t>(parent), count);
        }
    }

    void v3_try_link(V3ContainerSnapshot &s, uintptr_t root, uintptr_t source_off,
                     uintptr_t candidate, bool indirect)
    {
        if (s.link_count >= V3_LINK_CAP || !v3_heap_ptr(candidate))
            return;
        for (uint32_t i = 0; i < s.link_count; ++i)
            if (s.link[i].candidate == candidate)
                return;

        uint64_t list = 0, count = 0, first = 0, second = 0;
        uint64_t ctx_key = 0, node_key = 0;
        uint32_t depth = 0;
        if (!v3_read64(candidate + 0x28, list) ||
            !v3_read64(candidate + 0x30, count) ||
            !v3_heap_ptr(list) || count == 0 || count >= 20000 ||
            !v3_read64(static_cast<uintptr_t>(list), first) || !v3_heap_ptr(first))
            return;
        // A PO Execute list is a dense pointer array (stride 8). The +d8/+e0
        // container we started from has stride 0x10 and normally fails here.
        if (count > 1 &&
            (!v3_read64(static_cast<uintptr_t>(list) + 8, second) || !v3_heap_ptr(second)))
            return;
        if (!v3_read64(candidate, ctx_key) || !v3_read64(static_cast<uintptr_t>(first), node_key) ||
            ctx_key == 0 || ctx_key != node_key ||
            !v3_read32(static_cast<uintptr_t>(first) + 0x14, depth) || depth > 0xffff)
            return;

        V3LinkSnapshot &out = s.link[s.link_count++];
        out.root = root;
        out.source_off = source_off;
        out.candidate = candidate;
        out.list = static_cast<uintptr_t>(list);
        out.first_node = static_cast<uintptr_t>(first);
        out.count = count;
        out.shared_key = ctx_key;
        out.first_depth = depth;
        out.indirect = indirect;
    }

    void v3_scan_links(V3ContainerSnapshot &s, uintptr_t root)
    {
        if (!v3_heap_ptr(root))
            return;
        for (uintptr_t off = 0; off <= 0x300 && s.link_count < V3_LINK_CAP; off += 8)
        {
            // Some Scaleform helper/context structures are embedded in the
            // display object; others are reached through one object field.
            v3_try_link(s, root, off, root + off, false);
            uint64_t pointed = 0;
            if (v3_read64(root + off, pointed))
                v3_try_link(s, root, off, static_cast<uintptr_t>(pointed), true);
        }
    }

    void v3_note_container(void *object, uintptr_t ret)
    {
        uintptr_t parent = reinterpret_cast<uintptr_t>(object);
        if (parent < 0x10000 || parent >= 0x7fffffffffffULL)
            return;

        uint64_t entries64 = 0, count = 0;
        if (!v3_read64(parent + 0xd8, entries64) ||
            !v3_read64(parent + 0xe0, count) ||
            entries64 < 0x10000 || entries64 >= 0x7fffffffffffULL ||
            count == 0 || count > 100000)
            return;
        if (count <= g_v3_published_count.load(std::memory_order_relaxed))
            return;
        if (g_v3_snapshot_lock.test_and_set(std::memory_order_acquire))
            return;

        if (count > g_v3_published_count.load(std::memory_order_relaxed))
        {
            V3ContainerSnapshot s{};
            s.parent = parent;
            s.entries = static_cast<uintptr_t>(entries64);
            s.ret = ret;
            s.count = count;
            v3_read64(parent, s.parent_vt);
            v3_read64(parent + 0x28, s.parent_28);
            v3_read64(parent + 0x30, s.parent_30);
            v3_read64(parent + 0xb8, s.parent_b8);
            for (int q = 0; q < 3; ++q)
                v3_read64(parent + 0x130 + static_cast<uintptr_t>(q) * 8, s.parent_name[q]);

            const uint64_t indexes[3] = {0, count / 2, count - 1};
            for (int k = 0; k < 3; ++k)
            {
                uint64_t child64 = 0;
                v3_read64(s.entries + indexes[k] * 0x10, child64);
                s.child[k] = static_cast<uintptr_t>(child64);
                uint64_t aux64 = 0;
                v3_read64(s.entries + indexes[k] * 0x10 + 8, aux64);
                s.child_aux[k] = static_cast<uintptr_t>(aux64);
                if (s.child[k] < 0x10000 || s.child[k] >= 0x7fffffffffffULL)
                    continue;
                v3_read64(s.child[k], s.child_vt[k]);
                v3_read64(s.child[k] + 0x28, s.child_28[k]);
                v3_read64(s.child[k] + 0x30, s.child_30[k]);
                for (int q = 0; q < 3; ++q)
                    v3_read64(s.child[k] + 0x130 + static_cast<uintptr_t>(q) * 8,
                              s.child_name[k][q]);
            }
            if (count >= 1000)
            {
                v3_scan_links(s, s.parent);
                for (int k = 0; k < 3; ++k)
                {
                    v3_scan_links(s, s.child[k]);
                    v3_scan_links(s, s.child_aux[k]);
                }
            }
            g_v3_container = s;
            g_v3_published_count.store(count, std::memory_order_release);
        }
        g_v3_snapshot_lock.clear(std::memory_order_release);
    }

    // ── typed-find pointer-scan accelerator ("sequential predictor") ─────────
    // At the widget-b call site (ret = widget-b entry + 0x178) the engine does a
    // pointer-find over the map widget's entry array: an O(N) scan per pin makes
    // the register burst at open and the unregister burst at close O(N^2) -
    // ~280 ms each at ~9k markers (measured 2026-07-15). Both bursts visit pins
    // in container order, so the NEXT query's entry is almost always at the last
    // hit index + 1. We pre-seed the container's own result-cache slot
    // (container[4], which the engine REVALIDATES on every call by locking the
    // entry and comparing the object pointer - see FUN_14113feb0) with that
    // guess: a correct guess returns through the engine's own validated fast
    // path, a wrong one falls back to the normal scan. Either way only engine
    // code decides the result - the predictor cannot change semantics.
    uintptr_t g_wb_find_ret = 0;  // call-site gate; 0 = accelerator off
    uintptr_t g_pred_base = 0;    // container array base we are synced to
    size_t g_pred_idx = SIZE_MAX; // index of the last confirmed hit
    uintptr_t g_pred_guess = 0;   // entry we seeded this call
    size_t g_pred_guess_idx = 0;
    std::atomic<uint64_t> g_pred_hit{0}, g_pred_miss{0};
    std::atomic<uint64_t> g_pred_gate_seen{0}; // ret matched, before the other gate terms
                                               // (diagnoses the round-4 coverage anomaly)

    // ── close-wait job spy ───────────────────────────────────────────────────
    // FUN_140e811c0 = the generic "async job done?" poll (waits on job+0x10); the
    // map UI thread sits in it ~140ms after map close. Recording (caller ret, job
    // vtable) pairs during capture windows identifies WHICH job the close waits
    // on - its vtable RVA is the key to decompiling the job body.
    Fn4 *o_jobpoll = nullptr;
    struct JobSeen
    {
        std::atomic<uintptr_t> ret{0};
        std::atomic<uintptr_t> vt{0};
        std::atomic<uint32_t> cnt{0};
        std::atomic<uint64_t> ticks{0};    // total time INSIDE the poll (incl. blocking)
        std::atomic<uint64_t> max_ticks{0}; // worst single poll - names the freeze site
    };
    JobSeen g_jobs[24];

    void record_job(uintptr_t ret, uintptr_t vt, uint64_t ticks)
    {
        size_t h = ((ret >> 4) ^ (vt >> 4)) % 24;
        for (size_t i = 0; i < 24; ++i)
        {
            JobSeen &s = g_jobs[(h + i) % 24];
            uintptr_t cur = s.ret.load(std::memory_order_relaxed);
            if (cur == 0)
            {
                uintptr_t expected = 0;
                if (!s.ret.compare_exchange_strong(expected, ret, std::memory_order_relaxed))
                {
                    cur = expected;
                }
                else
                {
                    s.vt.store(vt, std::memory_order_relaxed);
                    cur = ret;
                }
            }
            if (cur == ret && s.vt.load(std::memory_order_relaxed) == vt)
            {
                s.cnt.fetch_add(1, std::memory_order_relaxed);
                s.ticks.fetch_add(ticks, std::memory_order_relaxed);
                uint64_t prev = s.max_ticks.load(std::memory_order_relaxed);
                while (ticks > prev &&
                       !s.max_ticks.compare_exchange_weak(prev, ticks, std::memory_order_relaxed))
                {
                }
                return;
            }
        }
    }

#if MFG_STALL_PROFILER
    void *jobpoll_detour(void *a, void *b, void *c, void *d)
    {
        if (!g_count.load(std::memory_order_relaxed) || !a)
            return o_jobpoll(a, b, c, d);
        uintptr_t obj = *reinterpret_cast<uintptr_t *>(a);
        uintptr_t vt = obj ? *reinterpret_cast<uintptr_t *>(obj) : 0;
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        void *r = o_jobpoll(a, b, c, d);
        QueryPerformanceCounter(&t1);
        record_job(reinterpret_cast<uintptr_t>(_ReturnAddress()), vt,
                   static_cast<uint64_t>(t1.QuadPart - t0.QuadPart));
        return r;
    }
#endif // MFG_STALL_PROFILER

    void pred_seed(uintptr_t *container)
    {
        g_pred_guess = 0;
        uintptr_t base = container[0];
        size_t cnt = container[1];
        if (!base || base != g_pred_base)
        {
            g_pred_base = base;
            g_pred_idx = SIZE_MAX; // new container: resync on the first result
            return;
        }
        size_t guess = g_pred_idx + 1; // SIZE_MAX + 1 wraps to 0 = first slot
        if (guess >= cnt) return;
        uintptr_t e = *reinterpret_cast<uintptr_t *>(base + guess * 0x10);
        // Same raw guards the engine's own scan applies before locking an entry.
        if (e && (*reinterpret_cast<uint8_t *>(e + 0x6b) & 1))
        {
            container[4] = e; // engine revalidates before trusting it
            g_pred_guess = e;
            g_pred_guess_idx = guess;
        }
    }

    void pred_update(uintptr_t *container, uintptr_t result)
    {
        if (!result)
        {
            g_pred_idx = SIZE_MAX;
            return;
        }
        if (g_pred_guess && result == g_pred_guess)
        {
            g_pred_idx = g_pred_guess_idx;
            g_pred_hit.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // Miss: locate the result so the next guess is its neighbor (one O(N)
        // resync per miss keeps the burst ~O(N) overall).
        uintptr_t base = container[0];
        size_t cnt = container[1];
        g_pred_idx = SIZE_MAX;
        if (base == g_pred_base)
            for (size_t i = 0; i < cnt; ++i)
                if (*reinterpret_cast<uintptr_t *>(base + i * 0x10) == result)
                {
                    g_pred_idx = i;
                    break;
                }
        g_pred_miss.fetch_add(1, std::memory_order_relaxed);
    }

    void record_ret(int fn, uintptr_t ret)
    {
        size_t h = (ret >> 4) % RET_SLOTS;
        for (size_t i = 0; i < RET_SLOTS; ++i)
        {
            RetSlot &s = g_ret[fn][(h + i) % RET_SLOTS];
            uintptr_t cur = s.addr.load(std::memory_order_relaxed);
            if (cur == ret)
            {
                s.cnt.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (cur == 0)
            {
                uintptr_t expected = 0;
                if (s.addr.compare_exchange_strong(expected, ret, std::memory_order_relaxed))
                {
                    s.cnt.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                if (expected == ret)
                {
                    s.cnt.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            }
        }
        // table full: drop (histogram is best-effort)
    }

    template <int I> void *reg_detour(void *a, void *b, void *c, void *d)
    {
        bool accel = false;
        if constexpr (I == 3) // typed-find: sequential predictor at the gated site
        {
            if (g_wb_find_ret &&
                reinterpret_cast<uintptr_t>(_ReturnAddress()) == g_wb_find_ret)
            {
                g_pred_gate_seen.fetch_add(1, std::memory_order_relaxed);
                if (goblin::variants::kFastMapOpen && a)
                {
                    accel = true;
                    pred_seed(reinterpret_cast<uintptr_t *>(a));
                }
            }
        }
        if (!g_count.load(std::memory_order_relaxed))
        {
            void *r = o_reg[I](a, b, c, d);
            if constexpr (I == 3)
                if (accel)
                    pred_update(reinterpret_cast<uintptr_t *>(a),
                                reinterpret_cast<uintptr_t>(r));
            return r;
        }
        record_ret(I, reinterpret_cast<uintptr_t>(_ReturnAddress()));
        if constexpr (I == 4)
            v3_note_container(a, reinterpret_cast<uintptr_t>(_ReturnAddress()));
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        void *r = o_reg[I](a, b, c, d);
        QueryPerformanceCounter(&t1);
        if constexpr (I == 3)
            if (accel)
                pred_update(reinterpret_cast<uintptr_t *>(a), reinterpret_cast<uintptr_t>(r));
        uint64_t dt = static_cast<uint64_t>(t1.QuadPart - t0.QuadPart);
        g_reg_calls[I].fetch_add(1, std::memory_order_relaxed);
        g_reg_ticks[I].fetch_add(dt, std::memory_order_relaxed);
        uint64_t prev = g_reg_max[I].load(std::memory_order_relaxed);
        while (dt > prev &&
               !g_reg_max[I].compare_exchange_weak(prev, dt, std::memory_order_relaxed))
        {
        }
        return r;
    }

    void counters_reset()
    {
        g_v3_published_count.store(0, std::memory_order_relaxed);
        g_v3_container = {};
        for (int i = 0; i < N_REG; ++i)
        {
            g_reg_calls[i].store(0, std::memory_order_relaxed);
            g_reg_ticks[i].store(0, std::memory_order_relaxed);
            g_reg_max[i].store(0, std::memory_order_relaxed);
            for (auto &s : g_ret[i])
            {
                s.addr.store(0, std::memory_order_relaxed);
                s.cnt.store(0, std::memory_order_relaxed);
            }
        }
    }

    void counters_log(const std::string &tag, uintptr_t exe_base)
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        for (int i = 0; i < N_REG; ++i)
        {
            uint64_t calls = g_reg_calls[i].load(std::memory_order_relaxed);
            if (!calls) continue;
            double ms = double(g_reg_ticks[i].load(std::memory_order_relaxed)) * 1000.0 /
                        double(f.QuadPart);
            double max_ms = double(g_reg_max[i].load(std::memory_order_relaxed)) * 1000.0 /
                            double(f.QuadPart);
            std::vector<std::pair<uint32_t, uintptr_t>> tops;
            for (auto &s : g_ret[i])
                if (s.addr.load(std::memory_order_relaxed))
                    tops.push_back({s.cnt.load(std::memory_order_relaxed),
                                    s.addr.load(std::memory_order_relaxed)});
            std::sort(tops.rbegin(), tops.rend());
            std::string line;
            for (size_t k = 0; k < tops.size() && k < 6; ++k)
            {
                char buf[64];
                snprintf(buf, sizeof(buf), "exe+0x%llX x%u  ",
                         (unsigned long long)(tops[k].second - exe_base), tops[k].first);
                line += buf;
            }
            spdlog::info("[stallprobe] {}: {} = {} calls / {:.1f} ms (max {:.1f}); ret: {}",
                         tag, REG_NAME[i], calls, ms, max_ms, line);
        }

        uint64_t v3_count = g_v3_published_count.load(std::memory_order_acquire);
        if (v3_count)
        {
            const V3ContainerSnapshot &s = g_v3_container;
            spdlog::info("[v3tree] {}: busiest parent=0x{:X} entries=0x{:X} count={} ret=exe+0x{:X}",
                         tag, s.parent, s.entries, s.count,
                         s.ret >= exe_base ? s.ret - exe_base : s.ret);
            spdlog::info("[v3tree] parent vt=0x{:X} +28=0x{:X} +30=0x{:X} +b8=0x{:X} "
                         "name130={:016X} {:016X} {:016X}",
                         s.parent_vt, s.parent_28, s.parent_30, s.parent_b8,
                         s.parent_name[0], s.parent_name[1], s.parent_name[2]);
            static const char *const which[3] = {"first", "middle", "last"};
            for (int k = 0; k < 3; ++k)
                spdlog::info("[v3tree] {} child=0x{:X} aux=0x{:X} vt=0x{:X} +28=0x{:X} +30=0x{:X} "
                             "name130={:016X} {:016X} {:016X}",
                             which[k], s.child[k], s.child_aux[k], s.child_vt[k],
                             s.child_28[k], s.child_30[k],
                             s.child_name[k][0], s.child_name[k][1], s.child_name[k][2]);
            spdlog::info("[v3link] {}: {} PO-context candidate(s) under count={} container",
                         tag, s.link_count, s.count);
            for (uint32_t i = 0; i < s.link_count; ++i)
            {
                const V3LinkSnapshot &l = s.link[i];
                spdlog::info("[v3link] root=0x{:X} {}+0x{:X} -> ctx=0x{:X} "
                             "list=0x{:X} count={} first=0x{:X} depth={} key=0x{:X}",
                             l.root, l.indirect ? "ptr" : "embedded", l.source_off,
                             l.candidate, l.list, l.count, l.first_node,
                             l.first_depth, l.shared_key);
            }
        }

#if MFG_STALL_PROFILER
        // Drains the insert-core counters. Same guard as the counters and the recorder: without it
        // this gated on g_v3_add_calls, which is permanently 0 in a default build, so the block was
        // unreachable while its two format strings still shipped.
        const uint64_t add_calls = g_v3_add_calls.exchange(0, std::memory_order_relaxed);
        if (add_calls)
        {
            while (g_v3_add_lock.test_and_set(std::memory_order_acquire))
                _mm_pause();
            const V3AddSnapshot s = g_v3_add;
            g_v3_add = {};
            g_v3_add_max_count.store(0, std::memory_order_relaxed);
            g_v3_add_lock.clear(std::memory_order_release);
            spdlog::info("[v3add] {} since previous report: {} calls, grew={} same={} owner-match={}; "
                         "max {}->{} vector=0x{:X} parent=0x{:X} match={} ret=exe+0x{:X}",
                         tag, add_calls,
                         g_v3_add_grew.exchange(0, std::memory_order_relaxed),
                         g_v3_add_same.exchange(0, std::memory_order_relaxed),
                         g_v3_add_owner_match.exchange(0, std::memory_order_relaxed),
                         s.before_count, s.after_count, s.vector, s.parent, s.owner_match,
                         s.ret >= exe_base ? s.ret - exe_base : s.ret);
            spdlog::info("[v3add] insert-index={}; child=0x{:X} vt=0x{:X} child-depth={}",
                         s.insert_index,
                         s.child, s.child_vt, s.child_depth);
        }
#endif // MFG_STALL_PROFILER

        // The [v3custom] and [v3visual] dumps stood here. Both were gated on state that only the
        // retired custom-capture path wrote, so neither could ever print a line.
    }

// The stall sampler is COMPILED OUT unless MFG_STALL_PROFILER is set. It is the only user of
// CreateToolhelp32Snapshot / Thread32First / Thread32Next / OpenThread / SuspendThread /
// ResumeThread / Get/SetThreadContext in this file, so leaving it behind a runtime flag kept
// that whole thread-inspection import cluster - and every [stallprobe] literal - in a shipped
// DLL that can never run it.
#if MFG_STALL_PROFILER
    struct ModRange
    {
        uintptr_t base = 0, end = 0;
    };

    ModRange module_range(const wchar_t *name)
    {
        HMODULE h = GetModuleHandleW(name);
        if (!h) return {};
        auto base = reinterpret_cast<uintptr_t>(h);
        auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
        auto *nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
        return {base, base + nt->OptionalHeader.SizeOfImage};
    }

    // First executable section (.text) of a module. Frames are only accepted from
    // here: whole-image filtering let .data globals (e.g. exe+0x3D8xxxx singleton
    // pointers sitting on the stack) pollute the caller histograms in round 1.
    ModRange text_range(const wchar_t *name)
    {
        HMODULE h = GetModuleHandleW(name);
        if (!h) return {};
        auto base = reinterpret_cast<uintptr_t>(h);
        auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
        auto *nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
        auto *sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
            if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)
                return {base + sec->VirtualAddress, base + sec->VirtualAddress + sec->Misc.VirtualSize};
        return {base, base + nt->OptionalHeader.SizeOfImage};
    }

    struct Sample
    {
        uintptr_t rip = 0;
        uintptr_t exe_frames[4] = {};
        int n_exe = 0;
    };

    void log_hist(const std::string &tag, const char *what,
                  const std::map<uintptr_t, int> &hist, uintptr_t exe_base, int total)
    {
        if (hist.empty() || !total) return;
        // top 10 by count
        std::vector<std::pair<uintptr_t, int>> v(hist.begin(), hist.end());
        std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.second > b.second; });
        std::string line;
        int n = 0;
        for (auto &[addr, cnt] : v)
        {
            if (n++ >= 10) break;
            char buf[64];
            snprintf(buf, sizeof(buf), "exe+0x%llX x%d  ",
                     (unsigned long long)(addr - exe_base), cnt);
            line += buf;
        }
        spdlog::info("[stallprobe] {}: {} ({} samples): {}", tag, what, total, line);
    }

    void run_capture(std::string tag, DWORD tid, unsigned duration_ms)
    {
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                   THREAD_QUERY_INFORMATION,
                               FALSE, tid);
        if (!th)
        {
            g_running.store(false);
            return;
        }
        ModRange exe = module_range(nullptr);
        ModRange ntdll = module_range(L"ntdll.dll");
        ModRange xtext = text_range(nullptr); // frames accepted from exe .text ONLY

        // All-thread sweep (every ~20ms): the target (map UI) thread turned out to
        // WAIT on a DLConditionSignal during the post-close stall while a worker
        // thread does the actual work - the sweep finds that worker. One thread is
        // paused at a time and only its context/stack is read while paused.
        struct SweepSample
        {
            DWORD tid;
            uintptr_t rip, frame0;
            bool active; // rip moved since this thread's previous sweep pass
        };
        std::vector<DWORD> sw_tids;
        std::vector<HANDLE> sw_handles;
        {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap != INVALID_HANDLE_VALUE)
            {
                THREADENTRY32 te{};
                te.dwSize = sizeof(te);
                DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
                if (Thread32First(snap, &te)) do
                    {
                        if (te.th32OwnerProcessID == pid && te.th32ThreadID != self &&
                            sw_tids.size() < 256)
                        {
                            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                                      THREAD_QUERY_INFORMATION,
                                                  FALSE, te.th32ThreadID);
                            if (h)
                            {
                                sw_tids.push_back(te.th32ThreadID);
                                sw_handles.push_back(h);
                            }
                        }
                    } while (Thread32Next(snap, &te));
                CloseHandle(snap);
            }
        }
        std::vector<SweepSample> sweeps;
        sweeps.reserve((duration_ms / 20 + 2) * (sw_tids.empty() ? 1 : sw_tids.size()));
        std::vector<uintptr_t> sw_last(sw_tids.size(), 0); // parked-thread filter

        counters_reset();
        g_count.store(true, std::memory_order_relaxed);

        std::vector<Sample> samples;
        samples.reserve(duration_ms + 64);

        // Scan a paused thread's stack for return addresses into the exe .text.
        auto scan_first_text_frame = [&](uintptr_t sp, uintptr_t *out, int max_out) -> int {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!sp || !VirtualQuery(reinterpret_cast<void *>(sp), &mbi, sizeof(mbi)) ||
                mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                return 0;
            uintptr_t lim = std::min<uintptr_t>(
                reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize, sp + 48 * 1024);
            int n = 0;
            for (uintptr_t p = sp; p + 8 <= lim && n < max_out; p += 8)
            {
                uintptr_t v = *reinterpret_cast<uintptr_t *>(p);
                if (v >= xtext.base && v < xtext.end) out[n++] = v;
            }
            return n;
        };

        ULONGLONG t0 = GetTickCount64();
        alignas(16) CONTEXT ctx;
        unsigned iter = 0;
        unsigned stall_run = 0; // consecutive samples with the target parked in ntdll
        while (GetTickCount64() - t0 < duration_ms)
        {
            if (SuspendThread(th) == (DWORD)-1) break;
            ctx = {};
            ctx.ContextFlags = CONTEXT_CONTROL;
            Sample s{};
            bool ok = GetThreadContext(th, &ctx) != 0;
            if (ok)
            {
                s.rip = static_cast<uintptr_t>(ctx.Rip);
                s.n_exe = scan_first_text_frame(static_cast<uintptr_t>(ctx.Rsp), s.exe_frames, 4);
            }
            ResumeThread(th);
            if (ok) samples.push_back(s);

            // Adaptive burst: while the target sits in ntdll for >20ms straight (a
            // long wait = the stall we hunt), sweep every pass so the thread doing
            // the actual work during the stall is captured densely.
            bool stalled = ok && s.rip >= ntdll.base && s.rip < ntdll.end;
            stall_run = stalled ? stall_run + 1 : 0;

            if ((iter++ % 20) == 0 || (stall_run > 20 && sweeps.size() < 400000))
            {
                for (size_t k = 0; k < sw_handles.size(); ++k)
                {
                    if (SuspendThread(sw_handles[k]) == (DWORD)-1) continue;
                    ctx = {};
                    ctx.ContextFlags = CONTEXT_CONTROL;
                    SweepSample ss{sw_tids[k], 0, 0, false};
                    if (GetThreadContext(sw_handles[k], &ctx))
                    {
                        ss.rip = static_cast<uintptr_t>(ctx.Rip);
                        // A parked thread (blocked in a wait) reports the same rip
                        // every pass; only a MOVING rip means the thread works.
                        ss.active = (ss.rip != sw_last[k]);
                        sw_last[k] = ss.rip;
                        // Only active ntdll-time threads get the (pricier) stack scan.
                        if (ss.active && ss.rip >= ntdll.base && ss.rip < ntdll.end)
                        {
                            uintptr_t f = 0;
                            if (scan_first_text_frame(static_cast<uintptr_t>(ctx.Rsp), &f, 1))
                                ss.frame0 = f;
                        }
                    }
                    ResumeThread(sw_handles[k]);
                    if (ss.rip) sweeps.push_back(ss);
                }
            }
            Sleep(1);
        }
        CloseHandle(th);
        for (HANDLE h : sw_handles) CloseHandle(h);
        g_count.store(false, std::memory_order_relaxed);

        // Aggregate. rip location tells WHERE time went (exe vs ntdll = heap ops);
        // for non-exe rips the first stack frame into the exe is the caller we
        // need for a targeted patch. "other" rips are attributed by MODULE NAME
        // (GPU driver vs other mods vs D3D runtime decides the close-freeze story).
        auto module_of = [](uintptr_t rip) -> std::string {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<void *>(rip), &mbi, sizeof(mbi)) ||
                !mbi.AllocationBase)
                return "<jit/unmapped>";
            char path[MAX_PATH] = {};
            if (!GetModuleFileNameA(reinterpret_cast<HMODULE>(mbi.AllocationBase), path,
                                    MAX_PATH))
                return "<jit/unmapped>";
            const char *base = strrchr(path, '\\');
            return base ? base + 1 : path;
        };
        int in_exe = 0, in_ntdll = 0, other = 0;
        std::map<uintptr_t, int> rip_exe, caller0, caller1;
        std::map<std::string, int> other_mods;
        for (const auto &s : samples)
        {
            if (s.rip >= exe.base && s.rip < exe.end)
            {
                in_exe++;
                rip_exe[s.rip]++;
            }
            else
            {
                if (s.rip >= ntdll.base && s.rip < ntdll.end)
                {
                    in_ntdll++;
                }
                else
                {
                    other++;
                    other_mods[module_of(s.rip)]++;
                }
                if (s.n_exe > 0) caller0[s.exe_frames[0]]++;
                if (s.n_exe > 1) caller1[s.exe_frames[1]]++;
            }
        }
        int total = static_cast<int>(samples.size());
        spdlog::info("[stallprobe] {}: {} samples over {} ms: exe {} / ntdll {} / other {}",
                     tag, total, duration_ms, in_exe, in_ntdll, other);
        if (!other_mods.empty())
        {
            std::vector<std::pair<int, std::string>> om;
            for (auto &[n, c] : other_mods) om.push_back({c, n});
            std::sort(om.rbegin(), om.rend());
            std::string line;
            for (size_t j = 0; j < om.size() && j < 6; ++j)
                line += om[j].second + " x" + std::to_string(om[j].first) + "; ";
            spdlog::info("[stallprobe] {}: other-module rip: {}", tag, line);
        }
        log_hist(tag, "exe rip hot spots", rip_exe, exe.base, in_exe);
        log_hist(tag, "ntdll-time exe callers (frame0)", caller0, exe.base, in_ntdll + other);
        log_hist(tag, "ntdll-time exe callers (frame1)", caller1, exe.base, in_ntdll + other);

        // Sweep result: which threads spent the window inside ntdll (heap/waits),
        // ranked; each with its top exe-.text callers. This is what identifies the
        // worker that performs the deferred post-close release work.
        {
            // Rank threads by ACTIVE samples only (moving rip): parked worker-pool
            // threads sit in ntdll waits with a frozen rip and are not work.
            std::map<DWORD, int> tid_act_ntdll, tid_act_exe, tid_act_other, tid_total;
            std::map<DWORD, std::map<uintptr_t, int>> tid_frames, tid_ntoff;
            std::map<std::string, int> sweep_other_mods;
            for (const auto &ss : sweeps)
            {
                tid_total[ss.tid]++;
                if (!ss.active) continue;
                if (ss.rip >= ntdll.base && ss.rip < ntdll.end)
                {
                    tid_act_ntdll[ss.tid]++;
                    tid_ntoff[ss.tid][ss.rip - ntdll.base]++;
                    if (ss.frame0) tid_frames[ss.tid][ss.frame0]++;
                }
                else if (ss.rip >= exe.base && ss.rip < exe.end)
                {
                    tid_act_exe[ss.tid]++;
                }
                else
                {
                    tid_act_other[ss.tid]++;
                    sweep_other_mods[module_of(ss.rip)]++;
                }
            }
            auto top_of = [](std::map<uintptr_t, int> &m, const char *pfx, uintptr_t rebase) {
                std::vector<std::pair<int, uintptr_t>> v;
                for (auto &[a, c] : m) v.push_back({c, a});
                std::sort(v.rbegin(), v.rend());
                std::string s;
                for (size_t j = 0; j < v.size() && j < 3; ++j)
                {
                    char b[48];
                    snprintf(b, sizeof(b), "%s+0x%llX x%d ", pfx,
                             (unsigned long long)(v[j].second - rebase), v[j].first);
                    s += b;
                }
                return s;
            };
            std::map<DWORD, int> tid_act_all;
            for (auto &[t, c] : tid_act_ntdll) tid_act_all[t] += c;
            for (auto &[t, c] : tid_act_exe) tid_act_all[t] += c;
            for (auto &[t, c] : tid_act_other) tid_act_all[t] += c;
            std::vector<std::pair<int, DWORD>> rank;
            for (auto &[t, c] : tid_act_all) rank.push_back({c, t});
            std::sort(rank.rbegin(), rank.rend());
            std::string line;
            int shown = 0;
            for (auto &[cnt, t] : rank)
            {
                if (shown++ >= 6) break;
                char buf[240];
                snprintf(buf, sizeof(buf),
                         "tid %lu act %d (nt %d exe %d oth %d) of %d [%s| %s]; ",
                         (unsigned long)t, cnt, tid_act_ntdll[t], tid_act_exe[t],
                         tid_act_other[t], tid_total[t],
                         top_of(tid_ntoff[t], "nt", 0).c_str(),
                         top_of(tid_frames[t], "exe", exe.base).c_str());
                line += buf;
            }
            spdlog::info("[stallprobe] {}: sweep {} threads (active only): {}", tag,
                         sw_tids.size(), line.empty() ? "no active threads" : line);
            if (!sweep_other_mods.empty())
            {
                std::vector<std::pair<int, std::string>> om;
                for (auto &[n, c] : sweep_other_mods) om.push_back({c, n});
                std::sort(om.rbegin(), om.rend());
                std::string ml;
                for (size_t j = 0; j < om.size() && j < 6; ++j)
                    ml += om[j].second + " x" + std::to_string(om[j].first) + "; ";
                spdlog::info("[stallprobe] {}: sweep other-module rip: {}", tag, ml);
            }
        }

        counters_log(tag, exe.base);
        // Sequential-predictor effectiveness (accumulated since the last window).
        {
            uint64_t ph = g_pred_hit.exchange(0, std::memory_order_relaxed);
            uint64_t pm = g_pred_miss.exchange(0, std::memory_order_relaxed);
            uint64_t gs = g_pred_gate_seen.exchange(0, std::memory_order_relaxed);
            if (ph + pm + gs)
                spdlog::info("[stallprobe] {}: find-predictor {} hits / {} misses / {} gate-seen",
                             tag, ph, pm, gs);
        }
        // Job-poll spy: which async jobs were waited on during this window and for
        // how long. The site with the big max is the one the close-freeze blocks in.
        {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            const double to_ms = 1000.0 / double(f.QuadPart);
            std::string line;
            for (auto &s : g_jobs)
            {
                uintptr_t r = s.ret.exchange(0, std::memory_order_relaxed);
                uintptr_t v = s.vt.exchange(0, std::memory_order_relaxed);
                uint32_t c = s.cnt.exchange(0, std::memory_order_relaxed);
                uint64_t tk = s.ticks.exchange(0, std::memory_order_relaxed);
                uint64_t mx = s.max_ticks.exchange(0, std::memory_order_relaxed);
                if (!r || !c) continue;
                // Only report sites that actually spent time (>0.5ms total) or ran
                // often - keeps the line readable.
                double total_ms = double(tk) * to_ms, max_ms = double(mx) * to_ms;
                if (total_ms < 0.5 && c < 50) continue;
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "ret exe+0x%llX vt exe+0x%llX x%u %.1fms (max %.1f); ",
                         (unsigned long long)(r - exe.base),
                         (unsigned long long)(v ? v - exe.base : 0), c, total_ms, max_ms);
                line += buf;
            }
            if (!line.empty())
                spdlog::info("[stallprobe] {}: job-poll: {}", tag, line);
        }
        g_running.store(false);
    }
#endif // MFG_STALL_PROFILER
} // namespace

// Rows of our native settings page (see goblin_inject.hpp). ini_key doubles as the i18n
// entry_labels key, and goblin_messages injects the localized label into GR_MenuText at startup.
// Live consumers of THIS TABLE are menu_cfg_snapshot_take / menu_cfg_apply_if_changed, which give
// the native menu its per-frame live-apply, plus goblin_messages, which builds the GR_MenuText
// labels from it. (It used to say build_our_rows bound each row to its bool; that was the F11
// settings-tab populate, retired 2026-07-31 - the table itself is load-bearing, so do not follow
// that name into deleting it.)
//
// NOT the same thing as goblin::g_menutext_row_ids, the parallel vector of injected FMG ids. That
// one has no unguarded reader left since the same removal: its only remaining consumers sit behind
// #if MFG_CMDLIST_PROTO. It is kept with that prototype, not because this table needs it.
// Proof set for now - grows as the overlay settings migrate over.
const goblin::NativeMenuRowDef *goblin::native_menu_rows(size_t *count)
{
    // The "general" (non-category) settings, 16 rows = exactly the page list cap
    // (BasicViewItemList<EditProperty,16>). The ~45 show_* category toggles need
    // their own tabs/pages (multi-page design) - next phase. ERR-only entries are
    // harmless no-ops on other profiles (their config vars exist everywhere).
    // Live-loot defaults differ per profile (vanilla=true, others=false) - use the
    // compiled-in initial value of the vars themselves as the schema default.
    static goblin::NativeMenuRowDef rows[] = {
        {"require_map_fragments", &goblin::config::requireMapFragments, true},
        {"show_world_maps_ignore_fragments", &goblin::config::worldMapsIgnoreFragments, true},
        {"hide_killed_bosses", &goblin::config::hideKilledBosses, false},
        {"hide_dungeon_icons_on_clear", &goblin::config::hideDungeonIconsOnClear, false},
        {"patch_overworld_boss_icons", &goblin::config::patchOverworldBossIcons, true},
        {"patch_dungeon_boss_icons", &goblin::config::patchDungeonBossIcons, true},
        {"patch_camp_icons", &goblin::config::patchCampIcons, true},
        {"patch_merchant_icons", &goblin::config::patchMerchantIcons, true},
#ifdef MFG_PROFILE_VANILLA // matches the schema's MFG_LL_DEF (vanilla bake only)
        {"live_loot_flags", &goblin::config::liveLootFlags, true},
        {"live_loot_labels", &goblin::config::liveLootLabels, true},
        {"live_loot_icons", &goblin::config::liveLootIcons, true},
#else
        {"live_loot_flags", &goblin::config::liveLootFlags, false},
        {"live_loot_labels", &goblin::config::liveLootLabels, false},
        {"live_loot_icons", &goblin::config::liveLootIcons, false},
#endif
        {"anonymous_loot", &goblin::config::anonymousLoot, false},
        {"show_bosses", &goblin::config::showBosses, true},
    };
    *count = sizeof(rows) / sizeof(rows[0]);
    return rows;
}

void goblin::stall_probe::v3_pin_build_begin(void *owner, void *ctx)
{
    if (!goblin::config::debugLogging)
        return;
    auto &corr = g_v3_build_corr;
    if (corr.depth == 0)
    {
        corr = {};
        corr.owner = owner;
        corr.ctx = ctx;
    }
    ++corr.depth;
}

void goblin::stall_probe::v3_pin_build_end()
{
    if (!goblin::config::debugLogging)
        return;
    auto &corr = g_v3_build_corr;
    if (corr.depth == 0 || --corr.depth != 0)
        return;

    std::sort(corr.parents, corr.parents + corr.parent_count,
              [](const V3BuildParentStat &a, const V3BuildParentStat &b)
              {
                  if (a.calls != b.calls) return a.calls > b.calls;
                  return a.max_count > b.max_count;
              });
    spdlog::info("[v3corr] native buildMarkers owner=0x{:X} ctx=0x{:X}: "
                 "{} attachMovie call(s), {} parent(s)",
                 reinterpret_cast<uintptr_t>(corr.owner),
                 reinterpret_cast<uintptr_t>(corr.ctx),
                 corr.total_calls, corr.parent_count);
    const uint32_t shown = std::min<uint32_t>(corr.parent_count, 8);
    for (uint32_t i = 0; i < shown; ++i)
    {
        const auto &s = corr.parents[i];
        spdlog::info("[v3corr] rank={} parent=0x{:X} wrapper=0x{:X} calls={} "
                     "countRange={}..{}",
                     i, s.parent, s.wrapper, s.calls,
                     s.min_count == UINT64_MAX ? 0 : s.min_count, s.max_count);
    }
    corr = {};
}

namespace
{
    // 1 Hz while the map screen is up: our own per-frame cost, the factory's cost per pulse, and
    // the safemem counter deltas, all in one line. Added for the 2026-08-05 frame-rate report -
    // the map thread was visibly slow (29 factory pulses in 13 s on a first open, 3-10 on
    // reopens) and nothing in the log could say where the time went. Map frames and pulses run
    // on the same thread (the [v3] thread map lines), so plain fields are enough.
    // (V3Perf / g_v3_perf are declared with the timing primitives near the top: the tick feeds
    //  them too, and it lives far above this point.)

    void v3_perf_snapshot_counters(V3Perf &p)
    {
        p.copies0 = goblin::safemem::g_copies.load(std::memory_order_relaxed);
        p.queries0 = goblin::safemem::g_queries.load(std::memory_order_relaxed);
        p.refused0 = goblin::safemem::g_refused.load(std::memory_order_relaxed);
        p.vq_qpc0 = goblin::safemem::g_query_qpc.load(std::memory_order_relaxed);
        p.pages0 = goblin::safemem::g_page_hits.load(std::memory_order_relaxed);
        p.tpq0 = goblin::safemem::t_probe_qpc;
        p.tq0 = goblin::safemem::t_queries;   // thread_local: called from v3_perf_note_frame,
        p.tvq0 = goblin::safemem::t_query_qpc; // i.e. on the map thread
    }

    // ── Map-stall sampler ────────────────────────────────────────────────────────────
    // 2026-08-05, after the hot-path fixes: icons and steady FPS are back, but a COLD map
    // open (real teardown + movie reload) still freezes the map thread for 2-4 s between
    // the open command and the engine's build burst - a window where on_map_frame does not
    // run, so v3perf cannot see it and our log is silent. The watcher samples the map
    // thread's stack whenever the map-frame heartbeat has been silent >700 ms while the
    // map phase says a dialog exists. The suspend lasts only long enough to copy the
    // CONTEXT; unwinding and module-name resolution happen AFTER ResumeThread, so a thread
    // parked inside the loader or the unwind tables can never deadlock us. For a
    // multi-second stall the deep frames are stable after resume; the top may churn.
    // (g_map_hb_ms / g_map_hb_tid are declared with the timing primitives near the top - the
    //  factory's budget gate reads the heartbeat too, and it lives far above this point.)

    // Set once the unwind has faulted. A CAUGHT EXCEPTION IS NOT A FREE EXCEPTION: a
    // first-chance AV is a process-wide event, and a third party's filter (ERSS-FG, and
    // me3_mod_host too) answers it by running a ~39-frame dbghelp symbolization ON OUR
    // FAULTING THREAD - dbghelp is single-threaded and churns the heap, which is how a
    // guarded read of ours ends as heap corruption somebody else dies in. So this sampler
    // gets what every faultable loop needs and the __try alone does not give: a poison path.
    // One fault and it never runs again this session. Report 32 (ERSS-FG present, crash
    // shortly after launch) is the second player report in this class.
    std::atomic<bool> g_stallcap_poisoned{false};

    // POD-only frame so __try is legal; returns captured frame count.
    int stall_capture_frames(HANDLE th, uintptr_t *out, int cap)
    {
        CONTEXT c{};
        c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (SuspendThread(th) == static_cast<DWORD>(-1))
            return 0;
        const BOOL got = GetThreadContext(th, &c);
        ResumeThread(th);
        if (!got)
            return 0;
        int n = 0;
        bool faulted = false;
        // Unwinding ANOTHER thread's stack cannot be pre-validated the way a read can: the
        // thread was suspended mid-instruction and its unwind data may not describe the frame,
        // so RtlVirtualUnwind is entitled to fault. The counter keeps OUR crash log from
        // recording a fault we asked for (it was missing until report 32, which is why that
        // log carries [EXCEPTION] ntdll+0x46F77 records). It does NOT make the fault private -
        // nothing can - which is what the poison flag above is for.
        ++goblin::guarded::depth;
        __try
        {
            while (n < cap)
            {
                out[n++] = static_cast<uintptr_t>(c.Rip);
                DWORD64 base = 0;
                PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
                if (!rf)
                {
                    // Leaf with no unwind data: one manual step, then stop if implausible.
                    const uintptr_t ret = *reinterpret_cast<uintptr_t *>(c.Rsp);
                    if (ret < 0x10000)
                        break;
                    c.Rip = ret;
                    c.Rsp += 8;
                    continue;
                }
                PVOID hd = nullptr;
                DWORD64 est = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, rf, &c, &hd, &est, nullptr);
                if (!c.Rip)
                    break;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            faulted = true;
        }
        --goblin::guarded::depth; // reached on both paths
        if (faulted)
        {
            g_stallcap_poisoned.store(true, std::memory_order_relaxed);
            // At INFO and not behind debug_logging, for the reason the movie-def retire line
            // is: this whole class ran for entire sessions with nothing in our own log, and
            // was found only in somebody else's.
            spdlog::info("[stallcap] stack sampling stopped for this session: the unwind "
                         "faulted (expected on some hosts) and one fault is enough");
        }
        return n;
    }

    void stall_log_frame(int i, uintptr_t rip)
    {
        HMODULE m = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(rip), &m) &&
            m)
        {
            char path[MAX_PATH] = {};
            const char *name = path;
            if (GetModuleFileNameA(m, path, MAX_PATH))
            {
                if (const char *b = strrchr(path, '\\'))
                    name = b + 1;
            }
            spdlog::info("[stallcap]   #{:02} {}+0x{:X}", i, name,
                         rip - reinterpret_cast<uintptr_t>(m));
        }
        else
            spdlog::info("[stallcap]   #{:02} 0x{:X}", i, rip);
    }

    void v3_perf_note_frame(int64_t dt_qpc)
    {
        ++g_v3_open.frames;
        g_v3_open.frame_qpc += dt_qpc;
        auto &p = g_v3_perf;
        ++p.frames;
        p.total_qpc += dt_qpc;
        if (dt_qpc > p.max_qpc)
            p.max_qpc = dt_qpc;
        const uint64_t now = GetTickCount64();
        if (p.window_ms == 0)
        {
            p.window_ms = now;
            v3_perf_snapshot_counters(p);
            return;
        }
        const uint64_t span = now - p.window_ms;
        if (span < 1000)
            return;
        const int64_t f = v3_perf_freq();
        const auto us = [f](int64_t q) { return q * 1000000 / f; };
        spdlog::info("[v3perf] {} map frames / {} ms: ours {} us (avg {}, max {}) = tick {} us + "
                     "reconcile {} us (of the tick, {} merges cost {} us, emphasis {} us / {} "
                     "writes, zoom reapply {} us / {} passes); tick split: head {} (gate {} + "
                     "rest {}, worst frame {}) + emph {} + refresh {} + tail {} us; {} pulses {} us; "
                     "safemem copies +{} page-probes +{} (map thread {} us) lookups +{} ({} us in "
                     "VirtualQuery; map thread +{} / {} us) refused +{}",
                     p.frames, span, us(p.total_qpc),
                     us(p.total_qpc / (p.frames ? p.frames : 1)), us(p.max_qpc),
                     us(p.tick_qpc), us(p.recon_qpc), p.merges, us(p.merge_qpc), us(p.emph_qpc),
                     p.emph_writes, us(p.zoom_qpc), p.zoom_passes,
                     us(p.head_qpc), us(p.head_gate_qpc), us(p.head_rest_qpc),
                     us(p.head_max_qpc), us(p.emphsec_qpc), us(p.mergesec_qpc), us(p.tail_qpc),
                     p.pulses, us(p.pulse_qpc),
                     goblin::safemem::g_copies.load(std::memory_order_relaxed) - p.copies0,
                     goblin::safemem::g_page_hits.load(std::memory_order_relaxed) - p.pages0,
                     us(static_cast<int64_t>(goblin::safemem::t_probe_qpc - p.tpq0)),
                     goblin::safemem::g_queries.load(std::memory_order_relaxed) - p.queries0,
                     us(static_cast<int64_t>(
                         goblin::safemem::g_query_qpc.load(std::memory_order_relaxed) -
                         p.vq_qpc0)),
                     goblin::safemem::t_queries - p.tq0,
                     us(static_cast<int64_t>(goblin::safemem::t_query_qpc - p.tvq0)),
                     goblin::safemem::g_refused.load(std::memory_order_relaxed) - p.refused0);
        p = V3Perf{};
        p.window_ms = now;
        v3_perf_snapshot_counters(p);
    }
}

namespace
{
    // A reconcile that stops completing while the map is up leaves every detached marker frozen
    // out of the display list - invisible, though its row still answers the hover popup. That is
    // silent by construction (the straggler line lives past the early returns), so the gap
    // between calls and completed passes is reported here instead.
    void v3_report_reconcile_stall()
    {
        static uint64_t s_next_ms = 0;
        static uint64_t s_last_done = 0;
        const uint64_t now = GetTickCount64();
        // First call is the baseline: no window has elapsed yet, so "in 2 s" would be a lie
        // (it fired on the very first map frame with `1 calls, 0 completed`).
        if (s_next_ms == 0)
        {
            s_next_ms = now + 2000;
            s_last_done = g_recon_done;
            return;
        }
        if (now < s_next_ms)
            return;
        s_next_ms = now + 2000;
        if (g_recon_done == s_last_done && g_recon_calls > g_recon_done)
            spdlog::warn("[v3view] viewport reconcile has not completed a pass in 2 s while the "
                         "map is open ({} calls, {} completed) - detached markers cannot come "
                         "back until it does",
                         g_recon_calls, g_recon_done);
        s_last_done = g_recon_done;
    }
}

void goblin::stall_probe::on_map_frame()
{
    g_map_hb_ms.store(GetTickCount64(), std::memory_order_relaxed);
    g_map_hb_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    const int64_t perf_t0 = v3_perf_now();

    // Drive the category-by-category lightweight native-marker rollout.
    const int64_t t_tick0 = v3_perf_now();
    v3_native_tick();
    const int64_t t_tick1 = v3_perf_now();
    g_v3_open.tick_qpc += t_tick1 - t_tick0;
    g_v3_perf.tick_qpc += t_tick1 - t_tick0;

    // Lever B: reconcile which markers are attached to the visible map window.
    v3_viewport_reconcile();
    const int64_t recon_dt = v3_perf_now() - t_tick1;
    g_v3_open.recon_qpc += recon_dt;
    g_v3_perf.recon_qpc += recon_dt;
    v3_report_reconcile_stall();

    v3_perf_note_frame(v3_perf_now() - perf_t0);

    // A counter-scale pass for one transplanted child followed here: it read the child's matrix
    // through the vtable and divided out the map zoom. It was permanently disarmed - g_v3_scale_ready
    // is initialised false, stored false in two places and set true nowhere - so the ~40 lines after
    // the gate never ran, and g_v3_scale was never written either. The deliberate disarm is recorded
    // where the seed used to arm it: do not restore this without a reason to.
}


bool goblin::stall_probe::map_screen_alive()
{
    return v3_map_object_alive();
}

void goblin::stall_probe::sample_map_stall()
{
    if (g_stallcap_poisoned.load(std::memory_order_relaxed))
        return;
    const uint64_t hb = g_map_hb_ms.load(std::memory_order_relaxed);
    const uint32_t tid = g_map_hb_tid.load(std::memory_order_relaxed);
    if (!hb || !tid || tid == GetCurrentThreadId())
        return;
    // Phase 0 = no dialog: the heartbeat is EXPECTED silent (map closed), not a stall.
    // 0xFF (unresolved) is sampled - an unknown phase must not hide a freeze.
    if (g_map_phase.load(std::memory_order_acquire) == 0)
        return;
    const uint64_t age = GetTickCount64() - hb;
    static uint64_t s_stall_hb = 0; // which heartbeat value this stall was keyed on
    static int s_taken = 0;
    if (age < 700)
        return;
    if (s_stall_hb != hb)
    {
        s_stall_hb = hb;
        s_taken = 0;
    }
    // A few samples per stall are enough to name the culprit; a 20 s ceiling stops a
    // stuck phase byte from turning this into a permanent per-tick suspend.
    if (s_taken >= 3 || age > 20000)
        return;
    ++s_taken;
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                           FALSE, tid);
    if (!th)
        return;
    uintptr_t frames[24] = {};
    const int n = stall_capture_frames(th, frames, 24);
    CloseHandle(th);
    spdlog::info("[stallcap] map frame heartbeat silent {} ms (phase {}); map thread tid {} "
                 "stack, {} frame(s):",
                 age, g_map_phase.load(std::memory_order_relaxed), tid, n);
    for (int i = 0; i < n; ++i)
        stall_log_frame(i, frames[i]);
}

void goblin::stall_probe::v3_native_factory_pulse(void *ctx, unsigned frame)
{
    // [v3pump] The nested proof's GotoFrame is on the stack (we are inside the engine's burst, in
    // the factory's first pulse). Count the pulse and build nothing - no seed, no factory, no owner
    // check: the proof asks only whether pulses arrive, and from which sprite's snapshot (ctx+0x58).
    // Pump strokes (t_v3_pump.pumping) are the opposite case: counted, then built as usual.
    if (t_v3_pump.active)
    {
        ++t_v3_pump.pulses;
        uint64_t from = 0;
        if (v3_read64(reinterpret_cast<uintptr_t>(ctx) + 0x58, from) && from == t_v3_pump.expect)
            ++t_v3_pump.own;
        else
            ++t_v3_pump.other;
        return;
    }
    if (t_v3_pump.pumping)
        ++t_v3_pump.pumped;
    try
    {
        v3_note_thread("v3_native_factory_pulse (RM2 burst)");
        v3_check_owner("v3_native_factory_pulse");
        g_v3_pulse_seen.fetch_add(1, std::memory_order_relaxed);
        if (!v3_native_seed_from_live_callback())
        {
            g_v3_pulse_unseeded.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const int64_t perf_t0 = v3_perf_now();
        v3_native_factory_consume(ctx, static_cast<uint32_t>(frame));
        const int64_t perf_dt = v3_perf_now() - perf_t0;
        if (!t_v3_pump.pumping) // the burst's cost; the pump's is timed as a whole in the tick
        {
            ++g_v3_perf.pulses;
            g_v3_perf.pulse_qpc += perf_dt;
            ++g_v3_open.pulses;
            g_v3_open.pulse_qpc += perf_dt;
        }
    }
    catch (const std::exception &e)
    {
        g_v3_native.frame_budget = 0;
        g_v3_native.in_factory = false;
        spdlog::error("[v3native] factory exception: {}", e.what());
    }
}

void goblin::stall_probe::on_map_close()
{
    // Deliberately KEEP g_v3_native and the g_v3_target_* anchors: a quick
    // reopen (before the engine's deferred teardown) reuses the same movie and
    // never emits a new attachMovie burst, so a reset here would orphan our
    // still-alive children with stale visibility. After a REAL teardown the
    // next build burst re-targets a fresh parent (parent-changed arm, gated on
    // this flag) and the map-frame tick resets and reseeds the manager; until
    // then every touch is liveness-validated and SEH-guarded.
    g_v3_close_ms.store(GetTickCount64(), std::memory_order_release);
    g_v3_map_closed.store(true, std::memory_order_release);
    // The matrix-slot, custom-child and visual-state resets that stood here went with the state
    // they cleared: nothing wrote any of it once the eight-cell discovery grid was retired.
    // (A candidate-parent table was locked and zeroed here on every map close; it had no reader.)
}

uint32_t goblin::stall_probe::v3_detach_all_children()
{
    // [v3pump] The generator belongs to this generation. Forgotten here, ahead of the early
    // returns below (no markers yet, no remove-at), and never touched again: a parent address can
    // come back as a new movie's parent. The next generation makes its own.
    const bool pump_was_building = g_v3_native.pump_capped;
    g_v3_native.pump_gen = 0;
    g_v3_native.pump_tried = false;
    g_v3_native.pump_capped = false;
    g_v3_native.pump_dry = 0;
    g_v3_native.pump_stuck = 0;
    if (!goblin::variants::kSelfDetach || !g_v3_remove_at)
        return 0;
    const uintptr_t wrapper = g_v3_native.wrapper;
    const uintptr_t parent = g_v3_native.parent;
    if (!v3_heap_ptr(wrapper) || !v3_heap_ptr(parent) || g_v3_native.objects.empty())
        return 0;

    // Snapshot our live child pointers, sorted+unique for the leaf's binary search.
    // Runs on the map UI thread from the WMD dtor detour, single-shot per close.
    static std::vector<uintptr_t> ours;
    ours.clear();
    ours.reserve(g_v3_native.objects.size());
    for (const auto &o : g_v3_native.objects)
        if (v3_heap_ptr(o.child))
            ours.push_back(o.child);
    if (ours.empty())
        return 0;
    std::sort(ours.begin(), ours.end());
    ours.erase(std::unique(ours.begin(), ours.end()), ours.end());

    static std::vector<uint32_t> idxbuf;
    idxbuf.assign(ours.size(), 0);

    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    uint32_t faulted = 0;
    const uint32_t found = v3_detach_scan(parent, ours.data(),
                                          static_cast<uint32_t>(ours.size()),
                                          idxbuf.data(),
                                          static_cast<uint32_t>(idxbuf.size()), &faulted);
    if (faulted)
        spdlog::warn("[v3native] detach-all: child-vector scan faulted, removing nothing "
                     "(indices from a vector that moved would free engine-owned children).");
    uint32_t skipped = 0;
    const uint32_t removed = v3_detach_remove(wrapper, parent, idxbuf.data(), found, ours.data(),
                                              static_cast<uint32_t>(ours.size()), &skipped);
    if (skipped)
        spdlog::warn("[v3native] detach-all: {} of {} indices no longer pointed at our children and "
                     "were SKIPPED (vector moved between scan and removal).", skipped, found);
    QueryPerformanceCounter(&t1);
    const uint64_t us = freq.QuadPart
        ? static_cast<uint64_t>((t1.QuadPart - t0.QuadPart) * 1000000 / freq.QuadPart)
        : 0;
    // (g_v3_last_detached / g_v3_last_detach_us were stored here for a reader that never existed,
    // not even the Debug page. The measurement itself is not lost - it is the log line below.)
    spdlog::info("[v3native] self-detach: matched={} removed={} tracked={} in {} us",
                 found, removed, g_v3_native.objects.size(), us);
    spdlog::info("[v3gate] precondition rejects this session: slot={} array={}",
                 g_v3_reject_slot.load(std::memory_order_relaxed),
                 g_v3_reject_array.load(std::memory_order_relaxed));
    goblin::crashdiag::note_map_closed(static_cast<uint32_t>(g_v3_native.objects.size()));
    goblin::crashdiag::memory("close", static_cast<uint32_t>(g_v3_native.objects.size()));

    // The children were bulk-DETACHED (their TreeCacheNodes are gone, which is what pays for the
    // close freeze), but with the viewport window on they carry our retained reference, so the
    // child OBJECTS stay alive while detached. Keep them tracked, marked detached, so a QUICK
    // reopen - same movie and parent reused, no rebuild burst - re-attaches the in-view ones
    // through v3_viewport_reconcile instead of showing an empty map. A REAL teardown changes the
    // parent, and the reset arm in v3_native_tick drops the tracking there.
    // Without that retained reference the detach really did free them, so clearing is correct.
    // GIVE THE REFERENCE BACK HERE, while the movie is still alive.
    //
    // Measured in the 11.84 GB full-memory dump: the Scaleform arena is a FIXED, fully pre-committed
    // 160.00 MiB (0x7FF38C0E0000..0x7FF3960E0000, 167,770,368 usable), and at the hang it was
    // 137,819,248 bytes in use - 82.15%. Inside it sat 49,952 of our marker children with parent==0
    // and refcount==1, exactly 7.0000 x 7136, i.e. seven whole abandoned generations. All 49,952
    // pointed at ONE movie, 0x7FF38E183D50, whose own refcount read 51,001 - the movie is pinned
    // alive BY our orphans, so neither it nor anything it owns can ever go back to the arena. When
    // the arena finally cannot satisfy a request the engine stores the NULL unchecked and the
    // process dies: two independent sites did exactly that within one second (exe+0x11BFE7E write
    // 0x1770 and exe+0x12326E8 write 0x0), with no frame of ours anywhere in either chain.
    //
    // Process commit never moved through any of this, which is why an earlier measurement wrongly
    // cleared the leak: the arena is committed up front, so filling it is invisible to the OS.
    //
    // WHY HERE and not at the reset, where an earlier attempt put it: at reset the movie is already
    // gone, and a child's destructor reaches into its movie's context - that attempt reported
    // "0 destroyed, 7136 refused" because the destructor itself faulted, after the refcount had
    // already been decremented. Map close is the last moment the whole structure is intact.
    //
    // Release is the engine's own: decrement, and at zero call vtable slot 0, the MSVC scalar
    // deleting destructor (exe+0x10EF320 for this class), which destructs and frees through
    // Scaleform's allocator - the same heap the block came from. If anything else still holds a
    // reference the count simply drops and its owner destroys it later, which is correct too.
    if (goblin::variants::kViewportWindow)
    {
        uint32_t released = 0, destroyed = 0, refused = 0, unsafe = 0;
        const uint64_t vt = g_v3_native.child_vtable;
        // The movie that owns this generation, read directly rather than inferred. Destroying a
        // child is safe only while the MovieImpl that created it is alive: both fault paths
        // (exe+0x1136C9B reading [movie+0x5338], and the entry-release virtual call in
        // exe+0x11578F0) resolve through it.
        const uintptr_t movie = v3_movie_now();

        // SURVEY, no action taken. On game 1.17 every child fails the pre-test and the whole
        // generation leaks (player log 2026-08-28: "0 released ... 7189 unsafe(pre-test)" on
        // every close, then DL_PANIC once the fixed 160 MiB Scaleform arena could not serve a
        // 0x110-byte request). The single ADDRESS in the chain was measured on 2.6.2 and could
        // not match there; it is resolved at runtime now (v3_slot_vtable). This survey stays as
        // the field check on that: 7189 independent children agreeing on one vtable pointer, and
        // that pointer being the resolved one, is what says the walk holds on this build. A low
        // agreement rate would say it does not, whatever the offline verification concluded.
        const int64_t r0 = v3_perf_now(); // the release's own cost, survey included (UI thread frame)
        {
            uint64_t top = 0;
            uint32_t top_n = 0, walked = 0, distinct = 0;
            std::unordered_map<uint64_t, uint32_t> tally;
            for (auto &o : g_v3_native.objects)
            {
                if (!o.ref_held || !v3_heap_ptr(o.child))
                    continue;
                const uint64_t svt = v3_child_slot_vtable(o.child, movie);
                if (!svt)
                    continue;
                ++walked;
                const uint32_t n = ++tally[svt];
                if (n > top_n) { top_n = n; top = svt; }
            }
            distinct = static_cast<uint32_t>(tally.size());
            const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            spdlog::info("[v3slotvt] survey: {} of {} children walked, {} distinct vtable(s); "
                         "top exe+0x{:X} on {} of them ({}%); resolved expectation exe+0x{:X} - {}",
                         walked, g_v3_native.objects.size(), distinct,
                         (top && exe && top > exe) ? top - exe : 0, top_n,
                         walked ? top_n * 100 / walked : 0,
                         (v3_slot_vtable() && exe) ? v3_slot_vtable() - exe : 0,
                         (top && v3_slot_vtable() && top == v3_slot_vtable()) ? "MATCHES"
                                                                             : "does NOT match");
        }
        const int64_t r1 = v3_perf_now();
        for (auto &o : g_v3_native.objects)
        {
            if (!o.ref_held || !v3_heap_ptr(o.child) || vt == 0)
                continue;
            // Refuse BEFORE entering the destructor. Reaching the SEH net means the destructor has
            // already rewritten vtable slots and run a movie-registry unregister - "refused" there
            // is damage, not a decline.
            if (!v3_child_releasable(o.child, vt, movie))
            {
                ++unsafe;
                continue;
            }
            bool gone = false;
            if (v3_release_held(o.child, vt, gone))
            {
                ++released;
                if (gone) ++destroyed;
                o.child = 0;
                o.ref_held = false;
            }
            else
            {
                ++refused;
            }
        }
        const int64_t r2 = v3_perf_now();
        spdlog::info("[v3native] generation released at close: {} released, {} destroyed, "
                     "{} refused, {} unsafe(pre-test), of {} tracked, movie=0x{:X} in {} us "
                     "(survey {} us)",
                     released, destroyed, refused, unsafe, g_v3_native.objects.size(), movie,
                     park_us(r2 - r0), park_us(r1 - r0));
        // Ask for this movie's parked entry to be expired, so the next open cannot reuse it and
        // therefore cannot skip the burst we build from. Harmless if the entry is never found.
        if (v3_heap_ptr(movie))
            g_park_target.store(movie, std::memory_order_relaxed);
    }
    // Nothing is kept for a quick reopen any more. That path was written for a reopen that reuses
    // the same movie and parent; measured across every session on 2026-08-03, 30 of 30 opens
    // retargeted to a NEW parent, so it never once fired - and holding a generation for it is what
    // pins the movie.
    // Decided BEFORE the clear below: the branch further down tested objects.empty() after clearing
    // it, so it could never fire. "Unfinished" = rows queued but neither built nor failed (a close
    // mid-burst), not "READY not printed yet": 6 of 16 closes on ERR 2026-09-23 came before READY
    // with every row built. ONLY at the dialog close (g_v3_map_closed, set by on_map_close right
    // before wmd_dtor_detour calls this): the seed's re-anchor and v3_drop_dead_anchor also call
    // this function with the map open, and clearing the target there would pull the anchor out from
    // under the build the seed is about to start on it.
    const size_t built_at_close = g_v3_native.objects.size();
    const size_t queued_at_close = g_v3_native.pending.size();
    const bool unfinished_at_close = g_v3_map_closed.load(std::memory_order_acquire) &&
                                     g_v3_native.seeded && !g_v3_native.completion_reported &&
                                     built_at_close + g_v3_native.failed < queued_at_close;
    g_v3_native.objects.clear();
    goblin::map_api::reset_depths();
    g_v3_native.by_row.clear();
    g_v3_native.queued.clear();
    // The generation is GONE now, not merely detached, so the manager must not believe it still has
    // one. Leaving `seeded` set was worth a bug report on its own: a QUICK reopen reuses the same
    // parent, so the tick's reset arm never fires, no reseed happens, and the map comes up empty -
    // while a slow reopen gets a new parent, resets, reseeds, and looks fine. Dropping the anchor
    // as well means the next attach burst re-anchors through arm A at the seed floor, which is the
    // same path a first open takes.
    // AN UNFINISHED BUILD MUST NOT HAND ITS ANCHOR TO THE NEXT OPEN. Keeping it is right for a
    // generation that completed (see below): the next open reuses the container and the deferred
    // seed re-validates it. But if this generation never reached CATEGORIES READY, keeping the
    // anchor lets the next open adopt the same container and skip the fresh-anchor path - and
    // since a reopen that reuses the movie emits no burst, there is nothing left to build the
    // missing markers with. Measured 2026-08-05 on Convergence: a generation stopped at 1968 of
    // 8425 and the map then sat open for minutes with no seed and no CATEGORIES READY at all,
    // showing the fragment. The fragment is not random - the seed sorts the player's OWN map's
    // markers LAST, so a short build is missing exactly the ones the player is standing among.
    // Clearing the target here forces the next attach burst through arm A, the same path a first
    // open takes, which is the one path guaranteed to have a whole pulse pool ahead of it.
    if (unfinished_at_close)
    {
        // [v3pump] A close in the first frames after an open, while the pump still builds the
        // hidden rows, is ordinary now: same handling, no warning.
        if (pump_was_building)
            spdlog::info("[v3native] closed while the pump was building ({} of {} built); "
                         "clearing the anchor so the next open rebuilds from scratch",
                         built_at_close, queued_at_close);
        else
            spdlog::warn("[v3native] generation ended UNFINISHED ({} of {} built); clearing the "
                         "anchor so the next open rebuilds from scratch instead of adopting it",
                         built_at_close, queued_at_close);
        g_v3_target_parent.store(0, std::memory_order_relaxed);
        g_v3_target_wrapper.store(0, std::memory_order_release);
        g_v3_target_layer.store(-1, std::memory_order_relaxed);
        g_v3_target_count.store(0, std::memory_order_relaxed);
    }
    g_v3_native.seeded = false;
    g_v3_native.completion_reported = false;
    // KEEP THE ANCHOR. Only the generation is gone; the container it lived in usually is not.
    //
    // Clearing it as well was wrong, and the log says exactly how: on a QUICK reopen the game
    // reuses the same movie and emits NO attachMovie burst at all - three opens in a row logged
    // `mapphase 0 -> 3 (open)` followed by nothing, no RETARGET, no seed, no CATEGORIES READY,
    // because v3_note_movie_attach only runs on a burst. With no anchor there was nothing to seed
    // onto, so the map came up empty and stayed empty until a real teardown or a layer switch
    // produced a fresh burst. That is the "icons invisible on quick reopen" report.
    //
    // Keeping it is safe because the deferred-seed path validates it before committing: a detached
    // parent, or one whose movie no longer resolves, is dropped there and the next burst re-anchors.
    // Liveness, not our bookkeeping, decides.
    // The receipt for the release above, and the only proof that matters: destroying 7136 objects
    // means nothing unless the arena actually got the bytes back. Deliberately the LAST thing this
    // function does - read before the release and it just reports the old number.
    goblin::crashdiag::arena("close");
    return removed;
}

void goblin::stall_probe::setup()
{
    // AOB-resolved entries (v1.16 file VAs in the comment above). Each pattern was
    // verified unique in the exe; a miss disables just that counter (feature is
    // diagnostics-only, the game runs unchanged).
    struct Target
    {
        int idx;
        const char *aob;
    };
    const Target targets[] = {
        {0, "48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8D 6C 24 C9 48 81 EC "
            "A0 00 00 00 48 8B"},
        {1, "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8D "
            "6C 24 D1 48 81 EC 90 00 00 00 48 8B 41 08"},
        {2, "48 89 5C 24 20 4C 89 44 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D AC"},
        {3, "40 53 41 55 41 57 48 83 EC 30 33 DB 4C 8B F9 89 5C 24 58 4C 8B EA 48 8B"},
        {4, "48 89 6C 24 18 48 89 74 24 20 41 56 48 83 EC 20 8B A9 B8 00 00 00 4C 8B F1 "
            "48 8B B1 E0 00 00 00 C1 ED 03 40 80 E5 01 48"},
        {5, "48 89 74 24 10 57 48 83 EC 20 48 8B FA 48 8B F1 48 8B 51 50 48 85 D2 0F"},
        {6, "40 53 56 57 48 83 EC 20 48 8B F9 83 CA FF 48 81 C1 88 00 00 00"},
        {7, "48 89 5C 24 20 57 41 56 41 57 48 83 EC 20 48 8B 19 4D 8B F8 4C 8B F2 48"},
        {8, "48 89 5C 24 18 48 89 74 24 20 57 41 56 41 57 48 81 EC 80 00 00 00 48 8B"},
        {9, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 49 8B F8 48 8B F2 "
            "E8 01 C4 FE FF 83"},
    };
    static Fn4 *detours[N_REG] = {reg_detour<0>, reg_detour<1>, reg_detour<2>,
                                  reg_detour<3>, reg_detour<4>, reg_detour<5>,
                                  reg_detour<6>, reg_detour<7>, reg_detour<8>,
                                  reg_detour<9>};
    int armed = 0;
    for (const auto &t : targets)
    {
        try
        {
            auto *fn = modutils::hook<Fn4>({.aob = t.aob}, *detours[t.idx], o_reg[t.idx]);
            // widget-b: its typed-find call site (entry+0x173, E8 rel32) returns to
            // entry+0x178 - the gate for the sequential predictor above.
            if (t.idx == 1)
                g_wb_find_ret = reinterpret_cast<uintptr_t>(fn) + 0x178;
            armed++;
        }
        catch (const std::exception &e)
        {
            spdlog::warn("[stallprobe] counter '{}' unavailable: {}", REG_NAME[t.idx], e.what());
        }
    }
    spdlog::info("[stallprobe] {} of {} cost counters armed", armed, N_REG);

    // Scaleform DisplayObjContainer native insert core
    // (v1.16 FUN_14113e970). Pass-through at all times; debug sessions retain
    // counters and the highest-count call until the next capture report.
#if MFG_STALL_PROFILER
    try
    {
        modutils::hook<V3AddFn>(
            {.aob = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 "
                    "48 C7 44 24 28 FF FF FF FF 49 8B D9 48 89 5C 24 20 49 8B F0 48 8B EA 48 8B F9"},
            v3_add_detour, o_v3_add);
        spdlog::info("[stallprobe] v3 native insert-core spy armed");
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[stallprobe] v3 native insert-core spy unavailable: {}", e.what());
    }
#endif // MFG_STALL_PROFILER

    // PlaceObject insertion path (v1.16 FUN_14113e7e0, DisplayList::Add). The detour
    // only matches the marker factory's outstanding (depth, charId) slots - our ids,
    // chosen per world-map parse - so ordinary display-tree construction is untouched
    // apart from the pass-through hook itself.
    try
    {
        modutils::hook<V3PlaceFn>(
            {.aob = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 "
                    "41 54 41 56 41 57 48 83 EC 20 45 8B 70 4C 4C 8B FA 4C 8B 61 08 "
                    "41 8B D6 49 8B D9 49 8B E8 48 8B F9 E8"},
            v3_place_detour, o_v3_place);
        spdlog::info("[stallprobe] v3 custom PlaceObject spy armed");
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[stallprobe] v3 custom PlaceObject spy unavailable: {}", e.what());
    }

    // Record-materialization driver (v2.6.2.0 FUN_1411bf1b0): the engine-side
    // walker that turns pending timeline records into real children. Called by
    // the factory right after queueing a batch; a miss disables native marker
    // creation entirely (markers then fall back to the finite build-burst
    // trickle, ~1900/session).
    try
    {
        g_v3_mat_driver = reinterpret_cast<V3MatDriverFn *>(modutils::scan<void>(
            {.aob = "4C 8B DC 55 56 41 55 49 8D 6B D8 48 81 EC 10 01 00 00 "
                    "48 8D 41 48 4C 8B EA"}));
        spdlog::info("[stallprobe] v3 record driver resolved @ 0x{:X}",
                     reinterpret_cast<uintptr_t>(g_v3_mat_driver));
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[stallprobe] v3 record driver AOB miss (native marker "
                     "creation limited): {}",
                     e.what());
    }

    // [v3pump] Sprite::GotoFrame. Accepted only if the match also calls the record driver twice
    // within its first 0x300 bytes (it is that driver's only caller, E8 sites at +0x115 and +0x241
    // on the live exe), so a byte-twin elsewhere is refused. A miss: every open builds all markers
    // at once, as before the pump.
    if (goblin::variants::kFramePump && g_v3_mat_driver)
    {
        try
        {
            auto *gf = static_cast<const uint8_t *>(modutils::scan<void>(
                {.aob = "4C 8B DC 55 53 56 49 8D 6B A1 48 81 EC 90 00 00 00 48 8B D9 8B F2 "
                        "0F B7 49 6A 0F B7 C1 66 C1 E8 0B A8 01 0F 84"}));
            uint32_t calls = 0;
            const uintptr_t driver = reinterpret_cast<uintptr_t>(g_v3_mat_driver);
            for (uint32_t i = 0; gf && i + 5 <= 0x300; ++i)
            {
                if (gf[i] != 0xE8)
                    continue;
                int32_t rel = 0;
                memcpy(&rel, gf + i + 1, 4);
                if (reinterpret_cast<uintptr_t>(gf) + i + 5 + static_cast<intptr_t>(rel) == driver)
                    ++calls;
            }
            const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            if (gf && calls == 2)
            {
                g_v3_goto_frame = reinterpret_cast<V3GotoFrameFn *>(const_cast<uint8_t *>(gf));
                spdlog::info("[v3pump] GotoFrame resolved @ exe+0x{:X}",
                             reinterpret_cast<uintptr_t>(gf) - exe);
            }
            else
                spdlog::warn("[v3pump] GotoFrame candidate @ exe+0x{:X} calls the record driver {} "
                             "time(s), expected 2 - pump off",
                             gf ? reinterpret_cast<uintptr_t>(gf) - exe : 0, calls);
        }
        catch (const std::exception &e)
        {
            spdlog::warn("[v3pump] GotoFrame AOB miss - pump off: {}", e.what());
        }
    }

    // Render-node GetWritableData(entry, changeFlags): the copy-on-write + change-record
    // step every visual property of a display object goes through. Identified from the
    // Scaleform SDK, not guessed - the projection-matrix setter calls it with 0x100000,
    // which is exactly Render_Constants.h Change_State_ProjectionMatrix3D. Used by the
    // location emphasis to fade the markers of other maps; a miss only costs the colour.
    try
    {
        g_v3_get_writable_data = reinterpret_cast<V3GetWritableDataFn *>(
            modutils::scan<void>({.aob = "48 89 6C 24 20 56 41 54 41 56 48 83 EC 20 "
                                         "48 8B F1 4C 8B C1 48 81 E6 00 F0 FF FF"}));
        spdlog::info("[stallprobe] node writable-data getter resolved @ 0x{:X}",
                     reinterpret_cast<uintptr_t>(g_v3_get_writable_data));
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[stallprobe] node writable-data AOB miss (location emphasis keeps "
                     "to size and order): {}",
                     e.what());
    }

    // Lever C self-detach: the engine's remove-from-container primitive
    // FUN_1410c87c0 (the removal half of the reparent path FUN_1410c8440). A miss
    // just disables self-detach (close falls back to the engine's full teardown).
    try
    {
        g_v3_remove_at = reinterpret_cast<V3RemoveAtFn *>(modutils::scan<void>(
            {.aob = "40 57 48 83 EC 20 48 8B 41 18 48 8B F9 3B 90 E0 00 00 00 72 08 "
                    "33 C0 48 83 C4 20 5F C3"}));
        spdlog::info("[stallprobe] v3 self-detach primitive resolved: removeAt=0x{:X}",
                     reinterpret_cast<uintptr_t>(g_v3_remove_at));
    }
    catch (const std::exception &e)
    {
        g_v3_remove_at = nullptr;
        spdlog::warn("[stallprobe] v3 self-detach primitive AOB miss (lever C off): {}",
                     e.what());
    }

    // Resolve the snapshot-slot class now, on this init thread: otherwise the first map close pays
    // the one-time .rdata scan (9-10 ms on every session's first close, ERR 2026-09-23) inside the
    // game's menu update. anchors::warm() placed the two slot functions it keys on before this step.
    if (goblin::variants::kViewportWindow)
        (void)v3_slot_vtable();

    // Six DrawingContext primitives (begin / beginFill / moveTo / lineTo / endFill / shapeReset)
    // were AOB-scanned here on every startup for the solid-fill spike. The spike itself no longer
    // exists - nothing ever called any of the six - so this was six .text scans and three log
    // lines for nothing. The patterns themselves live on in tools/aob_signatures.py as
    // non-critical entries if the experiment is ever repeated.

    // Load-time movie interception (see goblin_own_movie.hpp). ABOVE the menu-mode gate on purpose:
    // it serves BOTH movies, and the map's half (our own hover panel and banner) is not part of the
    // in-game menu. Its menu half checks the mode itself.
    goblin::own_movie::install();

    // CSMenuMan::updateTask. ABOVE the menu-mode gate, and it must stay there: this detour is
    // the ONLY writer of the map phase byte, and the map phase is what starts a marker
    // generation (it clears g_v3_map_closed on the open edge). It sat BELOW the gate until
    // report 32, so `menu_render_mode = imgui` - the workaround we hand out for report 31 -
    // also switched the markers off: measured in that player's log, 8 sessions with the map
    // opened, phase stuck at 255, 0 factory pulses, no CATEGORIES READY, and the stall sampler
    // firing on a heartbeat it had no phase to interpret. The two sessions that DID render
    // markers were the two running in `native` mode. The detour gates its own in-game-menu half
    // on native_menu_enabled(), so installing it always costs the imgui user nothing.
    try
    {
        modutils::hook<MenuUpdateFn>(
            {.aob = "48 8B C4 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 68 A1 "
                    "48 81 EC 98 00 00 00 48 C7 45 A7 FE FF FF FF 0F 29 70 A8 "
                    "0F 29 78 98 44 0F 29 40 88 44 0F 29 4C 24 50 44 0F 29 54 24 40 "
                    "48 8B FA 48 8B D9 0F 57 FF F3 0F 10 05 ?? ?? ?? ?? 0F 2E C7 "
                    "7A 16 75 14 B9 29 0A 00 00"},
            menu_update_detour, o_menu_update);
        spdlog::info("[menuprobe] CSMenuMan::updateTask hook armed");
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[menuprobe] updateTask hook unavailable: {}", e.what());
    }

    // The two attach hooks: ABOVE the menu-mode gate for the same reason as updateTask - they
    // serve the MAP MARKERS. They sat below it until 2026-09-11, so `menu_render_mode = imgui`
    // still left the markers invisible after the report-32 fix: the phase byte moved, the factory
    // pulsed, 7877 sprite-171 tags were in the display list, but no anchor was ever written - no
    // seed, no CATEGORIES READY, and "[v3view] viewport reconcile has not completed a pass" every
    // 2 s while the map was open.
    //
    // Scaleform attachMovie DAPI bridge (v1.16 FUN_1410e00c0). Its export name
    // and initializer object describe the ActionScript object that is about to
    // enter a display-list parent; the nested high-level attach hook records the
    // actual parent/count while this thread-local context is active.
    try
    {
        modutils::hook<V3AttachMovieFn>(
            {.aob = "4C 8B DC 4D 89 4B 20 4D 89 43 18 55 56 41 57 49 8D 6B D8 "
                    "48 81 EC 10 01 00 00 48 8B 41 08 49 8B F1 48 8B 4A 28 4C 8B 78 18 "
                    "8B 81 90 00 00 00 83 E8 1F 83 F8 05"},
            v3_attach_movie_detour, o_v3_attach_movie);
        spdlog::info("[stallprobe] v3 attachMovie bridge spy armed");
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[stallprobe] v3 attachMovie bridge spy unavailable: {}", e.what());
    }

    // High-level display-object move/attach API (v1.16 FUN_1410c8440). LOAD-BEARING: the detour
    // on it is the only writer of the native-marker anchor (v3_note_movie_attach), which the seed,
    // the tick and the viewport reconcile all read - see the note on v3_attach_detour. It also
    // feeds two profiler-only records. (An earlier note here said v3_try_visual_move performs an
    // engine attach through this trampoline; that function had no callers and is gone.)
    try
    {
        modutils::hook<V3AttachFn>(
            {.aob = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 40 "
                    "48 8B DA 45 8B F0 48 8B 51 18 48 8B E9 48 8B 4B 38 8B 82 E0 00 00 00"},
            v3_attach_detour, o_v3_attach);
        spdlog::info("[stallprobe] v3 high-level attach experiment armed");
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[stallprobe] v3 high-level attach experiment unavailable: {}", e.what());
    }

    if (!goblin::config::native_menu_enabled())
    {
        // menu_render_mode = imgui: everything from here down exists only to serve the IN-GAME menu
        // - its tab, its page and row hooks, its row icons, the load-time movie transform - so none of
        // it is patched into the game. What sits above this line (the map's own native panels among
        // them) is unaffected and keeps working.
        spdlog::info("[nmenu] in-game menu not injected (menu_render_mode = imgui)");
        return;
    }

    // The list's row-path helper: its argument pair is the slot about to be drawn, which is how
    // row icons find their clip. A miss only costs icons.
    try
    {
        // By SIGNATURE, not by address - see report 20 and the note in aob_signatures.py. The
        // literal 0x736FC0 was measured on game build 2.6.2.0 and would land inside an instruction
        // on any other, which MinHook would then overwrite.
        modutils::hook<RowPathFn>(
            {.aob = "4C 8B DC 57 48 81 EC 90 00 00 00 49 C7 43 90 FE FF FF FF 49 89 5B 20 48 8B 05 "
                    "?? ?? ?? ?? 48 33 C4 48 89 84 24 80 00 00 00 48 8B FA 48 8B D9 49 89 53 98 C7 "
                    "44 24 20 00 00 00 00 45 8B 08 45 8B 40 04"},
            row_path_detour, o_row_path);
        spdlog::info("[menuicons] row-slot hook armed");
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[menuicons] row-slot hook unavailable: {}", e.what());
    }

    // Input-action predicate hook. Was a diagnostic (names the action ESC produces, inert unless
    // armed); since 2026-09-02 it is LIVE: while the search page's text row is being typed into,
    // it answers "no Back" for a Backspace or Q press (see action_test_detour), so those keys
    // erase a character instead of leaving the page. Installed always; the per-call cost is one
    // relaxed atomic load on the way to the original.
    try
    {
        modutils::hook<ActionTestFn>(
            {.aob = "4C 8B DC 48 81 EC 88 00 00 00 49 C7 43 98 FE FF FF FF 49 8D 43 B0 49 89 43 20 "
                    "41 89 53 18 41 0F B6 00"},
            action_test_detour, o_action_test);
        spdlog::info("[action] input-action predicate hook armed (inert until ESC)");
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[action] input-action hook unavailable: {}", e.what());
    }

    // FIVE HOOKS AND TWO AOB SCANS FOR THE F11 SETTINGS TAB WERE ARMED HERE UNTIL 2026-07-31:
    // the OptionSettingTopDialog ctor and dtor, the tab-append, the show-page dispatch and the
    // borrowed page's populate, plus scans for the category build/destruct primitives. Every one
    // of them patched a function in the GAME'S OWN settings menu, on every launch, to serve a
    // feature that had no trigger - see the note where that code used to live. Removing them takes
    // five prologue rewrites and two .text scans off every start, and six [optmenu] literals out
    // of the binary.

    // Keybinding-form row hooks (dev prototype, keysetting_*_re.txt): the row-vector
    // builder FUN_140868590 and the row VALUE provider FUN_140867de0. Both are cold
    // options-UI functions and both pass straight through unless our form is armed, so
    // the game's own keybinding screen is untouched. Hooked by address: their prologues
    // are short/shared, so an AOB would not be unique.
    //
    // PATCHING BY ADDRESS IS THE ONE THING THAT MUST NOT BE ATTEMPTED ON AN EXE WE DID NOT
    // MEASURE. Each of these four rewrites a prologue in place, so a shifted address means
    // MinHook overwrites the middle of an unrelated instruction - the report-20 failure mode,
    // but on every launch instead of on one call. So they take resolved addresses only, and
    // only when EVERY anchor resolved: a partial table means the shift prior is unproven, and
    // an unproven prologue address is not worth a cosmetic dev feature.
    {
        const bool anchored = goblin::anchors::all_ok();
        const uintptr_t a_build = anchored ? goblin::anchors::at(goblin::AnchorId::menu_row_build_dispatch) : 0;
        const uintptr_t a_render = anchored ? goblin::anchors::at(goblin::AnchorId::menu_row_render) : 0;
        const uintptr_t a_decide = anchored ? goblin::anchors::at(goblin::AnchorId::menu_row_decide) : 0;
        const uintptr_t a_update = anchored ? goblin::anchors::at(goblin::AnchorId::form_update_heartbeat) : 0;
        if (a_build && a_render && a_decide && a_update)
        {
            modutils::hook(reinterpret_cast<void *>(a_build),
                           reinterpret_cast<void *>(&build_items_detour),
                           reinterpret_cast<void **>(&o_build_items));
            // Row DRAW (the item's vt+0x8): we paint name/value ourselves, so our rows are
            // not limited to FMG strings.
            modutils::hook(reinterpret_cast<void *>(a_render),
                           reinterpret_cast<void *>(&row_render_detour),
                           reinterpret_cast<void **>(&o_row_render));
            modutils::hook(reinterpret_cast<void *>(a_decide),
                           reinterpret_cast<void *>(&form_decide_detour),
                           reinterpret_cast<void **>(&o_form_decide));
            // The dialog's per-frame update, used only as a liveness heartbeat.
            modutils::hook(reinterpret_cast<void *>(a_update),
                           reinterpret_cast<void *>(&form_update_detour),
                           reinterpret_cast<void **>(&o_form_update));
        }
        if (o_build_items && o_row_render && o_form_decide)
            spdlog::info("[form] keybinding-form row hooks armed (build + draw + decide)");
        else if (!anchored)
            spdlog::warn("[form] row hooks NOT armed: this exe build is not the one this DLL "
                         "was measured on and not every helper could be located");
        else
            spdlog::warn("[form] keybinding-form row hooks incomplete; row swap off");
    }

    // Job-push primitives: ref-move + push-job (they reuse the confirm-dialog machinery), plus the
    // two singleton slots resolved alongside them. This is what open_screen() uses to put our screen
    // up over plain gameplay; a miss disables that, and the two slots only cost the HUD-mode restore
    // and the in-game test.
    // (A third scan, p_build_job, resolved the settings-open job builder for the retired F11
    //  prototype. It was never called - only null-checked, and its result gated the log line below,
    //  which is why that line announced "settings-open (F11) primitives resolved" in a build with no
    //  F11 at all.)
    try
    {
        p_refmove = reinterpret_cast<RefMoveFn *>(modutils::scan<void>(
            {.aob = "48 89 54 24 10 53 48 83 EC 30 48 C7 44 24 28 FE FF FF FF 48 8B DA "
                    "C7 44 24 20 00 00 00 00 48 8B 09 48 89 0A 48 85 C9 74"}));
        p_push_job = reinterpret_cast<PushJobFn *>(modutils::scan<void>(
            {.aob = "4C 89 4C 24 20 48 89 54 24 10 55 56 57 41 56 41 57 48 81 EC A0 00 00 00 "
                    "48 C7 44 24 40 FE FF FF FF 48 89 9C 24 D0 00 00"}));
        // CSFeManImp singleton slot: the store at the tail of its init. Unique in the exe and it
        // resolves to the same 0x3D6B880 the live process uses; a miss only disables the HUD-mode
        // restore.
        g_feman_slot = reinterpret_cast<void **>(modutils::scan<void *>(
            {.aob = "48 89 05 ?? ?? ?? ?? 48 8B 8B 80 00 00 00 48 85 C9 74 05 E8",
             .relative_offsets = {{3, 7}}}));
        p_worldchrman_slot = reinterpret_cast<void **>(modutils::scan<void *>(
            {.aob = "48 8B 05 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? ?? 48 8B 98 08 E5 01 00",
             .relative_offsets = {{3, 7}}}));
        if (p_refmove && p_push_job)
            spdlog::info("[form] job-push primitives resolved (worldChrMan={})",
                         p_worldchrman_slot != nullptr);
        else
            spdlog::warn("[form] job-push primitives incomplete; over-gameplay open off");
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[form] job-push primitive AOB miss: {}", e.what());
    }

    // Task #9 capture: hook the SAME pushJob AFTER the raw scan above (the hook
    // rewrites the prologue, so scanning must come first). Pass-through logger; a
    // miss just disables the capture.
    try
    {
        modutils::hook<PushJobFn>(
            {.aob = "4C 89 4C 24 20 48 89 54 24 10 55 56 57 41 56 41 57 48 81 EC A0 00 00 00 "
                    "48 C7 44 24 40 FE FF FF FF 48 89 9C 24 D0 00 00"},
            pushjob_detour, o_push_job);
        spdlog::info("[pushjob] push-job capture hook armed");
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[pushjob] capture hook unavailable: {}", e.what());
    }

    // Task #9 capture 2: the generic settings movie-job builder FUN_140808630.
    // Pass-through logger; a miss just disables the capture.
    try
    {
        modutils::hook<BuilderFn>(
            {.aob = "40 55 56 57 41 56 41 57 48 8D 6C 24 C0 48 81 EC 40 01 00 00 "
                    "48 C7 44 24 50 FE FF FF FF 48 89 9C 24 88 01 00 00 48 8B 05 ?? ?? ?? ?? "
                    "48 33 C4 48 89 45 30 4D 8B F0 48 8B F2 48 8B F9"},
            builder_detour, o_builder);
        spdlog::info("[subopen] movie-job builder capture hook armed");
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[subopen] builder capture hook unavailable: {}", e.what());
    }

    // The memo-dialog proto's three AOB scans stood here. They resolved the view-couple, the
    // game allocator and the allocator global on every startup, and the only thing that ever
    // read the three results was the log line that printed them: no detour was installed, and
    // the prototype they belonged to is gone (see the note further up this file). Removing them
    // takes three .text scans off every launch.

    // (The attachMovie bridge and the high-level attach hook moved ABOVE the menu-mode gate -
    //  they serve the map markers, not the in-game menu; see there.)

    // Close-wait job timing (profiler-only; pass-through outside capture windows).
#if MFG_STALL_PROFILER
    try
    {
        modutils::hook<Fn4>(
            // The call's rel32 was baked in, so this was a 2.6.2-only pattern. Wildcarded it
            // matches two places on every build, and both are byte-identical copies of the
            // same poll (measured on 2.6.2/2.6.1/2.6.0/2.2.3/2.2.0), so the first-match rule
            // lands on the same code either way.
            {.aob = "48 83 EC 28 48 8B 09 48 85 C9 74 16 48 83 C1 10 E8 ?? ?? ?? ?? 85 C0 0F"},
            jobpoll_detour, o_jobpoll);
        spdlog::info("[stallprobe] job-poll spy armed");
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[stallprobe] job-poll spy unavailable: {}", e.what());
    }
#endif // MFG_STALL_PROFILER
}

void goblin::stall_probe::capture(const char *tag, unsigned duration_ms)
{
#if MFG_STALL_PROFILER
    // This suspends the calling (map UI) thread from a worker for the whole window - see
    // goblin_build_variants.hpp. The `#if` is not decoration: a runtime `if (kStallProfiler)`
    // still leaves every string literal and every imported API of the sampler in the shipped
    // binary, because unreferenced .rdata is not swept the way unreferenced code is. Measured on
    // the 2026-07-30 build: with the runtime gate, "spy armed" was in the DLL 4 times and
    // "stallprobe" 18 times with the profiler OFF.
    if (!goblin::config::debugLogging) return;
    bool expected = false;
    if (!g_running.compare_exchange_strong(expected, true)) return;
    DWORD tid = GetCurrentThreadId();
    try
    {
        std::thread(run_capture, std::string(tag ? tag : "capture"), tid, duration_ms).detach();
    }
    catch (...)
    {
        g_running.store(false);
    }
#else
    (void)tag;
    (void)duration_ms;
#endif
}
