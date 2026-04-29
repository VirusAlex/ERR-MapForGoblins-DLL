#!/usr/bin/env python3
"""
Generate Loot MASSEDIT files from items_database.json.
Fully automatic — no dependency on existing MASSEDIT files.
Uses goodsId as textId1 for localized names via GoodsName FMG.

Output: data/massedit_generated/Loot - <category>.MASSEDIT
"""

import json
import re
from pathlib import Path
from collections import defaultdict, Counter

from massedit_common import (DATA_DIR, OUT_DIR, UNDERGROUND_AREAS, DLC_AREAS,
                             OVERWORLD_AREAS, VALID_LOCATION_IDS, resolve_location_id,
                             get_disp_mask)
DB_PATH = DATA_DIR / 'items_database.json'

# Goods sortGroupId lookup (goodsType=0 items only) for consumable filtering
_sort_groups_path = DATA_DIR / 'goods_sort_groups.json'
GOODS_SORT_GROUPS = {}
if _sort_groups_path.exists():
    with open(_sort_groups_path) as _f:
        GOODS_SORT_GROUPS = {int(k): v for k, v in json.load(_f).items()}

# sortGroupId values for combat consumables:
#   20 = healing/buffs (boluses, cured meats, dried livers, flesh)
#   50 = throwing items/tools (darts, daggers, stones, chakrams)
#   70 = greases
#   80 = utility (rainbow stone, glowstone, soap, soft cotton)
CONSUMABLE_SORT_GROUPS = {20, 50, 70, 80}

# Prattling Pate IDs — excluded from Consumables/Utilities to avoid
# double-markers (they have their own dedicated category).
PATE_IDS = {2200, 2201, 2202, 2203, 2204, 2205, 2206, 2207, 2002150}


def is_consumable_goods(item_id):
    """Check if a goods item is a combat consumable by sortGroupId."""
    return GOODS_SORT_GROUPS.get(item_id, -1) in CONSUMABLE_SORT_GROUPS


# Crafting material IDs (goodsType=2)
_crafting_path = DATA_DIR / 'goods_crafting_ids.json'
CRAFTING_IDS = set()
if _crafting_path.exists():
    with open(_crafting_path) as _f:
        CRAFTING_IDS = set(json.load(_f))

# Spirit Ash IDs (goodsType=8)
_spirit_path = DATA_DIR / 'goods_spirit_ash_ids.json'
SPIRIT_ASH_IDS = set()
if _spirit_path.exists():
    with open(_spirit_path) as _f:
        SPIRIT_ASH_IDS = set(json.load(_f))

# Sorcery IDs (goodsType=17)
_sorc_path = DATA_DIR / 'goods_sorcery_ids.json'
SORCERY_IDS = set()
if _sorc_path.exists():
    with open(_sorc_path) as _f:
        SORCERY_IDS = set(json.load(_f))

# Incantation IDs (goodsType=18)
_incan_path = DATA_DIR / 'goods_incantation_ids.json'
INCANTATION_IDS = set()
if _incan_path.exists():
    with open(_incan_path) as _f:
        INCANTATION_IDS = set(json.load(_f))

# Which item names go into which file, with iconId and start row ID
# Filter by item goodsId or item name
# Equipment categories use ItemLotParam category (2=weapon, 3=armour, 4=accessory, 5=gem)
LOOT_CATEGORIES = {
    'Key - Celestial Dew': {
        'filter': lambda items: any(i['id'] == 2130 and i['category'] == 1 for i in items),
        'iconId': 419,
        'startId': 3050000,
    },
    'Key - Cookbooks': {
        'filter': lambda items: any(
            i['category'] == 1 and 'cookbook' in i.get('name', '').lower()
            for i in items
        ),
        'iconId': 424,
        'startId': 3100000,
    },
    'Key - Crystal Tears': {
        # Crystal Tears, Cracked Tears, Hardtears, Bubbletears, Hidden Tears
        'filter': lambda items: any(
            i['category'] == 1 and ('crystal tear' in i.get('name', '').lower()
                or 'cracked tear' in i.get('name', '').lower()
                or 'hardtear' in i.get('name', '').lower()
                or 'bubbletear' in i.get('name', '').lower()
                or 'hidden tear' in i.get('name', '').lower()
                or 'oil-soaked tear' in i.get('name', '').lower())
            for i in items
        ),
        'iconId': 392,
        'startId': 3200000,
    },
    'Key - Imbued Sword Keys': {
        'filter': lambda items: any(i['id'] == 8186 and i['category'] == 1 for i in items),
        'iconId': 432,
        'startId': 3300000,
    },
    'Key - Larval Tears': {
        'filter': lambda items: any(
            i['category'] == 1 and i['id'] in (8185, 2008033)
            for i in items
        ),
        'iconId': 418,
        'startId': 3400000,
    },
    'Key - Lost Ashes': {
        'filter': lambda items: any(i['id'] == 10070 and i['category'] == 1 for i in items),
        'iconId': 396,
        'startId': 3500000,
    },
    'Key - Pots n Perfumes': {
        'filter': lambda items: any(
            i['category'] == 1 and i['id'] in (9500, 9501, 9510, 2009500)
            for i in items
        ),
        'iconId': 425,
        'startId': 3600000,
    },
    'Key - Seeds Tears Ashes': {
        # Golden Seeds, Sacred Tears, Revered Spirit Ashes
        'filter': lambda items: any(
            i['category'] == 1 and i['id'] in (10010, 10020, 2010100)
            for i in items
        ),
        'iconId': 377,
        'startId': 3700000,
    },
    'Key - Scadutree Fragments': {
        'filter': lambda items: any(
            i['category'] == 1 and i['id'] == 2010000
            for i in items
        ),
        'iconId': 401,
        'startId': 3750000,
    },
    'Key - Whetblades': {
        'filter': lambda items: any(
            i['category'] == 1 and i['id'] in (8970, 8971, 8972, 8973, 8974)
            for i in items
        ),
        'iconId': 431,
        'startId': 3800000,
    },
    'Quest - Deathroot': {
        'filter': lambda items: any(i['id'] == 2090 and i['category'] == 1 for i in items),
        'iconId': 428,
        'startId': 2300000,
    },
    'Quest - Seedbed Curses': {
        'filter': lambda items: any(i['id'] == 8193 and i['category'] == 1 for i in items),
        'iconId': 430,
        'startId': 2100000,
    },
    'Quest - Progression': {
        # Key quest items: medallions, keys, quest-specific goods
        'filter': lambda items: any(i['id'] in (
            8174, 8109,  # Academy Glintstone Key (+ variant)
            8105, 8106,  # Dectus Medallion (Left/Right)
            8176, 8177,  # Haligtree Secret Medallion (Left/Right)
            8159,   # Fingerslayer Blade
            1240,   # Shabriri Grape
            8121,   # Dark Moon Ring
            8010,   # Rusty Key
            8162,   # Gold Sewing Needle
            8129,   # Serpent's Amnion
            8191,   # Cursemark of Death
            8166,   # The Stormhawk King
            8198,   # Meeting Place Map
            8171,   # Chrysalids' Memento
            8142,   # Amber Starlight
            2008034,  # Message from Leda
            2002120,  # Iris of Grace
            2002130,  # Iris of Occultation
            2190,     # Miquella's Needle
            8196,     # Unalloyed Gold Needle
            8714,     # Note: Miquella's Needle
            8716,     # Note: The Lord of Frenzied Flame
            8183,     # Mending Rune of the Death-Prince
            8867,     # Three Fingers' Scrawls
            2008004,  # Well Depths Key
            2008005,  # Gaol Upper Level Key
            2008006,  # Gaol Lower Level Key
            2008013,  # Storeroom Key
            2008023,  # Keep Wall Key
        ) and i['category'] == 1 for i in items),
        'iconId': 376,
        'startId': 2200000,
    },
    'Loot - Rada Fruit': {
        # DLC stat-up consumable (ERR-tracked)
        'filter': lambda items: any(
            i['id'] == 2020001 and i['category'] == 1 for i in items
        ),
        'iconId': 437,
        'startId': 3850000,
    },
    'Reforged - Items': {
        # ERR-added items: Oracle Effigy/Remedy, Starlight Tokens
        'filter': lambda items: any(i['id'] in (
            900000,    # Oracle Effigy
            900010,    # Oracle's Remedy
            22000,     # Starlight Token
        ) and i['category'] == 1 for i in items),
        'iconId': 421,
        'startId': 3900000,
    },
    'Reforged - Fortunes': {
        # 12 Fortune types (ERR-specific stat/resistance trinkets)
        'filter': lambda items: any(i['id'] in (
            900218,  # Fortune of the Houses
            900238,  # Fortune of the Godslayers
            900258,  # Fortune of Haima
            900268,  # Fortune of the Crucible
            900278,  # Fortune of the Dynasts
            900288,  # Fortune of the Warmaster
            900308,  # Fortune of the Bold
            900318,  # Fortune of the Wise
            900328,  # Fortune of the Cunning
            900338,  # Fortune of the Bulwark
            900348,  # Fortune of the Reeds
            900368,  # Fortune of the Brave
        ) and i['category'] == 1 for i in items),
        'iconId': 422,
        'startId': 3920000,
    },
    'Reforged - Sealed Curios': {
        # ERR-added Sealed Curio family (9 types)
        'filter': lambda items: any(i['id'] in (
            1301900,   # Sealed Knifeprint Curio
            1302900,   # Sealed Poacher's Curio
            1303900,   # Physician's Sealed Curio
            1304900,   # Sealed Scadutear Curio
            1305900,   # Sealed Fanatic's Curio
            1306900,   # Sealed Gate Curio
            1307900,   # Sealed Curio of Ranah
            1308900,   # Sealed Academy Curio
            1309900,   # Sealed Dragonscale Curio
        ) and i['category'] == 1 for i in items),
        'iconId': 423,
        'startId': 3950000,
    },
    'Equipment - Armaments': {
        # Weapons (cat=2), excluding ammo (id>=50M, already in Loot - Ammo)
        'filter': lambda items: any(i['category'] == 2 and i['id'] < 50000000 for i in items),
        'iconId': 380,
        'startId': 4000000,
    },
    'Equipment - Armour': {
        'filter': lambda items: any(i['category'] == 3 for i in items),
        'iconId': 381,
        'startId': 4100000,
    },
    'Equipment - Talismans': {
        'filter': lambda items: any(i['category'] == 4 for i in items),
        'iconId': 382,
        'startId': 4200000,
    },
    'Equipment - Spirits': {
        # Spirit Ashes — goods IDs 300000-399999
        # Exclude Lhutel (358000) — ERR EMEVD placeholder with shared flag 520000
        'filter': lambda items: any(
            i['category'] == 1 and 300000 <= i['id'] <= 399999 and i['id'] != 358000
            for i in items
        ),
        'iconId': 383,
        'startId': 4300000,
    },
    'Equipment - Ashes of War': {
        'filter': lambda items: any(i['category'] == 5 for i in items),
        'iconId': 384,
        'startId': 4400000,
    },
    'Magic - Incantations': {
        'filter': lambda items: any(i['category'] == 1 and i['id'] in INCANTATION_IDS for i in items),
        'iconId': 385,
        'startId': 4500000,
    },
    'Magic - Sorceries': {
        'filter': lambda items: any(i['category'] == 1 and i['id'] in SORCERY_IDS for i in items),
        'iconId': 386,
        'startId': 4600000,
    },
    'Magic - Memory Stones': {
        'filter': lambda items: any(i['id'] == 10030 and i['category'] == 1 for i in items),
        'iconId': 429,
        'startId': 4700000,
    },
    'Magic - Prayerbooks': {
        # Prayerbooks and Scrolls that unlock spells at vendors
        'filter': lambda items: any(
            i['category'] == 1 and i['id'] in (
                8850, 8851, 8852, 8854,  # Conspectus, Royal House, Ranni's, Gelmirian Scrolls
                8855, 8856, 8857, 8858,  # Fire Monks', Giant's, Godskin, Two Fingers' Prayerbooks
                8859, 8862, 8864, 8865, 8866,  # Assassin's, Golden Order, Dragon Cult, Ancient Dragon, Academy
                2008014,  # Secret Rite Scroll (DLC)
            )
            for i in items
        ),
        'iconId': 427,
        'startId': 4800000,
    },
    'Loot - Stonesword Keys': {
        'filter': lambda items: any(i['id'] == 8000 and i['category'] == 1 for i in items),
        'iconId': 398,  # custom: key icon, 75% size, 80% bg, red tint
        'startId': 5000000,
    },
    'Loot - Bell-Bearings': {
        # Bell bearings from treasures / EMEVD awards (chests, quest rewards).
        'filter': lambda items: any('bell bearing' in i.get('name', '').lower() for i in items),
        'source_filter': lambda rec: rec.get('source') != 'enemy',
        'iconId': 426,
        'startId': 5100000,
    },
    'Loot - Merchant Bell-Bearings': {
        # Bell bearings dropped by killed merchant NPCs (Kalé, Patches, Gostoc,
        # nomadic/hermit/isolated merchants, etc.). Separated so players can
        # toggle merchant-killing rewards independently of chest pickups.
        'filter': lambda items: any('bell bearing' in i.get('name', '').lower() for i in items),
        'source_filter': lambda rec: rec.get('source') == 'enemy',
        'iconId': 426,
        'startId': 5150000,
    },
    'Loot - Ammo': {
        # Ammo = weapon category (cat=2) with IDs 50000000+ (arrows/bolts/greatbolts)
        'filter': lambda items: any(i['category'] == 2 and i['id'] >= 50000000 for i in items),
        'iconId': 388,
        'startId': 5200000,
    },
    'Loot - Smithing Stones (Low)': {
        # Smithing Stone [1]-[6], Somber [1]-[6]
        'filter': lambda items: any(i['id'] in (
            10100, 10101, 10102, 10103, 10104, 10105,  # Smithing Stone [1]-[6]
            10160, 10161, 10162, 10163, 10164, 10165,  # Somber [1]-[6]
        ) and i['category'] == 1 for i in items),
        'iconId': 433,
        'startId': 5300000,
    },
    'Loot - Smithing Stones': {
        # Smithing Stone [7]-[8], Somber [7]-[9], Scadushards
        'filter': lambda items: any(i['id'] in (
            10106, 10107,          # Smithing Stone [7]-[8]
            10166, 10167, 10200,   # Somber [7]-[9]
            10150, 10151,          # Smithing Scadushard, Somber Smithing Scadushard
        ) and i['category'] == 1 for i in items),
        'iconId': 378,
        'startId': 5350000,
    },
    'Loot - Smithing Stones (Rare)': {
        # Ancient Dragon Smithing Stone, Somber Ancient Dragon Smithing Stone
        'filter': lambda items: any(i['id'] in (
            10140,  # Ancient Dragon Smithing Stone
            10168,  # Somber Ancient Dragon Smithing Stone
        ) and i['category'] == 1 for i in items),
        'iconId': 434,
        'startId': 5380000,
    },
    'Loot - Golden Runes (Low)': {
        # Golden Rune [200]-[3000], Broken Rune [500], Shadow Realm [2500]-[5000]
        'filter': lambda items: any(i['id'] in (
            2900, 2901, 2902, 2903, 2904, 2905, 2906, 2907,  # Golden Rune [200]-[3000]
            2002951,       # Broken Rune [500]
            2002952, 2002953,  # Shadow Realm Rune [2500]-[5000]
        ) and i['category'] == 1 for i in items),
        'iconId': 400,  # custom: consumable icon, 75% size, 80% bg, pale yellow tint
        'startId': 5400000,
    },
    'Loot - Golden Runes': {
        # Golden Rune [4000]+, Hero's, Numen's, Lord's, Shadow Realm [7500]+, Marika's
        'filter': lambda items: any(i['id'] in (
            2908, 2909, 2910, 2911, 2912,  # Golden Rune [4000]-[10000]
            2913,          # Numen's Rune [12500]
            2914, 2915, 2916, 2917, 2918,  # Hero's Rune [15000]-[35000]
            2919,          # Lord's Rune [50000]
            2002954, 2002955, 2002956, 2002957, 2002958,  # Shadow Realm [7500]-[30000]
            2002959,       # Rune of an Unsung Hero [50000]
            2002960,       # Marika's Rune [80000]
        ) and i['category'] == 1 for i in items),
        'iconId': 399,  # custom: consumable icon, 75% size, 80% bg, golden tint
        'startId': 5450000,
    },
    'Loot - Rune Arcs': {
        'filter': lambda items: any(i['id'] == 150 and i['category'] == 1 for i in items),
        'iconId': 411,
        'startId': 5500000,
    },
    'Loot - Dragon Hearts': {
        'filter': lambda items: any(i['id'] == 10060 and i['category'] == 1 for i in items),
        'iconId': 413,
        'startId': 5510000,
    },
    'Loot - Gloveworts': {
        # Grave Glovewort [1-9] + Ghost Glovewort [1-9]
        'filter': lambda items: any(
            i['category'] == 1 and (10900 <= i['id'] <= 10908 or 10910 <= i['id'] <= 10918)
            for i in items
        ),
        'iconId': 435,
        'startId': 5900000,
    },
    'Loot - Great Gloveworts': {
        # Great Grave Glovewort (10909) + Great Ghost Glovewort (10919)
        'filter': lambda items: any(
            i['id'] in (10909, 10919) and i['category'] == 1 for i in items
        ),
        'iconId': 436,
        'startId': 5950000,
    },
    'Loot - Consumables': {
        # Healing/buff consumables: boluses, cured meats, dried livers (sortGroup=20),
        # plus ERR-specific consumables (sortGroup=61 — Lamp Oil etc).
        # Prattling Pates are excluded (they have their own category).
        'filter': lambda items: any(
            i['category'] == 1
            and GOODS_SORT_GROUPS.get(i['id'], -1) in (20, 61)
            and i['id'] not in PATE_IDS
            for i in items
        ),
        'iconId': 387,
        'startId': 5600000,
    },
    'Loot - Greases': {
        # Weapon-buff greases (sortGroup=70)
        'filter': lambda items: any(
            i['category'] == 1 and GOODS_SORT_GROUPS.get(i['id'], -1) == 70
            for i in items
        ),
        'iconId': 408,
        'startId': 5610000,
    },
    'Loot - Utilities': {
        # Utility items: rainbow stones, glowstones, soap, soft cotton (sortGroup=80)
        # Excludes Prattling Pates (own category).
        'filter': lambda items: any(
            i['category'] == 1
            and GOODS_SORT_GROUPS.get(i['id'], -1) == 80
            and i['id'] not in PATE_IDS
            for i in items
        ),
        'iconId': 412,
        'startId': 5620000,
    },
    'Loot - Stat Boosts': {
        # Permanent/session stat-up: Starlight Shards, Sacrificial Twig,
        # Blessing of Marika, Sign of the All-Knowing (sortGroup=10).
        # Excludes Rune Arc (id 150, also sg=10) — it belongs to Unique Drops.
        'filter': lambda items: any(
            i['category'] == 1
            and GOODS_SORT_GROUPS.get(i['id'], -1) == 10
            and i['id'] != 150  # Rune Arc → Unique Drops
            for i in items
        ),
        'iconId': 410,
        'startId': 5630000,
    },
    'Loot - Throwables': {
        # Throwing items and tools: darts, daggers, stones, chakrams, DLC throwables
        # sortGroupId=50
        'filter': lambda items: any(
            i['category'] == 1 and GOODS_SORT_GROUPS.get(i['id'], -1) == 50
            for i in items
        ),
        'iconId': 409,
        'startId': 5650000,
    },
    'Loot - Crafting Materials': {
        # Gathering ingredients: herbs, bones, bugs, flowers, etc. (goodsType=2)
        'filter': lambda items: any(
            i['category'] == 1 and i['id'] in CRAFTING_IDS
            for i in items
        ),
        'iconId': 389,
        'startId': 5750000,
    },
    'Loot - Reusables': {
        # Multi-use tools (sortGroupId=60, goodsType=0, excluding AoW and Great Runes)
        'filter': lambda items: any(
            i['category'] == 1
            and GOODS_SORT_GROUPS.get(i['id'], -1) == 60
            and i['id'] < 4000000  # exclude Ashes of War goods
            and i['id'] not in (2008000,)  # exclude Miquella's Great Rune
            for i in items
        ),
        'iconId': 416,
        'startId': 5800000,
    },
    'Loot - MP-Fingers': {
        # Multiplayer items: Furled Fingers, Recusant Finger, etc.
        'filter': lambda items: any(i['id'] in (
            100,   # Tarnished's Furled Finger
            101,   # Duelist's Furled Finger
            103,   # Finger Severer
            104,   # Host's Injured Finger
            105,   # Redeemer's Plated Finger
            106,   # Tarnished's Wizened Finger
            110,   # Small Red Effigy
            111,   # Festering Bloody Finger
            112,   # Recusant Finger
        ) and i['category'] == 1 for i in items),
        'iconId': 414,
        'startId': 5850000,
    },
    'Loot - Prattling Pates': {
        'filter': lambda items: any(i['id'] in (
            2200, 2201, 2202, 2203, 2204, 2205, 2206, 2207,  # base game
            2002150,  # DLC: "Lamentation"
        ) and i['category'] == 1 for i in items),
        'iconId': 415,
        'startId': 5700000,
    },
}


def deduplicate(records):
    """Remove _00/_10 MSB duplicates by (primary item id, rounded coords)."""
    seen = set()
    unique = []
    dupes = 0
    for rec in records:
        primary_id = rec['items'][0]['id'] if rec['items'] else 0
        key = (primary_id, round(rec['x'], 1), round(rec['y'], 1), round(rec['z'], 1))
        if key in seen:
            dupes += 1
            continue
        seen.add(key)
        unique.append(rec)
    return unique, dupes


def load_enemy_names():
    """Load enemy model -> TutorialTitle FMG ID mapping."""
    path = DATA_DIR / 'enemy_tutorial_mapping.json'
    if path.exists():
        with open(path) as f:
            return json.load(f)
    return {}

ENEMY_NAMES = load_enemy_names()


def load_tutorial_ids():
    """Load set of all valid TutorialTitle main entry IDs (for variant validation)."""
    path = DATA_DIR / 'tutorial_title_ids.json'
    if path.exists():
        with open(path) as f:
            return set(json.load(f))
    return set()

TUTORIAL_IDS = load_tutorial_ids()


def _load_tutorial_names():
    """Load TutorialTitle ID -> clean name (without number prefix)."""
    path = DATA_DIR / 'tutorial_title_names.json'
    if path.exists():
        with open(path, encoding='utf-8') as f:
            return {int(k): v for k, v in json.load(f).items()}
    return {}

TUTORIAL_NAMES = _load_tutorial_names()


def resolve_enemy_tutorial_id(enemy_model, npc_param_id, vanilla_place_name=None):
    """Resolve variant-specific TutorialTitle ID from NpcParam.

    If vanilla_place_name is provided, tries to match by text first
    (finds the variant whose TutorialTitle text matches the PlaceName).
    Falls back to NpcParam variant digit formula.
    """
    base_id = ENEMY_NAMES.get(enemy_model, 0)
    if base_id <= 0:
        return 0

    # Strategy 1: match by vanilla PlaceName text
    if vanilla_place_name:
        target = vanilla_place_name.lower().strip()
        for variant in range(10):
            vid = base_id + variant * 100
            tut_name = TUTORIAL_NAMES.get(vid, '')
            if tut_name and tut_name.lower().strip() == target:
                return vid

    # Strategy 2: NpcParam variant digit
    if npc_param_id <= 0:
        return base_id
    variant = (npc_param_id // 1000) % 10
    if variant == 0:
        return base_id
    variant_id = base_id + variant * 100
    if variant_id in TUTORIAL_IDS:
        return variant_id
    return base_id


def write_massedit(records, filepath, icon_id, start_id):
    """Write MASSEDIT file + slots JSON from records."""
    lines = []
    row_id = start_id

    for rec in records:
        area = rec['areaNo']
        gx = rec['gridX']
        gz = rec['gridZ']

        # Primary item ID for localized text, offset-encoded by item category:
        #   cat=1 (goods):    id as-is         → DLL reads GoodsName FMG
        #   cat=2 (weapon):   id + 100000000   → DLL reads WeaponName FMG
        #   cat=3 (armour):   id + 200000000   → DLL reads ProtectorName FMG
        #   cat=4 (talisman): id + 300000000   → DLL reads AccessoryName FMG
        #   cat=5 (gem/aow):  id + 400000000   → DLL reads GemName FMG
        CATEGORY_OFFSETS = {1: 500000000, 2: 100000000, 3: 200000000, 4: 300000000, 5: 400000000}
        primary_item = rec['items'][0] if rec['items'] else {}
        item_id = primary_item.get('id', 0)
        item_cat = primary_item.get('category', 1)
        text_id1 = item_id + CATEGORY_OFFSETS.get(item_cat, 0) if item_id > 0 else 0

        # Determine display mask
        if area in UNDERGROUND_AREAS:
            disp = 'dispMask01'
        elif area in DLC_AREAS:
            disp = 'pad2_0'
        else:
            disp = 'dispMask00'

        lines.append(f'param WorldMapPointParam: id {row_id}: iconId: = {icon_id};')
        lines.append(f'param WorldMapPointParam: id {row_id}: {disp}: = 1;')
        lines.append(f'param WorldMapPointParam: id {row_id}: areaNo: = {area};')

        if area in OVERWORLD_AREAS or area in DLC_AREAS or gx > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: gridXNo: = {gx};')
        if area in OVERWORLD_AREAS or area in DLC_AREAS or gz > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: gridZNo: = {gz};')

        if rec['x'] != 0.0 or rec['z'] != 0.0:
            lines.append(f'param WorldMapPointParam: id {row_id}: posX: = {rec["x"]:.3f};')
            if rec['y'] != 0.0:
                lines.append(f'param WorldMapPointParam: id {row_id}: posY: = {rec["y"]:.3f};')
            lines.append(f'param WorldMapPointParam: id {row_id}: posZ: = {rec["z"]:.3f};')

        if text_id1 > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: textId1: = {text_id1};')

        # Event flag: hide text when collected
        flag = rec.get('eventFlag', 0)
        if flag > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: textDisableFlagId1: = {flag};')

        # textId2: dungeon/location name from PlaceName FMG (area*1000 + sub*10)
        next_text_slot = 2
        if area not in OVERWORLD_AREAS:
            loc_id = resolve_location_id(rec.get('map', ''))
            if loc_id > 0:
                lines.append(f'param WorldMapPointParam: id {row_id}: textId{next_text_slot}: = {loc_id};')
                if flag > 0:
                    lines.append(f'param WorldMapPointParam: id {row_id}: textDisableFlagId{next_text_slot}: = {flag};')
                next_text_slot += 1

        # Enemy name from TutorialTitle FMG (for enemy/emevd drops)
        # Offset by 900000000 to avoid collision with GoodsName IDs
        # Uses NpcParam to resolve variant-specific names (e.g. Ash-of-War Scarab vs Somber Scarab)
        enemy_model = rec.get('enemyModel', '')
        npc_param = rec.get('npcParamId', 0)
        tutorial_id = resolve_enemy_tutorial_id(enemy_model, npc_param)
        if tutorial_id > 0:
            enemy_text_id = tutorial_id + 900000000
            lines.append(f'param WorldMapPointParam: id {row_id}: textId{next_text_slot}: = {enemy_text_id};')
            if flag > 0:
                lines.append(f'param WorldMapPointParam: id {row_id}: textDisableFlagId{next_text_slot}: = {flag};')

        lines.append(f'param WorldMapPointParam: id {row_id}: selectMinZoomStep: = 1;')

        row_id += 1

    with open(filepath, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')

    return row_id - start_id


def main():
    OUT_DIR.mkdir(exist_ok=True)

    print('Loading items database...')
    with open(DB_PATH, encoding='utf-8') as f:
        db = json.load(f)
    print(f'  {len(db)} records')

    # Skip fallback records (no coordinates) and respawning enemy drops (no event flag)
    db = [r for r in db if not r.get('from_fallback')]
    print(f'  {len(db)} with coordinates')
    db = [r for r in db if r.get('eventFlag', 0) > 0]
    print(f'  {len(db)} with event flags (one-time pickups)')

    # For enemy drops: exclude shared flags (generic drops from common enemies)
    # Only keep enemy entries whose flag is unique (used by 1 entry) or from EMEVD/treasure
    from collections import Counter
    flag_counts = Counter(r.get('eventFlag', 0) for r in db)
    before = len(db)
    db = [r for r in db if r.get('source') != 'enemy' or flag_counts[r['eventFlag']] == 1]
    print(f'  {len(db)} after filtering shared enemy flags (-{before - len(db)})')

    for cat_name, cat_cfg in LOOT_CATEGORIES.items():
        filter_fn = cat_cfg['filter']
        icon_id = cat_cfg['iconId']
        start_id = cat_cfg['startId']

        # Filter matching records
        source_filter = cat_cfg.get('source_filter')
        matched = [r for r in db if filter_fn(r['items']) and (not source_filter or source_filter(r))]
        print(f'\n=== {cat_name} ===')
        print(f'  Matched: {len(matched)}')

        # Deduplicate
        unique, dupes = deduplicate(matched)
        if dupes:
            print(f'  Deduplicated: {dupes} removed, {len(unique)} unique')

        # Sort by area, grid, position
        unique.sort(key=lambda r: (r['areaNo'], r['gridX'], r['gridZ'], r['x'], r['z']))

        # Write MASSEDIT
        massedit_path = OUT_DIR / f'{cat_name}.MASSEDIT'
        count = write_massedit(unique, massedit_path, icon_id, start_id)
        print(f'  Written {count} entries to {massedit_path.name}')

        # Stats
        areas = defaultdict(int)
        with_flag = 0
        for r in unique:
            areas[r['areaNo']] += 1
            if r.get('eventFlag', 0) > 0:
                with_flag += 1
        print(f'  With event flags: {with_flag}/{len(unique)}')
        print(f'  By area: {dict(sorted(areas.items()))}')


    # ── Bosses category: from boss_list.json (vanilla 217 + MSB coords + text matching) ──
    print('\n=== World - Bosses ===')

    _boss_list_path = DATA_DIR / 'boss_list.json'
    if _boss_list_path.exists():
        with open(_boss_list_path, encoding='utf-8') as _f:
            boss_list = json.load(_f)
    else:
        boss_list = []
        print('  WARNING: boss_list.json not found')

    lines = []
    row_id = 9000000
    boss_count = 0
    text_matched = 0
    for rec in sorted(boss_list, key=lambda r: (r['areaNo'], r.get('gridX', 0), r.get('gridZ', 0))):
        area = rec['areaNo']
        gx = rec.get('gridX', 0)
        gz = rec.get('gridZ', 0)

        if area in UNDERGROUND_AREAS:
            disp = 'dispMask01'
        elif area in DLC_AREAS:
            disp = 'pad2_0'
        else:
            disp = 'dispMask00'

        lines.append(f'param WorldMapPointParam: id {row_id}: iconId: = 374;')
        lines.append(f'param WorldMapPointParam: id {row_id}: {disp}: = 1;')
        lines.append(f'param WorldMapPointParam: id {row_id}: areaNo: = {area};')

        if area in OVERWORLD_AREAS or area in DLC_AREAS or gx > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: gridXNo: = {gx};')
        if area in OVERWORLD_AREAS or area in DLC_AREAS or gz > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: gridZNo: = {gz};')

        if rec['x'] != 0.0 or rec['z'] != 0.0:
            lines.append(f'param WorldMapPointParam: id {row_id}: posX: = {rec["x"]:.3f};')
            if rec['y'] != 0.0:
                lines.append(f'param WorldMapPointParam: id {row_id}: posY: = {rec["y"]:.3f};')
            lines.append(f'param WorldMapPointParam: id {row_id}: posZ: = {rec["z"]:.3f};')

        # textId1: enemy name via TutorialTitle (text-matched with vanilla PlaceName)
        enemy_model = rec.get('enemyModel', '')
        npc_param = rec.get('npcParamId', 0)
        vanilla_place_name = rec.get('vanillaPlaceName', '')
        tutorial_id = resolve_enemy_tutorial_id(enemy_model, npc_param, vanilla_place_name)
        if vanilla_place_name and tutorial_id != resolve_enemy_tutorial_id(enemy_model, npc_param):
            text_matched += 1
        if tutorial_id > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: textId1: = {tutorial_id + 900000000};')
        else:
            # Fallback: use PlaceName ID from ERR WorldMapPointParam
            wmp_tid = rec.get('wmpTextId1', 0)
            if wmp_tid > 0:
                lines.append(f'param WorldMapPointParam: id {row_id}: textId1: = {wmp_tid};')

        # Kill flag for green checkmark AND hide-when-killed option
        kill_flag = rec.get('killEventFlagId', 0)
        cleared_flag = kill_flag if kill_flag > 0 else rec.get('clearedEventFlagId', 0)
        if cleared_flag > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: clearedEventFlagId: = {cleared_flag};')
            # Also set textDisableFlagId1 — C++ config chooses which to use:
            # green checkmark (clearedEventFlagId) or hide killed (textDisableFlagId1)
            lines.append(f'param WorldMapPointParam: id {row_id}: textDisableFlagId1: = {cleared_flag};')

        # textId2: location name for dungeons
        loc_id = resolve_location_id(rec.get('map', ''))
        if loc_id > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: textId2: = {loc_id};')
            if cleared_flag > 0:
                lines.append(f'param WorldMapPointParam: id {row_id}: textDisableFlagId2: = {cleared_flag};')

        lines.append(f'param WorldMapPointParam: id {row_id}: selectMinZoomStep: = 1;')
        row_id += 1
        boss_count += 1

    massedit_path = OUT_DIR / 'World - Bosses.MASSEDIT'
    with open(massedit_path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    print(f'  Written {boss_count} entries ({text_matched} text-matched) to {massedit_path.name}')


    # ── Great Runes: dropped by story bosses ──
    print('\n=== Key - Great Runes ===')

    # Great Rune item ID → boss vanillaPlaceName substring
    GREAT_RUNE_BOSSES = {
        191: 'Godrick the Grafted',
        192: 'Starscourge Radahn',
        193: 'Morgott',
        194: 'Rykard',
        195: 'Mohg, Lord of Blood',
        196: 'Malenia',
    }

    boss_by_name = {}
    for b in boss_list:
        boss_by_name[b['vanillaPlaceName']] = b

    lines = []
    row_id = 9100000
    gr_count = 0
    for rune_id, boss_name in sorted(GREAT_RUNE_BOSSES.items()):
        # Find boss
        boss = None
        for bname, b in boss_by_name.items():
            if boss_name.lower() in bname.lower():
                boss = b
                break
        if not boss:
            print(f'  WARNING: boss not found for rune {rune_id} "{boss_name}"')
            continue

        area = boss['areaNo']
        gx = boss.get('gridX', 0)
        gz = boss.get('gridZ', 0)
        kill_flag = boss.get('killEventFlagId', 0)

        if area in UNDERGROUND_AREAS:
            disp = 'dispMask01'
        elif area in DLC_AREAS:
            disp = 'pad2_0'
        else:
            disp = 'dispMask00'

        lines.append(f'param WorldMapPointParam: id {row_id}: iconId: = 420;')
        lines.append(f'param WorldMapPointParam: id {row_id}: {disp}: = 1;')
        lines.append(f'param WorldMapPointParam: id {row_id}: areaNo: = {area};')
        if area in OVERWORLD_AREAS or area in DLC_AREAS or gx > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: gridXNo: = {gx};')
            lines.append(f'param WorldMapPointParam: id {row_id}: gridZNo: = {gz};')
        lines.append(f'param WorldMapPointParam: id {row_id}: posX: = {boss["x"]:.3f};')
        lines.append(f'param WorldMapPointParam: id {row_id}: posY: = {boss["y"]:.3f};')
        lines.append(f'param WorldMapPointParam: id {row_id}: posZ: = {boss["z"]:.3f};')

        # Text: Great Rune name — hide when boss killed (rune obtained)
        lines.append(f'param WorldMapPointParam: id {row_id}: textId1: = {500000000 + rune_id};')
        if kill_flag > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: textDisableFlagId1: = {kill_flag};')

        # Dungeon location text
        loc_id = resolve_location_id(boss.get('map', ''))
        if loc_id > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: textId2: = {loc_id};')
            if kill_flag > 0:
                lines.append(f'param WorldMapPointParam: id {row_id}: textDisableFlagId2: = {kill_flag};')

        lines.append(f'param WorldMapPointParam: id {row_id}: selectMinZoomStep: = 1;')
        row_id += 1
        gr_count += 1

    massedit_path = OUT_DIR / 'Key - Great Runes.MASSEDIT'
    with open(massedit_path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    print(f'  Written {gr_count} entries to {massedit_path.name}')


if __name__ == '__main__':
    main()
