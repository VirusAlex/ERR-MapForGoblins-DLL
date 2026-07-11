#!/usr/bin/env python3
"""Detect same-position switched-chest loot pairs and cross-hide them on pickup.

A tiny, self-limiting rule (NOT hardcoded to any map): two DIFFERENT loot assets
stacked at the SAME world position whose EMEVD toggles them mutually-exclusively via
ChangeAssetEnableState (2005:3), gated by a GotoIfEventFlag (1003:101). Today this
matches exactly ONE spot in every profile - m31_00 (Patches' cave): the Cloth chest
AEG099_630_9001 vs the Glass Shard chest AEG099_630_9002, switched on flag 3691 - and
nothing else. Single-asset map-state toggles (e.g. Leyndell/Ashen Capital on flag 4060)
are NOT stacked pairs, so they are excluded. A future build adding such a stack is
picked up automatically.

We ENABLE-gate only the chest that is present when the switch flag is ON (group-2
textEnableFlag2Id*): its marker shows only while that chest exists, and still hides on
pickup via its group-1 pickup flag. The engine hides a slot only when its group-1 AND
group-2 DISABLE flags BOTH fire (an AND - verified live), so a group-2 DISABLE switch
flag would break the sibling's pickup-hide; the enabled-when-OFF sibling is therefore
left ungated (group-1 pickup only). See build_switch_gate_map for the full rationale.

Returns {(map, partName): show_on_flag} so the loot generator writes textEnableFlag2Id*
= flag on every populated slot (icon shows only when the flag is ON). Group 1
(textDisableFlagId*) stays free for the DLL's category-visibility / live-loot logic.
"""
import os, tempfile, struct
from collections import defaultdict

import config
from pythonnet import load
load('coreclr')
import clr
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object
from System.IO import File as SysFile
clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats

_asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
_str = SysType.GetType('System.String')
_emevd_read = _asm.GetType('SoulsFormats.EMEVD').GetMethod(
    'Read', BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str]), None)
_msbe_read = _asm.GetType('SoulsFormats.MSBE').GetMethod(
    'Read', BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str]), None)


def _read(meth, path, suf):
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + suf)
    SysFile.WriteAllBytes(tmp, SoulsFormats.DCX.Decompress(str(path)).ToArray())
    return meth.Invoke(None, Array[Object]([tmp]))


def _switches_in_event(evt):
    """{asset_entity: (flag, enabled_when_on)} for one switch event (empty if none).

    2005:3 = ChangeAssetEnableState(asset, state); 1014:0 = Label0; 1003:101 =
    GotoIfEventFlag (flag at arg offset 4, ON/OFF in byte 1 of arg 0). An asset that
    appears with BOTH state 1 and state 0 in the event is switched; its enable op being
    after Label0 means it belongs to the goto-taken (flag == guard_on) branch.
    """
    enates, label_idx, guard, guard_on = [], None, None, None
    for k, i in enumerate(evt.Instructions):
        b, iid = int(i.Bank), int(i.ID)
        a = bytes(i.ArgData) if i.ArgData else b''
        if b == 2005 and iid == 3 and len(a) >= 8:
            enates.append((k, struct.unpack_from('<i', a, 0)[0], struct.unpack_from('<i', a, 4)[0]))
        elif b == 1014 and iid == 0 and label_idx is None:
            label_idx = k
        elif b == 1003 and iid == 101 and guard is None and len(a) >= 8:
            guard = struct.unpack_from('<i', a, 4)[0]
            guard_on = ((struct.unpack_from('<i', a, 0)[0] >> 8) & 0xff) == 1
    if guard is None or guard <= 0 or label_idx is None:
        return {}
    by = defaultdict(list)
    for idx, asset, state in enates:
        by[asset].append((idx, state))
    out = {}
    for asset, v in by.items():
        if any(s == 1 for _, s in v) and any(s == 0 for _, s in v):
            en_idx = next(idx for idx, s in v if s == 1)
            ewo = guard_on if en_idx > label_idx else (not guard_on)
            out[asset] = (guard, ewo)
    return out


def build_switch_gate_map(records):
    """{(map, partName): show_on_flag} for switched-chest markers ENABLED when a
    flag is ON.

    Two DIFFERENT loot assets stacked at one spot that the EMEVD toggles
    mutually-exclusively (ChangeAssetEnableState XOR on a flag). We gate ONLY the
    chest that is enabled-when-flag-ON, via a group-2 ENABLE flag
    (textEnableFlag2Id*): its marker shows only while that chest is physically
    present (flag ON), and still hides on pickup via group-1.

    We deliberately do NOT gate the enabled-when-flag-OFF sibling. The engine hides
    a text slot only when its group-1 AND group-2 DISABLE flags BOTH fire (an AND,
    not an OR - verified live on m31_00: cloth with textDisableFlag2Id=3691 stayed
    visible after pickup because 3691 was OFF, so only ONE of the two disable flags
    fired). A textDisableFlag2Id switch flag on that sibling would therefore break
    its own pickup-hide: it would need BOTH its pickup flag AND the switch flag set
    to disappear. Leaving it with just its group-1 pickup flag keeps pickup-hide
    working; the only cost is it can linger after the switch flips to the other
    chest without the player looting it (rare; once looted it stays hidden).

    Returns {(map, partName): flag} -> the loot generator writes textEnableFlag2Id*
    = flag on every populated slot (icon shows only when the flag is ON).
    """
    root = config.require_err_mod_dir()
    msb_dir = root / 'map' / 'MapStudio'
    ev_dir = root / 'event'

    # 1. same-position stacked pairs: >=2 distinct asset partNames at one rounded spot
    groups = defaultdict(set)
    for r in records:
        if r.get('source') != 'treasure':
            continue
        pn = r.get('partName', '')
        if not pn:
            continue
        key = (r.get('map', ''), round(r.get('x', 0.0), 1),
               round(r.get('y', 0.0), 1), round(r.get('z', 0.0), 1))
        groups[key].add(pn)
    stacked = {k: parts for k, parts in groups.items() if len(parts) >= 2}
    if not stacked:
        return {}

    # 2. confirm the XOR-switch via EMEVD; keep ONLY the enabled-when-flag-ON parts
    #    (safe to ENABLE-gate). Enabled-when-flag-OFF parts are left ungated so their
    #    own group-1 pickup flag keeps working (see docstring - AND-disable engine).
    result = {}
    for tile in sorted({k[0] for k in stacked}):
        ev_path = ev_dir / f'{tile}.emevd.dcx'
        msb_path = msb_dir / f'{tile}.msb.dcx'
        if not ev_path.exists() or not msb_path.exists():
            continue
        try:
            em = _read(_emevd_read, ev_path, '_sw_e.tmp')
        except Exception:
            continue
        sw = {}  # asset_entity -> (flag, enabled_when_on)
        for evt in em.Events:
            for asset, fe in _switches_in_event(evt).items():
                sw.setdefault(asset, fe)
        if not sw:
            continue
        stacked_parts = set()
        for k, parts in stacked.items():
            if k[0] == tile:
                stacked_parts |= parts
        try:
            msb = _read(_msbe_read, msb_path, '_sw_m.tmp')
        except Exception:
            continue
        for a in msb.Parts.Assets:
            nm = str(a.Name)
            e = int(getattr(a, 'EntityID', 0) or 0)
            if e in sw and nm in stacked_parts:
                flag, enabled_when_on = sw[e]
                if enabled_when_on:  # safe textEnableFlag2Id gate; skip the OFF twin
                    result[(tile, nm)] = flag
    return result
