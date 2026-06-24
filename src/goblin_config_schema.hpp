#pragma once

// Single source of truth for the MapForGoblins.ini layout: every section,
// key, default value, comment, ERR-only flag and rename history lives here.
// Used to (a) GENERATE the shipped ini at build time (tools/mfg_inigen), and
// (b) by the DLL at runtime to create the ini if missing and to migrate an
// existing one (add new keys, apply renames, comment out obsolete keys).
//
// This translation unit must stay dependency-free (no spdlog/mINI) so the
// tiny inigen generator can link it without the rest of the mod.

#include "goblin_i18n.hpp"

#include <cstdint>
#include <functional>
#include <ostream>
#include <string>
#include <vector>

namespace goblin
{
    enum class IniType : uint8_t
    {
        Bool,        // -> bool*
        U8,          // -> uint8_t*
        VkKey,       // -> uint32_t*  (parsed via parse_vk_code)
        GamepadMask, // -> uint16_t*  (parsed via parse_gamepad_combo)
        String,      // -> std::string*
    };

    struct IniEntry
    {
        const char *key;
        IniType type;
        void *target;            // address of the goblin::config:: variable
        const char *def;         // default, exactly as written in the ini
        const char *comment;     // may contain '\n' for multi-line; nullptr = none
        bool err_only;           // omitted + disabled in the vanilla build
        const char *rename_from; // previous key name in this section, or nullptr
    };

    struct IniSection
    {
        const char *name;
        const char *comment; // section header comment block (may be '\n'-split), or nullptr
        bool err_only;       // whole section omitted + disabled in vanilla
        std::vector<IniEntry> entries;
    };

    // The schema, built once.
    const std::vector<IniSection> &ini_schema();

    // Compile-time profile. The vanilla DLL is built with -DMFG_VANILLA.
    constexpr bool profile_is_vanilla()
    {
#ifdef MFG_VANILLA
        return true;
#else
        return false;
#endif
    }

    // Resolver lets the caller supply an existing value for an entry (used by
    // the runtime migration to preserve user-set values + apply renames).
    // Return true and fill `value` to override the default; return false to
    // use the entry's default.
    using IniValueResolver =
        std::function<bool(const std::string &section, const IniEntry &e, std::string &value)>;

    // Emit a complete ini. ERR-only sections/entries are written only when
    // include_err_only is true. If `resolve` is set it supplies per-entry
    // values (else the schema default is used).
    void emit_ini(std::ostream &out, bool include_err_only, const IniValueResolver &resolve = {},
                  i18n::Language language = i18n::Language::English);
}
