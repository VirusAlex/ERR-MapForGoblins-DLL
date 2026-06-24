#include "goblin_diag.hpp"
#include <mutex>
#include <sstream>

namespace
{
    std::mutex g_mx;

    // -1 = unknown / not reached yet, 0 = failed, 1 = ok
    int s_hooks = -1;
    std::string s_hooks_reason;

    int s_s171 = -1;            // -1 = map not opened yet
    uint32_t s_base = 0;
    int s_placed = 0, s_total = 0;
    std::string s_s171_reason;

    int s_bm_reg = -1, s_bm_total = 0;

    int s_logo = -1;
    std::string s_logo_reason;

    int s_remap = -1;

    goblin::diag::OverlayState s_ov = goblin::diag::OverlayState::Pending;
    std::string s_ov_reason;
    bool s_ov_set = false;

    int s_selfheal = 0;
    uint32_t s_selfheal_base = 0;
}

namespace goblin::diag
{
    void set_hooks(bool ok, const char *reason)
    {
        std::lock_guard<std::mutex> lk(g_mx);
        s_hooks = ok ? 1 : 0;
        s_hooks_reason = reason ? reason : "";
    }

    void set_sprite171(bool ok, uint32_t base, int placed, int total, const char *reason)
    {
        std::lock_guard<std::mutex> lk(g_mx);
        s_s171 = ok ? 1 : 0;
        s_base = base;
        s_placed = placed;
        s_total = total;
        s_s171_reason = reason ? reason : "";
    }

    void set_bitmaps(int registered, int total)
    {
        std::lock_guard<std::mutex> lk(g_mx);
        s_bm_reg = registered;
        s_bm_total = total;
    }

    void set_logo(bool ok, const char *reason)
    {
        std::lock_guard<std::mutex> lk(g_mx);
        s_logo = ok ? 1 : 0;
        s_logo_reason = reason ? reason : "";
    }

    void set_remap(int count)
    {
        std::lock_guard<std::mutex> lk(g_mx);
        s_remap = count;
    }

    void set_overlay(OverlayState state, const char *reason)
    {
        std::lock_guard<std::mutex> lk(g_mx);
        s_ov = state;
        s_ov_reason = reason ? reason : "";
        s_ov_set = true;
    }

    void note_selfheal(uint32_t newbase)
    {
        std::lock_guard<std::mutex> lk(g_mx);
        ++s_selfheal;
        s_selfheal_base = newbase;
    }

    std::string report()
    {
        std::lock_guard<std::mutex> lk(g_mx);
        std::ostringstream o;

        // Load hooks - if these are not armed, nothing else can run.
        o << "Load hooks: ";
        if (s_hooks == 1)
            o << "armed (ok)\n";
        else if (s_hooks == 0)
            o << "FAILED - " << (s_hooks_reason.empty() ? "unknown" : s_hooks_reason)
              << "\n  -> no icons can load on this game version; please report this.\n";
        else
            o << "not armed yet\n";

        // World-map icon injection (the main event).
        o << "World map icons: ";
        if (s_s171 == 1)
        {
            o << "OK  (base charId " << s_base << ", " << s_placed << "/" << s_total << " frames)\n";
            if (s_placed < s_total)
                o << "  -> " << (s_total - s_placed) << " frame(s) did not append this load.\n";
        }
        else if (s_s171 == 0)
            o << "FAILED - " << (s_s171_reason.empty() ? "unknown" : s_s171_reason)
              << "\n  -> map opened but icons did not inject; please report this.\n";
        else
            o << "not injected yet - open the world map once, then re-check.\n";

        // Icon bitmaps.
        if (s_bm_reg >= 0)
        {
            o << "Icon bitmaps: " << s_bm_reg << "/" << s_bm_total << " registered";
            o << (s_bm_reg < s_bm_total ? "  (some failed)\n" : "\n");
        }

        // Marker remap.
        if (s_remap >= 0)
            o << "Markers pointed at icons: " << s_remap << "\n";

        // Logo (cosmetic).
        if (s_logo == 1)
            o << "Menu logo: ok\n";
        else if (s_logo == 0)
            o << "Menu logo: not placed - "
              << (s_logo_reason.empty() ? "(cosmetic, harmless)" : s_logo_reason) << "\n";

        // Self-heal events.
        if (s_selfheal > 0)
            o << "Self-heal re-base events: " << s_selfheal
              << " (last base " << s_selfheal_base << ")\n";

        // Overlay backend (you can only read this if it is active, but it is
        // still useful in a copied report).
        o << "Overlay (DX12): ";
        switch (s_ov)
        {
        case OverlayState::OffByConfig: o << "disabled in ini (enable_overlay=false)\n"; break;
        case OverlayState::Active:      o << "active\n"; break;
        case OverlayState::Failed:      o << "FAILED - " << (s_ov_reason.empty() ? "unknown" : s_ov_reason) << "\n"; break;
        case OverlayState::Pending:     o << (s_ov_set ? "initializing...\n" : "active\n"); break;
        }

        return o.str();
    }
}
