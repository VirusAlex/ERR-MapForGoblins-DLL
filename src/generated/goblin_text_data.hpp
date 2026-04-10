#pragma once

#include <cstddef>
#include <cstdint>

namespace goblin::generated
{

struct TextEntry
{
    int32_t id;
    const wchar_t *text;
};

struct LangData
{
    const char *steam_name;
    const TextEntry *entries;
    size_t count;
};

extern const size_t LANG_COUNT;
extern const LangData LANG_TABLE[];

} // namespace goblin::generated
