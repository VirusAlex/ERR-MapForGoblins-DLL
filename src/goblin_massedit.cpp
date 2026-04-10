#include "goblin_massedit.hpp"

#include <fstream>
#include <map>
#include <spdlog/spdlog.h>
#include <string>
#include <cstring>

namespace goblin::massedit
{

// Format: "param WorldMapPointParam: id XXXXX: fieldName: = value;"
static bool parse_line(const std::string &line, int32_t &row_id, std::string &field, std::string &value)
{
    if (line.size() < 30 || line.substr(0, 5) != "param")
        return false;

    auto id_pos = line.find(": id ");
    if (id_pos == std::string::npos)
        return false;
    id_pos += 5;

    auto id_end = line.find(':', id_pos);
    if (id_end == std::string::npos)
        return false;

    row_id = std::stoi(line.substr(id_pos, id_end - id_pos));

    auto field_start = id_end + 2;
    auto field_end = line.find(':', field_start);
    if (field_end == std::string::npos)
        return false;

    field = line.substr(field_start, field_end - field_start);

    auto eq_pos = line.find("= ", field_end);
    if (eq_pos == std::string::npos)
        return false;
    eq_pos += 2;

    auto val_end = line.find(';', eq_pos);
    if (val_end == std::string::npos)
        val_end = line.size();

    value = line.substr(eq_pos, val_end - eq_pos);

    return true;
}

static void apply_field(from::paramdef::WORLD_MAP_POINT_PARAM_ST &data, const std::string &field, const std::string &value)
{
    auto to_int = [&]() -> int { return std::stoi(value); };
    auto to_uint = [&]() -> unsigned int { return static_cast<unsigned int>(std::stoul(value)); };
    auto to_float = [&]() -> float { return std::stof(value); };
    auto to_u8 = [&]() -> uint8_t { return static_cast<uint8_t>(std::stoi(value)); };
    auto to_u16 = [&]() -> uint16_t { return static_cast<uint16_t>(std::stoi(value)); };

    if (field == "iconId") data.iconId = to_u16();
    else if (field == "dispMask00") data.dispMask00 = to_int() != 0;
    else if (field == "dispMask01") data.dispMask01 = to_int() != 0;
    else if (field == "dispMinZoomStep") data.dispMinZoomStep = to_u8();
    else if (field == "areaNo") data.areaNo = to_u8();
    else if (field == "gridXNo") data.gridXNo = to_u8();
    else if (field == "gridZNo") data.gridZNo = to_u8();
    else if (field == "posX") data.posX = to_float();
    else if (field == "posY") data.posY = to_float();
    else if (field == "posZ") data.posZ = to_float();
    else if (field == "textId1") data.textId1 = to_int();
    else if (field == "textId2") data.textId2 = to_int();
    else if (field == "textId3") data.textId3 = to_int();
    else if (field == "textId4") data.textId4 = to_int();
    else if (field == "textId5") data.textId5 = to_int();
    else if (field == "textId6") data.textId6 = to_int();
    else if (field == "textId7") data.textId7 = to_int();
    else if (field == "textId8") data.textId8 = to_int();
    else if (field == "textEnableFlagId1") data.textEnableFlagId1 = to_uint();
    else if (field == "textEnableFlagId2") data.textEnableFlagId2 = to_uint();
    else if (field == "textEnableFlagId4") data.textEnableFlagId4 = to_uint();
    else if (field == "textEnableFlagId5") data.textEnableFlagId5 = to_uint();
    else if (field == "textDisableFlagId1") data.textDisableFlagId1 = to_uint();
    else if (field == "textDisableFlagId2") data.textDisableFlagId2 = to_uint();
    else if (field == "textDisableFlagId3") data.textDisableFlagId3 = to_uint();
    else if (field == "textDisableFlagId4") data.textDisableFlagId4 = to_uint();
    else if (field == "textDisableFlagId5") data.textDisableFlagId5 = to_uint();
    else if (field == "textDisableFlagId6") data.textDisableFlagId6 = to_uint();
    else if (field == "textDisableFlagId7") data.textDisableFlagId7 = to_uint();
    else if (field == "textDisableFlagId8") data.textDisableFlagId8 = to_uint();
    else if (field == "selectMinZoomStep") data.selectMinZoomStep = to_u8();
    else if (field == "eventFlagId") data.eventFlagId = to_uint();
    else if (field == "textType2") data.textType2 = to_u8();
    else if (field == "textType3") data.textType3 = to_u8();
    // pad2_0 = 6-bit field at bits 2-7 of byte 0x18 = dispMask02 in our struct
    else if (field == "pad2_0") data.dispMask02 = to_int() != 0;
    // unkC0-unkDC = textEnableFlag2Id1-8
    else if (field == "unkC0") data.textEnableFlag2Id1 = to_int();
    else if (field == "unkC4") data.textEnableFlag2Id2 = to_int();
    else if (field == "unkC8") data.textEnableFlag2Id3 = to_int();
    else if (field == "unkCC") data.textEnableFlag2Id4 = to_int();
    else if (field == "unkD0") data.textEnableFlag2Id5 = to_int();
    else if (field == "unkD4") data.textEnableFlag2Id6 = to_int();
    else if (field == "unkD8") data.textEnableFlag2Id7 = to_int();
    else if (field == "unkDC") data.textEnableFlag2Id8 = to_int();
    // Skip: Name, pad4
}

static std::vector<MapEntry> load_file(const std::filesystem::path &path)
{
    std::map<int32_t, from::paramdef::WORLD_MAP_POINT_PARAM_ST> entries;

    std::ifstream file(path);
    if (!file.is_open())
        return {};

    std::string line;
    while (std::getline(file, line))
    {
        int32_t row_id;
        std::string field, value;
        if (parse_line(line, row_id, field, value))
        {
                if (entries.find(row_id) == entries.end())
                entries[row_id] = {};
            apply_field(entries[row_id], field, value);
        }
    }

    std::vector<MapEntry> result;
    result.reserve(entries.size());
    for (auto &[id, data] : entries)
        result.push_back({id, data});

    return result;
}

std::vector<MapEntry> load_from_directory(const std::filesystem::path &dir)
{
    std::vector<MapEntry> all;

    if (!std::filesystem::exists(dir))
    {
        spdlog::warn("MASSEDIT directory not found: {}", dir.string());
        return all;
    }

    int file_count = 0;
    for (auto &entry : std::filesystem::directory_iterator(dir))
    {
        if (!entry.is_regular_file())
            continue;

        auto ext = entry.path().extension().string();
        for (auto &c : ext) c = (char)tolower(c);
        if (ext != ".massedit")
            continue;

        auto entries = load_file(entry.path());
        if (!entries.empty())
        {
            spdlog::info("  {} : {} entries", entry.path().filename().string(), entries.size());
            all.insert(all.end(), entries.begin(), entries.end());
            file_count++;
        }
    }

    spdlog::info("Loaded {} entries from {} MASSEDIT files", all.size(), file_count);
    return all;
}

} // namespace goblin::massedit
