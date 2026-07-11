"""Single source of truth for the AOB byte-pattern signatures our DLL scans for
in eldenring.exe at runtime (modutils::scan).

A game update that shifts code silently breaks these runtime scans -> map
injection just stops working with no diagnostic. tools/check_aobs.py resolves
each signature statically against the target exe at BUILD time so a break is
caught before we ship.

Keep this list in sync with the .cpp string literals. The drift guard
(check_drift, run by check_aobs.py) greps src/ for hex-pattern string literals
and fails the build if any literal is not covered by an entry here, so a newly
added AOB cannot silently go unchecked.

Each entry:
  name     - short stable id (used in the report / JSON key)
  pattern  - the AOB string exactly as it appears in source ('??' = wildcard)
  slot     - (disp_off, instr_len) to resolve a RIP-relative slot RVA from the
             match (mirrors modutils ScanArgs.relative_offsets), or None if the
             match address itself is the target (direct function entry).
  critical - True: a miss/ambiguous match FAILS the build (load-bearing).
             False: a miss WARNS only (cosmetic / niche feature).
  refs     - source location(s) the literal lives at (file:line).
  note     - optional caveat (e.g. contains a build-specific rel32).
"""

# NOTE: patterns are duplicated verbatim from the source string literals. The
# drift guard cross-checks src/ against this list, so a divergence is caught.
SIGNATURES = [
    # ---- Event flag API (loot/boss/grace gating) - load-bearing ----
    {
        "name": "is_event_flag",
        "pattern": "48 83 EC 28 8B 12 85 D2",
        "slot": None,
        "critical": True,
        "refs": ["goblin_markers.cpp:81", "goblin_kindling.cpp:96", "goblin_inject.cpp:1297"],
        "note": "IsEventFlag(). Match address is the function entry (called directly).",
    },
    {
        "name": "event_man_slot",
        "pattern": "48 8B 3D ?? ?? ?? ?? 48 85 FF ?? ?? 32 C0 E9",
        "slot": (3, 7),
        "critical": True,
        "refs": ["goblin_markers.cpp:89", "goblin_kindling.cpp:103", "goblin_inject.cpp:1299"],
    },
    # ---- Live marker array chain (the whole point of the mod) ----
    {
        "name": "marker_chain_slot",
        "pattern": "48 8B 0D ?? ?? ?? ?? 48 8B 49 30 48 8D 55 5F",
        "slot": (3, 7),
        "critical": True,
        "refs": ["goblin_markers.cpp:171"],
    },
    {
        "name": "marker_container_vtable",
        "pattern": "48 8D 05 ?? ?? ?? ?? 48 89 07 48 8D 5F 10 48 8D 05 ?? ?? ?? ??",
        "slot": (3, 7),
        "critical": True,
        "refs": ["goblin_markers.cpp:177"],
    },
    # ---- Param list (needed before any marker injection) ----
    {
        "name": "param_list_slot",
        "pattern": "48 8B 0D ?? ?? ?? ?? 48 85 C9 0F 84 ?? ?? ?? ?? 45 33 C0 BA 90",
        "slot": (3, 7),
        "critical": True,
        "refs": ["from/params.cpp:15"],
    },
    # ---- Message repository (all marker text) ----
    {
        "name": "msg_repository_slot",
        "pattern": "48 8B 3D ?? ?? ?? ?? 44 0F B6 30 48 85 FF 75",
        "slot": (3, 7),
        "critical": True,
        "refs": ["goblin_messages.cpp:483"],
    },
    # ---- Collected-tracking (GEOF/WGM live hide) ----
    {
        "name": "geom_flag_slot",
        "pattern": "48 8B 3D ?? ?? ?? ?? 33 F6 48 85 FF 74 ?? 48 8B CF E8 ?? ?? ?? ?? 4C 8B 07",
        "slot": (3, 7),
        "critical": True,
        "refs": ["goblin_collected.cpp:118"],
    },
    {
        "name": "world_geom_man_slot",
        "pattern": "48 8B 0D ?? ?? ?? ?? 48 8D 53 10 E8 ?? ?? ?? ?? 4C 8B E8",
        "slot": (3, 7),
        "critical": True,
        "refs": ["goblin_collected.cpp:124"],
    },
    # ---- GFX icon injection (no-gfx icon rendering) - load-bearing ----
    {
        "name": "gfx_ctor",
        "pattern": "45 33 C0 48 8D 05 ?? ?? ?? ?? 48 89 01 48 8D 05 ?? ?? ?? ?? "
                   "C7 41 08 01 00 00 00 4C 89 41 10",
        "slot": None,
        "critical": True,
        "refs": ["goblin_gfx_probe.cpp:1156"],
    },
    {
        "name": "gfx_adddisp",
        "pattern": "4C 89 4C 24 20 4C 89 44 24 18 55 53 41 54 41 55 41 56 41 57 "
                   "48 8D 6C 24 F9",
        "slot": None,
        "critical": True,
        "refs": ["goblin_gfx_probe.cpp:1162"],
    },
    {
        "name": "gfx_lookup",
        "pattern": "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 49 8B F8 8B DA "
                   "48 8B F1 E8 F4 F8 FF FF",
        "slot": None,
        "critical": True,
        "refs": ["goblin_gfx_probe.cpp:1166"],
        "note": "Ends in a fixed rel32 call disp (E8 F4 F8 FF FF); if a game "
                "update shifts the call target this tail must be re-found.",
    },
    {
        "name": "gfx_registrar",
        "pattern": "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 "
                   "41 8B 00 48 8B F9 48 8B 49 38",
        "slot": None,
        "critical": True,
        "refs": ["goblin_gfx_probe.cpp:1174"],
    },
    {
        "name": "gfx_lossless",
        "pattern": "40 53 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 68 48 8B 99 "
                   "18 04 00 00",
        "slot": None,
        "critical": True,
        "refs": ["goblin_gfx_probe.cpp:1177"],
    },
    {
        "name": "gfx_spriteloader",
        "pattern": "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 99 18 04 "
                   "00 00 48 8B F9 48 85 DB",
        "slot": None,
        "critical": True,
        "refs": ["goblin_gfx_probe.cpp:1180"],
    },
    {
        "name": "game_crt_malloc",
        "pattern": "40 53 48 83 EC 20 48 8B D9 48 83 F9 E0 77 ?? 48 85 C9 B8 01 00 00 00 48 0F 44 D8 EB ?? E8 ?? ?? ?? ?? 85 C0 74 ?? 48 8B CB E8 ?? ?? ?? ?? 85 C0",
        "slot": None,
        "critical": True,
        "refs": ["goblin_gfx_probe.cpp:169"],
        "note": "_malloc_base (game static-CRT malloc). We allocate Scaleform-owned buffers (frame array/tag arrays/tags) here so the game's _free_base frees them on the same _crtheap; a foreign heap there = corruption (v2.0.4 crashes).",
    },
    # ---- Toast fallback (cosmetic on-screen popup) - non-critical ----
    {
        "name": "show_tutorial_popup",
        "pattern": "48 8B 05 ?? ?? ?? ?? 8B D1 48 85 C0 74 17 48 8B 88 80 00 00 00 48 85 C9",
        "slot": None,
        "critical": False,
        "refs": ["goblin_inject.cpp:1204"],
    },
    # ---- Kindling per-spirit liveness (niche feature) - non-critical ----
    {
        "name": "kindling_distance_vft",
        "pattern": "48 8D 05 ?? ?? ?? ?? 48 89 01 48 8D 05 ?? ?? ?? ?? 48 89 01 F6 C2 01 74 ?? "
                   "BA 40 00 00 00 E8 ?? ?? ?? ?? 90 48 8B C3 48 83 C4 30 5B C3 90 78 ??",
        "slot": (3, 7),
        "critical": False,
        "refs": ["goblin_kindling.cpp:181"],
    },
    {
        "name": "kindling_world_sfx_man_slot",
        "pattern": "48 8B 05 ?? ?? ?? ?? 48 8D 4D 98 48 89 4C 24 60",
        "slot": (3, 7),
        "critical": False,
        "refs": ["goblin_kindling.cpp:189"],
    },
    # ---- Fast map-open timing hooks (cosmetic perf) - non-critical ----
    {
        "name": "map_callsite_dispatcher",
        "pattern": "48 8B 89 18 01 00 00 E8 ?? ?? ?? ?? 33 D2 48 8B CF E8 ?? ?? ?? ?? "
                   "BA 01 00 00 00 48 8B CF E8 ?? ?? ?? ?? BA 03 00 00 00 48 8B CF "
                   "E8 ?? ?? ?? ?? 48 8B 5C 24 30 B0 01",
        "slot": None,
        "critical": False,
        "refs": ["goblin_map_timing.cpp:151"],
    },
    {
        "name": "map_refresh_hook",
        "pattern": "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 41 20 "
                   "48 8B D9 48 8B 50 10 48 8B",
        "slot": None,
        "critical": False,
        "refs": ["goblin_map_timing.cpp:169"],
    },
    {
        "name": "map_ce390_hook",
        "pattern": "40 53 48 83 EC 50 33 C0 89 54 24 48 4C 8D 81 C8 00 00 00 89",
        "slot": None,
        "critical": False,
        "refs": ["goblin_map_timing.cpp:173"],
    },
    {
        "name": "map_wmd_dtor_hook",
        "pattern": "48 89 4C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 30 "
                   "48 C7 45 F0 FE FF FF FF 48 89 9C 24 88 00 00 00 48 8B F1 48 8D 05 "
                   "B7 A3 16 02",
        "slot": None,
        "critical": False,
        "refs": ["goblin_map_timing.cpp:177"],
        "note": "Ends in a build-specific lea disp (B7 A3 16 02); expected to "
                "shift on a game update - non-critical by design.",
    },
]


def _tokens(pattern):
    """Normalize a pattern to a space-joined token string (collapses the
    whitespace introduced by C++ adjacent-string-literal concatenation)."""
    return " ".join(pattern.split())


# Pre-normalized patterns for substring coverage checks.
_NORM_PATTERNS = [_tokens(s["pattern"]) for s in SIGNATURES]

import re
from pathlib import Path

# A run of >=6 byte/wildcard tokens - the same shape used to enumerate AOBs.
_HEX_RUN = re.compile(r"(?:[0-9A-Fa-f]{2}|\?\?)(?: (?:[0-9A-Fa-f]{2}|\?\?)){5,}")
# A whole string literal that is ENTIRELY byte/wildcard tokens (an AOB fragment).
_AOB_LITERAL = re.compile(r"^(?:[0-9A-Fa-f]{2}|\?\?)(?: (?:[0-9A-Fa-f]{2}|\?\?))*$")
_STRING_LITERAL = re.compile(r'"((?:[^"\\]|\\.)*)"')


def check_drift(src_dir):
    """Grep src/ for AOB-shaped string literals and return a list of
    (file:line, literal) fragments NOT covered by any SIGNATURES pattern.

    A C++ AOB may be split across adjacent string literals, so each source
    fragment is checked as a SUBSTRING of some normalized signature pattern.
    An empty return list means the list is in sync with the source."""
    src_dir = Path(src_dir)
    uncovered = []
    for path in sorted(src_dir.rglob("*.[ch]pp")):
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for lineno, line in enumerate(text.splitlines(), 1):
            for m in _STRING_LITERAL.finditer(line):
                lit = m.group(1).strip()
                # Only consider literals that ARE an AOB fragment (all hex/??
                # tokens) and long enough to be a real pattern piece.
                if not _AOB_LITERAL.match(lit):
                    continue
                toks = _tokens(lit)
                if len(toks.split()) < 4:
                    continue
                if not any(toks in norm for norm in _NORM_PATTERNS):
                    rel = path.relative_to(src_dir.parent) if src_dir.parent in path.parents else path
                    uncovered.append((f"{rel}:{lineno}", toks))
    return uncovered
