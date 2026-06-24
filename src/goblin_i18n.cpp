// i18n LOGIC. The string DATA lives in per-locale JSON bundles (i18n/*.json),
// baked into src/generated_shared/goblin_i18n_strings.cpp by tools/generate_i18n.py
// (run from build.bat's gen_shared step, which also validates that every key is
// present in every locale). To add/translate strings, edit the JSON - never C++.
#include "goblin_i18n.hpp"
#include "goblin_i18n_bundle.hpp"
#include "goblin_config.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace
{
    using goblin::i18n::Language;
    using goblin::i18n::TextId;
    using goblin::i18n::ToastId;
    using goblin::i18n::detail::bundle;
    using goblin::i18n::detail::LocaleBundle;
    using goblin::i18n::detail::NameKV;

    std::string lower(std::string_view s)
    {
        std::string out(s);
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    }

    const char *pick(Language language, const char *en, const char *sc, const char *tc)
    {
        switch (language)
        {
        case Language::SimplifiedChinese: return (sc && sc[0]) ? sc : en;
        case Language::TraditionalChinese: return (tc && tc[0]) ? tc : en;
        default: return en;
        }
    }

    const char *find_text(const LocaleBundle &b, TextId id)
    {
        for (size_t i = 0; i < b.n_texts; ++i)
            if (b.texts[i].id == id) return b.texts[i].s;
        return nullptr;
    }
    const wchar_t *find_toast(const LocaleBundle &b, ToastId id)
    {
        for (size_t i = 0; i < b.n_toasts; ++i)
            if (b.toasts[i].id == id) return b.toasts[i].s;
        return nullptr;
    }
    const char *find_name(const NameKV *rows, size_t n, const char *key)
    {
        for (size_t i = 0; i < n; ++i)
            if (std::strcmp(rows[i].key, key) == 0) return rows[i].val;
        return nullptr;
    }

    std::string detect_steam_language()
    {
        HMODULE steam = GetModuleHandleA("steam_api64.dll");
        if (!steam) return "";
        typedef void *(*SteamApps_fn)();
        typedef const char *(*GetLang_fn)(void *);
        auto steamApps = reinterpret_cast<SteamApps_fn>(GetProcAddress(steam, "SteamAPI_SteamApps_v008"));
        auto getLang = reinterpret_cast<GetLang_fn>(GetProcAddress(steam, "SteamAPI_ISteamApps_GetCurrentGameLanguage"));
        if (!steamApps || !getLang) return "";
        void *apps = steamApps();
        if (!apps) return "";
        const char *lang = getLang(apps);
        return (lang && lang[0]) ? lang : "";
    }

    Language cached_auto_language()
    {
        static std::mutex lock;
        static bool cached = false;
        static Language language = Language::English;

        std::lock_guard<std::mutex> guard(lock);
        if (cached)
            return language;

        std::string steam_language = detect_steam_language();
        if (steam_language.empty())
            return Language::English;

        language = goblin::i18n::language_from_steam(steam_language);
        cached = true;
        return language;
    }
}

Language goblin::i18n::language_from_steam(std::string_view steam_language)
{
    std::string s = lower(steam_language);
    if (s == "schinese" || s == "zhocn" || s == "zh-cn" || s == "zh_hans")
        return Language::SimplifiedChinese;
    if (s == "tchinese" || s == "zhotw" || s == "zh-tw" || s == "zh_hant")
        return Language::TraditionalChinese;
    return Language::English;
}

std::string goblin::i18n::normalize_language_config(std::string_view config_value)
{
    std::string s = lower(config_value);
    s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) { return c == ' ' || c == '\t'; }), s.end());
    if (s.empty() || s == "auto") return "auto";
    if (s == "english" || s == "en" || s == "engus") return "english";
    if (s == "schinese" || s == "simplified" || s == "simplified_chinese" ||
        s == "zhocn" || s == "zh-cn" || s == "zh_hans") return "schinese";
    if (s == "tchinese" || s == "traditional" || s == "traditional_chinese" ||
        s == "zhotw" || s == "zh-tw" || s == "zh_hant") return "tchinese";
    return "english";
}

Language goblin::i18n::language_from_config(std::string_view config_value)
{
    std::string s = normalize_language_config(config_value);
    if (s == "schinese") return Language::SimplifiedChinese;
    if (s == "tchinese") return Language::TraditionalChinese;
    if (s == "auto") return cached_auto_language();
    return Language::English;
}

Language goblin::i18n::current_language()
{
    return language_from_config(goblin::config::uiLanguage);
}

const char *goblin::i18n::language_code(Language language)
{
    switch (language)
    {
    case Language::SimplifiedChinese: return "schinese";
    case Language::TraditionalChinese: return "tchinese";
    default: return "english";
    }
}

const char *goblin::i18n::language_option_label(std::string_view config_value, Language language)
{
    std::string s = normalize_language_config(config_value);
    if (s == "auto")
        return pick(language, "Auto", "自动", "自動");
    if (s == "schinese")
        return pick(language, "Simplified Chinese", "简体中文", "簡體中文");
    if (s == "tchinese")
        return pick(language, "Traditional Chinese", "繁體中文", "繁體中文");
    return "English";
}

const char *goblin::i18n::language_preview_label(std::string_view config_value, Language language)
{
    std::string s = normalize_language_config(config_value);
    if (s != "auto")
        return language_option_label(s, language);
    switch (language_from_config(config_value))
    {
    case Language::SimplifiedChinese:
        return pick(language, "Auto: Simplified Chinese", "自动：简体中文", "自動：簡體中文");
    case Language::TraditionalChinese:
        return pick(language, "Auto: Traditional Chinese", "自动：繁體中文", "自動：繁體中文");
    default:
        return pick(language, "Auto: English", "自动：English", "自動：English");
    }
}

const char *goblin::i18n::tr(TextId id, Language language)
{
    const char *s = find_text(bundle(language), id);
    if (s && s[0]) return s;
    s = find_text(bundle(Language::English), id);
    return s ? s : "";
}

const wchar_t *goblin::i18n::wtr(ToastId id, Language language)
{
    const wchar_t *s = find_toast(bundle(language), id);
    if (s && s[0]) return s;
    s = find_toast(bundle(Language::English), id);
    return s ? s : L"";
}

const char *goblin::i18n::section_label(const char *section_name, Language language)
{
    const LocaleBundle &b = bundle(language);
    const char *v = find_name(b.sect_labels, b.n_sect_labels, section_name);
    return v ? v : section_name;
}

const char *goblin::i18n::section_comment(const char *section_name, const char *fallback, Language language)
{
    const LocaleBundle &b = bundle(language);
    const char *v = find_name(b.sect_comments, b.n_sect_comments, section_name);
    return v ? v : fallback;
}

const char *goblin::i18n::entry_label(const char *entry_key, Language language)
{
    const LocaleBundle &b = bundle(language);
    const char *v = find_name(b.entry_labels, b.n_entry_labels, entry_key);
    return v ? v : entry_key;
}

const char *goblin::i18n::entry_comment(const char *entry_key, const char *fallback, Language language)
{
    const LocaleBundle &b = bundle(language);
    const char *v = find_name(b.entry_comments, b.n_entry_comments, entry_key);
    return v ? v : fallback;
}

const char *goblin::i18n::font_glyph_seed_utf8()
{
    static std::string seed;
    if (!seed.empty())
        return seed.c_str();
    auto add = [](const char *t) { if (t && t[0]) seed += t; };
    // Language-dropdown labels live inline in language_option_label/preview_label,
    // not in the bundles, so seed their glyphs explicitly.
    add("自动自動简体中文繁體中文");
    for (Language lang : {Language::SimplifiedChinese, Language::TraditionalChinese})
    {
        const LocaleBundle &b = bundle(lang);
        for (size_t i = 0; i < b.n_texts; ++i) add(b.texts[i].s);
        for (size_t i = 0; i < b.n_sect_labels; ++i) add(b.sect_labels[i].val);
        for (size_t i = 0; i < b.n_sect_comments; ++i) add(b.sect_comments[i].val);
        for (size_t i = 0; i < b.n_entry_labels; ++i) add(b.entry_labels[i].val);
        for (size_t i = 0; i < b.n_entry_comments; ++i) add(b.entry_comments[i].val);
    }
    return seed.c_str();
}
