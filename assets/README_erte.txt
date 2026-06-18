Map For Goblins - DLL Edition v%VERSION%
For the ERTE overhaul (https://www.nexusmods.com/eldenring/mods/2747).
Loot & world-map icons generated from ERTE's own game data.
No regulation.bin changes. Unofficial: not affiliated with the ERTE
author - please don't report issues with this add-on to them.
Enemy/boss/world markers come from ERTE's data merged over the base
game, so content ERTE didn't change is covered too.

This package contains a single folder "MapForGoblins":
  MapForGoblins.dll   - the mod
  MapForGoblins.ini   - settings (toggle icon categories on/off)
  menu/               - world-map UI asset (02_120_worldmap.gfx; ERTE
                        ships no map UI of its own, so this is built on
                        the base game's map UI)

IMPORTANT: this build matches the ERTE version it was generated from
(see the mod page). After an ERTE update, markers can be slightly off
until this mod is updated too.

============================================================
Install (ERTE runs on Mod Engine 3 / me3)
============================================================
ERTE is launched through a .me3 profile (e.g.
eldenring-ERTE-SOTE-CoopReady.me3). Add this mod to that profile:

1. Copy the whole "MapForGoblins" folder next to ERTE's mod folder,
   i.e. into  ...\me3\config\profiles\eldenring-mods\  (alongside the
   "ERTE SOTE" folder).

2. Open the ERTE .me3 profile file in a text editor and add two blocks:

   - a native for the DLL (anywhere in the file):
       [[natives]]
       path = 'eldenring-mods\MapForGoblins\MapForGoblins.dll'

   - a package for the icon/asset folder, listed BEFORE the ERTE
     package so our world-map file is used:
       [[packages]]
       path = 'eldenring-mods\MapForGoblins'

       [[packages]]
       path = 'eldenring-mods\ERTE SOTE'

   (ERTE has no world-map UI of its own, so there is no conflict - this
   just makes sure our 02_120_worldmap.gfx is loaded.)

3. Launch via the ERTE .me3 profile as usual.

============================================================
Settings & notes
============================================================
- Edit MapForGoblins/MapForGoblins.ini to turn icon categories on/off.
  The mod creates this file if missing and auto-adds any new options on
  launch, so it stays current across updates.
- In-game config overlay: press F10 (or Y+R3 on a controller) to open
  a settings panel and toggle icon categories live. Re-open the world
  map to see the changes.
- Questions and bug reports: https://discord.gg/JvTMwPCygB
