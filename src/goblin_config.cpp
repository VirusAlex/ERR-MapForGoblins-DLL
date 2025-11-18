#include "goblin_config.hpp"
#include <spdlog/spdlog.h>

uint8_t goblin::config::loadDelay = 10;
bool goblin::config::requireMapFragments = true;
bool goblin::config::showOverworldBossIcons = true;
bool goblin::config::showDungeonBossIcons = true;
bool goblin::config::showCampIcons = true;
bool goblin::config::showMerchantIcons = true;
bool goblin::config::redifyBossIcons = false;

using namespace goblin::config;

void goblin::load_config(const std::filesystem::path& ini_path)
{
    spdlog::info("Loading config from {}", ini_path.string());

    mINI::INIFile file(ini_path.string());
    mINI::INIStructure ini;
    if (file.read(ini) && ini.has("Goblin"))
    {
        auto& config = ini["Goblin"];

        load_line(config, "load_delay", loadDelay);
        load_line(config, "require_map_fragments", requireMapFragments);
        load_line(config, "show_overworld_boss_icons", showOverworldBossIcons);
        load_line(config, "show_dungeon_boss_icons", showDungeonBossIcons);
        load_line(config, "show_camp_icons", showCampIcons);
        load_line(config, "show_merchant_icons", showMerchantIcons);
        load_line(config, "redify_boss_icons", redifyBossIcons);
    }
}

void goblin::load_line(mINI::INIMap<std::string> config, std::string lineInIni, bool& boolVariable) {
    if (config.has(lineInIni)) {
        boolVariable = config[lineInIni] != "false";
        spdlog::info("Loaded: " + lineInIni);
        spdlog::info("Loaded value = {}", boolVariable);
    }
    else {
        spdlog::info("Failed to load: " + lineInIni);
    }
}

void goblin::load_line(mINI::INIMap<std::string> config, std::string lineInIni, uint8_t& intVariable) {
    if (config.has(lineInIni)) {
        int val = std::stoi(config[lineInIni]);
        if (val < 0 || val > 255) {
            val = 10; // default to 10 if incorrect
        }
        intVariable = static_cast<uint8_t>(val);
        spdlog::info("Loaded: " + lineInIni);
        spdlog::info("Loaded value = {}", intVariable);
    }
    else {
        spdlog::info("Failed to load: " + lineInIni);
    }
}