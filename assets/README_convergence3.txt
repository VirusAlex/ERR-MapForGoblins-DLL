Map For Goblins - DLL Edition v%VERSION%
For The Convergence 3.x (https://www.nexusmods.com/eldenring/mods/3419).
~7200 loot & world-map icons generated from The Convergence's own game data.
No regulation.bin changes. Pure DLL - no gfx or other extra files.
Unofficial: not affiliated with the Convergence Team - please don't
report issues with this add-on to them.

This build is for Convergence 3.x, which runs on ModEngine3 (a .me3
profile). There is a separate build for Convergence 2.x (ModEngine2) -
use the one that matches your Convergence version.

This package contains just two files:
  MapForGoblins.dll   - the mod
  MapForGoblins.ini   - settings (all icon categories ON by default;
                        edit to turn ones off)

IMPORTANT: this build matches the Convergence version it was generated
from (see the mod page). After a Convergence update, markers can be
slightly off until this mod is updated too.

============================================================
Install (Convergence 3.x runs on Mod Engine 3 / me3)
============================================================
Convergence 3.x is launched through a .me3 profile (in the ConvergenceER\me3
folder, e.g. convergence.me3 - and convergence - seamless.me3 if you use
the Seamless Co-op profile). Add this mod to the profile(s) you launch with:

1. Copy MapForGoblins.dll and MapForGoblins.ini into the mod\dll folder,
   i.e. ...\ConvergenceER\mod\dll\ (next to ErdTools.dll, erdyes.dll, etc.).

2. Open the .me3 profile in a text editor and add a native for the DLL
   (anywhere among the other [[natives]] entries):
       [[natives]]
       path = './../mod/dll/MapForGoblins.dll'

3. Launch via Start_Convergence.bat (or your .me3 profile) as usual.

============================================================
Updating from an older version
============================================================
Replace MapForGoblins.dll with this one (keep your MapForGoblins.ini -
new options are added automatically). If you previously ran the
Convergence 2.x (ModEngine2) build, remove its "MapForGoblins.dll" entry
from the old config_eldenring.toml external_dlls - on 3.x the mod loads
only through the .me3 [[natives]] entry above.

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
