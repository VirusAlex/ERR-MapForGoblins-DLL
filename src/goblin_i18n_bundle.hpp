#pragma once
// Internal bundle layout shared between the i18n LOGIC (goblin_i18n.cpp) and the
// GENERATED string data (src/generated_shared/goblin_i18n_strings.cpp, produced
// from i18n/*.json by tools/generate_i18n.py). Not part of the public API.
#include "goblin_i18n.hpp"

#include <cstddef>

namespace goblin::i18n::detail
{
    struct TextKV  { TextId id; const char *s; };
    struct ToastKV { ToastId id; const wchar_t *s; };
    struct NameKV  { const char *key; const char *val; };

    // One self-contained bundle per locale. English is the base: its name tables
    // are null (labels fall back to the key, comments to the caller's fallback),
    // so only the overlay texts/toasts are carried for EN.
    struct LocaleBundle
    {
        const TextKV *texts;          size_t n_texts;
        const ToastKV *toasts;        size_t n_toasts;
        const NameKV *sect_labels;    size_t n_sect_labels;
        const NameKV *sect_comments;  size_t n_sect_comments;
        const NameKV *entry_labels;   size_t n_entry_labels;
        const NameKV *entry_comments; size_t n_entry_comments;
    };

    // Defined in the generated translation unit.
    const LocaleBundle &bundle(Language language);
}
