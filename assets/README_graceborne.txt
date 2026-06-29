Map For Goblins - DLL Edition v%VERSION%
For the Graceborne (Bloodborne-inspired) overhaul
(https://www.nexusmods.com/eldenring/mods/5207).
Loot & world-map icons generated from Graceborne's own game data.
No regulation.bin changes. Pure DLL - no gfx or other extra files.
Unofficial: not affiliated with the Graceborne author - please don't
report issues with this add-on to them.

This package contains just two files:
  MapForGoblins.dll   - the mod
  MapForGoblins.ini   - settings (all icon categories ON by default;
                        edit to turn ones off)

IMPORTANT: this build matches the Graceborne version it was generated
from (see the mod page). After a Graceborne update, markers can be
slightly off until this mod is updated too.

IMPORTANT: Graceborne requires the game language set to ENGLISH (see its
mod page). Its non-English message files are incomplete, so on other
languages the GAME ITSELF crashes on startup - with or without this mod.
Set Elden Ring to English in Steam before playing Graceborne.

============================================================
Install (Graceborne runs on Mod Engine 3 / me3)
============================================================
Graceborne is launched through its .me3 profile (launch.me3) and serves
the mod from its "mod" folder.

1. Copy MapForGoblins.dll and MapForGoblins.ini into Graceborne's
   "mod" folder, i.e. ...\GRACEBORNE\mod\ (next to regulation.bin and
   Scripts-Data-Exposer-FS.dll).

2. Open launch.me3 in a text editor and add a native for the DLL
   (anywhere among the other [[natives]] entries):
       [[natives]]
       path = 'mod/MapForGoblins.dll'

3. Launch via Graceborne's usual .me3 launcher.

============================================================
Updating from an older version
============================================================
Replace MapForGoblins.dll with this one (keep your MapForGoblins.ini -
new options are added automatically).

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
- Markers come from Graceborne's map data merged over the base game, so
  loot the mod didn't change is covered too.
- Questions and bug reports: https://discord.gg/JvTMwPCygB
