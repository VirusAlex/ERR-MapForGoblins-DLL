#pragma once
#include <unordered_map>
#include "goblin_map_flags.hpp"
using namespace goblin::flag;
static std::unordered_map<int, int> ExceptionList = {
	{31443400, WestLimgrave}, // Bridge of Sacrifice - Tree Sentinel
	{35505701, Snowfields}, // Snowfield - Putrid Avatar
	{35505600, Snowfields}, // Snowfield - Great Wyrm Theodorix
	{30120100, LakeOfRot}, // Lake of Rot - Baleful Shadow
	{30120101, LakeOfRot}, // Lake of Rot - Dragonkin
	{30120103, LakeOfRot}, // Lake of Rot - Tree Spirit
	{38474500, GravesitePlain}, // Black Knight Garrew
	{38494100, SouthernShore}, // Jagged Peak Drake
	{38444500, StoryCharmBroken}, // Romina, Saint of the Bud
	{14247, Altus}, // Greatshield talisman
	{65535600, 62525}, // Vyke's being weird again
};