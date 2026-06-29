#include "goblin_config.hpp"
#include "goblin_config_schema.hpp"
#include "goblin_i18n.hpp"

#include <spdlog/spdlog.h>
#include <mini/ini.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// NOTE: the goblin::config:: variables and the ini schema/emitter live in
// goblin_config_schema.cpp (kept dependency-free so tools/mfg_inigen links it).
// This file holds the runtime: ensure/migrate the ini, then load values.

namespace
{
    std::string to_lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    // Parse a string value into the entry's typed target variable.
    void set_from_string(const goblin::IniEntry &e, const std::string &v)
    {
        using goblin::IniType;
        switch (e.type)
        {
        case IniType::Bool:
            *static_cast<bool *>(e.target) = (v != "false");
            break;
        case IniType::U8:
        {
            int val;
            try { val = std::stoi(v); }
            catch (...) { val = 15; }
            if (val < 0 || val > 255) val = 15;
            *static_cast<uint8_t *>(e.target) = static_cast<uint8_t>(val);
            break;
        }
        case IniType::VkKey:
        {
            uint32_t vk = goblin::parse_vk_code(v);
            if (vk) *static_cast<uint32_t *>(e.target) = vk;
            break;
        }
        case IniType::GamepadMask:
        {
            uint16_t m = goblin::parse_gamepad_combo(v);
            if (m) *static_cast<uint16_t *>(e.target) = m;
            break;
        }
        case IniType::String:
            *static_cast<std::string *>(e.target) = goblin::i18n::normalize_language_config(v);
            break;
        case IniType::Float:
        {
            float val;
            try { val = std::stof(v); }
            catch (...) { val = 1.0f; }
            if (val < 0.5f) val = 0.5f;
            if (val > 3.0f) val = 3.0f;
            *static_cast<float *>(e.target) = val;
            break;
        }
        }
    }

    // Seed every config variable from its schema default. In the vanilla build
    // ERR-only entries are force-disabled (so their features stay off even if
    // an ERR ini is present).
    void apply_defaults()
    {
        for (auto const &sec : goblin::ini_schema())
        {
            for (auto const &e : sec.entries)
            {
                bool err_only = e.err_only || sec.err_only;
                if (goblin::profile_is_vanilla() && err_only)
                {
                    if (e.type == goblin::IniType::Bool)
                        *static_cast<bool *>(e.target) = false;
                    continue;
                }
                set_from_string(e, e.def);
            }
        }
    }
}

void goblin::ensure_ini(const std::filesystem::path &ini_path)
{
    namespace fs = std::filesystem;
    const bool include_err = !profile_is_vanilla();

    mINI::INIStructure existing;
    bool had = fs::exists(ini_path);
    if (had)
    {
        mINI::INIFile f(ini_path.string());
        if (!f.read(existing)) had = false;
    }

    std::set<std::pair<std::string, std::string>> consumed; // lowercased (section,key)

    IniValueResolver resolve =
        [&](const std::string &section, const IniEntry &e, std::string &out) -> bool
    {
        if (!had) return false;
        std::string lsec = to_lower(section);
        if (existing.has(section) && existing[section].has(e.key))
        {
            out = existing[section].get(e.key);
            consumed.insert({lsec, to_lower(e.key)});
            return true;
        }
        if (e.rename_from && existing.has(section) && existing[section].has(e.rename_from))
        {
            out = existing[section].get(e.rename_from);
            consumed.insert({lsec, to_lower(e.rename_from)});
            return true;
        }
        // Cross-section fallback: a key (or its rename_from) that moved to a
        // different section in a newer schema still keeps its user value. Schema
        // keys are globally unique, so matching by key name in any section is safe.
        for (auto const &sp : existing)
        {
            if (sp.second.has(e.key))
            {
                out = sp.second.get(e.key);
                consumed.insert({to_lower(sp.first), to_lower(e.key)});
                return true;
            }
            if (e.rename_from && sp.second.has(e.rename_from))
            {
                out = sp.second.get(e.rename_from);
                consumed.insert({to_lower(sp.first), to_lower(e.rename_from)});
                return true;
            }
        }
        return false;
    };

    std::string language_value = goblin::config::uiLanguage;
    if (had)
    {
        for (auto const &sp : existing)
        {
            if (sp.second.has("ui_language"))
            {
                language_value = sp.second.get("ui_language");
                break;
            }
        }
    }
    auto emit_language = goblin::i18n::language_from_config(language_value);

    std::ostringstream ss;
    emit_ini(ss, include_err, resolve, emit_language);

    // Keys in the old file that the schema (for this build) didn't claim:
    // renamed-away, removed, or ERR-only in a vanilla build. Comment them out,
    // preserving the value, rather than silently dropping them.
    if (had)
    {
        std::vector<std::string> dead;
        for (auto const &sec_pair : existing)
        {
            const std::string &sname = sec_pair.first;
            for (auto const &kv : sec_pair.second)
            {
                if (!consumed.count({to_lower(sname), to_lower(kv.first)}))
                    dead.push_back("[" + sname + "] " + kv.first + " = " + kv.second);
            }
        }
        if (!dead.empty())
        {
            ss << "\n; ---------------------------------------------------------------\n"
               << "; No longer used by this version (renamed, removed, or not applicable\n"
               << "; to this build). Kept commented for reference; safe to delete.\n";
            for (auto const &d : dead) ss << "; " << d << "\n";
        }
    }

    std::string desired = ss.str();
    std::string current;
    if (had)
    {
        std::ifstream in(ini_path, std::ios::binary);
        std::ostringstream b;
        b << in.rdbuf();
        current = b.str();
    }
    if (!had || current != desired)
    {
        std::error_code ec;
        if (ini_path.has_parent_path()) fs::create_directories(ini_path.parent_path(), ec);
        std::ofstream out(ini_path, std::ios::binary | std::ios::trunc);
        out << desired;
        spdlog::info("Config: ini {}", had ? "re-synced" : "created with defaults");
    }
}

std::filesystem::path goblin::g_ini_path;

void goblin::save_config(const std::filesystem::path &ini_path)
{
    const bool include_err = !profile_is_vanilla();

    IniValueResolver resolve =
        [&](const std::string &section, const IniEntry &e, std::string &out) -> bool
    {
        switch (e.type)
        {
        case IniType::Bool:
            out = *static_cast<bool *>(e.target) ? "true" : "false";
            return true;
        case IniType::U8:
            out = std::to_string(static_cast<int>(*static_cast<uint8_t *>(e.target)));
            return true;
        case IniType::VkKey: // live value (the overlay can rebind these now)
            out = format_vk_code(*static_cast<uint32_t *>(e.target));
            return true;
        case IniType::GamepadMask:
            out = format_gamepad_combo(*static_cast<uint16_t *>(e.target));
            return true;
        case IniType::String:
            out = goblin::i18n::normalize_language_config(*static_cast<std::string *>(e.target));
            return true;
        case IniType::Float:
        {
            char buf[16];
            std::snprintf(buf, sizeof buf, "%.2f", *static_cast<float *>(e.target));
            out = buf;
            return true;
        }
        default:
            return false;
        }
    };

    std::ostringstream ss;
    emit_ini(ss, include_err, resolve, goblin::i18n::current_language());

    std::error_code ec;
    if (ini_path.has_parent_path())
        std::filesystem::create_directories(ini_path.parent_path(), ec);
    std::ofstream out(ini_path, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        spdlog::warn("save_config: cannot open {}", ini_path.string());
        return;
    }
    out << ss.str();
    spdlog::info("Config: saved to {}", ini_path.string());
}

void goblin::load_config(const std::filesystem::path &ini_path)
{
    spdlog::info("Config: {}", ini_path.string());
    g_ini_path = ini_path;

    ensure_ini(ini_path);
    apply_defaults();

    mINI::INIFile file(ini_path.string());
    mINI::INIStructure ini;
    if (!file.read(ini))
    {
        spdlog::warn("Failed to read INI file, using defaults");
        return;
    }

    const bool include_err = !profile_is_vanilla();
    for (auto const &sec : ini_schema())
    {
        if (sec.err_only && !include_err) continue;
        if (!ini.has(sec.name)) continue;
        auto &cfg = ini[sec.name];
        for (auto const &e : sec.entries)
        {
            if (e.err_only && !include_err) continue;
            std::string v;
            if (cfg.has(e.key))
                v = cfg.get(e.key);
            else if (e.rename_from && cfg.has(e.rename_from))
                v = cfg.get(e.rename_from);
            else
                continue;
            set_from_string(e, v);
        }
    }
}

// Parse an XInput button combo string like "Y+R3" or "LB+RB+START" into a
// bitmask. Buttons separated by '+'. Returns 0 if any token is unknown.
uint16_t goblin::parse_gamepad_combo(std::string s)
{
    static const std::unordered_map<std::string, uint16_t> name_to_mask = {
        {"A", 0x1000}, {"B", 0x2000}, {"X", 0x4000}, {"Y", 0x8000},
        {"LB", 0x0100}, {"RB", 0x0200},
        {"L3", 0x0040}, {"LSTICK", 0x0040},
        {"R3", 0x0080}, {"RSTICK", 0x0080},
        {"BACK", 0x0020}, {"SELECT", 0x0020}, {"VIEW", 0x0020},
        {"START", 0x0010}, {"MENU", 0x0010},
        {"DPAD_UP", 0x0001}, {"UP", 0x0001},
        {"DPAD_DOWN", 0x0002}, {"DOWN", 0x0002},
        {"DPAD_LEFT", 0x0004}, {"LEFT", 0x0004},
        {"DPAD_RIGHT", 0x0008}, {"RIGHT", 0x0008},
    };
    uint16_t mask = 0;
    std::transform(s.begin(), s.end(), s.begin(), ::toupper);
    size_t pos = 0;
    while (pos < s.size())
    {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '+'))
            pos++;
        size_t end = pos;
        while (end < s.size() && s[end] != '+' && s[end] != ' ' && s[end] != '\t')
            end++;
        if (end == pos) break;
        std::string tok = s.substr(pos, end - pos);
        auto it = name_to_mask.find(tok);
        if (it == name_to_mask.end())
        {
            spdlog::warn("Unknown gamepad button: '{}'", tok);
            return 0;
        }
        mask |= it->second;
        pos = end;
    }
    return mask;
}

// Parse a human key name ("F9", "A", "Home", "0", "Space") into Win32 VK_* code.
// Returns 0 on unknown name.
uint32_t goblin::parse_vk_code(std::string name)
{
    for (auto &c : name)
        if (c >= 'a' && c <= 'z') c -= 32; // uppercase

    if (name.empty()) return 0;

    if (name.size() > 2 && name[0] == '0' && name[1] == 'X') // raw "0xNN" code
    {
        try { return static_cast<uint32_t>(std::stoul(name.substr(2), nullptr, 16)); }
        catch (...) {}
    }

    if (name.size() >= 2 && name[0] == 'F')
    {
        try
        {
            int n = std::stoi(name.substr(1));
            if (n >= 1 && n <= 24) return 0x6F + n; // VK_F1=0x70
        }
        catch (...) {}
    }

    if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z')
        return static_cast<uint32_t>(name[0]);
    if (name.size() == 1 && name[0] >= '0' && name[0] <= '9')
        return static_cast<uint32_t>(name[0]);

    static const std::pair<const char *, uint32_t> named[] = {
        {"SPACE", 0x20}, {"ESCAPE", 0x1B}, {"ESC", 0x1B},
        {"TAB", 0x09}, {"ENTER", 0x0D}, {"RETURN", 0x0D}, {"BACKSPACE", 0x08},
        {"HOME", 0x24}, {"END", 0x23},
        {"PAGEUP", 0x21}, {"PAGEDOWN", 0x22}, {"INSERT", 0x2D}, {"DELETE", 0x2E},
        {"UP", 0x26}, {"DOWN", 0x28}, {"LEFT", 0x25}, {"RIGHT", 0x27},
    };
    for (auto &p : named)
        if (name == p.first) return p.second;
    return 0;
}

// Inverse of parse_vk_code: a name parse_vk_code accepts (or "0xNN" fallback).
std::string goblin::format_vk_code(uint32_t vk)
{
    if (vk >= 0x70 && vk <= 0x87) return "F" + std::to_string(vk - 0x6F); // F1..F24
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        return std::string(1, static_cast<char>(vk));
    switch (vk)
    {
    case 0x20: return "Space";
    case 0x1B: return "Escape";
    case 0x09: return "Tab";
    case 0x0D: return "Enter";
    case 0x08: return "Backspace";
    case 0x24: return "Home";
    case 0x23: return "End";
    case 0x21: return "PageUp";
    case 0x22: return "PageDown";
    case 0x2D: return "Insert";
    case 0x2E: return "Delete";
    case 0x26: return "Up";
    case 0x28: return "Down";
    case 0x25: return "Left";
    case 0x27: return "Right";
    default: { char b[8]; std::snprintf(b, sizeof b, "0x%02X", vk); return b; }
    }
}

// Inverse of parse_gamepad_combo: tokens joined with '+', in a stable order.
std::string goblin::format_gamepad_combo(uint16_t mask)
{
    static const std::pair<uint16_t, const char *> order[] = {
        {0x8000, "Y"}, {0x4000, "X"}, {0x1000, "A"}, {0x2000, "B"},
        {0x0100, "LB"}, {0x0200, "RB"}, {0x0040, "L3"}, {0x0080, "R3"},
        {0x0020, "BACK"}, {0x0010, "START"},
        {0x0001, "UP"}, {0x0002, "DOWN"}, {0x0004, "LEFT"}, {0x0008, "RIGHT"},
    };
    std::string out;
    for (auto &p : order)
        if (mask & p.first) { if (!out.empty()) out += '+'; out += p.second; }
    return out;
}
