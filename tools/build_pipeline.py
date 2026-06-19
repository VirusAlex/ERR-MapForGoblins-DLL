#!/usr/bin/env python3
"""
Incremental build orchestrator with hash-based caching.

For each stage: hash inputs (file content for files, mtime+size aggregate
for directories), compare with cached signature. If match AND all outputs
exist → skip. Otherwise re-run script and update cache.

Cache: data/.build_cache.json
Override: --force <stage_name> | --force-all
"""
import sys, os, json, hashlib, subprocess, time
from pathlib import Path


def _parse_profile(argv):
    """Resolve the build profile from argv/env BEFORE importing config
    (config reads MFG_PROFILE at import time)."""
    for i, a in enumerate(argv):
        if a == '--profile' and i + 1 < len(argv):
            return argv[i + 1].strip().lower()
        if a.startswith('--profile='):
            return a.split('=', 1)[1].strip().lower()
    return os.environ.get('MFG_PROFILE', 'err').strip().lower()


PROFILE = _parse_profile(sys.argv[1:])
if PROFILE not in ('err', 'vanilla', 'convergence', 'erte'):
    PROFILE = 'err'
os.environ['MFG_PROFILE'] = PROFILE  # propagate to every child subprocess

import config

REPO = Path(__file__).resolve().parent.parent
TOOLS = REPO / 'tools'
DATA = config.DATA_DIR                 # data/ (err) or data/<profile>/ otherwise
DATA.mkdir(parents=True, exist_ok=True)
CACHE_FILE = DATA / '.build_cache.json'

# The convergence source is a STAGED dir (built by the prepare_merged_src
# stage below) - create it up front so require_err_mod_dir passes on the
# very first run; the stage then populates it before anything reads it.
if PROFILE in ('convergence', 'erte'):
    config.DATA_SRC_DIR.mkdir(parents=True, exist_ok=True)

ERR_MOD = config.require_err_mod_dir()  # profile-aware: mod overlay / vanilla game / merged dir
MSB_DIR = ERR_MOD / 'map' / 'MapStudio'
EVENT_DIR = ERR_MOD / 'event'
REGULATION = ERR_MOD / 'regulation.bin'
MSGBND = ERR_MOD / 'msg' / 'engus' / 'item_dlc02.msgbnd.dcx'

MASSEDIT_OUT = DATA / 'massedit_generated'
GENERATED_CPP = config.GENERATED_DIR   # src/generated or src/generated_vanilla

# Stages that only make sense for the ERR mod (their source assets/items do
# not exist in vanilla). Dropped from the vanilla pipeline.
ERR_ONLY_STAGES = {'generate_pieces_massedit', 'generate_kindling_spirits',
                   'extract_rune_positions', 'extract_itemlot_csv'}

MENU_MSGBND = ERR_MOD / 'msg' / 'engus' / 'menu_dlc02.msgbnd.dcx'


# ── Hashing ──
def hash_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while True:
            chunk = f.read(1 << 20)
            if not chunk: break
            h.update(chunk)
    return h.hexdigest()


def hash_dir_meta(path, glob='*'):
    """Hash directory by (relpath, size, mtime_ns) tuples."""
    h = hashlib.sha256()
    files = sorted(path.rglob(glob)) if glob != '*' else sorted(path.rglob('*'))
    for f in files:
        if f.is_file():
            st = f.stat()
            rel = str(f.relative_to(path)).replace('\\', '/')
            h.update(f'{rel}\0{st.st_size}\0{st.st_mtime_ns}\0'.encode())
    return h.hexdigest()


def hash_input(p):
    p = Path(p)
    if not p.exists():
        return f'MISSING:{p}'
    if p.is_file():
        return f'F:{hash_file(p)}'
    if p.is_dir():
        return f'D:{hash_dir_meta(p)}'
    return f'?:{p}'


def stage_signature(inputs):
    h = hashlib.sha256()
    for p in inputs:
        h.update(str(p).encode() + b'\0' + hash_input(p).encode() + b'\0')
    return h.hexdigest()


# ── Stage definition ──
class Stage:
    def __init__(self, name, *, inputs, outputs, script, also_scripts=None, args=None):
        self.name = name
        self.inputs = [Path(p) for p in inputs]
        self.outputs = [Path(p) for p in outputs]
        self.script = TOOLS / script
        # Other Python files this script depends on (imports etc.)
        self.also_scripts = [TOOLS / p for p in (also_scripts or [])]
        self.args = list(args or [])

    def all_inputs(self):
        return self.inputs + self.also_scripts + [self.script]

    def signature(self):
        # Include args so signature changes if invocation changes
        sig = stage_signature(self.all_inputs())
        if self.args:
            h = hashlib.sha256()
            h.update(sig.encode())
            h.update(b'\0' + ' '.join(self.args).encode())
            return h.hexdigest()
        return sig

    def is_up_to_date(self, cache):
        if not all(o.exists() for o in self.outputs):
            return False
        return cache.get(self.name) == self.signature()

    def run(self):
        cmd = [sys.executable, str(self.script)] + self.args
        result = subprocess.run(cmd, cwd=str(TOOLS))
        return result.returncode == 0


# ── Pipeline ──
COMMON = ['config.py', 'massedit_common.py']

STAGES = [
    # Source-table extractors: regenerate the category/codex/placename tables
    # from the active profile's game data (these used to be committed one-off
    # snapshots; now they stay fresh and exist for the vanilla profile too).
    Stage('extract_goods_categories',
          inputs=[REGULATION, config.PARAMDEF_DIR],
          outputs=[DATA / 'goods_sort_groups.json',
                   DATA / 'goods_crafting_ids.json',
                   DATA / 'goods_sorcery_ids.json',
                   DATA / 'goods_incantation_ids.json',
                   DATA / 'goods_spirit_ash_ids.json'],
          script='extract_goods_categories.py',
          also_scripts=['config.py']),

    Stage('extract_tutorial_codex',
          inputs=[MENU_MSGBND],
          outputs=[DATA / 'tutorial_title_ids.json',
                   DATA / 'tutorial_title_names.json',
                   DATA / 'enemy_tutorial_mapping.json'],
          script='extract_tutorial_codex.py',
          also_scripts=['config.py']),

    Stage('extract_placename_dump',
          inputs=[MSGBND],
          outputs=[DATA / 'PlaceName_engus.json'],
          script='extract_placename_dump.py',
          also_scripts=['config.py']),

    # ERR-only: Rune/Ember Piece positions from ERR MSBs (AEG099_821/822).
    Stage('extract_rune_positions',
          inputs=[MSB_DIR],
          outputs=[DATA / 'rune_pieces.json',
                   DATA / 'ember_pieces.json'],
          script='extract_rune_positions.py',
          also_scripts=['config.py']),

    # ERR-only: ItemLotParam_map CSV dump (consumed by generate_pieces_massedit).
    Stage('extract_itemlot_csv',
          inputs=[REGULATION, config.PARAMDEF_DIR],
          outputs=[DATA / 'ItemLotParam_map.csv'],
          script='extract_itemlot_csv.py',
          also_scripts=['config.py']),

    Stage('extract_items',
          # emevd_lot_mapping is NOT read by extract_all_items, but listed as an input
          # on purpose: enrich_fallback mutates items_database.json IN PLACE using the
          # mapping, and is not idempotent (it can't revert stale upgrades). When the
          # mapping changes, the DB must be re-extracted from scratch so enrich applies
          # the new mapping to a virgin DB (converges one pipeline run after a scan
          # change; same-run ordering puts extract before emevd_scan).
          inputs=[REGULATION, MSB_DIR, MSGBND, config.PARAMDEF_DIR,
                  DATA / 'emevd_lot_mapping.json'],
          outputs=[DATA / 'items_database.json',
                   DATA / 'npc_name_ids.json',
                   DATA / 'unreachable_msb_lots.json'],
          script='extract_all_items.py',
          also_scripts=['unreachable.py'] + COMMON),

    Stage('entity_index',
          inputs=[MSB_DIR],
          outputs=[DATA / 'msb_entity_index.json'],
          script='build_entity_index.py',
          also_scripts=['config.py']),

    Stage('grace_index',
          inputs=[REGULATION, MSGBND],
          outputs=[DATA / 'grace_position_index.json'],
          script='build_grace_index.py',
          also_scripts=['config.py']),

    Stage('emevd_scan',
          inputs=[EVENT_DIR, REGULATION, DATA / 'msb_entity_index.json'],
          outputs=[DATA / 'emevd_lot_mapping.json'],
          script='scan_emevd_awards.py',
          also_scripts=['config.py']),

    Stage('enrich_fallback',
          inputs=[DATA / 'items_database.json',
                  DATA / 'emevd_lot_mapping.json',
                  DATA / 'unreachable_msb_lots.json'],
          outputs=[DATA / 'items_database.json'],  # modified in-place
          script='enrich_fallback_with_emevd.py',
          also_scripts=['config.py']),

    Stage('generate_boss_list',
          inputs=[REGULATION, MSB_DIR, MSGBND, EVENT_DIR],
          outputs=[DATA / 'boss_list.json'],
          script='generate_boss_list.py',
          also_scripts=['extract_all_items.py'] + COMMON),

    Stage('generate_loot_massedit',
          inputs=[REPO / 'data' / 'enemy_bloodmsg_mapping.json',
                  REPO / 'data' / 'enemy_names_i18n.json',
                  DATA / 'items_database.json',
                  DATA / 'goods_sort_groups.json',
                  DATA / 'goods_crafting_ids.json',
                  DATA / 'goods_sorcery_ids.json',
                  DATA / 'goods_incantation_ids.json',
                  DATA / 'goods_spirit_ash_ids.json',
                  DATA / 'boss_list.json',
                  DATA / 'enemy_tutorial_mapping.json',
                  DATA / 'tutorial_title_ids.json',
                  DATA / 'tutorial_title_names.json',
                  DATA / 'grace_position_index.json'],
          outputs=[MASSEDIT_OUT / 'Loot - Consumables.MASSEDIT',
                   MASSEDIT_OUT / 'Equipment - Armaments.MASSEDIT',
                   MASSEDIT_OUT / 'Quest - Progression.MASSEDIT',
                   MASSEDIT_OUT / 'World - Bosses.MASSEDIT',
                   DATA / 'loot_lot_linkage.json',
                   DATA / 'item_icon_table.json'],
          script='generate_loot_massedit.py',
          also_scripts=['massedit_common.py']),

    Stage('generate_pieces_massedit',
          inputs=[DATA / 'ItemLotParam_map.csv',
                  DATA / 'grace_position_index.json'],
          outputs=[MASSEDIT_OUT / 'Reforged - Rune Pieces.MASSEDIT',
                   MASSEDIT_OUT / 'Reforged - Ember Pieces.MASSEDIT',
                   MASSEDIT_OUT / 'Reforged - Rune Pieces_slots.json',
                   MASSEDIT_OUT / 'Reforged - Ember Pieces_slots.json'],
          script='generate_pieces_massedit.py',
          also_scripts=['massedit_common.py']),

    Stage('scan_gathering_nodes',
          inputs=[MSB_DIR, DATA / 'aeg099_item_mapping.json'],
          outputs=[DATA / 'all_gathering_nodes_final.json'],
          script='scan_all_gathering_nodes.py',
          also_scripts=['config.py']),

    Stage('scan_gathering_node_flags',
          inputs=[EVENT_DIR],
          outputs=[DATA / 'gathering_node_flags.json'],
          script='scan_gathering_node_flags.py',
          also_scripts=['config.py']),

    Stage('generate_material_nodes',
          inputs=[DATA / 'aeg099_item_mapping.json',
                  DATA / 'aeg463_item_mapping.json',
                  DATA / 'all_gathering_nodes_final.json',
                  DATA / 'gathering_node_flags.json'],
          outputs=[MASSEDIT_OUT / 'Loot - Material Nodes.MASSEDIT',
                   MASSEDIT_OUT / 'Loot - Material Nodes_slots.json'],
          script='generate_material_nodes.py',
          also_scripts=['massedit_common.py', 'unreachable.py']),

    Stage('generate_graces',
          inputs=[REGULATION],
          outputs=[MASSEDIT_OUT / 'World - Graces.MASSEDIT'],
          script='generate_graces.py',
          also_scripts=['extract_all_items.py', 'unreachable.py'] + COMMON),

    Stage('generate_summoning_pools',
          inputs=[REGULATION, MSB_DIR],
          outputs=[MASSEDIT_OUT / 'World - Summoning Pools.MASSEDIT'],
          script='generate_summoning_pools.py',
          also_scripts=['extract_all_items.py'] + COMMON),

    Stage('generate_kindling_spirits',
          inputs=[DATA / 'kindling_spirits.json'],
          outputs=[MASSEDIT_OUT / 'World - Kindling Spirits.MASSEDIT',
                   MASSEDIT_OUT / 'World - Kindling Spirits_slots.json'],
          script='generate_kindling_spirits_massedit.py',
          also_scripts=['massedit_common.py']),

    Stage('generate_spirit_springs',
          inputs=[MSB_DIR],
          outputs=[MASSEDIT_OUT / 'World - Spirit Springs.MASSEDIT',
                   MASSEDIT_OUT / 'World - Spiritspring Hawks.MASSEDIT'],
          script='generate_spirit_springs.py',
          also_scripts=['unreachable.py'] + COMMON),

    Stage('generate_imp_statues',
          inputs=[MSB_DIR],
          outputs=[MASSEDIT_OUT / 'World - Imp Statues.MASSEDIT'],
          script='generate_imp_statues.py',
          also_scripts=['unreachable.py'] + COMMON),

    Stage('generate_stakes',
          inputs=[MSB_DIR],
          outputs=[MASSEDIT_OUT / 'World - Stakes of Marika.MASSEDIT'],
          script='generate_stakes.py',
          also_scripts=COMMON),

    Stage('extract_seal_puzzles',
          inputs=[MSB_DIR, EVENT_DIR],
          outputs=[DATA / 'seal_puzzles.json'],
          script='extract_seal_puzzles.py',
          also_scripts=['config.py']),

    Stage('generate_seal_puzzles',
          inputs=[DATA / 'seal_puzzles.json'],
          outputs=[MASSEDIT_OUT / 'World - Seal Puzzles.MASSEDIT'],
          script='generate_seal_puzzles.py',
          also_scripts=['massedit_common.py']),

    Stage('generate_hero_tomb_statues',
          inputs=[MSB_DIR, EVENT_DIR],
          outputs=[MASSEDIT_OUT / "World - Hero's Tomb Statues.MASSEDIT"],
          script='generate_hero_tomb_statues.py',
          also_scripts=['massedit_common.py']),

    Stage('generate_paintings',
          inputs=[MSB_DIR, EVENT_DIR],
          outputs=[MASSEDIT_OUT / 'World - Paintings.MASSEDIT'],
          script='generate_paintings.py',
          also_scripts=COMMON),

    Stage('generate_maps',
          inputs=[DATA / 'items_database.json'],
          outputs=[MASSEDIT_OUT / 'World - Maps.MASSEDIT'],
          script='generate_maps.py',
          also_scripts=['massedit_common.py']),

    Stage('generate_gestures',
          inputs=[DATA / 'msb_entity_index.json', EVENT_DIR, REGULATION],
          outputs=[MASSEDIT_OUT / 'Loot - Gestures.MASSEDIT'],
          script='generate_gestures.py',
          also_scripts=['massedit_common.py', 'config.py']),

    Stage('generate_hostile_npcs',
          inputs=[REGULATION, MSB_DIR, config.PARAMDEF_DIR, EVENT_DIR,
                  DATA / 'items_database.json',
                  REPO / 'data' / 'quest_invader_overrides.json'],
          outputs=[MASSEDIT_OUT / 'World - Hostile NPC.MASSEDIT'],
          script='generate_hostile_npcs.py',
          also_scripts=['massedit_common.py', 'config.py']),

    # Relocating-boss fix (Lansseax): after all marker generators, before bake.
    # Removes the un-collectable duplicate loot at the boss's flee-spawn and
    # ensures a flee-spawn boss marker that clears on the flee flag. Edits the
    # MASSEDIT in place (idempotent); generate_data (below) re-bakes from it.
    Stage('relocating_boss_fix',
          inputs=[MASSEDIT_OUT,
                  config.PROJECT_DIR / 'data' / 'relocating_flee_spawns.json'],
          outputs=[DATA / '_relocating_boss_fix.done'],
          script='generate_relocating_boss_fix.py',
          also_scripts=['config.py']),

    Stage('generate_data',
          inputs=[MASSEDIT_OUT, DATA / 'loot_lot_linkage.json',
                  DATA / 'item_icon_table.json',
                  config.PROJECT_DIR / 'data' / 'enemy_names_i18n.json',
                  DATA / '_relocating_boss_fix.done'],
          outputs=[GENERATED_CPP / 'goblin_map_data.cpp',
                   GENERATED_CPP / 'goblin_item_icons.cpp',
                   GENERATED_CPP / 'goblin_enemy_names.cpp'],
          script='generate_data.py',
          args=['--massedit-dir', str(MASSEDIT_OUT)]),

    # Per-row ACTUAL gather-asset models (ERR substitutes some assets with DLC-era models
    # in the MSB: part NAME stays vanilla, ModelName differs; GEOF save entries carry the
    # actual model's hash) - used by collected-tracking. Runs AFTER generate_data.
    Stage('generate_geof_models',
          inputs=[GENERATED_CPP / 'goblin_map_data.cpp',
                  DATA / 'all_gathering_nodes_final.json'],
          outputs=[GENERATED_CPP / 'goblin_geof_models.cpp',
                   GENERATED_CPP / 'goblin_geof_models.hpp'],
          script='generate_geof_models.py'),

    # Alternative (hybrid) loot-location naming, baked as generated::LOCATION_ALT
    # (row_id -> textId2). Shown via INI [Goblin] show_location_compare = true.
    # Must run AFTER generate_data (reads the baked goblin_map_data.cpp).
    Stage('generate_location_overrides',
          inputs=[GENERATED_CPP / 'goblin_map_data.cpp', MSB_DIR,
                  DATA / 'WorldMapPointParam.json', DATA / 'grace_position_index.json',
                  DATA / 'PlaceName_engus.json'],
          outputs=[GENERATED_CPP / 'goblin_location_alt.cpp',
                   GENERATED_CPP / 'goblin_location_alt.hpp'],
          script='generate_location_overrides.py',
          also_scripts=['massedit_common.py', 'config.py']),
]


# Convergence-only: stage the merged overlay-over-vanilla source dir FIRST
# (everything else reads from it). Defined lazily - CONVERGENCE_MOD_DIR is
# None in the other profiles.
def _overlay_prepare_stage():
    """Merged-source staging for an overlay-mod profile (convergence / erte):
    the mod's partial file overlay laid over the vanilla game."""
    overlay = config.CONVERGENCE_MOD_DIR if PROFILE == 'convergence' else config.ERTE_MOD_DIR
    if not overlay or not overlay.exists():
        print(f'ERROR: {PROFILE} profile needs {PROFILE}_mod_dir in tools/config.ini')
        sys.exit(1)
    game = config.require_game_dir()
    return Stage('prepare_merged_src',
                 inputs=[overlay / 'regulation.bin',
                         overlay / 'msg' / 'engus',
                         overlay / 'map' / 'MapStudio',
                         overlay / 'event',
                         game / 'map' / 'mapstudio',
                         game / 'event',
                         game / 'msg' / 'engus'],
                 outputs=[ERR_MOD / 'regulation.bin',
                          ERR_MOD / '.merge_manifest.json'],
                 script='prepare_merged_src.py',
                 also_scripts=['config.py'])


# Non-ERR bootstrap stages: regenerate, from the active profile's game data,
# the committed inputs that ship pre-extracted for ERR (they don't exist under
# data/vanilla/ or data/convergence/). Run before the stages that consume
# them. For the err profile these are NOT added (the committed copies are
# authoritative).
VANILLA_BOOTSTRAP = [
    Stage('extract_param_bootstrap',
          inputs=[REGULATION, MSGBND],
          outputs=[DATA / 'WorldMapLegacyConvParam.json',
                   DATA / 'valid_location_ids.json'],
          script='extract_param_bootstrap.py',
          also_scripts=['config.py']),

    Stage('extract_world_map_param',
          inputs=[REGULATION],
          outputs=[DATA / 'WorldMapPointParam.json',
                   DATA / 'WorldMapPointParam.csv'],
          script='extract_world_map_param.py',
          also_scripts=['config.py']),

    Stage('extract_aeg099_mapping',
          inputs=[REGULATION, MSGBND],
          outputs=[DATA / 'aeg099_item_mapping.json'],
          script='extract_aeg099_mapping.py',
          also_scripts=['config.py']),

    Stage('extract_aeg463_mapping',
          inputs=[REGULATION, MSGBND],
          outputs=[DATA / 'aeg463_item_mapping.json'],
          script='extract_aeg463_mapping.py',
          also_scripts=['config.py']),
]


def active_stages():
    """The stage list for the selected profile.

    err:         the full STAGES list, unchanged (committed inputs are used as-is).
    vanilla:     bootstrap extractors first, then STAGES minus the ERR-only ones.
    convergence: merged-source staging, then the same as vanilla.
    """
    if PROFILE == 'vanilla':
        return VANILLA_BOOTSTRAP + [s for s in STAGES if s.name not in ERR_ONLY_STAGES]
    if PROFILE in ('convergence', 'erte'):
        return ([_overlay_prepare_stage()] + VANILLA_BOOTSTRAP
                + [s for s in STAGES if s.name not in ERR_ONLY_STAGES])
    return STAGES


def main():
    args = sys.argv[1:]
    force_all = '--force-all' in args
    force_stages = set()
    for i, a in enumerate(args):
        if a == '--force' and i + 1 < len(args):
            force_stages.add(args[i + 1])

    print(f'[PROFILE] {PROFILE}  (data={DATA}, generated={GENERATED_CPP})')

    cache = {}
    if CACHE_FILE.exists():
        try:
            cache = json.load(open(CACHE_FILE, encoding='utf-8'))
        except Exception:
            cache = {}

    overall_t0 = time.time()
    for stage in active_stages():
        t0 = time.time()
        forced = force_all or stage.name in force_stages
        if not forced and stage.is_up_to_date(cache):
            print(f'[CACHED]  {stage.name}')
            continue
        print(f'[BUILD]   {stage.name}  ({stage.script.name})')
        if not stage.run():
            print(f'[FAILED]  {stage.name}')
            return 1
        cache[stage.name] = stage.signature()
        with open(CACHE_FILE, 'w', encoding='utf-8') as f:
            json.dump(cache, f, indent=1)
        print(f'[OK]      {stage.name}  ({time.time()-t0:.1f}s)')

    print(f'\n[DONE]    pipeline in {time.time()-overall_t0:.1f}s')
    return 0


if __name__ == '__main__':
    sys.exit(main())
