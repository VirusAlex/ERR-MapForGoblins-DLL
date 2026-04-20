# AEG099 Gathering Nodes — Model to Item Mapping

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

### Models WITHOUT pickup (lot = -1)

These are NOT gathering nodes:

| Model | Purpose |
|-------|---------|
| AEG099_510 | Invisible interaction trigger (used by EMEVD for Rune Pieces) |
| AEG099_600, 601 | Breakable decoration |
| AEG099_610 | Breakable decoration (bushes, pots etc.) |
| AEG099_620 | Loot corpse / item pickup (linked via MSB Treasure event) |
| AEG099_630-641 | Breakable containers (crates, jars etc.) |
| AEG099_900 | Unknown |
| AEG099_951, 990, 991 | Non-pickup assets |

## Complete Gathering Node Mapping

### Flowers & Plants

| Model | Goods ID | Item | repick |
|-------|----------|------|--------|
| AEG099_650 | 20650 | Poisonbloom | yes |
| AEG099_651 | 20651 | Trina's Lily | yes |
| AEG099_653 | 20653 | Miquella's Lily | yes |
| AEG099_654 | 20654 | Grave Blossom | yes |
| AEG099_656 | 20651 | Trina's Lily (variant) | ? |
| AEG099_657 | 20653 | Miquella's Lily (variant) | ? |
| AEG099_660 | 20660 | Faded Erdleaf Flower | no |
| AEG099_680 | 20680 | Erdleaf Flower | no |
| AEG099_681 | 20681 | Altus Bloom | no |
| AEG099_682 | 20682 | Fire Blossom | no |
| AEG099_683 | 20683 | Golden Sunflower | no |
| AEG099_684 | 20652 | Fulgurbloom | no |
| AEG099_685 | 20683 | Golden Sunflower (variant) | no |
| AEG099_687 | 20652 | Fulgurbloom (variant) | no |
| AEG099_690 | 20690 | Herba | no |
| AEG099_691 | 20691 | Arteria Leaf | no |
| AEG099_696 | 20691 | Arteria Leaf (variant) | no |

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

Note: Rune Piece (800010) is awarded separately through EMEVD event 1045630910
via nearby AEG099_510 triggers, not through the pickup mechanism.

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

- `data/aeg099_item_mapping.json` — AEG099 mapping (285 models)
- `data/aeg463_item_mapping.json` — AEG463 DLC mapping (36 models)
- `data/all_gathering_nodes_final.json` — all AEG099+AEG463 positions from MSBs (20505 nodes)
- `data/massedit_generated/` — auto-generated MASSEDIT files

## How to regenerate

Run `tools/extract_aeg099_mapping.py` to re-extract from regulation.bin.
This script should:
1. Read AssetEnvironmentGeometryParam for rows 99000-99999 (AEG099) and 463000-463999 (AEG463)
2. For each row with pickUpItemLotParamId != -1, read ItemLotParam_map
3. Look up goods names from GoodsName FMG
4. Output `data/aeg099_item_mapping.json`
