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

import config

REPO = Path(__file__).resolve().parent.parent
TOOLS = REPO / 'tools'
DATA = config.DATA_DIR
CACHE_FILE = DATA / '.build_cache.json'

ERR_MOD = config.require_err_mod_dir()
MSB_DIR = ERR_MOD / 'map' / 'MapStudio'
EVENT_DIR = ERR_MOD / 'event'
REGULATION = ERR_MOD / 'regulation.bin'
MSGBND = ERR_MOD / 'msg' / 'engus' / 'item_dlc02.msgbnd.dcx'

MASSEDIT_OUT = DATA / 'massedit_generated'
GENERATED_CPP = REPO / 'src' / 'generated'


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
    Stage('extract_items',
          inputs=[REGULATION, MSB_DIR, MSGBND, config.PARAMDEF_DIR],
          outputs=[DATA / 'items_database.json',
                   DATA / 'goods_sort_groups.json',
                   DATA / 'goods_crafting_ids.json',
                   DATA / 'goods_sorcery_ids.json',
                   DATA / 'goods_incantation_ids.json',
                   DATA / 'goods_spirit_ash_ids.json',
                   DATA / 'tutorial_title_ids.json',
                   DATA / 'tutorial_title_names.json',
                   DATA / 'enemy_tutorial_mapping.json'],
          script='extract_all_items.py',
          also_scripts=COMMON),

    Stage('entity_index',
          inputs=[MSB_DIR],
          outputs=[DATA / 'msb_entity_index.json'],
          script='build_entity_index.py',
          also_scripts=['config.py']),

    Stage('emevd_scan',
          inputs=[EVENT_DIR, REGULATION, DATA / 'msb_entity_index.json'],
          outputs=[DATA / 'emevd_lot_mapping.json'],
          script='scan_emevd_awards.py',
          also_scripts=['config.py']),

    Stage('enrich_fallback',
          inputs=[DATA / 'items_database.json', DATA / 'emevd_lot_mapping.json'],
          outputs=[DATA / 'items_database.json'],  # modified in-place
          script='enrich_fallback_with_emevd.py',
          also_scripts=['config.py']),

    Stage('generate_boss_list',
          inputs=[REGULATION, MSB_DIR, MSGBND],
          outputs=[DATA / 'boss_list.json'],
          script='generate_boss_list.py',
          also_scripts=['extract_all_items.py'] + COMMON),

    Stage('generate_loot_massedit',
          inputs=[DATA / 'items_database.json',
                  DATA / 'goods_sort_groups.json',
                  DATA / 'goods_crafting_ids.json',
                  DATA / 'goods_sorcery_ids.json',
                  DATA / 'goods_incantation_ids.json',
                  DATA / 'goods_spirit_ash_ids.json',
                  DATA / 'boss_list.json',
                  DATA / 'enemy_tutorial_mapping.json',
                  DATA / 'tutorial_title_ids.json',
                  DATA / 'tutorial_title_names.json'],
          outputs=[MASSEDIT_OUT / 'Loot - Consumables.MASSEDIT',
                   MASSEDIT_OUT / 'Equipment - Armaments.MASSEDIT',
                   MASSEDIT_OUT / 'Quest - Progression.MASSEDIT',
                   MASSEDIT_OUT / 'World - Bosses.MASSEDIT'],
          script='generate_loot_massedit.py',
          also_scripts=['massedit_common.py']),

    Stage('generate_pieces_massedit',
          inputs=[DATA / 'ItemLotParam_map.csv'],
          outputs=[MASSEDIT_OUT / 'Reforged - Rune Pieces.MASSEDIT',
                   MASSEDIT_OUT / 'Reforged - Ember Pieces.MASSEDIT',
                   MASSEDIT_OUT / 'Reforged - Rune Pieces_slots.json',
                   MASSEDIT_OUT / 'Reforged - Ember Pieces_slots.json'],
          script='generate_pieces_massedit.py',
          also_scripts=['massedit_common.py']),

    Stage('generate_material_nodes',
          inputs=[DATA / 'aeg099_item_mapping.json',
                  DATA / 'aeg463_item_mapping.json',
                  DATA / 'all_gathering_nodes_final.json'],
          outputs=[MASSEDIT_OUT / 'Loot - Material Nodes.MASSEDIT',
                   MASSEDIT_OUT / 'Loot - Material Nodes_slots.json'],
          script='generate_material_nodes.py',
          also_scripts=['massedit_common.py']),

    Stage('generate_graces',
          inputs=[REGULATION],
          outputs=[MASSEDIT_OUT / 'World - Graces.MASSEDIT'],
          script='generate_graces.py',
          also_scripts=['extract_all_items.py'] + COMMON),

    Stage('generate_summoning_pools',
          inputs=[REGULATION, MSB_DIR],
          outputs=[MASSEDIT_OUT / 'World - Summoning Pools.MASSEDIT'],
          script='generate_summoning_pools.py',
          also_scripts=['extract_all_items.py'] + COMMON),

    Stage('generate_spirit_springs',
          inputs=[MSB_DIR],
          outputs=[MASSEDIT_OUT / 'World - Spirit Springs.MASSEDIT',
                   MASSEDIT_OUT / 'World - Spiritspring Hawks.MASSEDIT'],
          script='generate_spirit_springs.py',
          also_scripts=['config.py']),

    Stage('generate_imp_statues',
          inputs=[MSB_DIR],
          outputs=[MASSEDIT_OUT / 'World - Imp Statues.MASSEDIT'],
          script='generate_imp_statues.py',
          also_scripts=['config.py']),

    Stage('generate_stakes',
          inputs=[MSB_DIR],
          outputs=[MASSEDIT_OUT / 'World - Stakes of Marika.MASSEDIT'],
          script='generate_stakes.py',
          also_scripts=['config.py']),

    Stage('generate_paintings',
          inputs=[MSB_DIR, EVENT_DIR],
          outputs=[MASSEDIT_OUT / 'World - Paintings.MASSEDIT'],
          script='generate_paintings.py',
          also_scripts=['config.py']),

    Stage('generate_maps',
          inputs=[DATA / 'items_database.json'],
          outputs=[MASSEDIT_OUT / 'World - Maps.MASSEDIT'],
          script='generate_maps.py',
          also_scripts=['massedit_common.py']),

    Stage('generate_gestures',
          inputs=[DATA / 'msb_entity_index.json', EVENT_DIR],
          outputs=[MASSEDIT_OUT / 'Loot - Gestures.MASSEDIT'],
          script='generate_gestures.py',
          also_scripts=['massedit_common.py', 'config.py']),

    Stage('generate_hostile_npcs',
          inputs=[REGULATION, MSB_DIR, config.PARAMDEF_DIR],
          outputs=[MASSEDIT_OUT / 'World - Hostile NPC.MASSEDIT'],
          script='generate_hostile_npcs.py',
          also_scripts=['massedit_common.py', 'config.py']),

    Stage('generate_data',
          inputs=[MASSEDIT_OUT],
          outputs=[GENERATED_CPP / 'goblin_map_data.cpp',
                   GENERATED_CPP / 'goblin_text_data.cpp'],
          script='generate_data.py',
          args=['--massedit-dir', str(MASSEDIT_OUT)]),
]


def main():
    args = sys.argv[1:]
    force_all = '--force-all' in args
    force_stages = set()
    for i, a in enumerate(args):
        if a == '--force' and i + 1 < len(args):
            force_stages.add(args[i + 1])

    cache = {}
    if CACHE_FILE.exists():
        try:
            cache = json.load(open(CACHE_FILE, encoding='utf-8'))
        except Exception:
            cache = {}

    overall_t0 = time.time()
    for stage in STAGES:
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
