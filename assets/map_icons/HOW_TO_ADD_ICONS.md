# How to Add New Map Icons

## Prerequisites

- **[FFDEC](https://github.com/jindrapetrik/jpexs-decompiler)** (JPEXS Free Flash Decompiler) — any recent build. You need the CLI jar (`ffdec-cli.jar`) and a Java 8+ runtime to run it.
- Set the environment variable `FFDEC_CLI` to a command that runs it. Examples:
  - Linux/macOS: `export FFDEC_CLI="java -jar /path/to/ffdec-cli.jar"`
  - Windows (PowerShell): `$env:FFDEC_CLI = 'java -jar "C:\Tools\FFDec\ffdec-cli.jar"'`
  - Windows with a non-system Java: `set FFDEC_CLI="C:\Java\jdk1.8\bin\java.exe" -jar "C:\Tools\FFDec\ffdec-cli.jar"`
- If you add `ffdec-cli.jar` to your `PATH` wrapper script as just `ffdec`, you can use `FFDEC_CLI=ffdec`.

The commands below use `$FFDEC_CLI` — substitute it literally with your own command if you prefer.

## Files

- **Source of truth GFX XML**: decompile from `assets/menu/02_120_worldmap_new.gfx` each time
- **DO NOT use** `worldmap_modified.xml` — it's outdated (only has up to frame 397)
- **TGA textures**: `assets/menu/*.tga` — extracted from DDS sprite sheets
- **Rendered previews**: `assets/map_icons/composed/iconId_XXX.png`

## Full Workflow

### 1. Decompile GFX → XML

```bash
cd MapForGoblins
$FFDEC_CLI -swf2xml assets/menu/02_120_worldmap_new.gfx \
                    assets/map_icons/worldmap_new_actual.xml
```

### 2. Extract/fix TGA textures (if needed)

```bash
cd tools
python extract_subtextures.py
```

This extracts sub-textures from vanilla + ERR DDS sprite sheets into `assets/menu/*.tga`.

**Known issue**: Some TGAs may have broken alpha (max=1). To fix:
- Delete the broken `.tga` file
- Re-run `extract_subtextures.py` — it skips existing files, so deleting forces re-extraction

**ERR path fix**: `extract_subtextures.py` uses `config.require_err_mod_dir() / "menu" / "hi"` (NOT `"mod" / "menu" / "hi"` — the mod dir already includes `\mod`).

### 3. Find the charId and matrix for your icon

To check what an existing iconId uses:

```python
# In the decompiled XML, frames in sprite 171 are 1-indexed
# iconId N = frame N (ShowFrameTag #N)
# Each frame inherits layers from previous frames unless RemoveObject2Tag clears them
```

Run this to inspect frames:
```bash
cd tools
python -c "
import xml.etree.ElementTree as ET
tree = ET.parse('../assets/map_icons/worldmap_new_actual.xml')
root = tree.getroot()
char_names = {}
for item in root.iter('item'):
    if item.get('type') == 'DefineExternalImage2':
        char_names[int(item.get('characterID', 0))] = item.get('exportName', '')
for elem in root.iter('item'):
    if elem.get('type') == 'DefineSpriteTag' and elem.get('spriteId') == '171':
        sub = elem.find('subTags')
        frame = 0
        current = {}
        for child in sub:
            tt = child.get('type','')
            if tt == 'ShowFrameTag':
                frame += 1
                if frame == TARGET_ICONID:
                    for d in sorted(current):
                        info = current[d]
                        print(f'depth={d} cid={info[\"cid\"]}({char_names.get(info[\"cid\"],\"?\")}) tx={info[\"tx\"]} ty={info[\"ty\"]}')
            elif 'PlaceObject' in tt:
                depth = int(child.get('depth', 0))
                cid = int(child.get('characterId', 0))
                m = child.find('matrix')
                tx = m.get('translateX','0') if m is not None else '0'
                ty = m.get('translateY','0') if m is not None else '0'
                current[depth] = {'cid': cid, 'tx': tx, 'ty': ty}
            elif tt == 'RemoveObject2Tag':
                current.pop(int(child.get('depth', 0)), None)
        break
"
```

**CRITICAL**: Copy translateX/translateY from the SAME charId you're using, not from a different one! Different charIds have different texture sizes → different offsets.

### 4. Edit XML — add new frame

Find `</subTags>` that closes sprite 171. Insert BEFORE it:

**Single-layer icon** (like iconId 371 — no background):
```xml
<item type="RemoveObject2Tag" depth="1" forceWriteAsLong="false"/>
<item type="RemoveObject2Tag" depth="2" forceWriteAsLong="false"/>
<item type="PlaceObject3Tag" ... characterId="CHAR_ID" depth="2" placeFlagHasColorTransform="true" ...>
<matrix ... scaleX="SCALE" scaleY="SCALE" translateX="TX" translateY="TY"/>
<colorTransform ... redAddTerm="R" greenAddTerm="G" blueAddTerm="B" .../>
</item>
<item type="ShowFrameTag" forceWriteAsLong="false"/>
```

**Two-layer icon** (background + overlay, like iconId 397):
```xml
<item type="RemoveObject2Tag" depth="1" forceWriteAsLong="false"/>
<item type="RemoveObject2Tag" depth="2" forceWriteAsLong="false"/>
<!-- Background -->
<item type="PlaceObject3Tag" ... characterId="1000" depth="1" placeFlagHasColorTransform="true" ...>
<matrix ... scaleX="0.6294" scaleY="0.6294" translateX="-770" translateY="-770"/>
<colorTransform ... alphaMultTerm="205" .../>
</item>
<!-- Overlay -->
<item type="PlaceObject3Tag" ... characterId="CHAR_ID" depth="2" placeFlagHasColorTransform="true" ...>
<matrix ... scaleX="0.183" scaleY="0.183" translateX="TX" translateY="TY"/>
<colorTransform ... redAddTerm="R" greenAddTerm="G" blueAddTerm="B" .../>
</item>
<item type="ShowFrameTag" forceWriteAsLong="false"/>
```

Then update `frameCount` on the DefineSpriteTag line for sprite 171.

New iconId = new frameCount value (0-indexed from the old count).

### 5. Compile XML → GFX

```bash
$FFDEC_CLI -xml2swf assets/map_icons/worldmap_new_actual.xml \
                    assets/menu/02_120_worldmap_new.gfx
```

### 6. Render preview

```bash
cd tools
python render_map_icons.py --xml ../assets/map_icons/worldmap_new_actual.xml --frames NEW_ICONID
```

### 7. Build snapshot

```bash
build.bat snapshot
```

---

## Common Mistakes

1. **Wrong translateX/Y**: Each charId has its own offset. Copy from an existing frame that uses the SAME charId.
2. **Leftover depth=1**: If previous frame had a background (depth=1) and your new icon doesn't need one, you MUST add `RemoveObject2Tag depth="1"` to clear it.
3. **Broken TGA**: If rendered icon is blank, check TGA alpha: `max(img.split()[3].getdata())`. If <=1, delete TGA and re-run `extract_subtextures.py`.
4. **Using worldmap_modified.xml**: This file is outdated. Always decompile fresh from the GFX.

## Known charId → Texture Mapping

| charId | exportName | Description |
|--------|-----------|-------------|
| 21 | MENU_MAP_Coop_02 | Four-pointed star (green in-game) |
| 53 | MENU_MAP_Enemy_01 | Enemy crossed-swords icon |
| 57 | MENU_MAP_Friend_01 | Snowflake/rune icon |
| 1000 | MENU_MAP_MemoCursor | Blue drop background (160×160) |
| 1001 | MENU_ItemIcon_18110 | Flask icon |
| 1002 | MENU_Tab_03 | Key item icon |
| 1006 | MENU_Tab_Weapon | Weapon icon |
| 1007 | MENU_Tab_Armor | Armor icon |
| 1013 | MENU_Tab_00 | Consumable/rune icon |
| 1015 | MENU_Tab_01 | Ingredient/crafting icon |

## Color Recipes (colorTransform addTerm values)

| Color | redAdd | greenAdd | blueAdd |
|-------|--------|----------|---------|
| Green (material nodes) | 50 | 100 | 50 |
| Red (scadutree/keys) | 100 | -50 | -50 |
| Gold (golden runes) | 100 | 80 | 0 |
| Pale yellow (low runes) | 60 | 50 | 20 |

## Existing Custom Icons

| iconId | layers | charId | Color | Used for |
|--------|--------|--------|-------|----------|
| 397 | bg(1000) + overlay(1015) | 1015 | Green | Material Nodes |
| 398 | bg(1000) + overlay(1002) | 1002 | Red | Stonesword Keys |
| 399 | bg(1000) + overlay(1013) | 1013 | Gold | Golden Runes |
| 400 | bg(1000) + overlay(1013) | 1013 | Pale yellow | Golden Runes (Low) |
| 401 | single(21) | 21 | Red | Scadutree Fragments |

## Background Opacity

iconId 375-396 and 397-400 all use `alphaMultTerm=205` on depth 1 (80% opacity).
This is set on the PlaceObject3Tag for depth=1 in frame 375 (inherited by 376-396).
