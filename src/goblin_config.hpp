#pragma once

#include <filesystem>
#include <mini/ini.h>

namespace goblin
{
	/**
	 * Load user preferences from an .ini file
	 */
	void load_line(mINI::INIMap<std::string> config, std::string lineInIni, bool &lineVariable);
	void load_line(mINI::INIMap<std::string> config, std::string lineInIni, uint8_t& floatVariable);
	void load_config(const std::filesystem::path& ini_path);

	namespace config
	{
		extern uint8_t loadDelay;
		extern bool requireMapFragments;
		extern bool showOverworldBossIcons;
		extern bool showDungeonBossIcons;
		extern bool showCampIcons;
		extern bool showMerchantIcons;
		extern bool redifyBossIcons;
	};
};