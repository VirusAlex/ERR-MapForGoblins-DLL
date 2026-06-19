"""Manual exclude list for drops whose spawn / award chain in the current
ERR EMEVD never completes, so the item can't actually drop in-game and a
map marker for it would mislead players. Each entry has a specific check
function that validates whether the pattern is still present in the
current ERR EMEVD - if a future ERR version changes the event (no matter
HOW: instruction removed, entity ID changed, state flipped, event
restructured), the check returns False and the icon reappears
automatically on rebuild.

Used by:
  - extract_all_items.py (treasures/enemy drops filtered before MASSEDIT generation)

Add new entries via `BROKEN_SPAWNS` below. Each entry needs:
  - map: tile name
  - part_name: MSB part name (matches items_database.partName)
  - reason: human description for logs
  - check: function(emevd) -> True iff the pattern is still present
"""
import os, tempfile, struct
import config


_emevd_cache = {}


def _load_emevd(map_name):
    if map_name in _emevd_cache:
        return _emevd_cache[map_name]
    from pythonnet import load
    load('coreclr')
    import clr
    clr.AddReference(str(config.SOULSFORMATS_DLL))
    import SoulsFormats
    from System.Reflection import Assembly, BindingFlags
    from System import Array, Type as SysType, Object
    from System.IO import File as SysFile
    asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
    _str = SysType.GetType('System.String')
    _emevd_read = asm.GetType('SoulsFormats.EMEVD').GetMethod(
        'Read', BindingFlags.Public|BindingFlags.Static|BindingFlags.FlattenHierarchy,
        None, Array[SysType]([_str]), None)

    p = config.ERR_MOD_DIR / 'event' / f'{map_name}.emevd.dcx'
    if not p.exists():
        _emevd_cache[map_name] = None
        return None
    data = SoulsFormats.DCX.Decompress(str(p)).ToArray()
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_broken.emevd')
    SysFile.WriteAllBytes(tmp, data)
    em = _emevd_read.Invoke(None, Array[Object]([tmp]))
    _emevd_cache[map_name] = em
    return em


# ── Bug checkers ──
def _check_m30_08_revenant_deadlock(emevd):
    """m30_08_00_00 Event 30082550 contains `IF Char Dead/Alive(30080460, Dead)`
    which forms a circular dependency - c4020_9000 must be dead to set flag
    30082550, but flag 30082550 is what spawns c4020_9000 (via Event 30082501 →
    InvokeEnemyGenerator(30083460) → c4020_9000), so the spawn never happens.

    The pattern is "present" iff Event 30082550 contains an instruction
    `4:0 IF Char Dead/Alive(target=30080460, state=Dead)`.

    Any future change to the event (instruction removed, entity ID changed,
    Desired State flipped from Dead(1) to Alive(0)) flips this check to
    False and the marker comes back."""
    for ev in emevd.Events:
        if int(ev.ID) != 30082550:
            continue
        for ins in ev.Instructions:
            if int(ins.Bank) != 4 or int(ins.ID) != 0:
                continue  # not IF Char Dead/Alive
            args = bytes(ins.ArgData)
            if len(args) < 16:
                continue
            target_eid = struct.unpack_from('<I', args, 4)[0]
            desired_state = args[8]  # 0=Alive, 1=Dead
            if target_eid == 30080460 and desired_state == 1:
                return True  # exactly the bug pattern
        return False
    return False


# ── Registry ──
# Each entry: matches when items_database has this (map, part_name) AND
# the check function returns True (pattern present). When that happens,
# the drop is excluded from generated markers.
BROKEN_SPAWNS = [
    {
        'map': 'm30_08_00_00',
        'part_name': 'c4020_9000',
        'reason': 'Event 30082550 CharacterDead(30080460, Dead) circular dependency - c4020_9000 never spawns',
        'check': _check_m30_08_revenant_deadlock,
    },
]


def is_spawn_broken(map_name, part_name):
    """True iff (map, part_name) is in the registry AND the registered
    check confirms the pattern is still present in the current ERR EMEVD."""
    for entry in BROKEN_SPAWNS:
        if entry['map'] != map_name or entry['part_name'] != part_name:
            continue
        em = _load_emevd(map_name)
        if em is None:
            return False  # missing EMEVD - be conservative, keep icon
        try:
            return bool(entry['check'](em))
        except Exception:
            return False
    return False


def report():
    """For pipeline log - print which registered spawn issues are currently active."""
    print('Checking known ERR EMEVD spawn issues:')
    for entry in BROKEN_SPAWNS:
        em = _load_emevd(entry['map'])
        if em is None:
            status = '?(emevd missing)'
        else:
            try:
                active = entry['check'](em)
                status = 'still present' if active else 'no longer present - marker re-enabled'
            except Exception as e:
                status = f'check failed: {e}'
        print(f"  [{status}] {entry['map']}/{entry['part_name']}: {entry['reason']}")


if __name__ == '__main__':
    import sys
    sys.path.insert(0, '.')
    report()
