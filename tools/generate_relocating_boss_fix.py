#!/usr/bin/env python3
"""Relocating-boss marker fix (currently: Ancient Dragon Lansseax).

A few vanilla bosses flee their first arena and become killable at a SECOND
location (Lansseax: flies off at ~20% HP, then fought elsewhere). The drop is
only obtainable at the kill-spawn, but the boss's lot/rune is also referenced at
the flee-spawn MSB entity, so we generate DUPLICATE, un-collectable loot markers
there - and the flee-spawn shows loot instead of "the boss was here, it left".

This post-pass (runs after the marker generators, before generate_data) edits the
per-profile MASSEDIT to, for each detected flee-spawn:
  * REMOVE the duplicate loot/rune markers (the real ones stay at the kill-spawn);
  * ENSURE the flee-spawn boss marker clears on the FLEE flag (so it shows the
    boss, then checkmarks once it flies off).

Detection is data-derived, no hardcoded boss list:
  relocating boss = ONE npcParamID placed at 2 tiles (from boss_list.json)
  AND one tile's boss defeat flag is referenced as a CONDITION in the OTHER
  tile's EMEVD (the relocation gate). That intersection matches ONLY Lansseax
  today (Night's Cavalry / Burial Watchdog reuse an npc but have no cross-link;
  Ghostflame Dragon / Fallingstar Beast are distinct npcs). Vanilla content, so
  it applies to every profile.
"""
import sys, os, re, struct, tempfile, shutil, json, collections
sys.path.insert(0, os.path.dirname(__file__))
import config

MASSEDIT_DIR = config.DATA_DIR / "massedit_generated"
RADIUS = 50.0   # loot within this of the flee-spawn boss = its duplicate drop


def _sf():
    from pythonnet import load; load("coreclr")
    import clr
    from System.Reflection import Assembly
    asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL)); clr.AddReference(str(config.SOULSFORMATS_DLL))
    import SoulsFormats
    src = config.GAME_DIR / "oo2core_6_win64.dll"
    for d in (config.LIB_DIR, tempfile.gettempdir(), os.getcwd()):
        p = os.path.join(str(d), "oo2core_6_win64.dll")
        if src.exists() and not os.path.exists(p):
            shutil.copy2(str(src), p)
    return asm, SoulsFormats


def _emevd_flag_refs(asm, SoulsFormats, event_dir, tile):
    """Set of event flags referenced as conditions (bank 1003) in a tile's EMEVD."""
    from System import Array, Object
    from System import Type as SysType
    from System.IO import File as SysFile
    from System.Reflection import BindingFlags
    _s = SysType.GetType("System.String")
    read = asm.GetType("SoulsFormats.EMEVD").GetMethod(
        "Read", BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
        None, Array[SysType]([_s]), None)
    p = event_dir / f"{tile}.emevd.dcx"
    if not p.exists():
        return set()
    tmp = os.path.join(tempfile.gettempdir(), f"{os.getpid()}_rbf.tmp")
    SysFile.WriteAllBytes(tmp, SoulsFormats.DCX.Decompress(str(p)).ToArray())
    em = read.Invoke(None, Array[Object]([tmp])); os.unlink(tmp)
    refs = set()
    for ev in em.Events:
        for ins in ev.Instructions:
            if int(ins.Bank) != 1003:
                continue
            a = bytes(ins.ArgData)
            for o in range(0, len(a) - 3, 4):
                v = struct.unpack_from("<i", a, o)[0]
                if v > 10000000:
                    refs.add(v)
    return refs


CACHE = config.PROJECT_DIR / "data" / "relocating_flee_spawns.json"


def _scan_game_for_flee_spawns():
    """Derive relocating-boss flee-spawns from VANILLA game files (build-agnostic;
    the dragon is base-game content). Signature: a boss-death flag (= entity id)
    set by 90005860/70/80 in tile A and referenced as a condition in tile B,
    AND the same npcParamID present in both A and B (true relocation, not a
    reused-npc separate boss). Returns the list + caches it."""
    asm, SoulsFormats = _sf()          # loads pythonnet/clr first
    from System import Array, Object
    from System import Type as SysType
    from System.IO import File as SysFile
    from System.Reflection import BindingFlags
    _s = SysType.GetType("System.String")
    em_read = asm.GetType("SoulsFormats.EMEVD").GetMethod(
        "Read", BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
        None, Array[SysType]([_s]), None)
    msb_read = asm.GetType("SoulsFormats.MSBE").GetMethod(
        "Read", BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
        None, Array[SysType]([_s]), None)
    import glob
    boss_flag, refs = {}, {}   # flag -> set(tiles); flag -> set(tiles referenced in)
    for ep in sorted(glob.glob(str(config.GAME_DIR / "event" / "m60_*.emevd.dcx")) +
                     glob.glob(str(config.GAME_DIR / "event" / "m61_*.emevd.dcx"))):
        mp = os.path.basename(ep).replace(".emevd.dcx", "")
        try:
            tmp = os.path.join(tempfile.gettempdir(), f"{os.getpid()}_rbf.tmp")
            SysFile.WriteAllBytes(tmp, SoulsFormats.DCX.Decompress(ep).ToArray())
            em = em_read.Invoke(None, Array[Object]([tmp])); os.unlink(tmp)
        except Exception:
            continue
        for ev in em.Events:
            for ins in ev.Instructions:
                a = bytes(ins.ArgData); b = int(ins.Bank)
                if b == 2000 and len(a) >= 12 and struct.unpack_from("<i", a, 4)[0] in (90005860, 90005870, 90005880):
                    f = struct.unpack_from("<i", a, 8)[0]
                    if f > 10000000:
                        boss_flag.setdefault(f, set()).add(mp)
                elif b == 1003 and len(a) >= 8:
                    for o in range(0, len(a) - 3, 4):
                        v = struct.unpack_from("<i", a, o)[0]
                        if v > 10000000:
                            refs.setdefault(v, set()).add(mp)

    def msb_enemies(tile):
        p = config.GAME_DIR / "map" / "MapStudio" / f"{tile}.msb.dcx"
        if not p.exists():
            return {}
        msb = msb_read.Invoke(None, Array[Object]([str(p)]))
        return {int(e.EntityID): (str(e.ModelName), int(e.NPCParamID),
                                  float(e.Position.X), float(e.Position.Y), float(e.Position.Z))
                for e in msb.Parts.Enemies}

    def flee_flag(tile, gx, gz, death_flag):
        """The 'flew away' flag, NOT the death flag. A relocating boss never
        dies at its first arena (it flees at low HP), so 90005860/70/80's death
        flag is never set there - the marker must clear on the FLEE flag instead.
        Signature: a flag Set ON (2003:66) directly in this tile AND read back
        as a condition (bank 1003) that gates the boss's appearance - i.e. the
        local 'I've fled, stop spawning me here' flag. Confined to the tile's
        own flag range (1e9 + gx*1e6 + gz*1e4 + local), distinct from the death
        flag (which is set via a common-event template, never a direct 2003:66).
        Returns None if absent → caller falls back to the death flag."""
        p = config.GAME_DIR / "event" / f"{tile}.emevd.dcx"
        if not p.exists():
            return None
        tmp = os.path.join(tempfile.gettempdir(), f"{os.getpid()}_rbf.tmp")
        SysFile.WriteAllBytes(tmp, SoulsFormats.DCX.Decompress(str(p)).ToArray())
        em = em_read.Invoke(None, Array[Object]([tmp])); os.unlink(tmp)
        lo = 1_000_000_000 + gx * 1_000_000 + gz * 10_000
        hi = lo + 10_000
        set_on, cond = set(), set()
        for ev in em.Events:
            for ins in ev.Instructions:
                b, a = int(ins.Bank), bytes(ins.ArgData)
                if b == 2003 and int(ins.ID) == 66 and len(a) >= 9 and a[8] == 1:
                    f = struct.unpack_from("<i", a, 4)[0]       # Target Event Flag ID, ON
                    if lo <= f < hi and f != death_flag:
                        set_on.add(f)
                elif b == 1003:                                 # any flag-reading condition
                    for o in range(0, len(a) - 3, 4):
                        v = struct.unpack_from("<i", a, o)[0]
                        if lo <= v < hi:
                            cond.add(v)
        both = set_on & cond
        return min(both) if both else (min(set_on) if set_on else None)

    out = []
    for flag, set_tiles in boss_flag.items():
        dest_tiles = refs.get(flag, set()) - set_tiles
        if not dest_tiles:
            continue
        flee_tile = sorted(set_tiles)[0]                 # tile where the flag is set
        ents = msb_enemies(flee_tile)
        if flag not in ents:                             # flag == the flee-spawn entity id
            continue
        model, npc, x, y, z = ents[flag]
        # confirm the SAME npc exists at a destination tile (true relocation)
        if not any(npc in {n for (_, n, _, _, _) in msb_enemies(dt).values()} for dt in dest_tiles):
            continue
        a, gx, gz = int(flee_tile[1:3]), int(flee_tile[4:6]), int(flee_tile[7:9])
        mnum = int(model[1:]) if model[:1] == "c" and model[1:].isdigit() else 0
        marker_flag = flee_flag(flee_tile, gx, gz, flag) or flag   # flee flag, not death
        out.append({"tile": [a, gx, gz], "flag": marker_flag, "death_flag": flag,
                    "x": x, "y": y, "z": z,
                    "enemy_id": 900000000 + mnum * 1000 + 4 if mnum else 0, "model": model})
    try:
        CACHE.parent.mkdir(parents=True, exist_ok=True)
        json.dump(out, open(CACHE, "w", encoding="utf-8"), indent=1)
    except OSError:
        pass
    return out


def detect_flee_spawns(refresh=False):
    """Build-agnostic flee-spawn list (cached). tile stored as [a,gx,gz]."""
    if not refresh and CACHE.exists():
        data = json.load(open(CACHE, encoding="utf-8"))
    else:
        data = _scan_game_for_flee_spawns()
    return [{**d, "tile": tuple(d["tile"])} for d in data]


# ── MASSEDIT row parsing ──────────────────────────────────────────────────────
ROW = re.compile(r"^param WorldMapPointParam: id (\d+): (\w+): = (-?\d+(?:\.\d+)?);")


def parse_rows(text):
    """-> dict id -> dict field->value(str), preserving order via 'order' list."""
    rows = collections.OrderedDict()
    for ln in text.splitlines():
        m = ROW.match(ln)
        if not m:
            continue
        rid, field, val = int(m.group(1)), m.group(2), m.group(3)
        rows.setdefault(rid, {})[field] = val
    return rows


def _at_tile(fields, tile):
    return (int(float(fields.get("areaNo", -1))) == tile[0]
            and int(float(fields.get("gridXNo", -1))) == tile[1]
            and int(float(fields.get("gridZNo", -1))) == tile[2])


def _has_enemy(fields, enemy_id):
    return any(int(float(fields.get(f"textId{i}", 0))) == enemy_id for i in range(1, 9))


def _near(fields, x, z):
    try:
        return (float(fields.get("posX", 1e9)) - x) ** 2 + (float(fields.get("posZ", 1e9)) - z) ** 2 <= RADIUS ** 2
    except ValueError:
        return False


def main():
    report = "--report" in sys.argv
    spawns = detect_flee_spawns()
    print(f"[relocating-boss-fix] profile={config.PROFILE} flee-spawns detected: {len(spawns)}")
    for s in spawns:
        print(f"  {s.get('name') or s['model']} flee-spawn tile "
              f"m{s['tile'][0]}_{s['tile'][1]}_{s['tile'][2]} flag={s['flag']} enemy_id={s['enemy_id']}")
    if not spawns:
        return

    removed = boss_fixed = boss_added = 0

    # ── World - Bosses: fix the flee-spawn boss flag, or ADD a marker if none ──
    bf = MASSEDIT_DIR / "World - Bosses.MASSEDIT"
    if bf.exists():
        text = bf.read_text(encoding="utf-8")
        rows = parse_rows(text)
        fix_ids = {}          # rid -> flag (existing marker, wrong flag)
        covered = set()       # spawn indices already shown at the flee-spawn
        max_id = max((rid for rid in rows), default=9000000)
        for rid, fields in rows.items():
            for i, s in enumerate(spawns):
                if _at_tile(fields, s["tile"]) and _has_enemy(fields, s["enemy_id"]) and _near(fields, s["x"], s["z"]):
                    covered.add(i)
                    if (fields.get("clearedEventFlagId") != str(s["flag"])
                            or fields.get("textDisableFlagId1") != str(s["flag"])):
                        fix_ids[rid] = s["flag"]
        add = [(i, s) for i, s in enumerate(spawns) if i not in covered]
        if report:
            for rid, flag in sorted(fix_ids.items()):
                print(f"  [World - Bosses] would SET row {rid} cleared/disable flag -> {flag}")
            for i, s in add:
                print(f"  [World - Bosses] would ADD boss marker at flee-spawn "
                      f"m{s['tile'][0]}_{s['tile'][1]}_{s['tile'][2]} flag={s['flag']} enemy={s['enemy_id']}")
        else:
            out = []
            for ln in text.splitlines():
                m = ROW.match(ln)
                if m and int(m.group(1)) in fix_ids and m.group(2) in ("clearedEventFlagId", "textDisableFlagId1"):
                    ln = f"param WorldMapPointParam: id {int(m.group(1))}: {m.group(2)}: = {fix_ids[int(m.group(1))]};"
                out.append(ln)
            for n, (i, s) in enumerate(add):
                rid = max_id + 1 + n
                a, gx, gz = s["tile"]
                out += [
                    f"param WorldMapPointParam: id {rid}: iconId: = {__import__('icon_registry').iconid('bosses')};",
                    f"param WorldMapPointParam: id {rid}: dispMask00: = 1;",
                    f"param WorldMapPointParam: id {rid}: areaNo: = {a};",
                    f"param WorldMapPointParam: id {rid}: gridXNo: = {gx};",
                    f"param WorldMapPointParam: id {rid}: gridZNo: = {gz};",
                    f"param WorldMapPointParam: id {rid}: posX: = {s['x']:.3f};",
                    f"param WorldMapPointParam: id {rid}: posY: = {s.get('y',0.0):.3f};",
                    f"param WorldMapPointParam: id {rid}: posZ: = {s['z']:.3f};",
                    f"param WorldMapPointParam: id {rid}: textId1: = {s['enemy_id']};",
                    f"param WorldMapPointParam: id {rid}: clearedEventFlagId: = {s['flag']};",
                    f"param WorldMapPointParam: id {rid}: textDisableFlagId1: = {s['flag']};",
                    f"param WorldMapPointParam: id {rid}: selectMinZoomStep: = 1;",
                ]
            bf.write_text("\n".join(out) + "\n", encoding="utf-8")
        boss_fixed += len(fix_ids); boss_added += len(add)

    # ── loot / rune files: remove the duplicate flee-spawn drops ──
    for f in sorted(MASSEDIT_DIR.glob("*.MASSEDIT")):
        if f.name == "World - Bosses.MASSEDIT":
            continue
        text = f.read_text(encoding="utf-8")
        rows = parse_rows(text)
        drop_ids = {rid for rid, fields in rows.items()
                    for s in spawns
                    if _at_tile(fields, s["tile"]) and _has_enemy(fields, s["enemy_id"]) and _near(fields, s["x"], s["z"])}
        if not drop_ids:
            continue
        if report:
            print(f"  [{f.name}] would REMOVE rows: {sorted(drop_ids)}")
        else:
            out = [ln for ln in text.splitlines()
                   if not (ROW.match(ln) and int(ROW.match(ln).group(1)) in drop_ids)]
            f.write_text("\n".join(out) + "\n", encoding="utf-8")
        removed += len(drop_ids)

    print(f"[relocating-boss-fix] removed {removed} duplicate loot/rune markers, "
          f"fixed {boss_fixed} boss flag(s), added {boss_added} flee-spawn boss marker(s)")
    if not report:   # sentinel (OUTSIDE massedit dir so it doesn't churn that dir's signature)
        (config.DATA_DIR / "_relocating_boss_fix.done").write_text(
            f"removed={removed} boss_fixed={boss_fixed} boss_added={boss_added}\n", encoding="utf-8")


if __name__ == "__main__":
    main()
