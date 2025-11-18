#pragma once
#include "goblin_config.hpp"
#include "goblin_map_lookup.hpp"

static int GetIconFlag(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST& row);
static void SetupOverworldERR(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST& row);
static void SetupCampsERR(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST& row);
static void SetupDungeonERR(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST& row);
static void SetupMerchants(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST& row);
static void HideOnCompletion(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST& row);
static void Debug(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST& row);


static int GetMapFragment(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST& row);
static void SetSecondaryFlags(from::paramdef::WORLD_MAP_POINT_PARAM_ST& row, int flagId);

namespace goblin
{
	void Initialise();
	namespace iconIds
	{
		static constexpr ParamRange goblinIcons(1, 78500);
		static constexpr ParamRange goblinIconsERR(1000000, 10025000);
		static constexpr ParamRange reforgedIconsBosses(30000000, 40000000);
		static constexpr ParamRange reforgedIconsCamps(50000000, 60000000);
	};
};