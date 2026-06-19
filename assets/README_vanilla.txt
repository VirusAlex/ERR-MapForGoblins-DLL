Map For Goblins - DLL Edition v%VERSION%
For VANILLA Elden Ring (base game + Shadow of the Erdtree).
~6700 loot & world-map icons. No regulation.bin changes.

This package contains a single folder "MapForGoblins":
  MapForGoblins.dll   - the mod
  MapForGoblins.ini   - settings (toggle icon categories on/off)
  menu/               - world-map UI asset (02_120_worldmap.gfx)

You need a mod loader - ModEngine3 (me3) OR ModEngine2. Pick ONE.

============================================================
ModEngine3 (me3)  - recommended, actively maintained
============================================================
1. Install me3: download and run me3_installer.exe from
   https://github.com/garyttierney/me3/releases
   (docs: https://me3.help/). me3 creates default .me3 profiles
   for Elden Ring.
2. Copy the whole "MapForGoblins" folder next to the .me3 profile
   you want to use (e.g. the eldenring-default.me3 me3 created).
3. Right-click the .me3 file > edit with Notepad, and make sure it
   contains (paths are relative to the .me3 file):

   profileVersion = "v1"

   [[supports]]
   game = "eldenring"

   [[packages]]
   path = 'MapForGoblins'

   [[natives]]
   path = 'MapForGoblins/MapForGoblins.dll'

4. Double-click the .me3 file to launch the game.

============================================================
ModEngine2
============================================================
1. Get ModEngine2 from
   https://github.com/soulsmods/ModEngine2/releases and extract it
   anywhere (it does NOT go in the game folder).
2. Copy the whole "MapForGoblins" folder into the ModEngine2
   directory (next to launchmod_eldenring.bat).
3. Open config_eldenring.toml in a text editor and edit two lines:

   - under [modengine], register the DLL:
       external_dlls = [ "MapForGoblins\\MapForGoblins.dll" ]

   - under [extension.mod_loader], add the asset folder as the FIRST
     mods entry (in ModEngine2 the first entry is highest priority;
     listing MapForGoblins first lets its world-map file win if another
     mod ships one too):
       mods = [
           { enabled = true, name = "MapForGoblins", path = "MapForGoblins" },
           { enabled = true, name = "default", path = "mod" }
       ]

4. Run launchmod_eldenring.bat to launch.

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
- "Inappropriate activity detected, online play disabled" at launch
  is normal: the mod loader turned off EAC. Play OFFLINE (or via a
  Seamless Co-op setup). Do not play vanilla online with mods.
- Questions and bug reports: https://discord.gg/JvTMwPCygB
