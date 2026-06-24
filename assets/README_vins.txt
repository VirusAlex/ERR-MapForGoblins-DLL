Map For Goblins - DLL Edition v%VERSION%
For the ELDEN VINS overhaul.
Loot & world-map icons generated from Elden Vins' own game data.
No regulation.bin changes. Pure DLL - no gfx or other extra files.
Unofficial: not affiliated with the Elden Vins author - please don't
report issues with this add-on to them.

This package contains just two files:
  MapForGoblins.dll   - the mod
  MapForGoblins.ini   - settings (all icon categories ON by default;
                        edit to turn ones off)

IMPORTANT: this build matches the Elden Vins version it was generated
from (see the mod page). After an Elden Vins update, markers can be
slightly off until this mod is updated too.

============================================================
Install (into an existing Elden Vins install)
============================================================
Elden Vins runs on ModEngine2 (Launch ELDEN VINS.bat / ELDEN VINS.exe).
1. Copy MapForGoblins.dll and MapForGoblins.ini into your "ELDEN VINS"
   folder (next to config_eldenring.toml and Launch ELDEN VINS.bat).
2. Open config_eldenring.toml in a text editor and, under [modengine],
   add the DLL to the external_dlls list:
       external_dlls = [
           ...existing entries...,
           "MapForGoblins.dll",
       ]
3. Launch via Launch ELDEN VINS.bat (or ELDEN VINS.exe) as usual.

============================================================
Updating from an older version
============================================================
Replace MapForGoblins.dll with this one (keep your MapForGoblins.ini -
new options are added automatically). This version uses no gfx file: if
you set up an older version, you can DELETE the old MapForGoblins asset
folder (the "menu" folder / 02_120_worldmap.gfx) and remove its "mods"
entry from config_eldenring.toml - only the external_dlls entry is needed.

============================================================
Settings & notes
============================================================
- All icon categories are ON by default. Edit MapForGoblins.ini to turn
  off the ones you don't want. The mod creates the file if missing and
  auto-adds any new options on launch, so it stays current across updates.
- In-game mod menu: press F10 (or Y+R3 on a controller) to open a
  settings panel and toggle icon categories. Category toggles take effect
  right away on the open map; some options apply on the next map open.
- Markers come from Elden Vins' map data merged over the base game, so
  loot the mod didn't change is covered too.
- Questions and bug reports: https://discord.gg/JvTMwPCygB
