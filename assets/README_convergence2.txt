Map For Goblins - DLL Edition v%VERSION%
For The Convergence 2.x (https://www.nexusmods.com/eldenring/mods/3419).
~7200 loot & world-map icons generated from The Convergence's own game data.
No regulation.bin changes. Pure DLL - no gfx or other extra files.
Unofficial: not affiliated with the Convergence Team - please don't
report issues with this add-on to them.

This build is for Convergence 2.x, which runs on ModEngine2. There is a
separate build for Convergence 3.x (ModEngine3) - use the one that matches
your Convergence version.

This package contains just two files:
  MapForGoblins.dll   - the mod
  MapForGoblins.ini   - settings (all icon categories ON by default;
                        edit to turn ones off)

IMPORTANT: this build matches the Convergence version it was generated
from (see the mod page). After a Convergence update, markers can be
slightly off until this mod is updated too.

============================================================
Install (into an existing Convergence 2.x install)
============================================================
1. Copy MapForGoblins.dll and MapForGoblins.ini into your ConvergenceER
   folder (next to Start_Convergence.bat and config_eldenring.toml).
2. Open config_eldenring.toml in a text editor and, under [modengine],
   add the DLL to the external_dlls list:
       external_dlls = [
           ...existing entries...,
           "MapForGoblins.dll",
       ]
3. Launch via Start_Convergence.bat as usual.

Note: the Convergence Launcher may rewrite config_eldenring.toml when
it updates the mod - re-check the external_dlls edit after updates.

============================================================
Updating from an older version
============================================================
Replace MapForGoblins.dll with this one (keep your MapForGoblins.ini -
new options are added automatically). This version no longer uses a gfx
file: if you set it up before, you can DELETE the old MapForGoblins asset
folder (the "menu" folder / 02_120_worldmap.gfx) and remove its "mods"
entry from config_eldenring.toml - only the external_dlls entry is needed.

============================================================
Settings & notes
============================================================
- All icon categories are ON by default. Edit MapForGoblins.ini to turn
  off the ones you don't want. The mod creates the file if missing and
  auto-adds any new options on launch, so it stays current across updates.
- In-game mod menu: press F10 (or Y+R3 on a controller) to open a
  settings panel and toggle icon categories. Category toggles take
  effect right away on the open map; some options apply on the next
  map open.
- Markers come from The Convergence's map data merged over the base
  game, so loot the mod didn't change is covered too.
- Questions and bug reports: https://discord.gg/JvTMwPCygB
