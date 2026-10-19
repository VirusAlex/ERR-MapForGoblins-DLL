#define WIN32_LEAN_AND_MEAN
#include <filesystem>
#include <memory>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <thread>
#include <atomic>
#include <windows.h>

#include "from/params.hpp"
#include "modutils.hpp"

#include "goblin_anchors.hpp"
#include "goblin_collected.hpp"
#include "goblin_config.hpp"
#include "goblin_crashdiag.hpp" // the mod's own state, printed into every crash record
#include "goblin_guarded.hpp"  // "we asked for this fault": what the crash logger must not record
#include "goblin_inject.hpp"
#include "goblin_kindling.hpp"
#include "goblin_logic.hpp"
#include "goblin_map_api.hpp"  // maintain(): the map API's snapshots, built on this thread
#include "goblin_map_blob.hpp" // load_map_data(), which every MAP_ENTRIES reader depends on
#include "goblin_markers.hpp"
#include "goblin_names_blob.hpp" // load_name_tables(): ENEMY_NAMES + ITEM_NAME_FALLBACK
#include "goblin_messages.hpp"
#include "goblin_status_line.hpp"
#include "goblin_overlay.hpp"
#include "sc2/overlay_present.hpp" // capture_creation_entrypoints(), called first thing below
#include "goblin_map_timing.hpp"
#include "goblin_gfx_probe.hpp"
#include "goblin_safemem.hpp" // this thread's own VirtualQuery counters, for [tickcost]
#include "goblin_maphover.hpp"
#include "goblin_stall_probe.hpp"
#include "goblin_worldmap_probe.hpp"

#include "version.h"

static std::thread mod_thread;

// SEH wrapper - catches access violations from refresh() during multiplayer transitions
static int safe_refresh_seh()
{
    __try
    {
        return goblin::collected::refresh();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

static uint64_t safe_change_signature_seh()
{
    __try
    {
        return goblin::collected::change_signature();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

static int safe_kindling_refresh_seh()
{
    __try
    {
        return goblin::kindling::refresh();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

static void safe_flag_or_pairs_seh()
{
    __try
    {
        goblin::apply_flag_or_pairs();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

static void safe_apply_category_visibility_seh()
{
    __try
    {
        goblin::apply_category_visibility();
        goblin::apply_focus_highlight();  // keep focus glow/labels in sync as the collected set changes
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    // Announce it. Every caller of this wrapper has just decided that what should be on screen
    // changed - a pickup, a focus that emptied out, a category rewrite - and the rows are already
    // written. Saying so lets the map tick merge on its next frame; without it the change was
    // invisible until the tick's own periodic re-snapshot happened to run, which is why that poll
    // had to be frequent and therefore expensive. Outside the __try on purpose: it must also fire
    // on the path where the guarded call faulted partway, or the tick would keep showing rows
    // that no longer match the params.
    goblin::note_visibility_changed();
}

// Every live setting, re-derived (reapply_live_settings) - for a watcher-side change that has to
// undo more than the category gates: a focus that ends here leaves its rows forced visible
// (eventFlagId, group 2) until apply_map_logic runs again. reapply_live_settings bumps the
// visibility epoch itself; the bump below covers a pass that faulted partway, as above.
static void safe_reapply_live_settings_seh()
{
    __try
    {
        goblin::reapply_live_settings();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    goblin::note_visibility_changed();
}

// The world-state flags (which Leyndell stands) read under __try: the watcher runs at the main
// menu and through loading screens too. What changed is said outside the guard.
static size_t safe_world_state_poll_seh(goblin::WorldStateChange *out, size_t max_out)
{
    __try
    {
        return goblin::world_state_poll(out, max_out);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

static bool safe_world_state_changed_seh()
{
    goblin::WorldStateChange changes[4]{};
    const size_t n = safe_world_state_poll_seh(changes, 4);
    for (size_t i = 0; i < n && i < 4; ++i)
        if (changes[i].was_known)  // the first read after injection is not a change
            spdlog::info("[state] flag {} is now {} ({} markers follow it)", changes[i].flag,
                         changes[i].now_on ? "ON" : "OFF", changes[i].markers);
    return n != 0;
}

// The map API's snapshot reads our marker rows, and they live in the param file the GAME owns: on
// the way out of the game it is freed under this thread (2026-10-19: a quit with a reader attached
// faulted in native_row_visible on a freed row). maintain() itself waits for a character in the
// world; this catches the race that check cannot close.
static void safe_map_api_maintain_seh()
{
    __try
    {
        goblin::map_api::maintain();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

static void safe_gfx_tick_seh()
{
    __try
    {
        goblin::gfx_probe::tick(); // audit, stall sampler + (debug_logging) diagnostics
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// ── SEH-guarded init-phase wrappers ──
// MSVC's /EHsc disallows __try in functions that contain C++ objects with
// destructors, so each init step goes through a plain C-style adapter +
// a shared invoker. If any step access-violates (e.g. another mod shifted
// the game's memory map mid-init), we log and continue - losing that
// feature is better than the DLL crashing the entire game.

using InitFn = void (*)();

static bool seh_invoke_void(InitFn fn)
{
    __try { fn(); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void init_map_data()         { goblin::generated::load_map_data(); }
static void init_name_tables()      { goblin::generated::load_name_tables(); }
static void init_modutils()         { modutils::initialize(); }
static void init_from_params()      { from::params::initialize(); }
static void init_collected()        { goblin::collected::initialize(); }
static void init_kindling()         { goblin::kindling::initialize(); }
static void init_inject_entries()   { goblin::inject_map_entries(); }
static void init_apply_map_logic()  { goblin::apply_map_logic(); goblin::apply_worldmap_fragment_bypass(); }
static void init_status_line()      { goblin::status_line::setup(); }
static void init_setup_messages()   { goblin::setup_messages(); }
static void init_live_loot()        { goblin::refresh_loot_from_itemlot(); }
static void init_overlay()          { goblin::overlay::setup(); }
static void init_map_timing()       { goblin::map_timing::setup(); }
static void init_gfx_probe()        { goblin::gfx_probe::setup(); }
static void init_maphover()         { goblin::maphover::setup(); }
static void init_stall_probe()      { goblin::stall_probe::setup(); }
static void init_worldmap_probe()   { goblin::worldmap_probe::setup(); }
static void init_anchors()          { goblin::anchors::warm(); }
static void init_class_names()      { goblin::anchors::prewarm_vtables(); }

static void safe_init_step(InitFn fn, const char *name)
{
    if (!seh_invoke_void(fn))
        spdlog::error("SEH exception in init step '{}' - feature may be degraded", name);
}

// ── crash breadcrumbs ────────────────────────────────────────────────────────────────
// Under Proton there is no Windows minidump, so a crash report from a Linux player carries no
// fault address at all - the DLC map crash we are chasing arrived as "the log just stops". This
// records the fault ourselves: a vectored handler appends code, module+RVA and a short backtrace to
// its own file.
// HEAP-FREE on purpose - a stack buffer and WriteFile, no CRT, no spdlog. One of the codes we want
// to catch is heap corruption (0xC0000374), where any allocation could deadlock or fault again.
// It only RECORDS and returns CONTINUE_SEARCH: the process still crashes exactly as it would have,
// and nothing about behaviour changes.
static HANDLE g_crash_file = INVALID_HANDLE_VALUE;
static uintptr_t g_self_base = 0, g_self_size = 0;

// 64-bit hex, written by hand. wsprintfA does NOT support %llX (its format set is a small subset of
// printf's), so the first version of this logger recorded every address as the literal text "0xlX".
// Doing it manually keeps the handler heap-free, which is the whole point of this path.
static int crash_hex64(char *buf, unsigned long long v)
{
    static const char digits[] = "0123456789ABCDEF";
    char tmp[16];
    int n = 0;
    do
    {
        tmp[n++] = digits[v & 0xF];
        v >>= 4;
    } while (v && n < 16);
    int len = 0;
    buf[len++] = '0';
    buf[len++] = 'x';
    while (n)
        buf[len++] = tmp[--n];
    return len;
}

// addr -> "module+0xRVA", or bare hex when no module owns it. No heap use.
static int crash_fmt_addr(char *buf, uintptr_t addr)
{
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(addr), &mod) &&
        mod)
    {
        char full[MAX_PATH];
        DWORD n = GetModuleFileNameA(mod, full, MAX_PATH);
        const char *base = full;
        for (DWORD i = 0; i < n; ++i)
            if (full[i] == '\\' || full[i] == '/')
                base = full + i + 1;
        int len = wsprintfA(buf, "%s+", base);
        len += crash_hex64(buf + len, addr - reinterpret_cast<uintptr_t>(mod));
        return len;
    }
    return crash_hex64(buf, addr);
}

// ── write watch (HUD investigation) - DEV BUILDS ONLY ────────────────────────────────────────────
// Compiled out of shipping builds since 2026-08-01. What it does - OpenThread, SuspendThread,
// GetThreadContext(CONTEXT_DEBUG_REGISTERS), write Dr0/Dr7, SetThreadContext, then a vectored
// handler reading Dr6 - is a textbook anti-debug/rootkit shape sitting in .text, and VirusTotal's
// behaviour tab tags the DLL `detect-debug-environment` (report 13). It answered its question (who
// clears the menu-state bytes) long ago and has ONE caller. Imports do not change either way -
// MinHook already pulls Suspend/Get/SetThreadContext - so this is about the code pattern, not the
// import table. Rebuild with -DMFG_STALL_PROFILER=1 to get it back for an investigation.
#if MFG_STALL_PROFILER
// Static searching could not name the code that clears the menu-state bytes: the offsets are generic and
// there are a thousand candidates. So catch the writer in the act - a hardware data breakpoint on the one
// byte we know the game clears, and log the instruction that trips it. Debug only, armed on request from
// goblin_stall_probe, one address at a time.
namespace goblin::watch
{
    std::atomic<uintptr_t> g_addr{0};
    // A g_hits counter was incremented in the handler and read by nobody; the watch is one-shot
    // (g_addr is exchanged to 0 on the first hit), so it could only ever have held 0 or 1.

    // A request is QUEUED by the game thread and carried out by another thread. Setting debug registers
    // means SuspendThread + Get/SetThreadContext, and doing that to your OWN thread suspends you with
    // nobody left to resume it - which is exactly how the first version hung the game the instant our
    // screen closed, before any write could even happen. So arm() must never run on the target thread.
    std::atomic<uintptr_t> g_pending_addr{0};
    std::atomic<unsigned long> g_pending_tid{0};

    void request(uintptr_t address, unsigned long thread_id)
    {
        g_pending_tid.store(thread_id, std::memory_order_relaxed);
        g_pending_addr.store(address, std::memory_order_release);
    }

    bool arm(uintptr_t address, DWORD thread_id)
    {
        if (thread_id == GetCurrentThreadId())
        {
            spdlog::warn("[watch] refusing to arm on the calling thread - queue it instead");
            return false;
        }
        HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE,
                               thread_id);
        if (!th)
            return false;
        bool ok = false;
        if (SuspendThread(th) != (DWORD)-1)
        {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(th, &ctx))
            {
                ctx.Dr0 = address;
                // DR7: L0 (bit 0) enables Dr0; bits 16-17 = 01 (write), bits 18-19 = 00 (1 byte)
                ctx.Dr7 = (ctx.Dr7 & ~0xFULL) | 0x1ULL;
                ctx.Dr7 = (ctx.Dr7 & ~(0xFULL << 16)) | (0x1ULL << 16);
                ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                ok = SetThreadContext(th, &ctx) != 0;
            }
            ResumeThread(th);
        }
        CloseHandle(th);
        if (ok)
        {
            g_addr.store(address, std::memory_order_release);
            spdlog::info("[watch] armed a write watch on 0x{:X} (thread {})", address, thread_id);
        }
        else
            spdlog::warn("[watch] could not arm the write watch on 0x{:X}", address);
        return ok;
    }
}

namespace goblin::watch
{
    // Called from OUR background thread; performs any queued arming.
    void pump()
    {
        const uintptr_t addr = g_pending_addr.load(std::memory_order_acquire);
        if (!addr)
            return;
        const unsigned long tid = g_pending_tid.load(std::memory_order_relaxed);
        g_pending_addr.store(0, std::memory_order_release);
        arm(addr, tid);
    }
}
#else
// Shipping: the callers are compiled out too (goblin_gfx_probe's tick and one site in
// goblin_stall_probe), so nothing here needs a stub.
#endif // MFG_STALL_PROFILER

static void crash_write(const char *buf, int len)
{
    DWORD wr = 0;
    if (g_crash_file != INVALID_HANDLE_VALUE && len > 0)
        WriteFile(g_crash_file, buf, static_cast<DWORD>(len), &wr, nullptr);
}

// The stack, walked properly.
//
// This used to be RtlCaptureStackBackTrace from inside the handler, which has two faults that cost
// report 19 a day: it walks the HANDLER's stack, so the first frames are the logger and ntdll's
// dispatcher rather than anything that crashed, and it is a heuristic walk with no unwind info, so
// frames go missing wherever the engine uses a frame pointer or a chained unwind. When a CONTEXT is
// available (it always is - both handlers get one) RtlVirtualUnwind gives the real chain, starting
// at the instruction that actually faulted.
static void crash_write_stack(const CONTEXT *ctx)
{
    if (!ctx)
        return;
    CONTEXT c = *ctx; // unwinding mutates it
    char line[512];
    for (int i = 0; i < 32; ++i)
    {
        int len = wsprintfA(line, "  #%02d ", i);
        len += crash_fmt_addr(line + len, static_cast<uintptr_t>(c.Rip));
        line[len++] = '\n';
        crash_write(line, len);

        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(c.Rip, &image_base, nullptr);
        if (!rf)
        {
            // A leaf with no unwind data: the return address is at the stack pointer. One step of
            // this is worth taking (it is how a leaf helper's caller is recovered); more would be
            // guessing, so stop if it does not land somewhere plausible.
            uintptr_t ret = 0;
            __try
            {
                ret = *reinterpret_cast<uintptr_t *>(c.Rsp);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return;
            }
            if (ret < 0x10000)
                return;
            c.Rip = ret;
            c.Rsp += 8;
            continue;
        }
        PVOID handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, c.Rip, rf, &c, &handler_data, &establisher,
                         nullptr);
        if (!c.Rip)
            return;
    }
}

// One record: a header line with the label, the code and the faulting address, the mod's own state,
// and then the stack.
//
// The state line is the part that was missing. A record saying "the engine faulted at exe+0x1157BEE
// reading 0x20" cannot be acted on; the same record plus "opens=11 gen=11 tracked=7136 mapOpen=1"
// names the situation immediately. It is read from atomics, so it stays truthful even when our
// containers are the thing that went wrong.
static void crash_write_record(const char *label, DWORD code, uintptr_t fault,
                               const EXCEPTION_RECORD *rec, const CONTEXT *ctx)
{
    if (g_crash_file == INVALID_HANDLE_VALUE)
        return;
    char line[1024];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int len = wsprintfA(line, "\n[%04d-%02d-%02d %02d:%02d:%02d] [%s] code=0x%08X fault=", st.wYear,
                        st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, label,
                        static_cast<unsigned>(code));
    len += crash_fmt_addr(line + len, fault);
    // Read vs write and the address touched. Both are in the record already and both were being
    // thrown away, so every past report needed the minidump just to learn which it was.
    if (rec && code == static_cast<DWORD>(EXCEPTION_ACCESS_VIOLATION) &&
        rec->NumberParameters >= 2)
    {
        len += wsprintfA(line + len, " %s ",
                         rec->ExceptionInformation[0] == 0   ? "read"
                         : rec->ExceptionInformation[0] == 1 ? "write"
                                                             : "execute");
        len += crash_hex64(line + len, rec->ExceptionInformation[1]);
    }
    // Heap corruption names its victim. 0xC0000374 is raised by the heap when a block header or
    // guard fails to validate, which is at FREE time, not at corruption time - so the stack is
    // useless for finding the writer, but the parameters identify the heap and usually the block.
    // Recorded twice on 2026-08-03 (18:11:45 and 18:33:04), both while the game was shutting down,
    // both through the game's own _free_base. We put memory on that heap ourselves - gfx_alloc uses
    // the game's _malloc_base and hands the pointers to Scaleform - so the block address is the
    // first thing worth knowing.
    if (rec && code == 0xC0000374 && rec->NumberParameters > 0)
    {
        len += wsprintfA(line + len, " params[%u]", rec->NumberParameters);
        const DWORD n = rec->NumberParameters > 6 ? 6 : rec->NumberParameters;
        for (DWORD i = 0; i < n; ++i)
        {
            line[len++] = ' ';
            len += crash_hex64(line + len, rec->ExceptionInformation[i]);
        }
    }
    line[len++] = '\n';
    crash_write(line, len);
    // Which two binaries produced this record. Repeated per record, not only in the [SESSION]
    // banner at the top of the file: report 33 arrived hand-trimmed to the single interesting
    // record, so the banner was gone and neither the mod build nor the game build could be read
    // out of the log at all - both had to be recovered from the minidump, which a Proton player
    // does not have in the first place.
    len = goblin::crashdiag::format_env(line, static_cast<int>(sizeof(line)));
    crash_write(line, len);
    len = goblin::crashdiag::format_state(line, static_cast<int>(sizeof(line)));
    crash_write(line, len);
    // What the touched address actually IS. The states are four different bugs and they were all
    // arriving as the same line: FREE means the allocation is gone (use-after-free or a wild
    // pointer), COMMIT at a tiny offset means a null-ish struct base, RESERVE means a stack guard
    // page, and a VirtualQuery that answers nothing at all means the address is not even canonical.
    // Report 33 is the last of those - the record said `read 0xFFFFFFFFFFFFFFFF`, and establishing
    // that this meant "one bit flipped in a pointer" took a minidump the reporter happened to have.
    // VirtualQuery is safe here for the same reason the rest of this path is: no heap, no CRT.
    if (rec && code == static_cast<DWORD>(EXCEPTION_ACCESS_VIOLATION) && rec->NumberParameters >= 2)
    {
        const auto addr = static_cast<uintptr_t>(rec->ExceptionInformation[1]);
        MEMORY_BASIC_INFORMATION mbi{};
        len = wsprintfA(line, "  [mem] ");
        if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == sizeof(mbi))
        {
            len += wsprintfA(line + len, "st=%s pr=",
                             mbi.State == MEM_COMMIT    ? "COMMIT"
                             : mbi.State == MEM_RESERVE ? "RESERVE"
                             : mbi.State == MEM_FREE    ? "FREE"
                                                        : "?");
            len += crash_hex64(line + len, mbi.Protect);
            len += wsprintfA(line + len, " ty=");
            len += crash_hex64(line + len, mbi.Type);
            len += wsprintfA(line + len, " base=");
            len += crash_hex64(line + len, reinterpret_cast<uintptr_t>(mbi.AllocationBase));
            len += wsprintfA(line + len, " size=");
            len += crash_hex64(line + len, static_cast<uint64_t>(mbi.RegionSize));
        }
        else
        {
            len += wsprintfA(line + len, "st=NONE"); // not canonical, or not in this address space
        }
        line[len++] = '\n';
        crash_write(line, len);
    }
    if (ctx)
    {
        len = wsprintfA(line, "  [regs] rax=");
        len += crash_hex64(line + len, ctx->Rax);
        len += wsprintfA(line + len, " rcx=");
        len += crash_hex64(line + len, ctx->Rcx);
        len += wsprintfA(line + len, " rdx=");
        len += crash_hex64(line + len, ctx->Rdx);
        len += wsprintfA(line + len, " rbx=");
        len += crash_hex64(line + len, ctx->Rbx);
        len += wsprintfA(line + len, " rsi=");
        len += crash_hex64(line + len, ctx->Rsi);
        len += wsprintfA(line + len, " rdi=");
        len += crash_hex64(line + len, ctx->Rdi);
        len += wsprintfA(line + len, " r8=");
        len += crash_hex64(line + len, ctx->R8);
        len += wsprintfA(line + len, " r9=");
        len += crash_hex64(line + len, ctx->R9);
        line[len++] = '\n';
        crash_write(line, len);
        // The other half. Eight of sixteen registers were being dropped, and which half holds the
        // corrupted value is not ours to choose: report 33 happened to fault on r8 and was
        // readable, but the same record with the bad pointer in r13 would have said nothing.
        // rsp/rbp come along because they are what separates a stack overflow from a wild write.
        len = wsprintfA(line, "  [regs2] rsp=");
        len += crash_hex64(line + len, ctx->Rsp);
        len += wsprintfA(line + len, " rbp=");
        len += crash_hex64(line + len, ctx->Rbp);
        len += wsprintfA(line + len, " r10=");
        len += crash_hex64(line + len, ctx->R10);
        len += wsprintfA(line + len, " r11=");
        len += crash_hex64(line + len, ctx->R11);
        len += wsprintfA(line + len, " r12=");
        len += crash_hex64(line + len, ctx->R12);
        len += wsprintfA(line + len, " r13=");
        len += crash_hex64(line + len, ctx->R13);
        len += wsprintfA(line + len, " r14=");
        len += crash_hex64(line + len, ctx->R14);
        len += wsprintfA(line + len, " r15=");
        len += crash_hex64(line + len, ctx->R15);
        line[len++] = '\n';
        crash_write(line, len);
    }
    crash_write_stack(ctx);
    FlushFileBuffers(g_crash_file);
}

static LONG NTAPI crash_veh(PEXCEPTION_POINTERS ep)
{
    // The watch fires as a single-step with the DR6 hit bit set. Log who wrote, then carry on.
    // Only this BRANCH is dev-gated - the crash logging below it is what players send us and must
    // stay in every build. Reading Dr6 here is half of the debug-register pattern the shipping
    // binary no longer carries.
#if MFG_STALL_PROFILER
    if (ep->ExceptionRecord->ExceptionCode == STATUS_SINGLE_STEP &&
        goblin::watch::g_addr.load(std::memory_order_acquire) &&
        (ep->ContextRecord->Dr6 & 0xF) != 0)
    {
        // ONE SHOT. The first version stayed armed and the byte turns out to be written from a hot path,
        // so every write raised an exception and the thread drowned in them - the game hung. Disarm inside
        // the handler (this context belongs to the trapping thread, so clearing DR here takes effect on
        // continue), log once, and let everything run at full speed again.
        const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const uintptr_t rip = ep->ContextRecord->Rip;
        const uintptr_t watched = goblin::watch::g_addr.exchange(0, std::memory_order_acq_rel);
        ep->ContextRecord->Dr0 = 0;
        ep->ContextRecord->Dr7 = 0;
        ep->ContextRecord->Dr6 = 0;
        ep->ContextRecord->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
        if (watched)
        {
            spdlog::info("[watch] 0x{:X} written by exe+0x{:X} (rip 0x{:X}) - watch disarmed",
                         watched, rip > exe ? rip - exe : rip, rip);
            // The writing instruction alone is not enough: the menu-state bytes are written by ONE
            // generic writer, so what we actually need is who asked for it. Walk the stack
            // conservatively and print every value that looks like a return address into the exe.
            // No unwind info is consulted, so some of these are stale slots rather than real frames -
            // that is fine, it is a lead list to check in the disassembler, not a call stack.
            __try
            {
                const uintptr_t *sp = reinterpret_cast<const uintptr_t *>(ep->ContextRecord->Rsp);
                const uintptr_t lo = exe;
                const uintptr_t hi = exe + 0x6000000; // past the last .text of eldenring.exe
                int printed = 0;
                for (int i = 0; i < 128 && printed < 10; ++i)
                {
                    const uintptr_t v = sp[i];
                    if (v <= lo || v >= hi)
                        continue;
                    // A return address is preceded by a call, so require the previous bytes to look
                    // like one: E8 rel32 (5 bytes) or FF /2 indirect (2-7 bytes). Cheap filter, kills
                    // most of the noise.
                    const uint8_t *p = reinterpret_cast<const uint8_t *>(v);
                    const bool after_call = (p[-5] == 0xE8) || (p[-2] == 0xFF) || (p[-3] == 0xFF) ||
                                            (p[-6] == 0xFF) || (p[-7] == 0xFF);
                    if (!after_call)
                        continue;
                    spdlog::info("[watch]   caller candidate exe+0x{:X} (stack +0x{:X})", v - exe,
                                 i * 8);
                    ++printed;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
#endif // MFG_STALL_PROFILER
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    switch (code) // genuinely fatal codes only
    {
    case 0xC0000005: // access violation
    case 0xC0000374: // heap corruption
    case 0xC0000409: // fast fail / stack buffer
    case 0xC000001D: // illegal instruction
    case 0xC0000096: // privileged instruction
    case 0xC00000FD: // stack overflow
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const uintptr_t fault = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
    // An access violation faulting INSIDE this DLL is almost always one of our own guarded read
    // probes (v3_read64 and friends), which its own __except handles - recording those would bury
    // the real thing. Heap corruption is recorded wherever it lands, because our probes never
    // raise it. Latch after a handful so a fault loop cannot fill the disk.
    if (code == 0xC0000005 && fault >= g_self_base && fault < g_self_base + g_self_size)
        return EXCEPTION_CONTINUE_SEARCH;
    // Same thing one step out: a fault inside an ENGINE function we called on purpose, from a frame
    // that handles it. The faulting address is the engine's, so the test above cannot see it - the
    // caller says so instead (goblin_guarded.hpp). Twelve such records in one ERR session, with the
    // game alive throughout, are what this removes.
    if (code == 0xC0000005 && goblin::guarded::inside())
        return EXCEPTION_CONTINUE_SEARCH;
    // Budget per SIGNATURE, not per process.
    //
    // The flat "first twelve records win" latch that stood here loses the only record that matters
    // as soon as anything repeats. Report 19: one guarded engine call faulted twelve times inside a
    // single second, spent the entire budget, and the fault that actually killed the process ten to
    // twenty minutes later went unrecorded - in all three sessions. Three records per distinct
    // (code, address) still bounds the file, and a newcomer can no longer be crowded out by a storm
    // of something already known.
    static volatile LONG64 s_keys[16] = {};
    static volatile LONG s_hits[16] = {};
    const LONG64 key =
        static_cast<LONG64>((static_cast<uint64_t>(code) << 48) ^ (fault & 0xFFFFFFFFFFFFull));
    int slot = -1;
    for (int i = 0; i < 16; ++i)
    {
        const LONG64 seen = InterlockedCompareExchange64(&s_keys[i], key, 0);
        if (seen == 0 || seen == key)
        {
            slot = i;
            break;
        }
    }
    if (slot < 0) // sixteen distinct signatures already: stop, the disk is not a log sink
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&s_hits[slot]) > 3)
        return EXCEPTION_CONTINUE_SEARCH;
    if (g_crash_file == INVALID_HANDLE_VALUE)
        return EXCEPTION_CONTINUE_SEARCH;

    crash_write_record("EXCEPTION", code, fault, ep->ExceptionRecord, ep->ContextRecord);
    return EXCEPTION_CONTINUE_SEARCH;
}

// [EXCEPTION] vs [CRASH]. A vectored handler runs on EVERY fault, long before anything decides whether
// it is fatal, so it cannot honestly call one a crash - and while it did, a session with a live game
// carried a dozen "CRASH" lines and the one real crash looked no different. So the vectored handler
// says what it knows ("this fault happened"), and the record below is written only from the
// unhandled-exception filter, i.e. when nothing handled it and the process IS going down.
//
// Both are kept because either can be missing: a game that installs its own filter after ours shadows
// the second, and then the first-chance line is all there is.
// Our own minidump, written only here, i.e. only when the process is going down.
//
// The dumps that arrive with reports are triage dumps: thread stacks and nothing else. In report 19
// that was not enough to read even our OWN globals - 101 memory ranges, none of them our .data - so
// the corrupted object could never be walked back to whoever corrupted it. WithDataSegs adds the
// module data sections (our state) and WithIndirectlyReferencedMemory adds what the registers and
// stacks point at (the object that faulted). Deliberately NOT WithFullMemory: this process carries
// about 12 GB of commit and nobody can upload that.
// Returns why it did or did not produce a file. Every one of the exits below used to be a bare
// `return`, so "there is no .dmp next to the game" covered five different causes and none of them
// were in the log - the 0-byte-dump and dbghelp-loop defects both had to be inferred from what was
// missing. The value goes into the record as `[dump] rc=N`.
enum : int
{
    kDumpOk = 0,
    kDumpNoPath = 1,      // the exe path could not be read, so there is nowhere to write
    kDumpNoDbgHelp = 2,   // dbghelp.dll would not load (common under Proton/Wine)
    kDumpNoEntry = 3,     // it loaded but MiniDumpWriteDump is not in it
    kDumpReentered = 4,   // a previous attempt in this process faulted or never returned
    kDumpNoFile = 5,      // the file could not be created (permissions, read-only game folder)
    kDumpWriteFailed = 6, // dbghelp itself refused
};

static int crash_write_dump(PEXCEPTION_POINTERS ep, DWORD *gle_out)
{
    wchar_t path[MAX_PATH];
    SYSTEMTIME st;
    GetLocalTime(&st);
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH)
        return kDumpNoPath;
    wchar_t dir[MAX_PATH];
    lstrcpynW(dir, path, MAX_PATH);
    for (int i = static_cast<int>(lstrlenW(dir)) - 1; i >= 0; --i)
        if (dir[i] == L'\\' || dir[i] == L'/')
        {
            dir[i] = 0;
            break;
        }
    wchar_t file[MAX_PATH];
    wsprintfW(file, L"%s\\MapForGoblins_%04d%02d%02d_%02d%02d%02d.dmp", dir, st.wYear, st.wMonth,
              st.wDay, st.wHour, st.wMinute, st.wSecond);
    // Resolved dynamically: dbghelp is not otherwise linked, and adding an import for it would
    // change the import table this DLL's antivirus profile is sensitive to.
    HMODULE dbg = LoadLibraryW(L"dbghelp.dll");
    if (!dbg)
        return kDumpNoDbgHelp;
    using WriteDumpFn = BOOL(WINAPI *)(HANDLE, DWORD, HANDLE, int, void *, void *, void *);
    auto write = reinterpret_cast<WriteDumpFn>(
        reinterpret_cast<void *>(GetProcAddress(dbg, "MiniDumpWriteDump")));
    if (!write)
        return kDumpNoEntry;
    // MiniDumpWriteDump can FAULT rather than fail: on Wine its own worker thread AVs inside
    // dbghelp, and that fault comes back round to the unhandled-exception filter, which asks
    // for another dump. Report 32 caught the loop - 89 dbghelp records across nine sessions,
    // one per second, burying the single real crash in each. The latch is raised BEFORE the
    // call and lowered after, so an attempt that never returns leaves it raised and no second
    // attempt is ever made in this process.
    static volatile LONG s_dump_entered = 0;
    if (InterlockedCompareExchange(&s_dump_entered, 1, 0) != 0)
        return kDumpReentered;
    // The file is created only once there is something able to fill it - an empty .dmp next
    // to the game is a worse answer than no .dmp at all.
    HANDLE h = CreateFileW(file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return kDumpNoFile;
    struct
    {
        DWORD ThreadId;
        PEXCEPTION_POINTERS ExceptionPointers;
        BOOL ClientPointers;
    } info{GetCurrentThreadId(), ep, FALSE};
    constexpr int kNormal = 0x0000;
    constexpr int kWithDataSegs = 0x0001;
    constexpr int kWithIndirectlyReferencedMemory = 0x0040;
    constexpr int kWithThreadInfo = 0x1000;
    const BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), h,
                          kNormal | kWithDataSegs | kWithIndirectlyReferencedMemory |
                              kWithThreadInfo,
                          &info, nullptr, nullptr);
    // Ask BEFORE CloseHandle - closing a handle overwrites the thread's last error.
    if (!ok && gle_out)
        *gle_out = GetLastError();
    CloseHandle(h);
    // A refused write leaves the file it already created: 65 zero-byte MapForGoblins_*.dmp had
    // piled up next to eldenring.exe by 2026-08-07, one per session, and every one of them reads
    // as "the mod tried and produced nothing" to whoever finds it. The only file removed here is
    // the one this function created seconds earlier and dbghelp declined to fill.
    if (!ok)
        DeleteFileW(file);
    InterlockedExchange(&s_dump_entered, 0);
    return ok ? kDumpOk : kDumpWriteFailed;
}

static LONG WINAPI crash_ueh(PEXCEPTION_POINTERS ep)
{
    // Same budget the first-chance handler keeps, on its own counters: three records per
    // distinct (code, address). Without it a filter that gets re-entered - by a fault our own
    // dump attempt raised, or by a host that resumes execution after an unhandled one - writes
    // the same line until the file is unreadable. Report 32: twelve to fifteen identical
    // dbghelp records per session, the genuine crash sitting first and unremarked.
    static volatile LONG64 s_keys[16] = {};
    static volatile LONG s_hits[16] = {};
    const auto code = ep->ExceptionRecord->ExceptionCode;
    const auto fault = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
    const LONG64 key =
        static_cast<LONG64>((static_cast<uint64_t>(code) << 48) ^ (fault & 0xFFFFFFFFFFFFull));
    int slot = -1;
    for (int i = 0; i < 16; ++i)
    {
        const LONG64 seen = InterlockedCompareExchange64(&s_keys[i], key, 0);
        if (seen == 0 || seen == key)
        {
            slot = i;
            break;
        }
    }
    if (slot < 0 || InterlockedIncrement(&s_hits[slot]) > 3)
        return EXCEPTION_CONTINUE_SEARCH;

    // A breakpoint is not a crash report. eldenring.exe 2.6.2 ends EVERY session with an unhandled
    // int3 at exe+0xC57676 - CS::CSFreeListMemorySystem::quit (Havok) asserting that its list at
    // +0x1280 is empty, from FD4TaskThreadLocalProcess's destructor. Proven the game's own on
    // 2026-08-07 with a zero-natives control profile: same code, same address, same frames, and
    // the only registers that differ are heap bases. Recording it as "CRASH" - now with our
    // version and the player's whole module list under it - tells every player who simply quit the
    // game that the mod killed it. So it keeps its record (a real assert mid-session must not go
    // missing, and [state]'s up=/tOpen=/tClose= tell the two apart) and loses the label and the
    // minidump.
    const bool is_break = (code == static_cast<DWORD>(EXCEPTION_BREAKPOINT) ||
                           code == static_cast<DWORD>(EXCEPTION_SINGLE_STEP));
    crash_write_record(is_break ? "BREAK" : "CRASH", code, fault, ep->ExceptionRecord,
                       ep->ContextRecord);
    if (is_break)
        return EXCEPTION_CONTINUE_SEARCH;
    // Written after the attempt, so the record says whether the .dmp beside it exists and, when it
    // does not, which of the six reasons applied plus what Windows said.
    DWORD gle = 0;
    const int dump_rc = crash_write_dump(ep, &gle);
    char tail[64];
    crash_write(tail, wsprintfA(tail, "  [dump] rc=%d gle=%u\n", dump_rc, gle));
    // Hand it on: whatever wrote the process dumps before still does.
    return EXCEPTION_CONTINUE_SEARCH;
}

static void install_crash_logger(HINSTANCE dll_instance, std::filesystem::path log_file)
{
    g_self_base = reinterpret_cast<uintptr_t>(dll_instance);
    auto dos = reinterpret_cast<PIMAGE_DOS_HEADER>(dll_instance);
    auto nt = reinterpret_cast<PIMAGE_NT_HEADERS>(reinterpret_cast<uint8_t *>(dll_instance) +
                                                  dos->e_lfanew);
    g_self_size = nt->OptionalHeader.SizeOfImage;

    // Read the game's own version/size/stamp now, on this ordinary thread, so that everything the
    // crash writer prints later is a cached number - a handler must not be walking a resource
    // directory while the process is going down.
    goblin::crashdiag::resolve_game_build();
    // Count what our own guarded reads publish. Costs a range compare per process exception
    // and answers a question nothing in this mod could answer before: whether a read loop of
    // ours is feeding another DLL's first-chance filter.
    goblin::crashdiag::arm_fault_counter();

    g_crash_file = CreateFileW(log_file.wstring().c_str(), FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    // Stamp which binary this session is, in the crash file itself. Identifying the build behind
    // report 19 meant byte-matching the shipped releases against a module record in a minidump, and
    // for an older release the matching symbols might not have existed at all. TimeDateStamp is the
    // key the linker map is filed under, so this one line makes symbolisation a lookup.
    {
        char hdr[256];
        SYSTEMTIME st;
        GetLocalTime(&st);
        int len = wsprintfA(hdr, "\n[%04d-%02d-%02d %02d:%02d:%02d] [SESSION] %s v%s git=%s base=",
                            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                            BUILD_NAME, PROJECT_VERSION, GIT_HASH);
        len += crash_hex64(hdr + len, g_self_base);
        len += wsprintfA(hdr + len, " size=");
        len += crash_hex64(hdr + len, g_self_size);
        len += wsprintfA(hdr + len, " stamp=");
        len += crash_hex64(hdr + len, nt->FileHeader.TimeDateStamp);
        hdr[len++] = '\n';
        DWORD wr = 0;
        if (g_crash_file != INVALID_HANDLE_VALUE)
            WriteFile(g_crash_file, hdr, static_cast<DWORD>(len), &wr, nullptr);
        // ... and the game build right under it, so a file that never gets a crash record still
        // says what it was running.
        len = goblin::crashdiag::format_env(hdr, static_cast<int>(sizeof(hdr)));
        if (g_crash_file != INVALID_HANDLE_VALUE && len > 0)
            WriteFile(g_crash_file, hdr, static_cast<DWORD>(len), &wr, nullptr);
    }
    // The module inventory goes into THIS file, not only the session log: reports arrive as the
    // crash log alone often enough, and "which other mods were in the process" has been the first
    // question of every conflict investigation we have run.
    goblin::crashdiag::set_raw_sink(&crash_write);
    goblin::crashdiag::log_modules("init");
    AddVectoredExceptionHandler(1, crash_veh);
    SetUnhandledExceptionFilter(crash_ueh);
}

static void setup_logger(std::filesystem::path log_file)
{
    auto logger = std::make_shared<spdlog::logger>("mapforgoblins");
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] %^[%l]%$ %v");
    // The _mt sink: this logger is written from the watcher, the map/render thread, the hotkey and
    // worker threads at once. The _st sink has no lock at all, so two writers shared the pattern
    // formatter's cached time and, at the daily rotation, the file close/open.
    logger->sinks().push_back(
        std::make_shared<spdlog::sinks::daily_file_sink_mt>(log_file.string(), 0, 0, false, 5));
    logger->flush_on(spdlog::level::info);

#if _DEBUG
    AllocConsole();
    FILE *stream;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
    freopen_s(&stream, "CONIN$", "r", stdin);
    logger->sinks().push_back(std::make_shared<spdlog::sinks::stdout_color_sink_st>());
    logger->set_level(spdlog::level::trace);
#endif

    spdlog::set_default_logger(logger);
}

static std::filesystem::path g_mod_folder;

// Manual per-marker hide: on hide_marker_key, hide/unhide the marker under the map
// cursor (goblin::maphover::hovered_row() -> our WorldMapPointParam row). Applies live
// and persists. Runs on its own thread (like the other hotkeys).
static void manual_hide_hotkey_loop()
{
    bool prev = false;
    while (true)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        if (!goblin::config::enableManualHide) { prev = false; continue; }
        bool down = goblin::overlay::key_down(static_cast<int>(goblin::config::hideMarkerKey)) ||
                    goblin::overlay::gamepad_mask_down(goblin::config::hideMarkerGamepad);
        if (down && !prev)
        {
            void *row = goblin::maphover::hovered_row();
            if (!row)
                // V3 native markers have no engine pin; use the overlay's
                // reticle-distance hover so hide-under-cursor keeps working.
                row = goblin::overlay::native_hover_row();
            if (row)
            {
                goblin::ManualHideResult res = goblin::toggle_hovered_marker(row);
                if (res.matched)
                {
                    goblin::reapply_live_settings();
                    // If that was the last shown marker of the focused category, drop focus.
                    if (goblin::prune_focus_if_empty())
                        goblin::reapply_live_settings();
                    goblin::persist_manual_hidden();
                    spdlog::info("[hide] marker textId={} -> {} ({} hidden total)",
                                 res.textId, res.now_hidden ? "HIDDEN" : "shown",
                                 goblin::manual_hidden_count());
                }
            }
        }
        prev = down;
    }
}

static void setup_mod()
{
    // This thread also runs the refresh loop at the bottom of the function for the rest of the
    // session, so it is the one most likely to be holding a fault of ours.
    goblin::crashdiag::note_own_thread();
    // FIRST, before anything else takes time: record DXGI's own swapchain-creation entry points.
    // They are only worth having while the factory vtable is still pristine, and this thread starts
    // ~1.3 s ahead of a co-loaded frame-generation overlay's hooks (measured, report 21), whereas
    // the SC2 install runs about ten seconds later - too late to capture anything but their proxy.
    // Deliberately here and NOT in DllMain: creating a DXGI factory loads dxgi.dll, and doing that
    // under the loader lock is a deadlock risk. See scratch/user_reports/ERSS_CONFLICT_HANDOFF.md.
    try { cte::overlay::present::capture_creation_entrypoints(); }
    catch (...) { /* a missing snapshot only costs the loop-break's best branch */ }

    // Expand the packed marker table before ANY step can read MAP_ENTRIES. It is a single inflate
    // plus one pass over the records (a few ms), and until it runs the table is empty, so this has
    // to come ahead of every other init rather than be done lazily by whichever reader is first.
    safe_init_step(&init_map_data,    "generated::load_map_data");
    // The enemy names and the English item fallback, packed the same way. Their readers are
    // setup_messages() below and the search index the menus build, so this is ahead of both.
    // (The i18n strings are packed too but expand on first use: config load in DllMain reads them
    // before this thread exists - see goblin_i18n_blob.cpp.)
    safe_init_step(&init_name_tables, "generated::load_name_tables");

    safe_init_step(&init_modutils,    "modutils::initialize");

    // Place the RVA anchors before anything can need one. The pass reads all of .text once
    // (~50ms) and every consumer of an anchor is a render or hook path, so it is done here
    // rather than lazily on whichever frame asks first. It also puts the "which exe build is
    // this" verdict near the top of the log, where a player report can be read from it.
    safe_init_step(&init_anchors,     "anchors::warm");

    // Arm + ENABLE the icon-injection hooks FIRST - before the params wait and before any other work. They
    // are passive (they fire when the worldmap movie loads its DefineSprite-171) and depend only on the
    // loaded exe, not on the game's params/world. The worldmap movie CANNOT be re-injected after it loads
    // (its bitmap-register load-context is transient), so these hooks must be live before the player first
    // opens the world map. Arming them at the earliest possible moment - not behind from::params or any
    // delay - makes icon injection robust to WHEN the DLL itself gets injected, as long as that is before
    // the first map open (always true for a process-start loader).
    safe_init_step(&init_gfx_probe, "gfx_probe::setup");
    try { modutils::enable_hooks(); }  // apply just the gfx_probe hooks queued so far (MH_ApplyQueued)
    catch (const std::exception &e) { spdlog::error("handler setup (gfx) FAILED: {}", e.what()); }

    // Find every class the mod checks by RTTI name (goblin::anchors::kRttiClasses) now, on this
    // thread. Everything that asks for one - the stall_probe menu paths on the UI thread, the
    // parked-movie scan, the kindling worker - is set up further down this same function, so
    // each answer is published before its first caller can exist, and none of them waits or
    // walks the exe. Here and not ahead of gfx_probe: those handlers must be live as early as
    // possible (above), and this walk (~8 ms) only moves the start of the params wait below.
    // Until 2026-09-23 each class was found on first use: 65-90 ms apiece, and the first
    // settings-screen open over the map paid for two of them in a single 186 ms frame.
    safe_init_step(&init_class_names, "anchors::class_names");

    // Blocks (polls internally) until the game's param tables are fully loaded. THIS is the real
    // "wait for game init" - no fixed startup sleep is used (a sleep would also push the hook-arming
    // above past the worldmap movie load on fast Proton boots, which breaks icons).
    safe_init_step(&init_from_params, "from::params::initialize");

    // Manual hides AND the search/category focus are PER CHARACTER: the sets live in
    // MapForGoblins_hidden_s<slot>.txt and MapForGoblins_focus_s<slot>.txt. We only register
    // the folder here; the watcher loop loads the active character's files (and reloads on a
    // character switch) via goblin::sync_hidden_slot(), because the save slot is unknown until
    // a character is loaded (the first map open is always after that). Registered whichever
    // way enable_manual_hide is set: it used to gate this, which silently left the focus with
    // no folder - so it was neither per character nor remembered - whenever hides were off.
    goblin::set_hidden_dir(g_mod_folder);

    safe_init_step(&init_collected,       "collected::initialize");
    safe_init_step(&init_kindling,        "kindling::initialize");
    safe_init_step(&init_inject_entries,  "add_map_entries");
    safe_init_step(&init_apply_map_logic, "apply_map_logic");
    // setup_messages builds the PlaceName textId remap used by the marker rows added above.
    safe_init_step(&init_setup_messages,  "setup_messages");
    safe_init_step(&init_status_line,     "status_line::setup");
    safe_init_step(&init_live_loot,       "refresh_loot_from_itemlot");
    safe_init_step(&init_overlay,         "overlay::setup");
    safe_init_step(&init_map_timing,      "map_timing::setup");
    safe_init_step(&init_maphover,        "maphover::setup");  // marker hover detection (hide/overlay)
    safe_init_step(&init_stall_probe,     "stall_probe::setup");  // debug-only map stall cost counters
    safe_init_step(&init_worldmap_probe,  "worldmap_probe::setup");  // fold non-overworld markers for overlay rings

    try
    {
        modutils::enable_hooks();  // apply the remaining hooks
    }
    catch (const std::exception &e)
    {
        spdlog::error("handler setup FAILED: {}", e.what());
    }

    spdlog::info("Initialization complete");

    if (goblin::config::enableMarkerDump)
    {
        goblin::markers::set_output_path(g_mod_folder / "logs" / "MapForGoblins_markers.log");
        std::thread([] { goblin::crashdiag::note_own_thread();
                         goblin::markers::hotkey_loop(); }).detach();
        spdlog::info("Marker dump hotkey: VK 0x{:X}", goblin::config::markerDumpKey);
    }

    if (goblin::config::enableToggleHotkey)
    {
        std::thread([] { goblin::crashdiag::note_own_thread();
                         goblin::toggle_hotkey_loop(); }).detach();
        spdlog::info("Icon toggle hotkey: VK 0x{:X}", goblin::config::toggleInjectionKey);
    }

    // Started whichever way enable_manual_hide is set: the loop re-reads the setting on every
    // tick and idles while it is off, and the setting is a checkbox in both menus. Starting it
    // only when the setting was on at launch left the hide key dead for a player who switched
    // the feature on from the menu - the checkbox took, nothing listened - until a restart.
    {
        std::thread([] { goblin::crashdiag::note_own_thread();
                         manual_hide_hotkey_loop(); }).detach();
        spdlog::info("Manual marker-hide hotkey: VK 0x{:X} ({})", goblin::config::hideMarkerKey,
                     goblin::config::enableManualHide ? "on" : "off until enabled");
    }

    // The watcher is the single owner of the WorldMapPointParam state - it
    // applies the master-off flag (set by the toggle hotkey OR the overlay's
    // "Show map icons" checkbox). Run it whenever EITHER path can set that flag,
    // so the overlay's master switch works even if the toggle hotkey is disabled.
    if (goblin::config::enableToggleHotkey || goblin::config::menuEnabled)
    {
        std::thread([] { goblin::crashdiag::note_own_thread();
                         goblin::menu_auto_toggle_loop(); }).detach();
        spdlog::info("Icon-state watcher started (icons EXPANDED always; master show/hide via hotkey or overlay)");
    }

    bool first_read = true;
    bool modules_rechecked = false;
    int prev_collected = -1, prev_kindling = -1;
    auto start = std::chrono::steady_clock::now();

    // Tick cadence. The regular tick (everything below) runs every 2 s on a deadline, so the period
    // stays 2.0 s instead of creeping by up to a poll each time. Between regular ticks a cheap poll
    // every 100 ms reads collected::change_signature() (the flag-save table gaining or losing a
    // block, the geometry manager's block tree changing) and the save slot; when it moves, an EARLY
    // tick runs - not sooner than 250 ms after the previous collected refresh - with only what a load
    // or a character switch needs at once: the collected refresh, the visibility gate it feeds, and
    // the per-character slot sync (hide file, saved focus). Kindling, the icon/stall tick, the flag
    // pairs, the fault report and the focus prune stay on the 2 s clock exactly as before. So any
    // load, not only one in the first half-minute, is picked up within a few hundred ms, and a quiet
    // world costs what it did.
    //
    // This replaced a fixed 100 ms "fast phase" for the first 30 s after init, whose stated purpose
    // (catching GeomNonActiveBlockManager data before it moved to the geometry manager) went away
    // when that manager stopped being read (v1.0.17 - it never held collected state). It only ever
    // showed up as 40-120 full walks at ~104 ms when a character loaded inside that window, and a
    // load after it waited up to 2 s. The 30 s mark is still what times the module re-check.
    constexpr auto kPollInterval = std::chrono::milliseconds(100);
    constexpr auto kTickInterval = std::chrono::seconds(2);
    constexpr auto kEarlyTickGap = std::chrono::milliseconds(250);
    auto next_regular = start;              // the first poll runs a regular tick
    auto last_refresh = start - kTickInterval;
    uint64_t last_signature = 0;

    // [tickcost] (debug_logging only): what a tick costs on this thread, how many ran early on a
    // change, and the real poll period - one line per 2 minutes.
    struct TickCost
    {
        int ticks = 0, early = 0, polls = 0;
        int64_t work_us = 0, worst_us = 0, early_us = 0, poll_period_us = 0;
    } tick_cost;
    auto tick_cost_since = start;
    auto prev_poll = start;
    bool tick_tid_logged = false;

    // ...and WHERE it goes. The totals above could say that one tick took 1.49 s (2026-09-23,
    // ERR 2.3.5.1) but not which tick or which part of it. Each tick is timed part by part with QPC
    // laps, beside this thread's own safemem numbers (VirtualQuery calls and time - the thread_local
    // counters, not the process-wide ones every thread adds to) and its CPU time. A tick of 50 ms or
    // more is named on the spot, so its log timestamp says WHEN; the 2-minute line is followed by
    // the window's per-part totals and the worst tick's own split. CPU far below the wall time means
    // the tick WAITED (a lock, a page brought in, the scheduler); kernel CPU close to it means a
    // system call - VirtualQuery above all - did the work.
    enum TickPart : int
    {
        TP_SIG, TP_MODULES, TP_FAULTS, TP_REFRESH, TP_KINDLING, TP_GFX_MSGCHECK, TP_GFX_STALL,
        TP_GFX_LAYER, TP_GFX_CHARIDS, TP_GFX_REST, TP_FLAGS, TP_VIS, TP_SLOT, TP_PRUNE, TP_REST,
        TP_COUNT
    };
    static constexpr const char *kTickPartName[TP_COUNT] = {
        "sig", "modules", "faults", "refresh", "kindling", "gfx.msgcheck", "gfx.stall", "gfx.layer",
        "gfx.charids", "gfx.rest", "flags", "vis", "slot", "prune", "rest"};
    struct TickSplit
    {
        int64_t qpc[TP_COUNT] = {};
        uint64_t vq_calls = 0, vq_qpc = 0, copies = 0, refused = 0, page_hits = 0;
        int64_t cpu_user_us = 0, cpu_kernel_us = 0;
    };
    TickSplit split_sum{}, split_worst{};
    bool worst_early = false;
    const int64_t qpc_hz = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<int64_t>(f.QuadPart ? f.QuadPart : 1);
    }();
    auto qpc_now = [] {
        LARGE_INTEGER v;
        QueryPerformanceCounter(&v);
        return static_cast<int64_t>(v.QuadPart);
    };
    // GetThreadTimes advances in scheduler quanta (~15.6 ms), so the CPU split is coarse on a short
    // tick; it is there for the long ones, where waited-versus-worked is the question.
    auto thread_cpu_us = [](int64_t &user, int64_t &kernel) {
        FILETIME created{}, exited{}, k{}, u{};
        user = kernel = 0;
        if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &k, &u))
            return;
        const auto us = [](const FILETIME &f) {
            return static_cast<int64_t>(((static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 10);
        };
        user = us(u);
        kernel = us(k);
    };
    // "refresh 3412, gfx.layer 288950; VQ 67 call(s) 288731 us, ...". Parts under 100 us are left out.
    auto split_text = [&](const TickSplit &s) {
        char b[768];
        int n = 0;
        for (int p = 0; p < TP_COUNT; ++p)
        {
            const long long us = static_cast<long long>(s.qpc[p] * 1000000 / qpc_hz);
            if (us < 100 || n >= 600)
                continue;
            const int w = _snprintf_s(b + n, sizeof(b) - n, _TRUNCATE, "%s%s %lld", n ? ", " : "",
                                      kTickPartName[p], us);
            if (w > 0)
                n += w;
        }
        _snprintf_s(b + n, sizeof(b) - n, _TRUNCATE,
                    "%sVQ %llu call(s) %lld us, %llu copies, %llu refused, %llu page-probe answers; cpu user %lld + "
                    "kernel %lld us",
                    n ? "; " : "every part under 100 us; ", static_cast<unsigned long long>(s.vq_calls),
                    static_cast<long long>(static_cast<int64_t>(s.vq_qpc) * 1000000 / qpc_hz),
                    static_cast<unsigned long long>(s.copies), static_cast<unsigned long long>(s.refused),
                    static_cast<unsigned long long>(s.page_hits), static_cast<long long>(s.cpu_user_us),
                    static_cast<long long>(s.cpu_kernel_us));
        return std::string(b);
    };

    while (true)
    {
        std::this_thread::sleep_for(kPollInterval);
        const auto poll_time = std::chrono::steady_clock::now();
        // One read of the switch per poll, so a tick is split, and reported, whole or not at all.
        const bool tc_on = goblin::config::debugLogging;
        if (tc_on)
        {
            ++tick_cost.polls;
            tick_cost.poll_period_us +=
                std::chrono::duration_cast<std::chrono::microseconds>(poll_time - prev_poll).count();
        }
        prev_poll = poll_time;
        // The map API's snapshots (sdk/mfg_map_api.h): built here, never on a reader's call. Returns
        // at once unless a reader asked for a layer in the last few seconds.
        safe_map_api_maintain_seh();
        TickSplit split{};
        int64_t lap_mark = tc_on ? qpc_now() : 0;
        auto lap = [&](TickPart part) {
            if (!tc_on)
                return;
            const int64_t now = qpc_now();
            split.qpc[part] += now - lap_mark;
            lap_mark = now;
        };

        uint64_t signature = 0;
        try
        {
            signature = safe_change_signature_seh() * 31u + static_cast<uint64_t>(goblin::active_save_slot() + 2);
        }
        catch (...)
        {
        }
        // Regular: the poll nearest the deadline (within half a poll either side). Early: the
        // signature moved and the last collected refresh is at least kEarlyTickGap old.
        const bool regular = poll_time + kPollInterval / 2 >= next_regular;
        const bool early_tick =
            !regular && signature != last_signature && poll_time - last_refresh >= kEarlyTickGap;
        if (!regular && !early_tick)
            continue;
        lap(TP_SIG);
        int64_t cpu_user0 = 0, cpu_kernel0 = 0;
        uint64_t vq0 = 0, vq_qpc0 = 0, copies0 = 0, refused0 = 0, page_hits0 = 0;
        if (tc_on)
        {
            thread_cpu_us(cpu_user0, cpu_kernel0);
            vq0 = goblin::safemem::t_queries;
            vq_qpc0 = goblin::safemem::t_query_qpc;
            copies0 = goblin::safemem::t_copies;
            refused0 = goblin::safemem::t_refused;
            page_hits0 = goblin::safemem::t_page_hits;
        }
        if (regular)
        {
            next_regular += kTickInterval;
            if (next_regular <= poll_time) // fell a whole interval behind (a long tick): restart the clock
                next_regular = poll_time + kTickInterval;
        }
        last_signature = signature;
        last_refresh = poll_time;
        const bool startup_window = poll_time - start < std::chrono::seconds(30);

        // NOTHING IN A TICK MAY UNWIND OUT OF setup_mod. This function does not return after
        // "Initialization complete" - it lives in this loop for the whole session - so a throw
        // here used to land in the DllMain catch, which read it as an init failure and tore the
        // mod down: MinHook uninstalled, trampolines freed under running worker threads, the
        // logger shut down while they were still writing to it. Report 43 lost two whole
        // sessions that way, 30 s in, to one unconvertible module file name. A tick that fails
        // is a tick that failed; it is not a reason to dismantle a working session.
        try
        {

            // Second and last inventory pass, once the frame-generation overlays and the other loader
            // DLLs have finished arriving - at init they are simply not there yet. Writes nothing if
            // the count is unchanged.
            if (regular && !startup_window && !modules_rechecked)
            {
                modules_rechecked = true;
                // Guarded because this walks paths that came from OTHER people's DLLs. Report 43: a
                // module whose file name had no mapping in the machine's ANSI code page threw out of
                // here, unwound the whole loop below out of setup_mod, and the catch in DllMain then
                // deinitialised MinHook under running worker threads - the mod died 30 s into every
                // session over one log line. The conversion no longer throws, and this is the belt to
                // that pair of braces: an inventory line is never worth the session.
                try
                {
                    goblin::crashdiag::log_modules("t+30s");
                }
                catch (const std::exception &e)
                {
                    spdlog::warn("[modules] t+30s inventory skipped: {}", e.what());
                }
            }
            lap(TP_MODULES);

            // Faults our own reads raised since the last poll. Printed whether or not the map is
            // open - the map being CLOSED is exactly the window that had no instrument at all.
            if (regular)
            {
                if (std::string faults = goblin::crashdiag::fault_report(); !faults.empty())
                    spdlog::warn("[faults] {}", faults);
            }
            lap(TP_FAULTS);

            try
            {
                int newly = safe_refresh_seh();
                if (first_read && newly > 0)
                {
                    spdlog::info("Initial state: {} pieces hidden",
                                 goblin::collected::collected_count());
                    first_read = false;
                }
            }
            catch (...)
            {
            }
            lap(TP_REFRESH);

            // The 2 s clock only (see the cadence note above the loop).
            if (regular)
            {
                try
                {
                    safe_kindling_refresh_seh();
                }
                catch (...)
                {
                }
                lap(TP_KINDLING);

                try
                {
                    safe_gfx_tick_seh(); // audit, stall sampler + diagnostics (overlay-independent)
                }
                catch (...)
                {
                }
                if (tc_on)
                {
                    // tick() times its own blocks; whatever it spent outside them (and a split cut
                    // short by a fault it swallowed) lands in gfx.rest.
                    const auto &g = goblin::gfx_probe::last_tick_split();
                    const int64_t now = qpc_now();
                    const int64_t inner = g.msgcheck + g.stall + g.layer + g.charids;
                    split.qpc[TP_GFX_MSGCHECK] += g.msgcheck;
                    split.qpc[TP_GFX_STALL] += g.stall;
                    split.qpc[TP_GFX_LAYER] += g.layer;
                    split.qpc[TP_GFX_CHARIDS] += g.charids;
                    split.qpc[TP_GFX_REST] += (now - lap_mark) - inner;
                    lap_mark = now;
                }

                try
                {
                    safe_flag_or_pairs_seh();
                }
                catch (...)
                {
                }
                lap(TP_FLAGS);
            }

            // When the collected set changes, refresh the live visibility gate so
            // collected pieces/nodes/kindling hide (and revealed ones reappear) on
            // the OPEN map without a reopen. Category-toggle changes come in via the
            // overlay (reapply_live_settings); this covers in-world collection.
            int cc = goblin::collected::collected_count();
            int kc = goblin::kindling::collected_count();
            if (cc != prev_collected || kc != prev_kindling)
            {
                prev_collected = cc;
                prev_kindling = kc;
                safe_apply_category_visibility_seh();
            }
            // World state (which Leyndell stands, flag 300): the visibility test reads it live, but
            // an open map only re-reads on a merge, and the engine-pin build can only be told
            // through the param flags - so a flip re-applies and announces itself, like a pickup.
            if (safe_world_state_changed_seh())
                safe_apply_category_visibility_seh();
            lap(TP_VIS);

            // A focus that ENDS on this thread (the slot switch drops the old character's, the
            // prune drops an emptied one) must be undone the way the menus undo one: the full
            // re-apply, since the focus forced its rows past the fragment and post-event gates
            // (eventFlagId, group 2) and only apply_map_logic puts those back. A category re-apply
            // alone left them showing until the next settings change.
            auto focus_on = [] { return goblin::focus_category() >= 0 || goblin::focus_rows_active(); };

            // Per-character state: on a save-slot (character) switch, load that character's
            // focus (and, when the feature is on, their hidden set) and reapply visibility.
            // BEFORE the prune below, not after: on the tick a switch happens, prune would
            // still be holding the previous character's focus while reading the new one's
            // collected/flag state, and would write that verdict into the PREVIOUS character's
            // file. Runs whichever way enable_manual_hide is set - the focus is its own feature.
            try
            {
                const bool had_focus = focus_on();
                if (goblin::sync_hidden_slot())
                {
                    if (had_focus && !focus_on())
                        safe_reapply_live_settings_seh();
                    else
                        safe_apply_category_visibility_seh();
                }
            }
            catch (...)
            {
            }
            lap(TP_SLOT);

            // Auto-clear a category focus once its last shown marker is gone (in-world pickup,
            // flag, GEOF, etc.) so a stale "showing only ..." highlight doesn't stick around.
            // On the 2 s clock, after the slot sync of the same tick as before.
            if (regular && goblin::prune_focus_if_empty())
                safe_reapply_live_settings_seh();
            lap(TP_PRUNE);
        }
        catch (const std::exception &e)
        {
            static int s_tick_errors = 0;
            if (s_tick_errors < 3)
            {
                ++s_tick_errors;
                spdlog::warn("[tick] a poll tick failed and was skipped: {}", e.what());
            }
        }
        catch (...)
        {
            static int s_tick_unknown = 0;
            if (s_tick_unknown < 3)
            {
                ++s_tick_unknown;
                spdlog::warn("[tick] a poll tick failed and was skipped (unknown exception)");
            }
        }

        lap(TP_REST); // whatever ran after the last lap (a tick cut short by an exception included)

        if (tc_on)
        {
            using namespace std::chrono;
            const auto done = steady_clock::now();
            const int64_t work = duration_cast<microseconds>(done - poll_time).count();
            split.vq_calls = goblin::safemem::t_queries - vq0;
            split.vq_qpc = goblin::safemem::t_query_qpc - vq_qpc0;
            split.copies = goblin::safemem::t_copies - copies0;
            split.refused = goblin::safemem::t_refused - refused0;
            split.page_hits = goblin::safemem::t_page_hits - page_hits0;
            {
                int64_t cpu_user1 = 0, cpu_kernel1 = 0;
                thread_cpu_us(cpu_user1, cpu_kernel1);
                split.cpu_user_us = cpu_user1 - cpu_user0;
                split.cpu_kernel_us = cpu_kernel1 - cpu_kernel0;
            }
            if (!tick_tid_logged)
            {
                tick_tid_logged = true;
                spdlog::info("[tickcost] watcher ticks run on tid {}", GetCurrentThreadId());
            }
            if (early_tick)
            {
                ++tick_cost.early;
                tick_cost.early_us += work;
            }
            else
            {
                ++tick_cost.ticks;
                tick_cost.work_us += work;
            }
            for (int p = 0; p < TP_COUNT; ++p)
                split_sum.qpc[p] += split.qpc[p];
            split_sum.vq_calls += split.vq_calls;
            split_sum.vq_qpc += split.vq_qpc;
            split_sum.copies += split.copies;
            split_sum.refused += split.refused;
            split_sum.page_hits += split.page_hits;
            split_sum.cpu_user_us += split.cpu_user_us;
            split_sum.cpu_kernel_us += split.cpu_kernel_us;
            // Named on the spot: this line's own timestamp is the answer to "which tick".
            if (work >= 50000)
                spdlog::info("[tickcost] slow {} tick {} us: {}; map {}", early_tick ? "early" : "regular", work,
                             split_text(split), goblin::maphover::map_dialog() ? "open" : "closed");
            if (work > tick_cost.worst_us)
            {
                tick_cost.worst_us = work;
                split_worst = split;
                worst_early = early_tick;
            }
            if (done - tick_cost_since >= minutes(2))
            {
                spdlog::info("[tickcost] {} s: {} regular tick(s) avg {} us, {} early on a change avg {} us, worst "
                             "{} us; {} poll(s), period avg {} us",
                             duration_cast<seconds>(done - tick_cost_since).count(), tick_cost.ticks,
                             tick_cost.ticks ? tick_cost.work_us / tick_cost.ticks : 0, tick_cost.early,
                             tick_cost.early ? tick_cost.early_us / tick_cost.early : 0, tick_cost.worst_us,
                             tick_cost.polls, tick_cost.polls ? tick_cost.poll_period_us / tick_cost.polls : 0);
                spdlog::info("[tickcost] parts over the window: {}", split_text(split_sum));
                spdlog::info("[tickcost] the worst tick ({}, {} us): {}", worst_early ? "early" : "regular",
                             tick_cost.worst_us, split_text(split_worst));
                tick_cost = {};
                split_sum = {};
                split_worst = {};
                worst_early = false;
                tick_cost_since = done;
            }
        }
    }
}

bool WINAPI DllMain(HINSTANCE dll_instance, unsigned int fdw_reason, void *lpv_reserved)
{
    if (fdw_reason == DLL_PROCESS_ATTACH)
    {
        wchar_t dll_filename[MAX_PATH] = {0};
        GetModuleFileNameW(dll_instance, dll_filename, MAX_PATH);
        auto folder = std::filesystem::path(dll_filename).parent_path();
        g_mod_folder = folder;

        setup_logger(folder / "logs" / "MapForGoblins.log");
        install_crash_logger(dll_instance, folder / "logs" / "MapForGoblins_crash.log");

        spdlog::info("Map For Goblins DLL v{} [{}] ({})", PROJECT_VERSION, BUILD_NAME, GIT_HASH);
        {
            // Which game build this session is. Everything the mod does is derived per exe build -
            // the anchor resolver, the RVA table, the param layouts - so a report that does not
            // name the exe leaves every one of those unverifiable. SizeOfImage and TimeDateStamp
            // go with the version string because a repacked or downpatched exe keeps the string
            // and changes those.
            wchar_t exe_path[MAX_PATH] = {0};
            GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
            const uint64_t v = goblin::crashdiag::game_version();
            spdlog::info("Game: {} v{}.{}.{}.{} img=0x{:X} ts=0x{:X}",
                         std::filesystem::path(exe_path).filename().string(),
                         static_cast<unsigned>((v >> 48) & 0xFFFF),
                         static_cast<unsigned>((v >> 32) & 0xFFFF),
                         static_cast<unsigned>((v >> 16) & 0xFFFF),
                         static_cast<unsigned>(v & 0xFFFF), goblin::crashdiag::game_image_size(),
                         goblin::crashdiag::game_timestamp());
        }
        goblin::load_config(folder / "MapForGoblins.ini");
        (void)goblin::config::menu_mode(); // seals the session's menu mode (see menu_mode)

        if (goblin::config::debugLogging)
            spdlog::default_logger()->set_level(spdlog::level::debug);

        mod_thread = std::thread([]()
                                 {
            try
            {
                setup_mod();
            }
            catch (std::runtime_error const &e)
            {
                // NO TEARDOWN HERE, deliberately. This catch has called
                // modutils::deinitialize() + spdlog::shutdown() since the first commit, back when
                // setup_mod really did only set things up and nothing of ours was running yet. It
                // has not meant that since v1.0.18 put worker threads behind hooks: by the time
                // anything throws, the pad-poll, hotkey and watcher threads are live, and both
                // calls pull the ground out from under them. MH_Uninitialize frees the trampoline
                // blocks, and a thread that CACHED a trampoline pointer - the overlay's
                // XInputGetState is one - calls into freed memory on its next tick; that is the
                // `execute` fault on a FREE 64 KB region in report 43, and the same signature in
                // report 42. spdlog::shutdown() is the same mistake with the logger.
                //
                // Neither call buys anything either. A failure before any hook is installed leaves
                // MinHook with nothing to uninstall, and a failure after is exactly when
                // uninstalling is unsafe. The process-detach path below still deinitialises, which
                // is the right place: by then the other threads are gone.
                //
                // The loop in setup_mod now guards its own ticks as well, so reaching here at all
                // means a genuine failure on the way up.
                spdlog::error("mod init failed: {}", e.what());
            } });
    }
    else if (fdw_reason == DLL_PROCESS_DETACH && lpv_reserved != nullptr)
    {
        try
        {
            mod_thread.join();
            modutils::deinitialize();
        }
        catch (std::runtime_error const &e)
        {
            spdlog::error("teardown failed: {}", e.what());
        }
        spdlog::shutdown();
    }
    return true;
}
