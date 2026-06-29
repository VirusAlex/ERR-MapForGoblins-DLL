#include "goblin_logic.hpp"
#include "goblin_config.hpp"
#include "from/params.hpp"
#include "from/paramdef/WORLD_MAP_POINT_PARAM_ST.hpp"
#include "goblin/goblin_structs.hpp"
#include "goblin/goblin_map_flags.hpp"
#include "goblin/goblin_map_tiles.hpp"
#include "goblin/goblin_map_exceptions.hpp"

#include <spdlog/spdlog.h>
#include <unordered_map>

using namespace goblin;
using namespace goblin::mapPoint;

// Goblin icon ID ranges (same as Goblin-ERR)
static constexpr ParamRange goblinIcons(1, 78500);
static constexpr ParamRange goblinIconsERR(1000000, 10025000);

// Original (pre-patch) eventFlagId per ERR-patched row, keyed by row id (these
// rows are the game's own - their ids are stable, never our reassigned ones).
// apply_map_logic runs again on every overlay-open (reapply_live_settings), and
// the ERR setup functions stash eventFlagId into a textEnableFlag slot then mutate
// eventFlagId - reading the LIVE eventFlagId on the 2nd run would stash the
// already-mutated value (e.g. SetupDungeonERR zeroes it, so textEnableFlagId3
// became 0 = always-enabled -> the icon showed with no map fragment). Capturing
// the original ONCE and reusing it makes the patching idempotent.
static std::unordered_map<int, int> g_err_orig_eventflag;

static int OrigEventFlag(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST &row)
{
    auto it = g_err_orig_eventflag.find(rowId);
    if (it != g_err_orig_eventflag.end())
        return it->second;
    int v = row.eventFlagId;
    g_err_orig_eventflag[rowId] = v;
    return v;
}

static bool HasException(int paramId, int &mapFragment)
{
    if (ExceptionList.count(paramId))
    {
        mapFragment = ExceptionList.at(paramId);
        return true;
    }
    return false;
}

static int GetMapFlagFromTile(MapTile location)
{
    for (const auto &fragment : MapList)
    {
        for (auto &chunk : fragment.mapFragmentTile)
        {
            if (chunk == location)
                return fragment.mapFragmentId;
        }
    }
    return 0;
}

static void SetSecondaryFlags(from::paramdef::WORLD_MAP_POINT_PARAM_ST &row, int flagId)
{
    row.textEnableFlag2Id1 = flagId;
    row.textEnableFlag2Id2 = flagId;
    row.textEnableFlag2Id3 = flagId;
    row.textEnableFlag2Id4 = flagId;
    row.textEnableFlag2Id5 = flagId;
    row.textEnableFlag2Id6 = flagId;
    row.textEnableFlag2Id7 = flagId;
    row.textEnableFlag2Id8 = flagId;
}

static int GetMapFragment(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST &row)
{
    int requiredMapFragment = 0;
    auto chunk = MapTile(row.areaNo, row.gridXNo, row.gridZNo);

    if (!HasException(rowId, requiredMapFragment))
    {
        requiredMapFragment = GetMapFlagFromTile(chunk);
    }

    // Lake of Rot shares fine tile m12_01 with Ainsel River but is revealed by its
    // OWN map fragment (LakeOfRot 62061, not Ainsel 62060). The two are one tile, so
    // the tile->fragment table put the whole tile under Ainsel - which leaked Lake of
    // Rot icons onto the map the moment the Ainsel fragment was owned (e.g. opening
    // the Ainsel River map). Distinguish by the marker's runtime-resolved location
    // text (LOCATION_ALT runs in inject_map_entries, before this): PlaceName 12011 =
    // Lake of Rot. (12011 is a raw PlaceName id, stored un-remapped on the row.)
    if (chunk == MapTile(12, 1))
    {
        const int loc[8] = {row.textId1, row.textId2, row.textId3, row.textId4,
                            row.textId5, row.textId6, row.textId7, row.textId8};
        for (int t : loc)
            if (t == 12011) { requiredMapFragment = flag::LakeOfRot; break; }
    }

    if (config::requireMapFragments)
    {
        // Post-event areas
        if (chunk == MapTile(11, 5) || chunk == MapTile(19))
        {
            SetSecondaryFlags(row, flag::StoryErdtreeOnFire);
        }
        else if (chunk == MapTile(21) || chunk == MapTile(21, 1) ||
                 chunk == MapTile(21, 2) || chunk == MapTile(22))
        {
            SetSecondaryFlags(row, flag::StoryCharmBroken);
        }
        else if (chunk == MapTile(20, 1))
        {
            SetSecondaryFlags(row, flag::StorySealingTreeBurnt);
        }
    }

    return requiredMapFragment;
}

static int GetIconFlag(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST &row)
{
    if (config::requireMapFragments)
        return GetMapFragment(rowId, row);
    else
        return flag::AlwaysOn;
}

static void HideOnCompletion(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST &row)
{
    if (row.textId2 == 5100)
    {
        auto disableFlag = row.textEnableFlagId4;
        row.textDisableFlagId1 = disableFlag;
        row.textDisableFlagId2 = disableFlag;
        row.textDisableFlagId3 = disableFlag;
        row.textDisableFlagId4 = disableFlag;
    }
    else
    {
        auto disableFlag = row.textEnableFlagId5;
        row.textDisableFlagId1 = disableFlag;
        row.textDisableFlagId2 = disableFlag;
        row.textDisableFlagId3 = disableFlag;
        row.textDisableFlagId4 = disableFlag;
        row.textDisableFlagId5 = disableFlag;
    }
}

static void SetupOverworldERR(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST &row)
{
    row.textEnableFlagId2 = OrigEventFlag(rowId, row);
    row.eventFlagId = GetIconFlag(rowId, row);
}

static void SetupDungeonERR(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST &row)
{
    int mapFragment = GetIconFlag(rowId, row);
    row.textEnableFlagId1 = mapFragment;
    row.textEnableFlagId2 = mapFragment;
    row.textEnableFlagId3 = OrigEventFlag(rowId, row);
    row.eventFlagId = 0;

    if (config::hideDungeonIconsOnClear)
    {
        HideOnCompletion(rowId, row);
    }
}

static void SetupCampsERR(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST &row)
{
    row.textEnableFlagId2 = OrigEventFlag(rowId, row);
    row.eventFlagId = GetIconFlag(rowId, row);
}

static void SetupMerchants(int rowId, from::paramdef::WORLD_MAP_POINT_PARAM_ST &row)
{
    if (config::requireMapFragments)
        row.textEnableFlagId3 = GetIconFlag(rowId, row);
    else
        row.textEnableFlagId3 = flag::AlwaysOn;
}

void goblin::apply_map_logic()
{

    int modified_goblin = 0;
    int modified_boss = 0;
    int modified_camp = 0;
    int modified_merchant = 0;

    for (auto [rowId, row] :
         from::params::get_param<from::paramdef::WORLD_MAP_POINT_PARAM_ST>(L"WorldMapPointParam"))
    {
        // Goblin icons (our injected entries + any existing ones)
        if (goblinIcons.IsInRange(rowId) || goblinIconsERR.IsInRange(rowId))
        {
            row.eventFlagId = GetIconFlag(rowId, row);
            modified_goblin++;
        }
        // Camp markers (textId2=5000) - ERR-placed, opt-in patching
        else if (row.textId2 == 5000)
        {
            if (config::patchCampIcons)
            {
                SetupCampsERR(rowId, row);
                modified_camp++;
            }
        }
        // Merchant markers (textId4=8800) - ERR-placed, opt-in patching
        else if (row.textId4 == 8800)
        {
            if (config::patchMerchantIcons)
            {
                SetupMerchants(rowId, row);
                modified_merchant++;
            }
        }
        // Boss markers - overworld (textId2=5100) or dungeon (textId3=5100/5300)
        else if (row.textId2 == 5100 || row.textId3 == 5100 || row.textId3 == 5300)
        {
            if (row.textId2 == 5100)
            {
                if (config::patchOverworldBossIcons)
                {
                    SetupOverworldERR(rowId, row);
                    modified_boss++;
                }
            }
            else
            {
                if (config::patchDungeonBossIcons)
                {
                    SetupDungeonERR(rowId, row);
                    modified_boss++;
                }
            }
        }
    }

    spdlog::debug("Map logic applied: {} goblin icons, {} bosses, {} camps, {} merchants",
                  modified_goblin, modified_boss, modified_camp, modified_merchant);
}
