# AEG099 Gathering Nodes - Model to Item Mapping

## Mechanism

Every AEG099_NNN model in an MSB file has a corresponding row `99NNN` in
**AssetEnvironmentGeometryParam** (inside regulation.bin). The field
`pickUpItemLotParamId` points to an **ItemLotParam_map** row, which defines
what item the player receives on pickup.

```
AEG099_NNN (MSB Asset)
  → AssetEnvironmentGeometryParam  row 99NNN
    → pickUpItemLotParamId         (e.g. 998210)
      → ItemLotParam_map           row 998210
        → lotItemId01              goods ID (e.g. 800011)
```

### Key fields in AssetEnvironmentGeometryParam

| Field | Meaning |
|-------|---------|
| `pickUpItemLotParamId` | ItemLotParam_map row ID (-1 = no pickup) |
| `pickUpActionButtonParamId` | Action prompt shown to player |
| `isBreakOnPickUp` | Object disappears after pickup |
| `isEnableRepick` | Object respawns after resting at grace |
| `isHiddenOnRepick` | Object is hidden until respawn |

### Models WITHOUT pickup

These are NOT gathering nodes. They have no `pickUpItemLotParamId` chain, so
they produce **no entry** in `aeg099_item_mapping.json` (rather than a literal
`-1`):

| Model | Purpose |
|-------|---------|
| AEG099_510 | Invisible interaction trigger wired into EMEVD (`ChangeAssetEnableState` in common events 1045632900/1045630910 - asset-enable + warp interactions, NOT the piece award) |
| AEG099_600, 601 | Breakable decoration |
| AEG099_610 | Breakable decoration (bushes, pots etc.) |
| AEG099_620 | Loot corpse / item pickup (linked via MSB Treasure event) |
| AEG099_630-641 | Breakable containers (crates, jars etc.) |
| AEG099_900 | Unknown |
| AEG099_951, 990, 991 | Non-pickup assets |

## Complete Gathering Node Mapping

### Flowers & Plants

Column shows the actual `(isEnableRepick, isHiddenOnRepick)` pair - see the
Repick/Hidden Flags convention: `(True, True)` = one-time, `(False, False)` =
respawning.

| Model | Goods ID | Item | (isEnableRepick, isHiddenOnRepick) |
|-------|----------|------|------------------------------------|
| AEG099_650 | 20650 | Poisonbloom | (False, False) - respawning |
| AEG099_651 | 20651 | Trina's Lily | (True, True) - one-time |
| AEG099_653 | 20653 | Miquella's Lily | (True, True) - one-time |
| AEG099_654 | 20654 | Grave Blossom | (False, False) - respawning |
| AEG099_656 | 20651 | Trina's Lily (variant) | (True, True) - one-time |
| AEG099_657 | 20653 | Miquella's Lily (variant) | (True, True) - one-time |
| AEG099_660 | 20660 | Faded Erdleaf Flower | (False, False) - respawning |
| AEG099_680 | 20680 | Erdleaf Flower | (False, False) - respawning |
| AEG099_681 | 20681 | Altus Bloom | (False, False) - respawning |
| AEG099_682 | 20682 | Fire Blossom | (False, False) - respawning |
| AEG099_683 | 20683 | Golden Sunflower | (False, False) - respawning |
| AEG099_684 | 20652 | Fulgurbloom | (False, False) - respawning |
| AEG099_685 | 20683 | Golden Sunflower (variant) | (False, False) - respawning |
| AEG099_687 | 20652 | Fulgurbloom (variant) | (False, False) - respawning |
| AEG099_690 | 20690 | Herba | (False, False) - respawning |
| AEG099_691 | 20691 | Arteria Leaf | (True, True) - one-time |
| AEG099_696 | 20691 | Arteria Leaf (variant) | (True, True) - one-time |

### Fruits & Berries

| Model | Goods ID | Item |
|-------|----------|------|
| AEG099_720 | 20720 | Rowa Fruit |
| AEG099_721 | 20721 | Golden Rowa |
| AEG099_722 | 20722 | Rimed Rowa |
| AEG099_723 | 20723 | Bloodrose |
| AEG099_730 | 20720 | Rowa Fruit (variant) |
| AEG099_740 | 20740 | Eye of Yelough |

### Crystals & Minerals

| Model | Goods ID | Item |
|-------|----------|------|
| AEG099_750 | 20750 | Crystal Bud |
| AEG099_751 | 20751 | Rimed Crystal Bud |
| AEG099_753 | 20753 | Sacramental Burgeon |
| AEG099_780 | 20780 | Cracked Crystal |
| AEG099_785 | 10090 | Golden Ore |
| AEG099_795 | 20795 | Sanctuary Stone |
| AEG099_796 | 1760 | Ruin Fragment |

### Mushrooms & Moss

| Model | Goods ID | Item |
|-------|----------|------|
| AEG099_760 | 20760 | Mushroom |
| AEG099_761 | 20761 | Melted Mushroom |
| AEG099_770 | 20770 | Toxic Mushroom |
| AEG099_775 | 20775 | Root Resin |
| AEG099_840 | 20840 | Medicinal Moss |
| AEG099_841 | 20841 | Budding Moss |
| AEG099_842 | 20842 | Crystal Moss |
| AEG099_845 | 20845 | Yellow Ember |

### Creatures & Remains

| Model | Goods ID | Item |
|-------|----------|------|
| AEG099_700 | 20700 | ??? |
| AEG099_800 | 20800 | Nascent Butterfly |
| AEG099_801 | 20801 | Aeonian Butterfly |
| AEG099_802 | 20802 | Smoldering Butterfly |
| AEG099_810 | 20810 | Silver Firefly |
| AEG099_811 | 20811 | Gold Firefly |
| AEG099_812 | 20812 | Glintstone Firefly |
| AEG099_820 | 20820 | Golden Centipede |
| AEG099_825 | 20825 | Silver Tear Husk |
| AEG099_830 | 20830 | Gold-Tinged Excrement |
| AEG099_831 | 20831 | Blood-Tainted Excrement |
| AEG099_850 | 20850 | Gaseous Stone |
| AEG099_852 | 20852 | Formic Rock |
| AEG099_855 | 20855 | Gravel Stone |

### Smithing Stones

| Model | Goods ID | Item |
|-------|----------|------|
| AEG099_860 | 10100 | Smithing Stone [1] |
| AEG099_861 | 10101 | Smithing Stone [2] |
| AEG099_862 | 10102 | Smithing Stone [3] |
| AEG099_863 | 10103 | Smithing Stone [4] |
| AEG099_864 | 10104 | Smithing Stone [5] |
| AEG099_865 | 10105 | Smithing Stone [6] |
| AEG099_866 | 10106 | Smithing Stone [7] |
| AEG099_867 | 10107 | Smithing Stone [8] |
| AEG099_868 | 10140 | Ancient Dragon Smithing Stone |
| AEG099_870 | 10160 | Somber Smithing Stone [1] |
| AEG099_871 | 10161 | Somber Smithing Stone [2] |
| AEG099_872 | 10162 | Somber Smithing Stone [3] |
| AEG099_873 | 10163 | Somber Smithing Stone [4] |
| AEG099_874 | 10164 | Somber Smithing Stone [5] |
| AEG099_875 | 10165 | Somber Smithing Stone [6] |
| AEG099_876 | 10166 | Somber Smithing Stone [7] |
| AEG099_877 | 10167 | Somber Smithing Stone [8] |
| AEG099_878 | 10168 | Somber Ancient Dragon Smithing Stone |
| AEG099_879 | 10200 | Somber Smithing Stone [9] |

### Smithing Scadushards (DLC)

| Model | Goods ID | Quantity | Item |
|-------|----------|----------|------|
| AEG099_880 | 10150 | 1 | Smithing Scadushard |
| AEG099_881 | 10150 | 3 | Smithing Scadushard |
| AEG099_882 | 10150 | 6 | Smithing Scadushard |
| AEG099_883 | 10150 | 9 | Smithing Scadushard |
| AEG099_884 | 10150 | 14 | Smithing Scadushard |
| AEG099_885 | 10150 | 18 | Smithing Scadushard |
| AEG099_886 | 10150 | 24 | Smithing Scadushard |
| AEG099_887 | 10150 | 30 | Smithing Scadushard |
| AEG099_890 | 10151 | 2 | Somber Smithing Scadushard |
| AEG099_891 | 10151 | 6 | Somber Smithing Scadushard |
| AEG099_892 | 10151 | 8 | Somber Smithing Scadushard |
| AEG099_893 | 10151 | 12 | Somber Smithing Scadushard |
| AEG099_894 | 10151 | 18 | Somber Smithing Scadushard |
| AEG099_895 | 10151 | 24 | Somber Smithing Scadushard |
| AEG099_896 | 10151 | 32 | Somber Smithing Scadushard |
| AEG099_897 | 10151 | 40 | Somber Smithing Scadushard |

### Gloveworts (Spirit Ash upgrade)

| Model | Goods ID | Item |
|-------|----------|------|
| AEG099_920 | 10900 | Grave Glovewort [1] |
| AEG099_921 | 10901 | Grave Glovewort [2] |
| AEG099_922 | 10902 | Grave Glovewort [3] |
| AEG099_923 | 10903 | Grave Glovewort [4] |
| AEG099_924 | 10904 | Grave Glovewort [5] |
| AEG099_925 | 10905 | Grave Glovewort [6] |
| AEG099_926 | 10906 | Grave Glovewort [7] |
| AEG099_927 | 10907 | Grave Glovewort [8] |
| AEG099_928 | 10908 | Grave Glovewort [9] |
| AEG099_929 | 10909 | Great Grave Glovewort |
| AEG099_930 | 10910 | Ghost Glovewort [1] |
| AEG099_931 | 10911 | Ghost Glovewort [2] |
| AEG099_932 | 10912 | Ghost Glovewort [3] |
| AEG099_933 | 10913 | Ghost Glovewort [4] |
| AEG099_934 | 10914 | Ghost Glovewort [5] |
| AEG099_935 | 10915 | Ghost Glovewort [6] |
| AEG099_936 | 10916 | Ghost Glovewort [7] |
| AEG099_937 | 10917 | Ghost Glovewort [8] |
| AEG099_938 | 10918 | Ghost Glovewort [9] |
| AEG099_939 | 10919 | Great Ghost Glovewort |

### Rune / Ember Pieces (ERR custom)

| Model | Goods ID | Item |
|-------|----------|------|
| AEG099_821 | 800011 | Runic Trace |
| AEG099_822 | 850011 | Ember Trace |

Note: AEG099_821 (Rune Piece) and AEG099_822 (Ember Piece) are asset pickups like the
other gathering nodes - AEG099_821 has a pickUpItemLotParamId chain (primary goods Runic
Trace 800011), AEG099_822 → Ember Trace 850011. EMEVD event 1045630910 is a warp/teleport
interaction (WarpPlayer) and event 1045632900 toggles asset enable-state
(ChangeAssetEnableState over 1045631100…) - neither event awards the piece. The mod
generates 821/822 specially in generate_pieces_massedit.py.

### Runes (currency)

| Model | Goods ID | Qty | Item |
|-------|----------|-----|------|
| AEG099_790 | 13900 | 10 | ??? (Runes?) |
| AEG099_791 | 13900 | 20 | ??? (Runes?) |
| AEG099_792 | 13900 | 30 | ??? (Runes?) |

## DLC Gathering Nodes: AEG463

DLC (Shadow of the Erdtree) uses **AEG463_NNN** models instead of AEG099.

### Mechanism
Same as AEG099, but row IDs are in 463000 range:
```
AEG463_NNN (MSB Asset in m61_XX_YY_00 tile)
  → AssetEnvironmentGeometryParam row 463NNN
    → pickUpItemLotParamId → ItemLotParam_map → goods ID
```

### DLC One-Time Nodes (repick=True, hidden=True)

| Model | Goods ID | Item |
|-------|----------|------|
| AEG463_771 | 2020015 | Nectarblood Burgeon |
| AEG463_781 | 2020017 | Swollen Grape |
| AEG463_840 | 2020023 | Dragon's Calorbloom |
| AEG463_850 | 2020024 | Finger Mimic |
| AEG463_860 | 20753 | Sacramental Burgeon |
| AEG463_920 | 2020031 | Blessed Bone Shard |
| AEG463_950 | 20855 | Gravel Stone |
| AEG463_960 | 2020035 | Furnace Visage |

### DLC Respawning Nodes (repick=False, hidden=False)

| Model | Goods ID | Item |
|-------|----------|------|
| AEG463_650 | 2020001 | Rada Fruit |
| AEG463_660/661 | 20760 | Mushroom |
| AEG463_670 | 20775 | Root Resin |
| AEG463_680 | 2020005 | Dewgem |
| AEG463_690 | 2020006 | Black Pyrefly |
| AEG463_700 | 20812 | Glintstone Firefly |
| AEG463_710 | 20652 | Fulgurbloom |
| AEG463_720 | 2020009 | Shadow Sunflower |
| AEG463_730 | 2020010 | Toxic Mossling |
| AEG463_740 | 2020011 | Scarlet Bud |
| AEG463_750 | 2020012 | Sanguine Amaryllis |
| AEG463_760 | 2020013 | Frozen Maggot |
| AEG463_770 | 2020014 | Deep-Purple Lily |
| AEG463_780 | 2020016 | Winter-Lantern Fly |
| AEG463_790 | 2020018 | Grave Keeper's Brainpan |
| AEG463_800 | 20830 | Gold-Tinged Excrement |
| AEG463_810 | 20850 | Gaseous Stone |
| AEG463_820 | 20654 | Grave Blossom |
| AEG463_830 | 2020022 | Grave Cricket |
| AEG463_870 | 2020026 | Congealed Putrescence |
| AEG463_880 | 2020027 | Roundrock |
| AEG463_890 | 2020028 | Spiritgrave Stone |
| AEG463_900 | 2020029 | Rauh Burrow |
| AEG463_910 | 2020030 | Ember of Messmer |
| AEG463_930 | 2020032 | Red Fulgurbloom |
| AEG463_940 | 2020033 | Nailstone |
| AEG777_800 | 20781 | Magnetic Ore |

## Repick/Hidden Flags (important!)

Naming is counterintuitive:
- `isEnableRepick=True` + `isHiddenOnRepick=True` = **ONE-TIME** pickup (disappears permanently)
- `isEnableRepick=False` + `isHiddenOnRepick=False` = **RESPAWNING** node (returns after rest)

## Data Files

- `data/aeg099_item_mapping.json` - AEG099 mapping (285 models)
- `data/aeg463_item_mapping.json` - AEG463 DLC mapping (36 models)
- `data/all_gathering_nodes_final.json` - all AEG099+AEG463 positions from MSBs (21824 nodes: 17082 AEG099 + 4742 AEG463; count drifts with each MSB re-extraction)
- `data/massedit_generated/` - auto-generated MASSEDIT files

## How to regenerate

Mapping extraction is split across **two** scripts, one per model family:

- `tools/extract_aeg099_mapping.py` - base game, rows 99NNN → `data/aeg099_item_mapping.json`
- `tools/extract_aeg463_mapping.py` - DLC, rows 463NNN → `data/aeg463_item_mapping.json`

Each script:
1. Reads AssetEnvironmentGeometryParam for its row range (99000-99999 / 463000-463999)
2. For each row with a valid pickUpItemLotParamId, reads ItemLotParam_map
3. Looks up goods names from GoodsName FMG
4. Outputs its respective JSON mapping file

## Consumption / generation

The one-time nodes (`isEnableRepick && isHiddenOnRepick`) feed
`tools/generate_material_nodes.py`, which:

- Skips nodes that sit below reachable terrain in ERR, detected via
  `unreachable.is_unreachable_in_err` (`unreachable.py`).
- Wires collection-hide flags (`textDisableFlagId1` / `textDisableFlagId2`)
  from `data/gathering_node_flags.json` for nodes that have an EMEVD-derived
  `(tile, entity_id)` binding. Nodes with `entity_id=0` fall back to the
  runtime `collected::refresh()`.

Note: AEG099_821 / AEG099_822 (Rune / Ember Pieces) are excluded here and
handled separately by `generate_pieces_massedit.py`.
