Map For Goblins - DLL Edition v%VERSION%
For The Convergence mod (https://www.nexusmods.com/eldenring/mods/3419).
~7200 loot & world-map icons generated from The Convergence's own game data.
No regulation.bin changes. Unofficial: not affiliated with the
Convergence Team - please don't report issues with this add-on to them.

This package contains a single folder "MapForGoblins":
  MapForGoblins.dll   - the mod
  MapForGoblins.ini   - settings (toggle icon categories on/off)
  menu/               - world-map UI asset (02_120_worldmap.gfx, built on
                        The Convergence's map UI so its own icons keep
                        working alongside ours)

IMPORTANT: this build matches the Convergence version it was generated
from (see the mod page). After a Convergence update, markers can be
slightly off until this mod is updated too.

============================================================
Install (into an existing Convergence install)
============================================================
1. Copy the whole "MapForGoblins" folder into your ConvergenceER
   folder (next to Start_Convergence.bat and config_eldenring.toml).
2. Open config_eldenring.toml in a text editor and edit two places:

   - under [modengine], add the DLL to the external_dlls list:
       external_dlls = [
           ...existing entries...,
           "MapForGoblins/MapForGoblins.dll",
       ]

   - under [extension.mod_loader], add the asset folder ABOVE the
     Convergence "mod" entry (in ModEngine2 the FIRST entry is highest
     priority; our world-map file must win the conflict, it already
     contains the Convergence map UI):
       mods = [
           { enabled = true, name = "MapForGoblins", path = "MapForGoblins" },
           { enabled = true, name = "mod", path = "mod" }
       ]

3. Launch via Start_Convergence.bat as usual.

Note: the Convergence Launcher may rewrite config_eldenring.toml when
it updates the mod - re-check the two edits above after updates.

============================================================
Settings & notes
============================================================
- Edit MapForGoblins/MapForGoblins.ini to turn icon categories
  on/off. The mod creates this file if missing and auto-adds any
  new options on launch, so it stays current across updates.
- In-game mod menu: press F10 (or Y+R3 on a controller) to open a
  settings panel and toggle icon categories. Category toggles take
  effect right away on the open map; some options apply on the next
  map open.
- Markers come from The Convergence's map data merged over the base
  game, so loot the mod didn't change is covered too.
- Questions and bug reports: https://discord.gg/JvTMwPCygB
