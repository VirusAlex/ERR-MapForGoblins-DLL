#pragma once
#include "goblin_structs.hpp"
#include "goblin_map_flags.hpp"

using namespace goblin::mapPoint;
static std::vector<MapFragments> MapList{
MapFragments(goblin::flag::AlwaysOff,
	{
	MapTile(0), // Blank

}),
MapFragments(goblin::flag::WestLimgrave,
	{
	MapTile(10), // Stormveil
	MapTile(10, 1), // Chapel
	MapTile(30, 11), // Deathtouched Catacombs
	MapTile(31, 17), // Highroad Cave
	MapTile(30,4), // Murkwater Catacombs
	MapTile(31), // Murkwater Cave
	MapTile(32,1), // Limgrave Tunnels
	MapTile(31,3), // Groveside Cave
	MapTile(31,15), // Coastal Cave
	MapTile(30,2), // Stormfoot Catacombs
	MapTile(18), // Stranded Graveyard
	MapTile(60, 40, 39), // 20_19 Stormveil upper left corner
	MapTile(60, 40, 38), // 20_19 Stormveil lower left corner
	MapTile(60, 41, 39), // 20_19 Stormveil upper right corner
	MapTile(60, 41, 38), // 20_19 Stormveil lower right corner
	MapTile(60, 41, 37), // 20_18 Stormveil beach upper right corner
	MapTile(60, 41, 36), // 20_18 Stormveil beach lower right corner
	MapTile(60, 42, 40), // 21_20 Limgrave Colosseum lower left corner
	MapTile(60, 42, 39), // 21_19 Warmaster upper left corner
	MapTile(60, 42, 38), // 21_19 Warmaster lower left corner
	MapTile(60, 42, 37), // 21_18 Agheel Lake upper left corner
	MapTile(60, 42, 36), // 21_18 Agheel Lake lower left corner
	MapTile(60, 42, 35), // 21_17 Gravitas upper left corner
	MapTile(60, 43, 40), // 21_20 Limgrave Colosseum lower right corner
	MapTile(60, 43, 39), // 21_19 Warmaster upper right corner
	MapTile(60, 43, 38), // 21_19 Warmaster lower right corner
	MapTile(60, 43, 37), // 21_18 Agheel Lake upper right corner
	MapTile(60, 43, 36), // 21_18 Agheel Lake lower right corner
	MapTile(60, 43, 35), // 21_17 Gravitas upper right corner
	MapTile(60, 44, 40), // 22_20 Divine Tower lower left corner
	MapTile(60, 44, 37), // 22_18 Witchbane Ruins upper left corner
	MapTile(60, 44, 36), // 22_18 Witchbane Ruins lower left corner
	MapTile(60, 44, 35), // 22_17 Bridge of Sacrifice upper left corner
	}),
MapFragments(goblin::flag::WeepingPeninsula,
	{
	MapTile(32), // Morne Tunnel
	MapTile(30), // Tombswards Catacombs
	MapTile(31,1), // Earthbore Cave
	MapTile(30,1), // Impaler's Catacombs
	MapTile(60, 40, 33), // 20_16 Isolated Merchant upper left corner
	MapTile(60, 40, 32), // 20_16 Isolated Merchant lower left corner
	MapTile(60, 41, 33), // 20_16 Isolated Merchant upper right corner
	MapTile(60, 41, 32), // 20_16 Isolated Merchant lower right corner
	MapTile(60, 42, 34), // 21_17 Gravitas lower left corner
	MapTile(60, 42, 33), // 21_16 Weeping Erdtree upper left corner
	MapTile(60, 42, 32), // 21_16 Weeping Erdtree lower left corner
	MapTile(60, 43, 34), // 21_17 Gravitas lower right corner
	MapTile(60, 43, 33), // 21_16 Weeping Erdtree upper right corner
	MapTile(60, 43, 32), // 21_16 Weeping Erdtree lower right corner
	MapTile(60, 43, 31), // 21_15 Castle Morne upper right corner
	MapTile(60, 43, 30), // 21_15 Castle Morne lower right corner
	MapTile(60, 44, 34), // 21_17 Bridge of Sacrifice lower left corner
	MapTile(60, 44, 33), // 22_16 Morne Rampart upper left corner
	MapTile(60, 44, 32), // 22_16 Morne Rampart lower left corner
	MapTile(60, 44, 31), // 22_16 Morne Swamp upper left corner
	MapTile(60, 45, 34), // 21_17 Bridge of Sacrifice lower right corner
	MapTile(60, 45, 33), // 22_16 Morne Rampart upper right corner
	MapTile(60, 45, 32), // 22_16 Morne Rampart lower right corner
	MapTile(60, 45, 31), // 22_16 Morne Swamp upper right corner
	}),
MapFragments(goblin::flag::EastLimgrave,
	{
	MapTile(30,14), // Minor Erdtree Catacombs
	MapTile(32,7), // Gael Tunnel
	MapTile(31,21), // Gaol Cave
	MapTile(60, 44, 39), // fok of
	MapTile(60, 44, 38),
	MapTile(60, 45, 40),
	MapTile(60, 45, 39),
	MapTile(60, 45, 38),
	MapTile(60, 45, 37),
	MapTile(60, 45, 36),
	MapTile(60, 45, 35),
	MapTile(60, 46, 41),
	MapTile(60, 46, 40),
	MapTile(60, 46, 39),
	MapTile(60, 46, 38),
	MapTile(60, 46, 37),
	MapTile(60, 46, 36),
	MapTile(60, 47, 40),
	MapTile(60, 47, 39),
	MapTile(60, 47, 38),
	MapTile(60, 47, 37),
	MapTile(60, 48, 40),
	MapTile(60, 48, 39),
	}),
MapFragments(goblin::flag::Caelid,
	{
	MapTile(34,13), // Divine tower of Caelid
	MapTile(30,15), // Caelid Catacombs
	MapTile(30,16), // War-Dead Catacombs
	MapTile(31,20), // Abandoned Cave
	MapTile(32,8), // Sellia Crystal Tunnel
	MapTile(31,11), // Sellia Hideaway
	MapTile(60, 48, 39),	// 24_19 Aeonia Lake upper left corner
	MapTile(60, 48, 38),	// 24_19 Aeonia Lake lower left corner
	MapTile(60, 48, 37),	// 24_18 Ekzykes crossroads upper left corner
	MapTile(60, 48, 36),	// 24_18 Ekzykes crossroads lower left corner
	MapTile(60, 49, 39),	// 24_19 Aeonia Lake upper right corner
	MapTile(60, 49, 38),	// 24_19 Aeonia Lake lower right corner
	MapTile(60, 49, 37),	// 24_18 Ekzykes crossroads upper right corner
	MapTile(60, 49, 36),	// 24_18 Ekzykes crossroads lower right corner
	MapTile(60, 50, 39),	// 25_19 Church of the Plague upper left corner
	MapTile(60, 50, 38),	// 25_19 Church of the Plague lower left corner
	MapTile(60, 50, 37),	// 25_18 Redmane Castle upper left corner
	MapTile(60, 50, 36),	// 25_18 Redmane Castle lower left corner
	MapTile(60, 51, 39),	// 25_19 Church of the Plague upper right corner
	MapTile(60, 51, 38),	// 25_19 Church of the Plague lower right corner
	MapTile(60, 51, 37),	// 25_18 Redmane Castle upper right corner
	MapTile(60, 51, 36),	// 25_18 Redmane Castle lower right corner
	MapTile(60, 52, 39),	// 26_19 Redmane Beach upper left corner
	MapTile(60, 52, 38),	// 26_19 Redmane Beach lower left corner
	MapTile(60, 52, 37),	// 26_19 Redmane Beach lower upper left corner
	MapTile(60, 52, 36),	// 26_19 Redmane Beach lower lower left corner
	MapTile(60, 53, 39),	// 26_19 Redmane Beach upper right corner
	MapTile(60, 53, 38),	// 26_19 Redmane Beach lower right corner
	MapTile(60, 53, 37)	// 26_19 Redmane Beach lower upper right corner
	}),
MapFragments(goblin::flag::Dragonbarrow,
	{
	MapTile(60, 47, 42),	// 23_21 Caelid Colosseum upper left corner
	MapTile(60, 47, 41),	// 23_20 Caelid Colosseum path
	MapTile(60, 48, 41),	// 24_20 Caelid Merchant
	MapTile(60, 49, 41),	// 24_20 Caelid Divine Tower north
	MapTile(60, 49, 40),	// 24_20 Caelid Divine Tower crossing
	MapTile(60, 50, 41),
	MapTile(60, 50, 40),
	MapTile(60, 51, 43),
	MapTile(60, 51, 42),
	MapTile(60, 51, 41),
	MapTile(60, 51, 40),
	MapTile(60, 52, 43),
	MapTile(60, 52, 42),
	MapTile(60, 52, 41),
	MapTile(60, 52, 40),
	MapTile(60, 53, 43),
	MapTile(60, 53, 42),
	MapTile(60, 53, 41),
	MapTile(60, 53, 40),
	}),
MapFragments(goblin::flag::EastLiurnia,
	{
	MapTile(31, 4), // Stillwater Cave
	MapTile(30, 5), // Black Knife Catacombs
	MapTile(31, 5), // Lakeside Crystal Cave
	MapTile(30, 6), // Cliffbottom Catacombs
	MapTile(34, 11), // Divine Tower of Liurnia
	MapTile(39, 20), // Ruin-Strewn Precipice
	MapTile(60, 38, 50),
	MapTile(60, 37, 49),
	MapTile(60, 38, 49),
	MapTile(60, 39, 49),
	MapTile(60, 36, 48),
	MapTile(60, 37, 48),
	MapTile(60, 38, 48),
	MapTile(60, 39, 48),
	MapTile(60, 37, 47),
	MapTile(60, 38, 47),
	MapTile(60, 39, 47),
	MapTile(60, 37, 46),
	MapTile(60, 38, 46),
	MapTile(60, 39, 46),
	MapTile(60, 38, 45),
	MapTile(60, 39, 45),
	MapTile(60, 40, 45),
	MapTile(60, 41, 45),
	MapTile(60, 38, 44),
	MapTile(60, 39, 44),
	MapTile(60, 40, 44),
	MapTile(60, 41, 44),
	MapTile(60, 38, 43),
	MapTile(60, 39, 43),
	MapTile(60, 40, 43),
	MapTile(60, 38, 42),
	MapTile(60, 39, 42),
	MapTile(60, 40, 42),
	MapTile(60, 41, 42),
	MapTile(60, 36, 41),
	MapTile(60, 37, 41),
	MapTile(60, 38, 41),
	MapTile(60, 39, 41),
	MapTile(60, 40, 41),
	MapTile(60, 41, 41),
	MapTile(60, 36, 40),
	MapTile(60, 37, 40),
	MapTile(60, 38, 40),
	MapTile(60, 39, 40),
	MapTile(60, 40, 40),
	MapTile(60, 41, 40),
	}),
MapFragments(goblin::flag::NorthLiurnia,
	{
	MapTile(14), // Academy of Raya Lucaria
	MapTile(31, 6), // Academy Crystal Cave
	MapTile(60, 35, 48),
	MapTile(60, 35, 47),
	MapTile(60, 36, 47),
	MapTile(60, 34, 46),
	MapTile(60, 35, 46),
	MapTile(60, 36, 46),
	MapTile(60, 34, 45),
	MapTile(60, 35, 45),
	MapTile(60, 36, 45),
	MapTile(60, 37, 45),
	MapTile(60, 34, 44),
	MapTile(60, 35, 44),
	MapTile(60, 36, 44),
	MapTile(60, 37, 44),
	MapTile(60, 35, 43),
	MapTile(60, 36, 43),
	MapTile(60, 37, 43),
	MapTile(60, 36, 42),
	MapTile(60, 37, 42),
	}),
MapFragments(goblin::flag::WestLiurnia,
	{
	MapTile(30, 3), // Road's End Catacombs
	MapTile(60, 34, 51),
	MapTile(60, 35, 51),
	MapTile(60, 34, 50),
	MapTile(60, 35, 50),
	MapTile(60, 36, 50),
	MapTile(60, 37, 50),
	MapTile(60, 34, 49),
	MapTile(60, 35, 49),
	MapTile(60, 36, 49),
	MapTile(60, 33, 48),
	MapTile(60, 34, 48),
	MapTile(60, 33, 47),
	MapTile(60, 34, 47),
	MapTile(60, 33, 46),
	MapTile(60, 33, 45),
	MapTile(60, 33, 44),
	MapTile(60, 33, 43),
	MapTile(60, 34, 43),
	MapTile(60, 33, 42),
	MapTile(60, 34, 42),
	MapTile(60, 35, 42),
	MapTile(60, 33, 41),
	MapTile(60, 34, 41),
	MapTile(60, 35, 41),
	MapTile(60, 33, 40),
	MapTile(60, 34, 40),
	MapTile(60, 35, 40),
	}),
MapFragments(goblin::flag::Altus,
	{
	MapTile(30,8), // Sainted Hero's Grave
	MapTile(32,5), // Altus Tunnel
	MapTile(31,18), // Perfumer's Grotto
	MapTile(60, 40, 55),
	MapTile(60, 41, 55),
	MapTile(60, 42, 55),
	MapTile(60, 40, 54),
	MapTile(60, 41, 54),
	MapTile(60, 42, 54),
	MapTile(60, 43, 54),
	MapTile(60, 40, 53),
	MapTile(60, 41, 53),
	MapTile(60, 42, 53),
	MapTile(60, 39, 52),
	MapTile(60, 40, 52),
	MapTile(60, 41, 52),
	MapTile(60, 39, 51),
	MapTile(60, 40, 51),
	MapTile(60, 41, 51),
	MapTile(60, 39, 50),
	MapTile(60, 40, 50),
	MapTile(60, 41, 50),
	}),
MapFragments(goblin::flag::Leyndell,
	{
	MapTile(11), // Leyndell Capital
	MapTile(35), // Leyndell Catacombs
	MapTile(30,10), // Auriza Hero's Grave
	MapTile(30,13), // Auriza Side Tomb
	// Inner wall stuff
	MapTile(60, 43, 53),
	MapTile(60, 44, 53),
	MapTile(60, 45, 53),
	MapTile(60, 42, 52),
	MapTile(60, 43, 52),
	MapTile(60, 44, 52),
	MapTile(60, 45, 52),
	MapTile(60, 42, 51),
	MapTile(60, 43, 51),
	MapTile(60, 44, 51),
	MapTile(60, 45, 51),
	MapTile(60, 46, 51),
	MapTile(60, 42, 50),
	MapTile(60, 43, 50),
	MapTile(60, 43, 49),
	}),
MapFragments(goblin::flag::Gelmir,
	{
	MapTile(16), // Volcano Manor
	MapTile(32,4), // Old Altus Tunnel
	MapTile(30,7), // Wyndham Catacombs
	MapTile(31,7), // Seethewater Cave
	MapTile(30,9), // Gelmir Hero's Grave
	MapTile(31,9), // Volcano Cave
	MapTile(30,12), // Unsightly Catacombs
	MapTile(60, 34, 55), // 17_27 Seethewater upper left corner
	MapTile(60, 34, 54), // 17_27 Seethewater lower left corner
	MapTile(60, 34, 53), // 17_26 Magma Wyrm upper left corner
	MapTile(60, 34, 52), // 17_26 Magma Wyrm lower left corner
	MapTile(60, 35, 55), // 17_27 Seethewater upper right corner
	MapTile(60, 35, 54), // 17_27 Seethewater lower right corner
	MapTile(60, 35, 53), // 17_26 Magma Wyrm lower left corner
	MapTile(60, 35, 52), // 17_26 Magma Wyrm upper left corner
	MapTile(60, 36, 55), // 18_27 Volcano Cave upper left corner
	MapTile(60, 36, 54), // 18_27 Volcano Cave lower left corner
	MapTile(60, 36, 53), // 18_26 Volcano Manor upper left corner
	MapTile(60, 36, 52), // 18_26 Volcano Manor lower left corner
	MapTile(60, 36, 51), // 18_25 Abandonded coffin upper left corner
	MapTile(60, 37, 55), // 18_27 Volcano Cave upper right corner
	MapTile(60, 37, 54), // 18_27 Volcano Cave lower right corner
	MapTile(60, 37, 53), // 18_26 Volcano Manor upper right corner
	MapTile(60, 37, 52), // 18_26 Volcano Manor lower right corner
	MapTile(60, 37, 51), // 18_25 Abandonded coffin upper right corner
	MapTile(60, 38, 55), // 19_27 Shaded Castle upper left corner
	MapTile(60, 38, 54), // 19_27 Shaded Castle lower left corner
	MapTile(60, 38, 53), // 19_26 Bridge of Iniquity upper left corner
	MapTile(60, 38, 52), // 19_26 Bridge of Iniquity lower left corner
	MapTile(60, 38, 51), // 19_25 Erdtree Gazing Hill  upper left corner
	MapTile(60, 39, 55), // 19_27 Shaded Castle upper right corner
	MapTile(60, 39, 54), // 19_27 Shaded Castle lower right corner
	MapTile(60, 39, 53), // 19_26 Bridge of Iniquity upper right corner
	}),
MapFragments(goblin::flag::MountaintopsWest,
	{
	MapTile(34, 14), // Divine Tower of East Altus
	MapTile(30, 17), // Giant-Conquering Hero's Grave
	MapTile(30, 18), // Giants' Mountaintop Catacombs
	MapTile(60, 47, 51), // Path to Rold
	MapTile(60, 48, 51), // Path to Rold
	MapTile(60, 49, 51), // Path to Rold
	MapTile(60, 49, 52), // Path to Rold
	MapTile(60, 49, 53), // Rold
	MapTile(60, 50, 53),
	MapTile(60, 50, 54),
	MapTile(60, 51, 55),
	MapTile(60, 51, 56),
	MapTile(60, 52, 56),
	MapTile(60, 50, 57),
	MapTile(60, 51, 57),
	MapTile(60, 52, 57),
	MapTile(60, 51, 58),
	MapTile(60, 52, 58),
	}),
MapFragments(goblin::flag::MountaintopsEast,
	{
	MapTile(31, 22), // Spiritcaller Cave
	MapTile(60, 53, 58),
	MapTile(60, 53, 57),
	MapTile(60, 54, 57),
	MapTile(60, 53, 56),
	MapTile(60, 54, 56),
	MapTile(60, 52, 55),
	MapTile(60, 53, 55),
	MapTile(60, 54, 55),
	MapTile(60, 51, 54),
	MapTile(60, 52, 54),
	MapTile(60, 53, 54),
	MapTile(60, 51, 53),
	MapTile(60, 52, 53),
	MapTile(60, 53, 53),
	MapTile(60, 54, 53),
	MapTile(60, 51, 52),
	MapTile(60, 52, 52),
	MapTile(60, 53, 52),
	}),
MapFragments(goblin::flag::Snowfields,
	{
	MapTile(31, 12), // Cave of Forlorn
	MapTile(31, 11), // Yelough Anix Ruins
	MapTile(30, 10), // Consecrated Snowfield Catacombs
	MapTile(30, 20), // Hidden Path to the Haligtree
	MapTile(60, 47, 58),
	MapTile(60, 46, 57),
	MapTile(60, 47, 57),
	MapTile(60, 48, 57),
	MapTile(60, 49, 57),
	MapTile(60, 47, 56),
	MapTile(60, 48, 56),
	MapTile(60, 49, 56),
	MapTile(60, 47, 55),
	MapTile(60, 48, 55),
	MapTile(60, 49, 55),
	MapTile(60, 50, 55),
	MapTile(60, 48, 54),
	MapTile(60, 49, 54),
	}),
MapFragments(goblin::flag::Siofra,
	{
		MapTile(12,2), // Siofra River / Nokron
		MapTile(12,7), // Nokron Start
		MapTile(12,8), // Regal Ancestor Spirit Chamber
		MapTile(12,9), // Regal Ancestor Spirit Chamber
	}),
MapFragments(goblin::flag::Mohgwyn,
	{
		MapTile(12,5), // Mohgwyn
	}),
MapFragments(goblin::flag::Ainsel,
	{
		MapTile(12,1), // Ainsel River / Lake of Rot
	}),
MapFragments(goblin::flag::LakeOfRot,
	{
		MapTile(12,4), // Astel's Chamber
	}),
MapFragments(goblin::flag::Haligtree,
	{
		MapTile(15),
	}),
MapFragments(goblin::flag::FarumAzula,
	{
		MapTile(13),
	}),
MapFragments(goblin::flag::Deeproot,
	{
		MapTile(12,3),
	}),
MapFragments(goblin::flag::StoryErdtreeOnFire,
	{
	MapTile(11, 5), // Ashen Capital
	MapTile(19), // Stone Platform
	}),
MapFragments(goblin::flag::GravesitePlain,
	{
		MapTile(20), // Belurat
		MapTile(20, 1), // Enir-Ilim
		MapTile(41), // Belurat Gaol
		MapTile(43), // Rivermouth Cave
		MapTile(42), // Ruined Forge Lava Intake
		MapTile(42, 2), // Ruined Forge of Starfall Past
		MapTile(43, 1), // Dragon's Pit
		MapTile(41, 2), // Lamenter's Gaol
		MapTile(61, 43, 45),
		MapTile(61, 44, 45),
		MapTile(61, 45, 45),
		MapTile(61, 46, 45),
		MapTile(61, 48, 45),
		MapTile(61, 43, 44),
		MapTile(61, 44, 44),
		MapTile(61, 45, 44),
		MapTile(61, 46, 44),
		MapTile(61, 47, 44), // Fort
		MapTile(61, 48, 44),
		MapTile(61, 43, 43),
		MapTile(61, 44, 43),
		MapTile(61, 45, 43),
		MapTile(61, 46, 43),
		MapTile(61, 47, 43),
		MapTile(61, 48, 43),
		MapTile(61, 45, 42),
		MapTile(61, 46, 42),
		MapTile(61, 47, 42),
		MapTile(61, 48, 42),
		MapTile(61, 44, 41),
		MapTile(61, 45, 41),
		MapTile(61, 46, 41),
		MapTile(61, 47, 41),
		MapTile(61, 48, 41),
		MapTile(61, 45, 40),
		MapTile(61, 46, 40),
		MapTile(61, 47, 40),
	}),
MapFragments(goblin::flag::ScaduAltus,
	{
		MapTile(21), // Shadow Keep
		MapTile(21, 1), // Specimen Storehouse
		MapTile(21, 2), // Specimen Storehouse (West Rampart)
		MapTile(25), // Metyr arena
		MapTile(41, 1), // Bonny Gaol
		MapTile(40, 2), // Darklight Catacombs
		MapTile(61, 48, 49),
		MapTile(61, 49, 49),
		MapTile(61, 50, 49),
		MapTile(61, 51, 49),
		MapTile(61, 52, 49),
		MapTile(61, 48, 48),
		MapTile(61, 49, 48),
		MapTile(61, 50, 48),
		MapTile(61, 51, 48),
		MapTile(61, 52, 48),
		MapTile(61, 53, 48),
		MapTile(61, 54, 48),
		MapTile(61, 48, 47),
		MapTile(61, 49, 47),
		MapTile(61, 50, 47),
		MapTile(61, 51, 47),
		MapTile(61, 52, 47),
		MapTile(61, 53, 47),
		MapTile(61, 54, 47),
		MapTile(61, 49, 46),
		MapTile(61, 50, 46),
		MapTile(61, 51, 46),
		MapTile(61, 52, 46),
		MapTile(61, 53, 46),
		MapTile(61, 54, 46),
		MapTile(61, 49, 45),
		MapTile(61, 50, 45),
		MapTile(61, 51, 45),
		MapTile(61, 52, 45),
		MapTile(61, 53, 45),
		MapTile(61, 54, 45),
		MapTile(61, 49, 44),
		MapTile(61, 50, 44),
		MapTile(61, 51, 44),
		MapTile(61, 52, 44),
		MapTile(61, 49, 43),
		MapTile(61, 50, 43),
		MapTile(61, 51, 43),
	}),
MapFragments(goblin::flag::SouthernShore,
	{
		MapTile(22), // Stone Coffin Fissure
		MapTile(61, 49, 40),
		MapTile(61, 50, 40),
		MapTile(61, 51, 40),
		MapTile(61, 52, 40),
		MapTile(61, 53, 40),
		MapTile(61, 54, 40),
		MapTile(61, 45, 39),
		MapTile(61, 46, 39),
		MapTile(61, 47, 39),
		MapTile(61, 48, 39),
		MapTile(61, 49, 39),
		MapTile(61, 50, 39),
		MapTile(61, 51, 39),
		MapTile(61, 52, 39),
		MapTile(61, 53, 39),
		MapTile(61, 54, 39),
		MapTile(61, 55, 39),
		MapTile(61, 46, 38),
		MapTile(61, 47, 38),
		MapTile(61, 48, 38),
		MapTile(61, 49, 38),
		MapTile(61, 50, 38),
		MapTile(61, 51, 38),
		MapTile(61, 47, 37),
		MapTile(61, 48, 37),
		MapTile(61, 49, 37),
		MapTile(61, 50, 37),
		MapTile(61, 47, 36),
		MapTile(61, 46, 35),
		MapTile(61, 47, 35),
	}),
MapFragments(goblin::flag::RauhRuins,
	{
		MapTile(42, 3), // Taylew's Ruined Forge
		MapTile(40), // Fog Rift Catacombs
		MapTile(40, 1), // Scorpion River Catacombs
		MapTile(61, 44, 49),
		MapTile(61, 45, 49),
		MapTile(61, 46, 49),
		MapTile(61, 47, 49),
		MapTile(61, 43, 48),
		MapTile(61, 44, 48),
		MapTile(61, 45, 48),
		MapTile(61, 46, 48),
		MapTile(61, 47, 48),
		MapTile(61, 48, 48),
		MapTile(61, 43, 47),
		MapTile(61, 44, 47),
		MapTile(61, 45, 47),
		MapTile(61, 46, 47),
		MapTile(61, 47, 47),
		MapTile(61, 48, 47),
		MapTile(61, 43, 46),
		MapTile(61, 44, 46),
		MapTile(61, 45, 46),
		MapTile(61, 46, 46),
		MapTile(61, 44, 45),
		MapTile(61, 45, 45),
		MapTile(61, 46, 45),
	}),
MapFragments(goblin::flag::Abyss,
	{
		MapTile(28), // Midra's Manse
		MapTile(61, 52, 43),
		MapTile(61, 49, 42),
		MapTile(61, 50, 42),
		MapTile(61, 51, 42),
		MapTile(61, 52, 42),
		MapTile(61, 49, 41),
		MapTile(61, 50, 41),
		MapTile(61, 51, 41),
		MapTile(61, 52, 41),
		MapTile(61, 53, 41),
	})
};