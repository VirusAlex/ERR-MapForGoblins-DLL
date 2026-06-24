Map For Goblins - DLL Edition v%VERSION%
For the ERTE overhaul (https://www.nexusmods.com/eldenring/mods/2747).
Loot & world-map icons generated from ERTE's own game data.
No regulation.bin changes. Pure DLL - no gfx or other extra files.
Unofficial: not affiliated with the ERTE author - please don't report
issues with this add-on to them.
Enemy/boss/world markers come from ERTE's data merged over the base
game, so content ERTE didn't change is covered too.

This package contains just two files:
  MapForGoblins.dll   - the mod
  MapForGoblins.ini   - settings (all icon categories ON by default;
                        edit to turn ones off)

IMPORTANT: this build matches the ERTE version it was generated from
(see the mod page). After an ERTE update, markers can be slightly off
until this mod is updated too.

============================================================
Install (ERTE runs on Mod Engine 3 / me3)
============================================================
ERTE is launched through a .me3 profile (e.g.
eldenring-ERTE-SOTE-CoopReady.me3). Add this mod to that profile:

1. Copy MapForGoblins.dll and MapForGoblins.ini into a "MapForGoblins"
   folder next to ERTE's mod folder, i.e.
   ...\me3\config\profiles\eldenring-mods\MapForGoblins\
   (alongside the "ERTE SOTE" folder).

2. Open the ERTE .me3 profile file in a text editor and add a native
   for the DLL (anywhere in the file):
       [[natives]]
       path = 'eldenring-mods\MapForGoblins\MapForGoblins.dll'

3. Launch via the ERTE .me3 profile as usual.

============================================================
Updating from an older version
============================================================
Replace MapForGoblins.dll with this one (keep your MapForGoblins.ini -
new options are added automatically). This version no longer uses a gfx
file: if you set it up before, you can DELETE the old "menu" folder /
02_120_worldmap.gfx and remove the old [[packages]] entry that pointed
at the MapForGoblins folder - only the [[natives]] DLL entry is needed.

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
- Questions and bug reports: https://discord.gg/JvTMwPCygB
