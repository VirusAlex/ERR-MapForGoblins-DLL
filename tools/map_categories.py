#!/usr/bin/env python3
"""Single source of truth for map-marker categories.

ONE row per WorldMapPointParam marker file, in MAP Z-ORDER (top of map first). Everything derives
from this table - add a category by adding ONE row here:
  * row_id_registry.LAYER_ORDER (z-order / row-id base)  = the file column, in this order;
  * icon_registry (iconId per icon slug, PNG per slug)    = the slug + png columns;
  * the icon PNG override used by both map tags and the overlay atlas = the png column.

Columns: (MASSEDIT file basename, icon slug, custom PNG or None=composed fallback).
Two categories sharing a slug share ONE icon/frame (e.g. Kindling Spirits reuses the incantations
icon). Reorder rows to relayer the map; rename/extend freely - downstream renumbers automatically.
"""

CATEGORIES = [
    ('World - Graces', 'graces', 'grace.png'),
    ('Reforged - Rune Pieces', 'rune_pieces', 'rune_piece.png'),
    ('Reforged - Ember Pieces', 'ember_pieces', 'ember_piece.png'),
    ('World - Maps', 'world_maps', 'map.png'),
    ('World - Imp Statues', 'imp_statues', 'imp_statue.png'),
    ('World - Summoning Pools', 'summoning_pools', 'pool.png'),

    ('Quest - Seedbed Curses', 'seedbed_curses', 'curse.png'),
    ('Quest - Progression', 'progression', 'quest.png'),
    ('Quest - Deathroot', 'deathroot', 'death.png'),
    ('Key - Celestial Dew', 'celestial_dew', 'dew.png'),
    ('Key - Cookbooks', 'cookbooks', 'cookbook.png'),
    ('Key - Crystal Tears', 'crystal_tears', 'crystal_tears.png'),
    ('Key - Imbued Sword Keys', 'imbued_sword_keys', 'stone_key_2.png'),
    ('Key - Larval Tears', 'larval_tears', 'larval.png'),
    ('Key - Lost Ashes', 'lost_ashes', 'lost_ash.png'),
    ('Key - Pots n Perfumes', 'pots_n_perfumes', 'pots_n_perfumes.png'),
    ('Key - Seeds Tears Ashes', 'seeds_tears', 'seed.png'),
    ('Key - Scadutree Fragments', 'scadutree_fragments', 'skadu.png'),
    ('Key - Whetblades', 'whetblades', 'whetblade.png'),
    ('Key - Great Runes', 'great_runes', 'great.png'),
    ('Reforged - Items', 'items_and_changes', 'reforged.png'),
    ('Reforged - Fortunes', 'fortunes', 'fortune.png'),
    ('Reforged - Sealed Curios', 'sealed_curios', 'curio.png'),
    ('Equipment - Armaments', 'armaments', 'weapon.png'),
    ('Equipment - Armour', 'armour', 'armor.png'),
    ('Equipment - Talismans', 'talismans', 'talisman.png'),
    ('Equipment - Spirits', 'spirits', 'spirit.png'),
    ('Equipment - Ashes of War', 'ashes_of_war', 'ash.png'),
    ('Magic - Incantations', 'incantations', 'incantation.png'),
    ('World - Kindling Spirits', 'incantations', 'incantation.png'),
    ('Magic - Sorceries', 'sorceries', 'sorceries.png'),
    ('Magic - Memory Stones', 'memory_stones', 'memory.png'),
    ('Magic - Prayerbooks', 'prayerbooks', 'prayerbook.png'),
    ('Loot - Gestures', 'gestures', 'gesture.png'),
    ('Loot - Stonesword Keys', 'stonesword_keys', 'stone_key.png'),
    ('Loot - Bell-Bearings', 'bell_bearings', 'bell.png'),
    ('Loot - Merchant Bell-Bearings', 'merchant_bell_bearings', 'bell_m.png'),
    ('Loot - Smithing Stones (Rare)', 'smithing_stones_rare', 'smst_high.png'),
    ('Loot - Golden Runes', 'golden_runes', 'rune_high.png'),
    ('Loot - Rune Arcs', 'rune_arcs', 'ark.png'),
    ('Loot - Dragon Hearts', 'dragon_hearts', 'dragon_heart.png'),
    ('Loot - Stat Boosts', 'stat_boosts', 'shard.png'),
    ('Loot - Prattling Pates', 'prattling_pates', 'pate.png'),
    ('Loot - Reusables', 'reusables', 'reusable.png'),
    ('World - Paintings', 'paintings', 'painting.png'),
    ('Loot - Smithing Stones', 'smithing_stones', 'smst.png'),
    ('Loot - MP-Fingers', 'mp_fingers', 'finger.png'),
    ('Loot - Great Gloveworts', 'great_gloveworts', 'glove_high.png'),

    ('World - Seal Puzzles', 'interactables', 'interactible.png'),
    ('Loot - Ammo', 'ammo', 'ammo.png'),
    ('Loot - Smithing Stones (Low)', 'smithing_stones_low', 'smst_low.png'),
    ('Loot - Golden Runes (Low)', 'golden_runes_low', 'rune_low.png'),
    ('Loot - Consumables', 'consumables', 'consumables.png'),
    ('Loot - Greases', 'greases', 'grease.png'),
    ('Loot - Utilities', 'utilities', 'utils.png'),
    ('Loot - Throwables', 'throwables', 'throw.png'),
    ('Loot - Crafting Materials', 'crafting_materials', 'materials.png'),
    ('Loot - Gloveworts', 'gloveworts', 'glove.png'),
    ('Loot - Material Nodes', 'material_nodes', 'nodes.png'),

    ('World - Stakes of Marika', 'stakes_of_marika', 'marika.png'),
    ('World - Spirit Springs', 'spirit_springs', 'jump.png'),
    ('World - Spiritspring Hawks', 'spiritspring_hawks', 'stormhawk.png'),
    ('World - Bosses', 'bosses', 'boss.png'),
    ('World - Hostile NPC', 'hostile_npc', 'npc.png'),
    ("World - Hero's Tomb Statues", 'hero_tomb_statues', 'statue.png'),
]

# Icons with no marker file of their own (runtime-only, e.g. the spoiler "?").
EXTRA_ICONS = [
    ('anon', 'anon.png'),
]
