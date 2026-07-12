#pragma once

#include <string>
#include <string_view>

namespace goblin::i18n
{
    enum class Language
    {
        English,
        SimplifiedChinese,
        TraditionalChinese,
        Korean,
        Russian,
        German,
        French,
        Spanish,
    };

    enum class TextId
    {
        IniHeader,
        AllOn,
        AllOff,
        RandomizerHint,
        FastMapOpenWarning,
        IniOnly,
        PressAKey,
        PressComboRelease,
        ReopenMapWarning,
        AllIconCategories,
        ShowAll,
        HideAll,
        DebugDumpDescription,
        DumpMarkersNow,
        DumpBeacons,
        DumpStamps,
        Copy,
        Chars,
        NoDumpYet,
        InjectStatusTitle,
        InjectStatusHint,
        CopyStatus,
        AboutDescription,
        Version,
        LinkNexus,
        LinkGithub,
        LinkDiscord,
        ControlHintGamepad,
        ControlHintKeyboard,
        WindowTitle,
        MasterToggle,
        MasterToggleTooltip,
        Close,
        TabSettings,
        TabDebug,
        TabAbout,
        TabProgress,
        ProgressHint,
        ProgressClickHint,
        ProgressShowingOnly,
        ProgressFocusClear,
        HiddenMarkers,
        HiddenMarkersNone,
        Unhide,
        UnhideAll,
        HoverLevel,
        HoverAbove,
        HoverBelow,
    };

    enum class ToastId
    {
        MapIconsOn,
        MapIconsOff,
        MarkersDumped,
        MarkerDumpFailed,
    };

    Language language_from_steam(std::string_view steam_language);
    Language language_from_config(std::string_view config_value);
    Language current_language();

    // Raw Steam game-language token (e.g. "russian"), or "" if Steam is unavailable.
    std::string steam_game_language();

    std::string normalize_language_config(std::string_view config_value);
    const char *language_code(Language language);
    const char *language_option_label(std::string_view config_value,
                                      Language language = current_language());
    const char *language_preview_label(std::string_view config_value,
                                       Language language = current_language());

    const char *tr(TextId id, Language language = current_language());
    const wchar_t *wtr(ToastId id, Language language = current_language());

    const char *section_label(const char *section_name,
                              Language language = current_language());
    const char *section_comment(const char *section_name,
                                const char *fallback,
                                Language language = current_language());
    const char *entry_label(const char *entry_key,
                            Language language = current_language());
    const char *entry_comment(const char *entry_key,
                              const char *fallback,
                              Language language = current_language());

    // Glyph seed used by the ImGui font atlas. Keep this in sync with the UI
    // resource table so we don't need to bake the entire CJK range.
    const char *font_glyph_seed_utf8();
}
