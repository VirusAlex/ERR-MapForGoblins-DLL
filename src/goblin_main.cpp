#include "goblin_main.hpp"

using namespace goblin;
using namespace goblin::config;
using namespace goblin::iconIds;

void goblin::Initialise() {

    // Loop through all WorldMapPointParam
    for (auto [rowId, row] :
        from::params::get_param<from::paramdef::WORLD_MAP_POINT_PARAM_ST>(L"WorldMapPointParam")) {

        // Select Goblin icons we're working with
        if (goblinIcons.IsInRange(rowId) || goblinIconsERR.IsInRange(rowId)) {
            // Set row's event flag to a map piece (or always on if map pieces are disabled)
            row.eventFlagId = GetIconFlag(rowId, row);
        }
        // Checks for "Discovered" text - camps
        else if (row.textId2 == 5000) {
            if (showCampIcons) {
                SetupCampsERR(rowId, row);
            }
        }
        // Checks for ERR's pledge icons - merchants
        else if (row.textId4 == 8800) {
            if (showMerchantIcons) {
                SetupMerchants(rowId, row);
            }
        }
        // Checks for "Encountered" text - bosses
        // textId2 is overworld, textId3 is dungeons
        else if (row.textId2 == 5100 || row.textId3 == 5100 || row.textId3 == 5300) {
            // overworld and dungeons each need specific adjustments
            if (row.textId2 == 5100) {
                if (showOverworldBossIcons)
                    SetupOverworldERR(rowId, row);
            }
            else {
                if (showDungeonBossIcons) {
                    SetupDungeonERR(rowId, row);
                }
            }
        }
    }
}

static int GetIconFlag(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST& row) {
    // Get map fragment if it's set up in config
    if (requireMapFragments)
        return GetMapFragment(rowId, row);
    else
        return flag::AlwaysOn;
}

static void SetupOverworldERR(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST& row) {
    // Put the encountered flag on encountered text
    row.textEnableFlagId2 = row.eventFlagId;
    // Set up as normal
    row.eventFlagId = GetIconFlag(rowId, row);

    if (config::redifyBossIcons) {
        row.iconId = 372;
        HideOnCompletion(rowId, row);
    }
}

static void SetupDungeonERR(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST& row) {
    // Apply the map fragment to both dungeon name and dungeon boss
    int mapFragment = GetIconFlag(rowId, row);
    row.textEnableFlagId1 = mapFragment;
    row.textEnableFlagId2 = mapFragment;
    // Put the encountered flag on encountered text
    row.textEnableFlagId3 = row.eventFlagId;
    // Needs this workaround otherwise it grants map fragments... wot
    row.eventFlagId = 0;

    if (config::redifyBossIcons) {
        row.iconId = 372;
        HideOnCompletion(rowId, row);
    }
}

static void SetupCampsERR(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST& row) {
    // Put the encountered flag on encountered text
    row.textEnableFlagId2 = row.eventFlagId;
    // Set up as normal
    row.eventFlagId = GetIconFlag(rowId, row);
}

static void SetupMerchants(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST& row) {
    if (requireMapFragments)
        row.textEnableFlagId3 = GetIconFlag(rowId, row);
    else
        row.textEnableFlagId3 = goblin::flag::AlwaysOn;
}


// just a piece of code for debugging
static void Debug(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST& row) {
#ifdef _DEBUG
    spdlog::info("eventFlagId = {}", row.eventFlagId);
    spdlog::info("textEnableFlagId1 = {}", row.textEnableFlagId1);
    spdlog::info("textEnableFlagId2 = {}", row.textEnableFlagId2);
    spdlog::info("textEnableFlagId3 = {}", row.textEnableFlagId3);
    spdlog::info("textDisableFlagId1 = {}", row.textDisableFlagId1);
    spdlog::info("textEnableFlag2Id1 = {}", row.textEnableFlag2Id1);
#endif
}

static int GetMapFragment(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST& row) {
    int requiredMapFragment = 0;
    // Get this icon's location
    auto chunk = mapPoint::MapTile(row.areaNo, row.gridXNo, row.gridZNo);
    // Get this icon's map fragment, if it has an exception, set it to the map fragment piece
    if (!mapPoint::HasException(rowId, requiredMapFragment)) {
        requiredMapFragment = mapPoint::GetMapFlagFromTile(chunk);
    }
    
    if (requireMapFragments){
        // If it's post-event, make sure it only appears after event occurs, we guarantee it by using textEnableFlag2 which is checked alongside textEnableFlag and eventFlagId, all 3 need to be ON then
        if (chunk == MapTile(11, 5) ||      // Ashen Capital
            chunk == MapTile(19)) {         // Stone Platform
            SetSecondaryFlags(row, flag::StoryErdtreeOnFire);
        }
        else if (chunk == MapTile(21) ||    // Shadow Keep
            chunk == MapTile(21, 1) ||      // Specimen Storehouse
            chunk == MapTile(21, 2) ||      // Specimen Storehouse (West Rampart)
            chunk == MapTile(22)) {         // Stone Coffin Fissure    
            SetSecondaryFlags(row, flag::StoryCharmBroken);
        }
        else if (chunk == MapTile(20, 1)) { // Enir-Ilim
            SetSecondaryFlags(row, flag::StorySealingTreeBurnt);
        }
    }
    return requiredMapFragment;
}

static void HideOnCompletion(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST& row) {
    if (row.textId2 == 5100) {
        auto disableFlag = &row.textEnableFlagId4;
        row.textDisableFlagId1 = *disableFlag;
        row.textDisableFlagId2 = *disableFlag;
        row.textDisableFlagId3 = *disableFlag;
        row.textDisableFlagId4 = *disableFlag;
    }
    else {
        auto disableFlag = &row.textEnableFlagId5;
        row.textDisableFlagId1 = *disableFlag;
        row.textDisableFlagId2 = *disableFlag;
        row.textDisableFlagId3 = *disableFlag;
        row.textDisableFlagId4 = *disableFlag;
        row.textDisableFlagId5 = *disableFlag;

    }
}

static void SetSecondaryFlags(from::paramdef::WORLD_MAP_POINT_PARAM_ST& row, int flagId) {
    row.textEnableFlag2Id1 = flagId;
    row.textEnableFlag2Id2 = flagId;
    row.textEnableFlag2Id3 = flagId;
    row.textEnableFlag2Id4 = flagId;
    row.textEnableFlag2Id5 = flagId;
    row.textEnableFlag2Id6 = flagId;
    row.textEnableFlag2Id7 = flagId;
    row.textEnableFlag2Id8 = flagId;
}