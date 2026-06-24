#include "goblin_gfx_probe.hpp"

#include "goblin_config.hpp"
#include "generated_shared/goblin_map_icons.hpp" // one DefineBitsLossless2 tag per custom map icon
#include "generated_shared/goblin_logo.hpp"       // runtime-injectable MapForGoblins logo (bitmap + matrix)
#include "generated/goblin_item_icons.hpp"       // goblin::generated::ANON_ICON_ID
#include "goblin_inject.hpp"   // goblin::remap_injected_icons (point markers at our injected frames)
#include "goblin_diag.hpp"     // goblin::diag inject-status registry (overlay Debug readout)
#include "modutils.hpp"

#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Path A (runtime icon-frame injection) instrumentation + PoC injector.
//
// We hook the GAME's OWN GFx SpriteDef CONSTRUCTOR (0x1411be1f0): it hands us the real
// SpriteDef `this` for every DefineSprite as the worldmap movie loads. The ctor hook only
// catches sprite 171 if our DLL loads BEFORE the movie - the offline/ME loader (shipping
// path) loads at process start and catches it; the dev injector loads too late. We ring-buffer
// the pointers and, once the worldmap movie finished loading (frameCount populated), find the
// one with charId@+0x18==171.
//
// LIVE-CONFIRMED SpriteDef layout (this build): charId@+0x18, frameCount@+0x30, vtable
// 0x142CC2E58; sealed frame GArray ON the SpriteDef: data@+0x38, count@+0x40, cap@+0x48; frame
// element stride 0x10 = {ExecuteTag** tags@+0, u32 tagCount@+8}. The tag objects are immutable
// display instructions, so a frame can SHARE another frame's tag list (shallow clone).
//
// Icon injection is UNCONDITIONAL: we register our DefineBitsLossless2 bitmaps in the live worldmap
// movie, append a frame per icon to SpriteDef-171, and remap markers to the injected frames - so icons
// render without a modified/shipped gfx. This is the mod's normal icon path (no ini toggle).
//   debug_logging (ini, default off) - DEV ONLY: additionally dumps the live SpriteDef/dict and arms the
//                              read-only RM2::Execute trace. Never changes injection behavior.
// See docs/research_no_gfx_icons.md.
namespace
{
    using CtorFn = void *(void *, void *); // rcx = new SpriteDef `this`, rdx = CDEF (load ctx)
    CtorFn *o_ctor = nullptr;

    // AddDisplayObject 0x140f01b10: rcx = timeline ctx; movieDef = *(rcx - 0x10). We hook it only
    // to capture movieDef deterministically (the read-side resource dict is at movieDef+0xd8).
    using AddDispFn = void *(void *, void *, void *, void *);
    AddDispFn *o_adddisp = nullptr;
    std::atomic<uint64_t> g_moviedef{0};
    std::atomic<unsigned> g_adddisp_calls{0};

    // Resource-dict lookup 0x14113fd90: rcx = container (= movieDef+0xd8), edx = charId. The most
    // reliable movieDef source - every charId resolution funnels here (both AddDisplayObject and the
    // 2nd caller). movieDef = rcx - 0xd8.
    using LookupFn = void *(void *, uint32_t, void *);
    LookupFn *o_lookup = nullptr;
    std::atomic<unsigned> g_lookup_calls{0};

    // R2 diagnostics: lossless image loader 0x1411e2670 (rcx = movieContext, rdx = tagInfo).
    using LosslessFn = void *(void *, void *);
    LosslessFn *o_lossless = nullptr;
    std::atomic<unsigned> g_lossless_calls{0};
    std::atomic<uint64_t> g_lossless_ctx{0};
    std::atomic<bool> g_r2_dumped{false};

    // DefineSprite loader 0x1411e2bc0 (rcx = movieContext). We hook it to inject our embedded "?"
    // bitmap by calling the NATIVE lossless loader during the worldmap load (when the load context +
    // image manager are valid) - feeding it our tag bytes via the live reader. Then charId 13507 is
    // registered natively into both dict containers, and we just place it in frames post-load.
    using SpriteLoaderFn = void *(void *, void *);
    SpriteLoaderFn *o_spriteloader = nullptr;
    std::atomic<bool> g_qmark_injected{false}; // frames appended (per worldmap load)
    std::atomic<bool> g_img_registered{false}; // our bitmaps registered (at native-13507 load)

    // srcIconId (the gfx iconId a category's markers are baked with) -> our injected iconId. Filled at
    // worldmap load; read by remap_injected_icons. srcIconIds are small (369..441) so a flat array is fine.
    constexpr int ICON_MAP_SIZE = 1024;
    uint32_t g_icon_iid[ICON_MAP_SIZE] = {0};

    void inject_all_icons(uint64_t ctx);      // defined below; called from lossless_detour (13507 moment)
    bool inject_logo_into_plaque(uint64_t sd); // defined below; called from spriteloader_detour (sprite 246)

    // DIAGNOSTIC: hook RemoveObject2::Execute (0x1411bcce0) to see what happens when OUR injected RM2
    // tags run - does it find the display entry at the depth, and is that entry sticky ([entry+0x71]&2)?
    // rcx=this(tag), rdx=ctx(display container; list base@+0x28, count@+0x30, node depth@+0x14), r8d=frame.
    using ExecFn = void *(void *, void *, uint32_t);
    ExecFn *o_rm2exec = nullptr;
    std::atomic<uint64_t> g_my_rm2_d1{0}, g_my_rm2_d2{0};
    std::atomic<unsigned> g_rmhook_logs{0};
    std::atomic<bool> g_in_inject{false};

    // ── Runtime injected-charId base (no build-time / per-profile guess) ───────────────────────────
    // The common resource-dict registrar 0x1411cf250 runs for EVERY native charId as a movie streams in
    // (rcx = movieDef/load ctx, r8 = &charId u32; confirmed: registrar derives dictOwner = [rcx+0x38],
    // the same dictOwner the DefineSprite loader uses from [loadctx+0x38] -> rcx == loadctx == movieDef).
    // We hook it to track each movie's real max NATIVE charId at runtime, SCOPED BY ctx (not global), so
    // our injected bitmaps sit above every native id no matter which/whose gfx actually loaded. This is
    // why no per-profile bake is needed and external gfx edits can't break us.
    constexpr uint64_t RVA_FN_DICT_REGISTRAR = 0x11CF250;
    using RegistrarFn = void *(void *, void *, void *, void *);
    RegistrarFn *o_registrar = nullptr;
    constexpr int REGN = 16;                    // a few movies can be loading/resident at once
    std::atomic<uint64_t> g_reg_ctx[REGN] = {};
    std::atomic<uint32_t> g_reg_max[REGN] = {};
    constexpr uint32_t CHARID_SANE_CEIL = 0xF0000; // ignore absurd ids (garbage, not a real charId)
    constexpr uint32_t INJECT_BASE_MARGIN = 64;    // headroom over the observed native max

    void reg_note(uint64_t ctx, uint32_t cid) // cheap; called very frequently during movie load
    {
        if (!ctx || cid == 0 || cid >= CHARID_SANE_CEIL) return;
        int slot = -1;
        for (int i = 0; i < REGN; ++i)
        {
            uint64_t c = g_reg_ctx[i].load(std::memory_order_relaxed);
            if (c == ctx)
            {
                uint32_t m = g_reg_max[i].load(std::memory_order_relaxed);
                while (cid > m && !g_reg_max[i].compare_exchange_weak(m, cid, std::memory_order_relaxed)) {}
                return;
            }
            if (c == 0 && slot < 0) slot = i;
        }
        if (slot >= 0)
        {
            uint64_t expect = 0;
            if (g_reg_ctx[slot].compare_exchange_strong(expect, ctx, std::memory_order_relaxed))
                g_reg_max[slot].store(cid, std::memory_order_relaxed);
            else
                reg_note(ctx, cid); // slot was taken meanwhile (rare); retry
        }
    }
    uint32_t reg_max_for(uint64_t ctx)
    {
        for (int i = 0; i < REGN; ++i)
            if (g_reg_ctx[i].load(std::memory_order_relaxed) == ctx)
                return g_reg_max[i].load(std::memory_order_relaxed);
        return 0;
    }

    // Injected-bitmap base charId, computed live at the 13507 inject moment (0 until then). When unset
    // (hook never fired / non-worldmap path) we fall back to the generated compile-time constant.
    std::atomic<uint32_t> g_inject_base{0};
    uint32_t inject_base()
    {
        uint32_t b = g_inject_base.load(std::memory_order_relaxed);
        return b ? b : (uint32_t)goblin::generated::MAP_ICON_CHARID_BASE;
    }

    // On-map MapForGoblins logo. The gfx baked it into the decorative-plaque sprite 246 (re-pointing its
    // char-10 PlaceObject to a logo bitmap-fill shape, scale 0.38 / translate -243). At runtime we instead
    // register the logo bitmap at charId = inject_base()+MAP_ICON_TAG_COUNT (right after the icons, at the
    // 13507 moment) and re-point sprite 246's char-10 PO3 to it with the baked LOGO_MATRIX. Sprite 246
    // loads AFTER 13507 (gfx tag-stream order: 13507 #175 -> sprite246 #291), so the base/charId are ready.
    constexpr uint32_t LOGO_PLAQUE_SPRITE = 246;
    constexpr uint16_t LOGO_PLAQUE_CHAR = 10;
    std::atomic<uint32_t> g_logo_charid{0};
    std::atomic<bool> g_logo_placed{false};
    std::atomic<uint64_t> g_worldmap_ctx{0}; // the worldmap movie's load ctx (captured at its 13507)
    uint32_t logo_charid() { return inject_base() + (uint32_t)goblin::generated::MAP_ICON_TAG_COUNT; }

    void *registrar_detour(void *rcx, void *rdx, void *r8, void *r9)
    {
        if (r8) reg_note((uint64_t)rcx, *reinterpret_cast<const uint32_t *>(r8)); // registrar itself derefs [r8]
        return o_registrar(rcx, rdx, r8, r9);
    }

    // XInput gamepad injection: hook XInputGetState (game reads XINPUT1_4) and OR in the buttons we
    // want, driven by sentinel file scratch\pad.cmd ("<hexmask> <frames>"). Bypasses ER's injected-
    // KEYBOARD filter because it's the game's OWN XInput poll returning our state.

    constexpr int RING = 8192;
    void *g_sprites[RING] = {nullptr};
    std::atomic<unsigned> g_idx{0};
    std::atomic<uint64_t> g_found_sd{0}; // located SpriteDef-171 (0 until found)
    std::atomic<bool> g_dict_dumped{false};

    // Resource dict READ container (movieDef+0xd8) - live-confirmed via AddDisplayObject disasm:
    //   [+0x00]=slot array base, [+0x08]=count; slot stride 16 (slot[0]=node ptr); node charId@+0x2c.
    constexpr unsigned OFF_MOVIEDEF_DICT = 0xd8;
    constexpr unsigned OFF_NODE_CHARID = 0x2c;

    // SpriteDef offsets (live-confirmed, this build).
    constexpr unsigned OFF_CHARID = 0x18;
    constexpr unsigned OFF_FRAMECOUNT = 0x30;
    constexpr unsigned OFF_FRAMEARR_DATA = 0x38;
    constexpr unsigned OFF_FRAMEARR_COUNT = 0x40;
    constexpr unsigned OFF_FRAMEARR_CAP = 0x48;
    constexpr unsigned FRAME_STRIDE = 0x10;
    // FALLBACK base charId for our injected bitmaps. The PRIMARY base is computed at RUNTIME from the
    // live native max charId of the actual worldmap movie (registrar hook -> reg_max_for -> inject_base),
    // so it survives any external gfx change with no per-profile bake. This generated constant is used
    // only if the registrar hook never observed the movie's ctx (inject_base() falls back to it).
    constexpr uint32_t INJECT_CHARID_BASE = goblin::generated::MAP_ICON_CHARID_BASE;
    // eldenring.exe RVAs of the Scaleform ExecuteTag vtables (proven via the SWF tag-loader dispatch +
    // each Execute's disassembly). The previous build had RM2 and PO2 SWAPPED, which is why our "removes"
    // were really PlaceObject2 re-places that never cleared anything. Correct mapping:
    //   RemoveObject2 vtable 0x142C65D80, Execute 0x1411bde10 - depth = bare u16 @body+0 (tag+8), NO flags
    //                                     byte, no short/long form; on a depth match it UNLINKS the node.
    //   PlaceObject2  vtable 0x142C65CC0, Execute 0x1411bcce0 - flags@body+0, depth u16 @body+1 (place).
    //   PlaceObject3  vtable 0x142C65D20, Execute 0x1411bdb40 - flags@body+0, depth u16 @body+2 (place).
    //   RemoveObject(tag5) vtable 0x142C65DD0 shares Execute 0x1411bde10 (also depth u16 @+8).
    // RM2 object is only 0x10 bytes {vtable@+0; depth u16 @+8}; the frame executor (0x1411bf131) dispatches
    // purely on tag[0]=vtable with no validity/heap check, so a VirtualAlloc'd synthesized tag runs like a
    // native one. See reference_scaleform_displaylist.
    constexpr uint64_t RVA_VT_REMOVEOBJECT2 = 0x2C65D80;     // FALLBACK only (vtables are captured live)
    constexpr uint64_t RVA_VT_PLACEOBJECT3 = 0x2C65D20;      // FALLBACK only
    constexpr uint64_t RVA_FN_REMOVEOBJECT2_EXEC = 0x11BDE10; // real RemoveObject2::Execute (for the diag hook)

    // Native tag vtables captured LIVE from the stock sprite-171 frames at load (a composite icon frame
    // is RM2+RM2+PO3). This avoids hardcoding the vtable addresses - the RVAs above are used only if the
    // capture fails. Set by capture_tag_vtables(); read by build_remove_tag / build_clean_place_tag.
    std::atomic<uint64_t> g_po3_vt{0};
    std::atomic<uint64_t> g_rm2_vt{0};
    uint64_t po3_vtable()
    {
        uint64_t v = g_po3_vt.load(std::memory_order_relaxed);
        return v ? v : (uint64_t)GetModuleHandleW(nullptr) + RVA_VT_PLACEOBJECT3;
    }
    uint64_t rm2_vtable()
    {
        uint64_t v = g_rm2_vt.load(std::memory_order_relaxed);
        return v ? v : (uint64_t)GetModuleHandleW(nullptr) + RVA_VT_REMOVEOBJECT2;
    }

    bool safe_copy(void *dst, const void *src, size_t n)
    {
        __try { memcpy(dst, src, n); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool looks_heap(uint64_t v) { return v > 0x10000ull && v < 0x7FF000000000ull; }
    uint64_t rq(uint64_t a) { uint64_t v = 0; return (a && safe_copy(&v, (void *)a, 8)) ? v : 0; }
    uint32_t rd32(uint64_t a) { uint32_t v = 0; return (a && safe_copy(&v, (void *)a, 4)) ? v : 0; }
    bool wr32(uint64_t a, uint32_t v) { return a && safe_copy((void *)a, &v, 4); }
    bool wr64(uint64_t a, uint64_t v) { return a && safe_copy((void *)a, &v, 8); }

    void dump_obj(const char *label, uint64_t addr)
    {
        unsigned char buf[0xC0];
        if (!addr || !safe_copy(buf, (void *)addr, sizeof(buf)))
        {
            spdlog::info("[gfxprobe] {} @ 0x{:X} <unreadable>", label, addr);
            return;
        }
        spdlog::info("[gfxprobe] ---- {} @ 0x{:X} ----", label, addr);
        for (size_t r = 0; r < sizeof(buf); r += 8)
        {
            uint64_t v;
            memcpy(&v, buf + r, 8);
            spdlog::info("[gfxprobe]   +0x{:02X}: {:016X}", (unsigned)r, v);
        }
    }

    // Find a charId's binding node in the read dict (movieDef+0xd8) and dump it + its resource, so we
    // can compare a gfx-native image (13507) vs our runtime-injected one (20000) side by side.
    void dump_charid_node(uint64_t movieDef, uint32_t charId, const char *label)
    {
        uint64_t cont = movieDef + OFF_MOVIEDEF_DICT;
        uint64_t base = rq(cont);
        uint32_t count = rd32(cont + 8);
        if (!looks_heap(base) || count == 0 || count > 100000)
            return;
        uint64_t node = 0;
        for (uint32_t k = 0; k < count; ++k)
        {
            uint64_t n = rq(base + (uint64_t)k * 16);
            if (n && rd32(n + OFF_NODE_CHARID) == charId) { node = n; break; }
        }
        if (!node)
        {
            spdlog::info("[cmp] {} charId {} NOT in dict.", label, charId);
            return;
        }
        spdlog::info("[cmp] {} charId {} node@0x{:X}:", label, charId, node);
        for (unsigned off = 0; off < 0x60; off += 8)
        {
            uint64_t v = rq(node + off);
            spdlog::info("[cmp]     +0x{:02X}: {:016X}{}", off, v, looks_heap(v) ? " <heap>" : "");
        }
    }

    void *ctor_detour(void *self, void *cdef)
    {
        unsigned i = g_idx.fetch_add(1, std::memory_order_relaxed);
        g_sprites[i % RING] = self;
        return o_ctor(self, cdef);
    }

    void *adddisp_detour(void *rcx, void *rdx, void *r8, void *r9)
    {
        g_adddisp_calls.fetch_add(1, std::memory_order_relaxed);
        if (!g_moviedef.load(std::memory_order_relaxed) && rcx)
        {
            uint64_t md = rq((uint64_t)rcx - 0x10); // movieDef = *(rcx - 0x10)
            if (looks_heap(md))
                g_moviedef.store(md, std::memory_order_relaxed);
        }
        return o_adddisp(rcx, rdx, r8, r9);
    }

    void *lossless_detour(void *rcx, void *rdx)
    {
        // Diagnostic only. The registration of OUR bitmaps NO LONGER happens here: it depended on the
        // gfx containing an embedded image (our "?" charId 13507) to land in a "warm image-manager"
        // moment, but a stock/overhaul gfx may carry ZERO embedded bitmaps (confirmed: this loader fired
        // 0 times on a pure-DLL run). Registration now runs at sprite-171 load instead (the image manager
        // is already warm there from the worldmap's external-image loads), which is gfx-content-independent.
        g_lossless_calls.fetch_add(1, std::memory_order_relaxed);
        if (!g_lossless_ctx.load(std::memory_order_relaxed) && rcx)
            g_lossless_ctx.store((uint64_t)rcx, std::memory_order_relaxed);
        return o_lossless(rcx, rdx);
    }

    // R2 read-only: dump the global image service [0x144593250] + its vtable, and whether any movie
    // loaded a lossless image (captured movieContext template for Route A).
    void dump_r2()
    {
        uint64_t exe = (uint64_t)GetModuleHandleW(nullptr);
        uint64_t svcptr = exe + (0x144593250ull - 0x140000000ull);
        uint64_t svc = rq(svcptr);
        uint64_t vt = rq(svc);
        spdlog::info("[r2] image service ptr@0x{:X} svc=0x{:X} vt=0x{:X}", svcptr, svc, vt);
        for (unsigned off = 0; off <= 0x70; off += 8)
            spdlog::info("[r2]   vtbl+0x{:02X}: 0x{:X}", off, rq(vt + off));
        unsigned lc = g_lossless_calls.load(std::memory_order_relaxed);
        uint64_t ctx = g_lossless_ctx.load(std::memory_order_relaxed);
        spdlog::info("[r2] lossless loader fired {} times; sample movieContext=0x{:X}", lc, ctx);
        if (ctx)
        {
            uint64_t mgr = rq(ctx + 0x18);
            spdlog::info("[r2]   ctx+0x18(mgr)=0x{:X} ->+0x40=0x{:X}; ctx+0x418(reader)=0x{:X}",
                         mgr, rq(mgr + 0x40), rq(ctx + 0x418));
        }
    }

    uint64_t build_clean_place_tag(uint16_t cid, uint16_t dp, const unsigned char *mtx, unsigned mlen); // below
    uint64_t build_remove_tag(uint16_t depth);                                // defined below
    void capture_tag_vtables(uint64_t sd);                                    // defined below
    uint32_t append_icon_frame(uint64_t sd, uint16_t newCharId, const unsigned char *mat, unsigned matLen); // defined below
    uint32_t compute_safe_base(uint64_t movieDef, uint32_t count);            // defined below (self-healing)
    void dump_frames(uint64_t sd);                                 // defined below

    // SEH-isolated call into the native lossless loader (kept object-free for __try/__except).
    void *seh_call_lossless(void *ctx, void *taginfo)
    {
        __try { return o_lossless(ctx, taginfo); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    // Peek the charId u16 at the head of the current tag, without consuming the reader.
    uint32_t peek_charid(uint64_t ctx)
    {
        uint64_t reader = rq(ctx + 0x418);
        if (!reader)
            reader = ctx + 0x50;
        uint32_t pos = rd32(reader + 0x4c);
        uint64_t buf = rq(reader + 0x60);
        if (!looks_heap(buf))
            return 0xFFFFFFFF;
        uint32_t lo = 0, hi = 0;
        if (!safe_copy(&lo, (void *)(buf + pos), 1) || !safe_copy(&hi, (void *)(buf + pos + 1), 1))
            return 0xFFFFFFFF;
        return lo | (hi << 8);
    }

    // Inject ONE DefineBitsLossless2 tag (an embedded bitmap) under `charId`, by driving the NATIVE
    // lossless loader with a borrowed reader pointed at our tag bytes. MUST run right after a native
    // lossless load (image-manager state fresh - any other moment builds a NULL pixel descriptor and
    // crashes). The loader STREAMS pixels from the MemoryFile source (reader+0x20), so we clone the
    // native MemoryFile and point it at our bytes. Returns true on a non-null loader result.
    bool inject_lossless_tag(uint64_t ctx, const unsigned char *bytes, unsigned len, uint16_t charId)
    {
        static unsigned char *buf = nullptr; // reused writable copy (grown to the largest tag)
        static unsigned bufcap = 0;
        if (bufcap < len)
        {
            if (buf) VirtualFree(buf, 0, MEM_RELEASE);
            buf = (unsigned char *)VirtualAlloc(nullptr, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            bufcap = buf ? len : 0;
        }
        if (!buf) return false;
        memcpy(buf, bytes, len);
        memcpy(buf, &charId, 2); // charId is the first u16 of the tag body

        uint64_t reader = rq(ctx + 0x418);
        if (!reader) reader = ctx + 0x50;
        uint64_t srcMF = rq(reader + 0x20);
        if (!looks_heap(srcMF)) return false;
        static uint64_t myMF = 0;
        if (!myMF)
        {
            void *mf = VirtualAlloc(nullptr, 0x48, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!mf || !safe_copy(mf, (void *)srcMF, 0x48)) return false;
            myMF = (uint64_t)mf;
        }
        wr64(myMF + 0x18, (uint64_t)buf); // data buffer = our tag
        wr32(myMF + 0x20, len);           // size
        wr32(myMF + 0x24, 0);             // pos = 0

        // save native reader state + window CONTENT (our refill dirties the shared window), restore after.
        uint64_t winbuf = rq(reader + 0x60);
        static unsigned char winsave[0x1000];
        bool havewin = winbuf && safe_copy(winsave, (void *)winbuf, sizeof(winsave));
        uint64_t s20 = rq(reader + 0x20);
        uint32_t s4c = rd32(reader + 0x4c), s50 = rd32(reader + 0x50), s54 = rd32(reader + 0x54);

        wr64(reader + 0x20, myMF);
        wr32(reader + 0x4c, 0); wr32(reader + 0x50, 0); wr32(reader + 0x54, 0);
        uint32_t taginfo[8] = {36u, 0, len, 0, 0, 0, 0, 0}; // [+0]=type36 (DefineBitsLossless2), [+8]=length

        g_in_inject.store(true, std::memory_order_relaxed);
        void *lret = seh_call_lossless((void *)ctx, taginfo);
        g_in_inject.store(false, std::memory_order_relaxed);

        wr64(reader + 0x20, s20);
        wr32(reader + 0x4c, s4c); wr32(reader + 0x50, s50); wr32(reader + 0x54, s54);
        if (havewin) safe_copy((void *)winbuf, winsave, sizeof(winsave));
        return lret != nullptr;
    }

    // Register ALL embedded map-icon bitmaps (charId BASE+i) at the proven 13507 moment.
    void inject_all_icons(uint64_t ctx)
    {
        int ok = 0;
        uint32_t base = inject_base();
        for (int i = 0; i < goblin::generated::MAP_ICON_TAG_COUNT; ++i)
        {
            const auto &e = goblin::generated::MAP_ICON_TAGS[i];
            if (inject_lossless_tag(ctx, e.tag, e.tagLen, (uint16_t)(base + i)))
                ++ok;
        }
        spdlog::info("[icons] registered {}/{} bitmaps (charId {}..{}).", ok,
                     goblin::generated::MAP_ICON_TAG_COUNT, base,
                     base + goblin::generated::MAP_ICON_TAG_COUNT - 1);
        goblin::diag::set_bitmaps(ok, goblin::generated::MAP_ICON_TAG_COUNT);

        // Register the MapForGoblins logo bitmap right after the icons (same fresh-manager moment).
        uint32_t lcid = logo_charid();
        if (inject_lossless_tag(ctx, goblin::generated::LOGO_TAG, goblin::generated::LOGO_TAG_LEN, (uint16_t)lcid))
        {
            g_logo_charid.store(lcid, std::memory_order_relaxed);
            spdlog::info("[logo] registered logo bitmap at charId {}.", lcid);
        }
        else
            spdlog::warn("[logo] logo bitmap registration FAILED (charId {}).", lcid);
    }

    // SEH-isolated worldmap sprite-171 injection (POD-only locals so __try is legal): register our
    // bitmaps, append a frame per icon, remap markers. This runs for EVERY user on every worldmap load
    // (injection is unconditional), so a fault here - a wrong struct offset on some game/overhaul version,
    // a hostile movie, etc. - must NEVER crash the game. On AV we log and bail; the native loader already
    // ran (called before this), so the map still works, just without our icons that load.
    void seh_inject_sprite171(uint64_t sd, uint64_t ctx, uint32_t fcnt)
    {
        __try
        {
            capture_tag_vtables(sd); // grab native PO3/RM2 vtables from stock frames (no hardcode)
            // Compute the charId base self-healingly (high floor, raised above any live charId, window
            // verified clear). Register our bitmaps (image manager is warm from the worldmap's external
            // images), then append a frame per icon and remap markers to the injected iconIds.
            uint32_t base = compute_safe_base(ctx, (uint32_t)goblin::generated::MAP_ICON_TAG_COUNT + 1);
            g_inject_base.store(base, std::memory_order_relaxed);
            g_worldmap_ctx.store(ctx, std::memory_order_relaxed); // scope sprite-246 logo to THIS movie
            uint64_t sub = rq(ctx + 0x18);
            uint64_t mgr = sub ? rq(sub + 0x40) : 0;
            spdlog::info("[icons] sprite-171 load: safe base={} (floor {}); reg_max={}; "
                         "img-manager [[ctx+0x18]+0x40]=0x{:X} (must be non-null to register).",
                         base, INJECT_CHARID_BASE, reg_max_for(ctx), mgr);
            inject_all_icons(ctx); // registers all icon bitmaps + the logo bitmap
            spdlog::info("[icons] worldmap sprite-171 (frameCount={}) loading; appending {} icon frames.",
                         fcnt, goblin::generated::MAP_ICON_TAG_COUNT);
            for (int k = 0; k < ICON_MAP_SIZE; ++k) g_icon_iid[k] = 0;
            int placed = 0;
            for (int i = 0; i < goblin::generated::MAP_ICON_TAG_COUNT; ++i)
            {
                const goblin::generated::MapIconTag e = goblin::generated::MAP_ICON_TAGS[i]; // POD copy
                uint32_t iid = append_icon_frame(sd, (uint16_t)(inject_base() + i), e.matrix, e.matrixLen);
                if (iid && e.srcIconId >= 0 && e.srcIconId < ICON_MAP_SIZE)
                {
                    g_icon_iid[e.srcIconId] = iid;
                    ++placed;
                }
            }
            dump_frames(sd); // DIAGNOSTIC: live tag layout of key frames
            spdlog::info("[icons] appended {} frames; remapping markers.", placed);
            goblin::diag::set_sprite171(true, base, placed, goblin::generated::MAP_ICON_TAG_COUNT, "");
            goblin::remap_injected_icons(); // point markers at our injected iconIds (before pins built)
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            spdlog::error("[icons] EXCEPTION during sprite-171 injection - icons may be incomplete this "
                          "load; game kept alive.");
            goblin::diag::set_sprite171(false, inject_base(), 0, goblin::generated::MAP_ICON_TAG_COUNT,
                                        "exception during injection (icons missing this load)");
        }
    }

    // SEH-isolated logo re-point (POD-only). Same all-users rationale.
    bool seh_inject_logo(uint64_t sd)
    {
        __try { return inject_logo_into_plaque(sd); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void *spriteloader_detour(void *rcx, void *rdx)
    {
        uint32_t cid = (rcx ? peek_charid((uint64_t)rcx) : 0xFFFFFFFF); // charId before the load consumes it
        void *ret = o_spriteloader(rcx, rdx);
        if (cid == 171 && rcx && !g_qmark_injected.load(std::memory_order_relaxed))
        {
            // gate to the WORLDMAP sprite-171 (hundreds of frames); other movies' 171 have 1 frame.
            unsigned i = g_idx.load(std::memory_order_relaxed);
            uint64_t sd = i ? (uint64_t)g_sprites[(i - 1) % RING] : 0;
            uint32_t fcnt = sd ? rd32(sd + OFF_FRAMECOUNT) : 0;
            if (fcnt >= 300 && fcnt <= 4096) // worldmap sprite-171 on any base (stock 348 / our 440 / Convergence 756)
            {
                g_qmark_injected.store(true, std::memory_order_relaxed);
                seh_inject_sprite171(sd, (uint64_t)rcx, fcnt); // SEH-guarded (runs for every user on map load)
            }
        }
        else if (cid == LOGO_PLAQUE_SPRITE && rcx &&
                 (uint64_t)rcx == g_worldmap_ctx.load(std::memory_order_relaxed) &&
                 !g_logo_placed.load(std::memory_order_relaxed))
        {
            // Same worldmap movie as the icon inject (gated by ctx): re-point the plaque's char-10
            // placement to our logo bitmap. Sprite 246 loads after 13507, so the logo charId is set.
            unsigned i = g_idx.load(std::memory_order_relaxed);
            uint64_t sd = i ? (uint64_t)g_sprites[(i - 1) % RING] : 0;
            uint32_t scid = sd ? rd32(sd + OFF_CHARID) : 0;
            if (sd && scid == LOGO_PLAQUE_SPRITE)
            {
                if (seh_inject_logo(sd))
                {
                    g_logo_placed.store(true, std::memory_order_relaxed);
                    goblin::diag::set_logo(true, "");
                }
                else
                    goblin::diag::set_logo(false, "plaque re-point failed");
            }
        }
        return ret;
    }

    // DIAGNOSTIC detour on RemoveObject2::Execute. For OUR injected RM2 tags, inspect the display list
    // (ctx+0x28 base, ctx+0x30 count; node depth@+0x14, sticky flag@+0x71) for an entry at our depth,
    // BEFORE the original runs - tells us whether the anon's layer is present and sticky.
    void *rm2exec_detour(void *thisTag, void *ctx, uint32_t frame)
    {
        uint64_t t = (uint64_t)thisTag;
        if ((t == g_my_rm2_d1.load(std::memory_order_relaxed) ||
             t == g_my_rm2_d2.load(std::memory_order_relaxed)) &&
            g_rmhook_logs.fetch_add(1, std::memory_order_relaxed) < 40)
        {
            uint16_t dep = 0; safe_copy(&dep, (void *)(t + 8), 2); // RM2 depth = u16 @body+0 (tag+8)
            uint64_t cx = (uint64_t)ctx;
            uint64_t lbase = rq(cx + 0x28);
            uint64_t lcnt = rq(cx + 0x30);
            uint64_t foundNode = 0; uint8_t f71 = 0; uint32_t fcid = 0;
            if (looks_heap(lbase) && lcnt > 0 && lcnt < 65536)
                for (uint64_t i = 0; i < lcnt; ++i)
                {
                    uint64_t node = rq(lbase + i * 8);
                    if (looks_heap(node) && rd32(node + 0x14) == dep)
                    {
                        foundNode = node;
                        safe_copy(&f71, (void *)(node + 0x71), 1);
                        break;
                    }
                }
            spdlog::info("[rmhook] my RM2 depth={} ctx=0x{:X} listBase=0x{:X} cnt={} -> entry@0x{:X} flag71=0x{:02X}",
                         dep, cx, lbase, lcnt, foundNode, f71);
        }
        return o_rm2exec(thisTag, ctx, frame);
    }

    // One-shot diagnostic: dump the live tag layout of selected frames so we can see, for THIS build's
    // gfx in memory, what RemoveObject2 vs PlaceObject tag objects actually look like (vtable + body).
    void dump_frames(uint64_t sd)
    {
        uint64_t fdata = rq(sd + OFF_FRAMEARR_DATA);
        uint32_t fcnt = rd32(sd + OFF_FRAMEARR_COUNT);
        if (!looks_heap(fdata) || fcnt == 0) return;
        uint32_t idxs[] = {0, 1, 2, 3, fcnt >= 3 ? fcnt - 3 : 0, fcnt >= 2 ? fcnt - 2 : 0, fcnt - 1};
        for (uint32_t ii = 0; ii < sizeof(idxs) / sizeof(idxs[0]); ++ii)
        {
            uint32_t fi = idxs[ii];
            uint64_t elem = fdata + (uint64_t)fi * FRAME_STRIDE;
            uint64_t tagsArr = rq(elem);
            uint32_t tc = rd32(elem + 8);
            spdlog::info("[framedump] frame[{}] elem@0x{:X} tagsArr=0x{:X} tagCount={}", fi, elem, tagsArr, tc);
            if (!looks_heap(tagsArr) || tc == 0 || tc > 32) continue;
            const uint64_t RM2_VT = (uint64_t)GetModuleHandleW(nullptr) + RVA_VT_REMOVEOBJECT2;
            for (uint32_t k = 0; k < tc; ++k)
            {
                uint64_t t = rq(tagsArr + (uint64_t)k * 8);
                unsigned char b[0x20] = {0};
                safe_copy(b, (void *)t, sizeof(b));
                uint64_t vt = 0; memcpy(&vt, b, 8);
                const char *kind = (vt == RM2_VT) ? "RM2" : "PO?";
                spdlog::info("[framedump]   tag[{}]@0x{:X} {} +0x08={:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} +0x10={:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} +0x18={:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                             k, t, kind,
                             b[0x08], b[0x09], b[0x0A], b[0x0B], b[0x0C], b[0x0D], b[0x0E], b[0x0F],
                             b[0x10], b[0x11], b[0x12], b[0x13], b[0x14], b[0x15], b[0x16], b[0x17],
                             b[0x18], b[0x19], b[0x1A], b[0x1B], b[0x1C], b[0x1D], b[0x1E], b[0x1F]);
            }
        }
    }

    // Append ONE frame at the END of the live timeline (any base gfx), placing newCharId, and return its
    // 1-based iconId (= the new frameCount). Frame = RemoveObject2(d1)+RemoveObject2(d2)+PlaceObject(@d1)
    // - exactly what a gfx-authored composite frame does, so it clears the predecessor and shows only our
    // icon. Returns 0 on failure. Called once per embedded icon at worldmap load.
    uint32_t append_icon_frame(uint64_t sd, uint16_t newCharId, const unsigned char *mat, unsigned matLen)
    {
        uint64_t fdata = rq(sd + OFF_FRAMEARR_DATA);
        uint32_t fcnt = rd32(sd + OFF_FRAMEARR_COUNT);
        uint32_t fcap = rd32(sd + OFF_FRAMEARR_CAP);
        if (fcnt == 0 || fcnt > 8192 || !looks_heap(fdata))
        {
            spdlog::warn("[icons] frame array looks wrong (cnt={}); skip.", fcnt);
            return 0;
        }
        // Replicate EXACTLY what a gfx-authored composite frame does (add_gfx_icon.py, proven in-game to
        // cleanly clear the predecessor): RemoveObject2(depth 1) + RemoveObject2(depth 2) + PlaceObject
        // (our image @depth 1). We SYNTHESIZE the two RemoveObject2 tags directly (build_remove_tag) -
        // a RemoveObject2 is just {vtable=RemoveObject2 @+0; depth u16 @+8}. (Earlier this borrowed/cloned
        // tags by a vtable that was actually PlaceObject2, so the "removes" re-placed and never cleared
        // depth 1 - that was the root-cause bug.) The real RemoveObject2::Execute unlinks the display node
        // at the depth, so depths 1 and 2 of the predecessor composite are cleared before our place.
        uint64_t rm2_d1 = build_remove_tag(1);
        uint64_t rm2_d2 = build_remove_tag(2);
        g_my_rm2_d1.store(rm2_d1, std::memory_order_relaxed); // for the rm2exec_detour diagnostic
        g_my_rm2_d2.store(rm2_d2, std::memory_order_relaxed);
        // The placement matrix is PER-ICON (passed in): each icon's bitmap is cropped tight, so its
        // centering depends on its own W x H. Computed at build time by generate_map_icons.icon_matrix().
        uint64_t placeTag = build_clean_place_tag(newCharId, 1, mat, matLen); // synthesized (no grace clone)
        if (!placeTag)
        {
            spdlog::warn("[icons] build_clean_place_tag failed; abort.");
            return 0;
        }
        uint64_t tags[3];
        uint32_t tagCount = 0;
        if (rm2_d1) tags[tagCount++] = rm2_d1;
        if (rm2_d2) tags[tagCount++] = rm2_d2;
        tags[tagCount++] = placeTag;
        uint64_t *tagsArr = (uint64_t *)VirtualAlloc(nullptr, (size_t)tagCount * 8, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!tagsArr)
        {
            spdlog::warn("[icons] tags array alloc failed; abort.");
            return 0;
        }
        for (uint32_t j = 0; j < tagCount; ++j)
            tagsArr[j] = tags[j];
        unsigned char elem[FRAME_STRIDE] = {0};
        memcpy(elem, &tagsArr, 8);
        memcpy(elem + 8, &tagCount, 4);

        uint32_t target = fcnt + 1; // append exactly one frame at the end
        uint64_t arr = fdata;
        if (target > fcap)
        {
            uint32_t newcap = target + 64;
            void *buf = VirtualAlloc(nullptr, (size_t)newcap * FRAME_STRIDE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!buf || !safe_copy(buf, (void *)fdata, (size_t)fcnt * FRAME_STRIDE))
            {
                spdlog::warn("[icons] frame grow failed; abort.");
                return 0;
            }
            arr = (uint64_t)buf;
            wr64(sd + OFF_FRAMEARR_DATA, arr);
            wr32(sd + OFF_FRAMEARR_CAP, newcap);
        }
        safe_copy((void *)(arr + (uint64_t)fcnt * FRAME_STRIDE), elem, FRAME_STRIDE);
        wr32(sd + OFF_FRAMEARR_COUNT, target);
        wr32(sd + OFF_FRAMECOUNT, target);
        uint32_t iconid = fcnt + 1; // 1-based iconId of the appended frame
        spdlog::debug("[icons] frame iconId {} charId {} ({} RM2 + place)", iconid, newCharId, tagCount - 1);
        return iconid;
    }

    void *lookup_detour(void *rcx, uint32_t charId, void *r8)
    {
        g_lookup_calls.fetch_add(1, std::memory_order_relaxed);
        // Capture ONLY when resolving charId 171 (the worldmap icon sprite) so movieDef is the
        // worldmap movie, not some other tiny menu movie that happens to resolve first.
        if (charId == 171 && !g_moviedef.load(std::memory_order_relaxed) && rcx)
        {
            uint64_t md = (uint64_t)rcx - OFF_MOVIEDEF_DICT; // container = movieDef+0xd8
            if (looks_heap(md))
                g_moviedef.store(md, std::memory_order_relaxed);
        }
        return o_lookup(rcx, charId, r8);
    }

    // READ-ONLY: dump the resource-dict read array so we can design resource insertion (R1) from
    // exact live structure. Logs count, head/tail nodes (charId@+0x2c, vtable), to learn sort order,
    // max charId, and a clonable image binding node. Never writes.
    void dump_dict(uint64_t movieDef)
    {
        uint64_t cont = movieDef + OFF_MOVIEDEF_DICT;
        uint64_t base = rq(cont);
        uint32_t count = rd32(cont + 8);
        spdlog::info("[dictdump] movieDef=0x{:X} container@0x{:X}: base=0x{:X} count={} (+0x10=0x{:X} +0x18=0x{:X})",
                     movieDef, cont, base, count, rq(cont + 0x10), rq(cont + 0x18));
        if (!looks_heap(base) || count == 0 || count > 100000)
        {
            spdlog::warn("[dictdump] container looks wrong; abort.");
            return;
        }
        auto node_at = [&](uint32_t k) { return rq(base + (uint64_t)k * 16); };
        // head + tail nodes: charId@+0x2c, to confirm ascending sort + find max charId
        spdlog::info("[dictdump] first 8 nodes (charId@+0x2c, vtable@+0):");
        for (uint32_t k = 0; k < 8 && k < count; ++k)
        {
            uint64_t n = node_at(k);
            spdlog::info("[dictdump]   [{}] node=0x{:X} charId={} vt=0x{:X}", k, n, rd32(n + OFF_NODE_CHARID), rq(n));
        }
        spdlog::info("[dictdump] last 6 nodes:");
        for (uint32_t k = (count > 6 ? count - 6 : 0); k < count; ++k)
        {
            uint64_t n = node_at(k);
            spdlog::info("[dictdump]   [{}] node=0x{:X} charId={} vt=0x{:X}", k, n, rd32(n + OFF_NODE_CHARID), rq(n));
        }
        // dump full structure of node[0] (low charId = image, the R1 clone template) and a mid node
        for (uint32_t idx : {0u, count / 2})
        {
            uint64_t nn = node_at(idx);
            spdlog::info("[dictdump] node [{}] @0x{:X} charId={} vt=0x{:X} - first 0x60 bytes:",
                         idx, nn, rd32(nn + OFF_NODE_CHARID), rq(nn));
            for (unsigned off = 0; off < 0x60; off += 8)
            {
                uint64_t v = rq(nn + off);
                const char *t = looks_heap(v) ? " <heap>" : "";
                spdlog::info("[dictdump]     +0x{:02X}: {:016X}{}", off, v, t);
            }
        }
    }

    // Among recorded ctor'd SpriteDefs, return the worldmap icon sprite: charId@+0x18==171 and
    // a plausible frameCount@+0x30. Returns 0 if not found yet.
    uint64_t locate_sprite171()
    {
        for (int k = 0; k < RING; ++k)
        {
            uint64_t sd = (uint64_t)g_sprites[k];
            if (!looks_heap(sd))
                continue;
            if (rd32(sd + OFF_CHARID) != 171)
                continue;
            uint32_t fc = rd32(sd + OFF_FRAMECOUNT);
            if (fc < 100 || fc > 4096) // worldmap sprite has hundreds of frames; skip 1-frame sprites
                continue;
            // sanity: frame array present and self-consistent
            uint64_t data = rq(sd + OFF_FRAMEARR_DATA);
            uint32_t cnt = rd32(sd + OFF_FRAMEARR_COUNT);
            if (!looks_heap(data) || cnt != fc)
                continue;
            return sd;
        }
        return 0;
    }

    // SYNTHESIZE a PlaceObject3 tag that places image `charId` at `depth` with OUR icon matrix - fully
    // from scratch, NO gfx template (drops the last gfx-content dependency: cloning grace frame 0). PO3
    // tag-object layout (from PO3::Execute 0x1411bdb40 disasm): vtable@+0, then the inline SWF body at +8:
    //   flags0@+8 = 0x06 (HasCharacter|HasMatrix, Move=0, no ColorTransform - exactly what grace's working
    //               placement uses), flags1@+9 = 0x10 (HasImage), depth u16@+0xa, charId u16@+0xc,
    //   MATRIX@+0xe (the per-icon matrix from generate_map_icons.icon_matrix: scale + centered for that
    //               icon's cropped WxH).
    // The frame executor dispatches purely on tag[0]=vtable with no heap/size check, so a VirtualAlloc'd
    // tag runs identically to a native one (same proven mechanism as build_remove_tag for RemoveObject2).
    uint64_t build_clean_place_tag(uint16_t charId, uint16_t depth, const unsigned char *matrix, unsigned matLen)
    {
        void *t = VirtualAlloc(nullptr, 0x40, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!t)
            return 0;
        uint64_t vt = po3_vtable();                            // captured-live (RVA fallback)
        // flags0 0x06 = HasCharacter|HasMatrix (NO ColorTransform) - matches the NATIVE vanilla icon
        // PO3 tags (verified via framedump: native icons place at 0x06 and render full), so no cxform.
        uint8_t flags0 = 0x06, flags1 = 0x10;
        safe_copy(t, &vt, 8);                                   // vtable @+0
        safe_copy((void *)((uint64_t)t + 8), &flags0, 1);      // flags0 @+8  (HasCharacter|HasMatrix)
        safe_copy((void *)((uint64_t)t + 9), &flags1, 1);      // flags1 @+9  (HasImage)
        safe_copy((void *)((uint64_t)t + 0xa), &depth, 2);     // depth  u16 @+0xa
        safe_copy((void *)((uint64_t)t + 0xc), &charId, 2);    // charId u16 @+0xc
        safe_copy((void *)((uint64_t)t + 0xe), (void *)matrix, matLen); // matrix @+0xe
        return (uint64_t)t;
    }

    // Capture the native PlaceObject3 + RemoveObject2 tag vtables from the stock sprite-171 frames, so we
    // synthesize tags without a hardcoded vtable RVA. A composite icon frame is RemoveObject2(d1) +
    // RemoveObject2(d2) + PlaceObject3(image) -> tag list [Xvt, Xvt, Yvt] where Y carries the HasImage flag
    // (flags1@+9 == 0x10). The first such frame gives RM2=Xvt, PO3=Yvt. The stock worldmap has hundreds of
    // these (246 RM2 / 234 PO3 tags). RVA fallback (po3_vtable/rm2_vtable) if none is found. One-shot.
    void capture_tag_vtables(uint64_t sd)
    {
        uint64_t fdata = rq(sd + OFF_FRAMEARR_DATA);
        uint32_t fcnt = rd32(sd + OFF_FRAMEARR_COUNT);
        if (!looks_heap(fdata) || fcnt == 0)
            return;
        for (uint32_t f = 0; f < fcnt; ++f)
        {
            uint64_t elem = fdata + (uint64_t)f * FRAME_STRIDE;
            uint64_t tags = rq(elem);
            uint32_t tc = rd32(elem + 8);
            if (!looks_heap(tags) || tc < 3 || tc > 64)
                continue;
            uint64_t t0 = rq(tags), t1 = rq(tags + 8), t2 = rq(tags + 16);
            if (!looks_heap(t0) || !looks_heap(t1) || !looks_heap(t2))
                continue;
            uint64_t v0 = rq(t0), v1 = rq(t1), v2 = rq(t2);
            uint8_t f2 = 0;
            safe_copy(&f2, (void *)(t2 + 9), 1);                 // PO3 image tag has flags1@+9 == 0x10
            if (looks_heap(v0) && v0 == v1 && looks_heap(v2) && v2 != v0 && f2 == 0x10)
            {
                g_rm2_vt.store(v0, std::memory_order_relaxed);
                g_po3_vt.store(v2, std::memory_order_relaxed);
                uint64_t mod = (uint64_t)GetModuleHandleW(nullptr);
                spdlog::info("[icons] captured native vtables (frame {}): RM2=0x{:X}(rva 0x{:X}) PO3=0x{:X}(rva 0x{:X})",
                             f, v0, v0 - mod, v2, v2 - mod);
                return;
            }
        }
        spdlog::warn("[icons] no composite RM2+RM2+PO3 frame found; using fallback vtable RVAs.");
    }

    // Place our registered logo bitmap onto the decorative-plaque sprite (246) by re-pointing its
    // char-10 PlaceObject3 to our logo charId. PO3 tag-object body is inline at this+8: flags0@+8,
    // flags1@+9, depth@+0xa, charId@+0xc, matrix@+0xe (confirmed via PO3::Execute 0x1411bdb40). We clone
    // the char-10 tag (valid vtable/flags0/depth envelope), set HasImage (flags1|=0x10), re-key charId to
    // our logo, and overwrite the matrix with the baked LOGO_MATRIX (scale 0.38 / translate -243). char-10
    // is a plain place (HasCharacter|HasMatrix, no colorTransform), so the body is self-terminating after
    // the matrix and a longer matrix just uses spare bytes of the 0x80 clone. Returns true on success.
    bool inject_logo_into_plaque(uint64_t sd)
    {
        uint32_t logo = g_logo_charid.load(std::memory_order_relaxed);
        if (!logo)
        {
            spdlog::warn("[logo] plaque sprite loaded but logo charId not registered yet; skip.");
            return false;
        }
        uint64_t farr = rq(sd + OFF_FRAMEARR_DATA);
        uint32_t fcnt = rd32(sd + OFF_FRAMEARR_COUNT);
        if (!looks_heap(farr) || fcnt == 0)
            return false;
        uint64_t tags = rq(farr);             // frame 0 tags array
        uint32_t tagCount = rd32(farr + 8);
        if (!looks_heap(tags) || tagCount == 0 || tagCount > 64)
            return false;
        // find the tag whose body charId (@+0xc) == char 10 (the plaque placement)
        for (uint32_t k = 0; k < tagCount; ++k)
        {
            uint64_t srcTag = rq(tags + (uint64_t)k * 8);
            if (!looks_heap(srcTag))
                continue;
            uint16_t cid = (uint16_t)(rd32(srcTag + 0xc) & 0xFFFF);
            if (cid != LOGO_PLAQUE_CHAR)
                continue;
            void *t = VirtualAlloc(nullptr, 0x80, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!t || !safe_copy(t, (void *)srcTag, 0x80))
                return false;
            uint8_t flags1 = 0;
            safe_copy(&flags1, (void *)((uint64_t)t + 9), 1);
            flags1 |= 0x10; // HasImage: treat charId as an image character
            safe_copy((void *)((uint64_t)t + 9), &flags1, 1);
            uint16_t lc = (uint16_t)logo;
            safe_copy((void *)((uint64_t)t + 0xc), &lc, 2);                    // charId 10 -> our logo
            safe_copy((void *)((uint64_t)t + 0xe), (void *)goblin::generated::LOGO_MATRIX,
                      goblin::generated::LOGO_MATRIX_LEN);                     // matrix 0.5 -> 0.38/-243
            wr64(tags + (uint64_t)k * 8, (uint64_t)t);                         // swap the tag pointer
            spdlog::info("[logo] plaque sprite {}: re-pointed char-{} placement (tag[{}]) to logo charId {}.",
                         LOGO_PLAQUE_SPRITE, LOGO_PLAQUE_CHAR, k, logo);
            return true;
        }
        spdlog::warn("[logo] plaque sprite {}: no char-{} placement found in frame 0 ({} tags).",
                     LOGO_PLAQUE_SPRITE, LOGO_PLAQUE_CHAR, tagCount);
        return false;
    }

    // Scan the live READ resource dict (movieDef+0xd8) once: return the highest charId < `ceil`, and (if
    // `in_window`) whether any charId falls in [lo, hi). Returns 0 if the dict isn't built/readable yet
    // (then callers fall back to the high floor). movieDef == the load ctx at sprite-171 time.
    uint32_t dict_scan(uint64_t movieDef, uint32_t lo, uint32_t hi, uint32_t ceil, bool *in_window)
    {
        if (in_window) *in_window = false;
        uint64_t cont = movieDef + OFF_MOVIEDEF_DICT;
        uint64_t base = rq(cont);
        uint32_t count = rd32(cont + 8);
        if (!looks_heap(base) || count == 0 || count > 100000)
            return 0;
        uint32_t mx = 0;
        for (uint32_t k = 0; k < count; ++k)
        {
            uint64_t node = rq(base + (uint64_t)k * 16);
            uint32_t c = node ? rd32(node + OFF_NODE_CHARID) : 0;
            if (c == 0 || c >= ceil)
                continue;
            if (c > mx) mx = c;
            if (in_window && c >= lo && c < hi) *in_window = true;
        }
        return mx;
    }

    // Self-healing charId base: a HIGH floor (generated MAP_ICON_CHARID_BASE) so natives loading after
    // sprite-171 cannot reach it, RAISED above any even-higher live charId (e.g. an overhaul gfx with
    // thousands of charIds), with the [base, base+count) window verified clear. Never a tight
    // native_max+margin (that risks a post-sprite-171 native landing inside the window).
    uint32_t compute_safe_base(uint64_t movieDef, uint32_t count)
    {
        uint32_t reg = reg_max_for(movieDef);                       // max native charId the registrar saw
        uint32_t live = dict_scan(movieDef, 0, 0, 0x100000, nullptr); // overall max in the live dict (0 if not built)
        uint32_t hi = reg > live ? reg : live;
        uint32_t base = INJECT_CHARID_BASE;                          // floor
        if (hi != 0 && hi + INJECT_BASE_MARGIN > base)               // raise above live natives if they exceed the floor
            base = hi + INJECT_BASE_MARGIN;
        bool clash = false;                                          // paranoia: scattered charIds at/above base
        dict_scan(movieDef, base, base + count, 0x100000, &clash);
        if (clash && live)
            base = live + INJECT_BASE_MARGIN;                        // above the overall max -> window clear
        return base;
    }

    // SYNTHESIZE a genuine RemoveObject2 ExecuteTag for `depth`. A RemoveObject2 tag is just
    // {vtable @+0 ; depth u16 @+8 (= body+0, NO flags byte)} - the real RemoveObject2::Execute
    // (0x1411bde10) reads `movzx r10d, word ptr [this+8]` as the depth and unlinks the matching display
    // node. The frame executor (0x1411bf131) dispatches purely on tag[0]=vtable with no validity/heap
    // check, so a VirtualAlloc'd tag with the correct vtable runs identically to a native one - no need
    // to find/clone a real RM2. Removing a depth that holds nothing is a harmless no-op. Returns 0 on fail.
    uint64_t build_remove_tag(uint16_t depth)
    {
        void *t = VirtualAlloc(nullptr, 0x10, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!t)
            return 0;
        uint64_t vt = rm2_vtable();                          // captured-live (RVA fallback)
        safe_copy(t, &vt, 8);                                 // vtable @+0
        safe_copy((void *)((uint64_t)t + 8), &depth, 2);      // depth u16 @+8 (body+0)
        return (uint64_t)t;
    }

}

uint32_t goblin::gfx_probe::injected_iconid(int srcIconId)
{
    if (srcIconId < 0 || srcIconId >= ICON_MAP_SIZE)
        return 0;
    return g_icon_iid[srcIconId];
}

// The anon "?" is one of the injected icons (its source iconId is goblin::generated::ANON_ICON_ID).
// Kept for the apply_loot_settings pre-load placeholder path.
uint32_t goblin::gfx_probe::anon_dynamic_iconid()
{
    return injected_iconid(static_cast<int>(goblin::generated::ANON_ICON_ID));
}


void goblin::gfx_probe::tick()
{
    // Called from the DLL's background watcher thread (setup_mod), NOT the overlay's Present hook - so the
    // collision self-heal and (dev) diagnostics run independently of enable_overlay. Icon/resource injection
    // itself is UNCONDITIONAL and happens at LOAD (the sprite-loader hook), independent of this tick; this
    // only runs the functional collision self-heal (always) + the read-only dumps (`probe` = debug_logging).
    // The background loop already paces us (100ms-2s), so no frame throttle here; the work below is one-shot.
    const bool probe = goblin::config::debugLogging;

    // log ctor activity once it changes (is the hook firing / has the movie loaded?)
    unsigned recorded = g_idx.load(std::memory_order_relaxed);
    static unsigned last_logged = 0xFFFFFFFF;
    if (recorded != last_logged)
    {
        last_logged = recorded;
        spdlog::info("[gfxprobe] SpriteDef ctor fired {} times so far; scanning...", recorded);
    }

    uint64_t sd = g_found_sd.load(std::memory_order_relaxed);
    if (!sd)
    {
        sd = locate_sprite171();
        if (!sd)
            return;
        g_found_sd.store(sd, std::memory_order_relaxed);
        spdlog::info("[gfxprobe] *** SpriteDef-171 @ 0x{:X}: frameCount={} ***", sd, rd32(sd + OFF_FRAMECOUNT));
        if (probe)
            dump_obj("SpriteDef-171", sd);
    }

    // Icon/logo register + frame append + remap all happen at LOAD (sprite-loader hook), before pins
    // are built - nothing to do here. tick() only runs the collision self-heal + read-only diagnostics below.

    // Once a lookup has handed us movieDef: the collision SELF-HEAL below is functional (always runs);
    // the resource-dict dumps are dev-only (gated by `probe`). One-shot, never writes.
    {
        if (probe)
        {
            static unsigned last_ad = 0xFFFFFFFF, last_lk = 0xFFFFFFFF;
            unsigned ad = g_adddisp_calls.load(std::memory_order_relaxed);
            unsigned lk = g_lookup_calls.load(std::memory_order_relaxed);
            if (ad != last_ad || lk != last_lk)
            {
                last_ad = ad;
                last_lk = lk;
                spdlog::info("[iconinject] hook calls: AddDisplayObject={} lookup={} movieDef=0x{:X}",
                             ad, lk, g_moviedef.load(std::memory_order_relaxed));
            }
        }
        uint64_t md = g_moviedef.load(std::memory_order_relaxed);
        if (md && !g_dict_dumped.exchange(true, std::memory_order_relaxed))
        {
            if (probe)
            {
                dump_dict(md);
                // side-by-side: gfx-native "?" (13507) vs our first runtime-injected bitmap (BASE)
                dump_charid_node(md, 13507, "NATIVE-gfx");
                dump_charid_node(md, inject_base(), "INJECTED");
            }

            // Collision guard + SELF-HEAL. Our injected charIds live in the image manager, NOT this READ
            // dict, so a collision = a NATIVE charId sitting in our window [base, base+COUNT+1) (icons +
            // logo). If found, bump the base above it and reset the load one-shots so the NEXT worldmap
            // (re)open re-injects at a clear base (best-effort: takes effect when the movie reloads).
            uint64_t cont = md + OFF_MOVIEDEF_DICT;
            uint64_t dbase = rq(cont);
            uint32_t dcount = rd32(cont + 8);
            if (looks_heap(dbase) && dcount && dcount < 100000)
            {
                uint32_t base = inject_base();
                uint32_t cnt = (uint32_t)goblin::generated::MAP_ICON_TAG_COUNT + 1; // icons + logo
                uint32_t native_max = 0, in_window = 0, window_max = 0;
                for (uint32_t k = 0; k < dcount; ++k)
                {
                    uint64_t node = rq(dbase + (uint64_t)k * 16);
                    uint32_t c = node ? rd32(node + OFF_NODE_CHARID) : 0;
                    if (c == 0) continue;
                    if (c >= base && c < base + cnt) { ++in_window; if (c > window_max) window_max = c; }
                    else if (c < base && c > native_max) native_max = c;
                }
                spdlog::info("[icons] dict: native maxCharId={} ; NATIVE entries in our window [{}, {})={}",
                             native_max, base, base + cnt, in_window);
                if (in_window > 0)
                {
                    uint32_t newbase = window_max + INJECT_BASE_MARGIN;
                    g_inject_base.store(newbase, std::memory_order_relaxed);
                    g_qmark_injected.store(false, std::memory_order_relaxed); // allow re-inject at sprite-171
                    g_logo_placed.store(false, std::memory_order_relaxed);
                    g_dict_dumped.store(false, std::memory_order_relaxed);
                    goblin::diag::note_selfheal(newbase);
                    spdlog::warn("[icons] charId COLLISION: {} native ids in [{}, {}) -> bumped base to {}; "
                                 "will re-inject on next worldmap open (reopen the map).",
                                 in_window, base, base + cnt, newbase);
                }
            }
        }
    }

    if (probe && !g_r2_dumped.exchange(true, std::memory_order_relaxed))
        dump_r2();
}

void goblin::gfx_probe::setup()
{
    // Icon/resource injection is UNCONDITIONAL (it's how icons render without a gfx), so the load-time
    // hooks below are ALWAYS armed. Only the read-only RM2::Execute trace is dev-only (debug_logging).
    try
    {
        modutils::hook<CtorFn>(
            {.aob = "45 33 C0 48 8D 05 ?? ?? ?? ?? 48 89 01 48 8D 05 ?? ?? ?? ?? "
                    "C7 41 08 01 00 00 00 4C 89 41 10"},
            ctor_detour, o_ctor);

        // AddDisplayObject 0x140f01b10 - captures movieDef = *(rcx - 0x10) for the collision self-heal.
        modutils::hook<AddDispFn>(
            {.aob = "4C 89 4C 24 20 4C 89 44 24 18 55 53 41 54 41 55 41 56 41 57 "
                    "48 8D 6C 24 F9"},
            adddisp_detour, o_adddisp);
        modutils::hook<LookupFn>(
            {.aob = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 49 8B F8 8B DA "
                    "48 8B F1 E8 F4 F8 FF FF"},
            lookup_detour, o_lookup);

        // Per-define dict registrar: track each movie's live native max charId (scoped by ctx) so the
        // injected-bitmap base is computed at runtime, above every native id, no build-time guess.
        // Must be armed before the worldmap movie loads (setup runs at DLL load via the offline loader).
        modutils::hook<RegistrarFn>(
            {.aob = "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 41 8B 00 48 8B F9 48 8B 49 38"},
            registrar_detour, o_registrar); // dict registrar (was file VA 0x1411cf250)
        modutils::hook<LosslessFn>(
            {.aob = "40 53 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 68 48 8B 99 "
                    "18 04 00 00"},
            lossless_detour, o_lossless);
        modutils::hook<SpriteLoaderFn>(
            {.aob = "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 99 18 04 "
                    "00 00 48 8B F9 48 85 DB"},
            spriteloader_detour, o_spriteloader);
        spdlog::info("[gfxprobe] icon-injection hooks armed (ctor + movieDef capture + registrar + lossless + "
                     "DefineSprite-loader). Open the world map.");
        goblin::diag::set_hooks(true, "");

        if (goblin::config::debugLogging)
        {
            // DEV diagnostic only: hot-path trace of RemoveObject2::Execute for our synthesized RM2 tags.
            modutils::hook<ExecFn>(
                {.address = reinterpret_cast<void *>((uint64_t)GetModuleHandleW(nullptr) + RVA_FN_REMOVEOBJECT2_EXEC)},
                rm2exec_detour, o_rm2exec); // DIAG: real RemoveObject2::Execute (file VA 0x1411bde10)
            spdlog::info("[gfxprobe] RM2::Execute diagnostic trace armed (debug_logging).");
        }
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[gfxprobe] setup failed: {}", e.what());
        goblin::diag::set_hooks(false, e.what());
    }
}
