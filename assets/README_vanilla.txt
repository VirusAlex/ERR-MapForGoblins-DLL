Map For Goblins - DLL Edition v%VERSION%
For VANILLA Elden Ring (base game + Shadow of the Erdtree).
~6700 loot & world-map icons. No regulation.bin changes.
Pure DLL - no gfx or other extra files.

This package contains just two files:
  MapForGoblins.dll   - the mod
  MapForGoblins.ini   - settings (all icon categories ON by default;
                        edit to turn ones off)

You need a mod loader - ModEngine3 (me3) OR ModEngine2. Pick ONE.
Keep MapForGoblins.dll and MapForGoblins.ini together in the same folder.

============================================================
ModEngine3 (me3)  - recommended, actively maintained
============================================================
1. Install me3: download and run me3_installer.exe from
   https://github.com/garyttierney/me3/releases
   (docs: https://me3.help/). me3 creates default .me3 profiles
   for Elden Ring.
2. Copy MapForGoblins.dll and MapForGoblins.ini next to the .me3
   profile you want to use (e.g. eldenring-default.me3).
3. Right-click the .me3 file > edit with Notepad, and make sure it
   contains (path is relative to the .me3 file):

   profileVersion = "v1"

   [[supports]]
   game = "eldenring"

   [[natives]]
   path = 'MapForGoblins.dll'

4. Double-click the .me3 file to launch the game.

============================================================
ModEngine2
============================================================
1. Get ModEngine2 from
   https://github.com/soulsmods/ModEngine2/releases and extract it
   anywhere (it does NOT go in the game folder).
2. Copy MapForGoblins.dll and MapForGoblins.ini into the ModEngine2
   directory (next to launchmod_eldenring.bat).
3. Open config_eldenring.toml in a text editor and, under [modengine],
   register the DLL:
       external_dlls = [ "MapForGoblins.dll" ]
4. Run launchmod_eldenring.bat to launch.

============================================================
Updating from an older version
============================================================
Replace MapForGoblins.dll with this one (keep your MapForGoblins.ini -
new options are added automatically). This version no longer uses a gfx
file: if you set it up before, you can DELETE the old "menu" folder /
02_120_worldmap.gfx and remove the old "mods"/"packages" entry that
pointed at the MapForGoblins asset folder - only the DLL is needed now.

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
- "Inappropriate activity detected, online play disabled" at launch
  is normal: the mod loader turned off EAC. Play OFFLINE (or via a
  Seamless Co-op setup). Do not play vanilla online with mods.
- Questions and bug reports: https://discord.gg/JvTMwPCygB
