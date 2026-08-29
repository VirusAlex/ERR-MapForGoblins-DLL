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
    # ---- Front-end status line (the small bottom text, one slot) - the toggle announcements ----
    # The FE update's status block: `lea rbx,[rdi+3720]; movsxd rax,[rdi+59BC]; mov esi,[rdi+rax*4+59A4]`.
    # goblin_status_line locates the MenuString builder and the empty ctor from it by the byte shapes
    # of their calls, then answers the builder for its own id. A miss only silences the announcements.
    {
        "name": "fe_status_anchor",
        "pattern": "48 8D 9F 20 37 00 00 48 63 87 BC 59 00 00 8B B4 87 A4 59 00 00",
        "slot": None,
        "critical": False,
        "refs": ["goblin_status_line.cpp:19"],
        "note": "Match address is mid-function (the block itself), not a call target.",
    },
    # ---- CSFeManImp singleton (HUD mode restore after our over-gameplay screen) ----
    # The store at the tail of the manager's init. Only needed to put CSFeManImp+0x78 (the HUD
    # visibility mode) back to the value it had before we pushed a screen; a miss just means the HUD
    # stays hidden until the player opens any native menu, so this is not load-bearing.
    {
        "name": "feman_slot",
        "pattern": "48 89 05 ?? ?? ?? ?? 48 8B 8B 80 00 00 00 48 85 C9 74 05 E8",
        "slot": (3, 7),
        "critical": False,
        "refs": ["goblin_stall_probe.cpp:8506"],
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
    {
        "name": "world_chr_man_slot",
        "pattern": "48 8B 05 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? ?? 48 8B 98 08 E5 01 00",
        "slot": (3, 7),
        "critical": False,
        "refs": ["goblin_collected.cpp:139"],
    },
    {
        "name": "worldmap_build_markers",
        "pattern": "40 55 53 56 57 41 54 41 56 41 57 48 8B EC 48 83 EC 60 48 C7 45 D0 FE FF "
                   "FF FF 4C 8B F9 8B 42 34",
        "slot": None,
        "critical": False,
        "refs": ["goblin_maphover.cpp"],
    },
    {
        "name": "worldmap_converter",
        "pattern": "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 "
                   "48 83 EC 20 33 DB 4D 8B F9 4D 8B E0 4C 8B EA 48 8B F1 48 39 99 80 02 00 00",
        "slot": None,
        "critical": False,
        "refs": ["goblin_worldmap_probe.cpp"],
    },
    # ---- Map hover/projection vtable gates (resolved from each vtable's ctor lea) ----
    {
        "name": "maparea_vtable",
        "pattern": "48 8D 05 ?? ?? ?? ?? 48 89 01 48 8D 79 70 48 8B 07 48 8B CF FF 50 08",
        "slot": (3, 7),
        "critical": False,
        "refs": ["goblin_maphover.cpp"],
        "note": "CS::WorldMapArea vtable (was RVA 0x2B2CB08). Gates the r8 the hover hook "
                "publishes -> drives the map-layer highlight rings + overlay projection. "
                "A miss just disables those, not core icons.",
    },
    {
        "name": "pin_vtable",
        "pattern": "48 8D 05 ?? ?? ?? ?? 48 89 06 48 89 BE 30 02 00 00",
        "slot": (3, 7),
        "critical": False,
        "refs": ["goblin_maphover.cpp"],
        "note": "CS::WorldMapPointPinData vtable (was RVA 0x2AD6688). Gates the hovered pin "
                "-> marker hover-detect (manual hide / hover overlay). Miss disables hover.",
    },
    {
        "name": "node_get_writable_data",
        "pattern": "48 89 6C 24 20 56 41 54 41 56 48 83 EC 20 "
                   "48 8B F1 4C 8B C1 48 81 E6 00 F0 FF FF",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Render::Context GetWritableData(entry, changeFlags) - the copy-on-write + "
                "change-record step every visual property of a display object goes through "
                "(FUN_141157a70 v2.6.2.0). Identified via the Scaleform SDK: the projection "
                "setter calls it with 0x100000 = Change_State_ProjectionMatrix3D. The "
                "location emphasis uses it to fade other-map markers; a miss costs only "
                "the colour, size and draw order still apply.",
    },
    {
        "name": "record_materialize_driver",
        "pattern": "4C 8B DC 55 56 41 55 49 8D 6B D8 48 81 EC 10 01 00 00 "
                   "48 8D 41 48 4C 8B EA",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Timeline-record materialization walker (FUN_1411bf1b0 v2.6.2.0). "
                "The V3 factory runs it on its queued records; a miss limits native "
                "marker creation to the finite build-burst trickle.",
    },
    {
        "name": "sprite_goto_frame",
        "pattern": "4C 8B DC 55 53 56 49 8D 6B A1 48 81 EC 90 00 00 00 48 8B D9 8B F2 "
                   "0F B7 49 6A 0F B7 C1 66 C1 E8 0B A8 01 0F 84",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Sprite::GotoFrame (Sprite vtable +0x378, live exe+0x11C3360), the only "
                "caller of record_materialize_driver (two E8 sites, cross-checked at "
                "runtime). Drives the [v3pump] frame pump; a miss turns the pump off and "
                "every map open builds all markers in the engine's burst, as before 2.1.5.",
    },
    {
        "name": "detach_remove_at",
        "pattern": "40 57 48 83 EC 20 48 8B 41 18 48 8B F9 3B 90 E0 00 00 00 72 08 "
                   "33 C0 48 83 C4 20 5F C3",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Remove-child-at-index (FUN_1410c87c0 v1.16), the removal half of the "
                "reparent path FUN_1410c8440. Lever C bulk self-detach calls it at WMD "
                "dtor; a miss just disables self-detach (engine full teardown instead).",
    },
    {
        "name": "pin_factory_site",
        "pattern": "48 89 BE 30 02 00 00 48 8D 05 ?? ?? ?? ?? 48 89 86 38 02 00 00 "
                   "8B 45 08 89 86 40 02 00 00",
        "slot": None,
        "critical": False,
        "refs": ["goblin_maphover.cpp"],
        "note": "Unique lea site inside the pin builder (FUN_14087ba70 v2.6.2.0, entry = "
                "site-0x112). Powers the own-pin native-tooltip proxy for V3 markers; a "
                "miss falls back to borrowing a live pin (label needs a prior real hover).",
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
        "retired":
            "the AddDisplayObject hook was removed; nothing scans for this any more",
        "pattern": "4C 89 4C 24 20 4C 89 44 24 18 55 53 41 54 41 55 41 56 41 57 "
                   "48 8D 6C 24 F9",
        "slot": None,
        "critical": True,
        "refs": ["goblin_gfx_probe.cpp:1162"],
    },
    {
        "name": "gfx_lookup",
        "retired":
            "2026-09-23: the detour on this function was removed. It is DisplayList::FindByDepth "
            "(2.6.2 0x14113FD90), not a character-id lookup, so the movie latch and the tick() id "
            "check built on it never saw an id; character ids are chosen per world-map parse now",
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
        "retired":
            "2026-09-23: the detour on this function was removed. It is the bind-index registrar "
            "(images, fonts, external images - never sprites, shapes or imports), and its per-movie "
            "count read 0 on every load; the ids come from the movie's own stream and registry now",
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
        "name": "gfx_file_opener_open",
        "pattern": "40 55 53 56 57 41 54 41 56 41 57 48 8D 6C 24 D9 48 81 EC B0 00 00 00 "
                   "48 C7 45 B7 FE FF FF FF 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 1F 45 8B F9 "
                   "45 8B F0 48 8B DA 48 8B F1 48 8B 0D ?? ?? ?? ?? 48 85 C9 75",
        "slot": None,
        "critical": False,
        "refs": ["goblin_own_movie.cpp"],
        "note": "GFx FileOpener::OpenFile (this, url UTF-8, int, int): turns `menu:/Win/<name>.gfx` "
                "into a memory-backed File by a pure lookup in the preloaded-file repository. Hooked "
                "to alias OUR movie name onto the game's 02_160 file, which gives our screens their "
                "own movie definition (2026-09-06). Not critical: a miss falls back to transforming "
                "the shared parse, which bleeds into the player's own key-binding screen but works.",
    },
    {
        "name": "gfx_loader_create_movie",
        "pattern": "45 8B D0 48 8B C1 48 85 D2 74 1A 80 3A 00 74 15 48 8B 49 08 48 85 C9 74 0C "
                   "44 8B 40 18 45 0B C2 E9",
        "slot": None,
        "critical": False,
        "refs": ["goblin_own_movie.cpp"],
        "note": "GFxLoader::CreateMovie stub (FUN_14112b130 on 1.16; tail-jumps into the loader). "
                "Observation hook only (which URLs the loader is asked for): the world map's file never "
                "passes the file opener in the stock flow, and this is the level above it.",
    },
    {
        "name": "menu_movie_path_format",
        "pattern": "48 8B C4 55 57 41 56 48 8D 68 A8 48 81 EC 40 01 00 00 48 C7 44 24 60 FE FF FF FF "
                   "48 89 58 18 48 89 70 20 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 30 48 8B FA 48 8B D9 "
                   "48 89 4C 24 68 33 F6 89 74 24 30 4C 8B 42 08 48 8D 15 ?? ?? ?? ?? 48 8D 4D C8 E8 ?? ?? ?? ?? "
                   "90 48 8D 50 08 48 83 7A 18 08 72 03 48 8B 12",
        "slot": None,
        "critical": False,
        "refs": ["goblin_own_movie.cpp"],
        "note": "CSMenuMan movie descriptor -> URL (FUN_140d7b800 on 1.16): `menu:/Win/<name>.gfx`, then the "
                "FD4 device step - where me3 substitutes its `\\\\me3??NN` token for an overridden movie. "
                "Hooked so our world-map descriptor also learns the GAME's resolved URL to alias onto.",
    },
    {
        "name": "menu_movie_request_file",
        "pattern": "4C 8B DC 57 48 81 EC 90 00 00 00 49 C7 43 B8 FE FF FF FF 49 89 5B 18 49 89 73 20 "
                   "48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 88 00 00 00 48 8B FA 48 8B 99 98 09 00 00 "
                   "48 8D B1 90 09 00 00 4C 8B C2 49 8D 53 98 48 8B CE E8 ?? ?? ?? ?? 48 39 18",
        "slot": None,
        "critical": False,
        "refs": ["goblin_own_movie.cpp"],
        "note": "CSMenuMan on-demand file request by descriptor (FUN_140d77400 on 1.16): list at +0x990, "
                "else `menu:/Win/<name>.gfx` -> CSFile::Load. CALLED (not hooked) with the game's world-map "
                "descriptor at every redirect, so the real file is in the repository for the opener alias.",
    },
    {
        "name": "menu_movie_name_to_def",
        "pattern": "40 55 53 56 57 41 54 41 56 41 57 48 8D AC 24 20 FE FF FF 48 81 EC E0 02 00 00 "
                   "48 C7 44 24 60 FE FF FF FF 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 D0 01 00 00 "
                   "49 8B C0 48 89 44 24 50 48 8B FA 48 8B D9 48 89 4C 24 38 48 89 54 24 68 33 F6 "
                   "89 74",
        "slot": None,
        "critical": False,
        "refs": ["goblin_own_movie.cpp"],
        "note": "CSMenuMan: movie definition by descriptor NAME (FUN_140d7a630 on 1.16): the name "
                "cache, then `menu:/Win/<name>.gfx` through the Scaleform loader. Hooked to redirect the "
                "world map's descriptor to our own name so its definition parses after our hooks are "
                "live (2026-09-07). Not critical: a miss only restores the injection-timing dependency.",
    },
    {
        "name": "menu_movie_pin",
        "pattern": "4C 8B 81 00 0D 00 00 4C 8B DA 4C 8B D1 49 8B 00 49 3B C0 74 59 4C 8B 40 10 "
                   "4D 85 C0 74 44 41 0F 10 48 20 4D 8B 4B 08 66 0F 73 D9 08 66 48 0F 7E C9 "
                   "4C 2B C9 0F 1F 40 00 0F 1F 84 00 00 00 00 00 44 0F B7 01 42 0F B7 14 09 "
                   "44 2B C2 75 08 48 83",
        "slot": None,
        "critical": False,
        "refs": ["goblin_own_movie.cpp"],
        "note": "Body of the keep-resident pin the map menu puts on its movie by name (timer -1 on the "
                "cache entry; the exported entry is a 13-byte thunk). Redirected with the one above so "
                "our world-map definition is the pinned one.",
    },
    {
        "name": "gfx_tag_loop",
        "pattern": "4C 89 44 24 18 53 55 56 57 41 55 41 56 48 83 EC 68 48 8B AA 18 04 00 00 "
                   "49 8B F8 4C 8B EA 4C 8B F1 48 85 ED",
        "slot": None,
        "critical": True,
        "refs": ["goblin_own_movie.cpp"],
        "note": "The movie tag loop. Entered with the header read and no tag read yet, which is where "
                "our own bytes are handed to the parser - the only point that sees a movie whichever "
                "loader served the file (ME3 answers for its overrides above the engine's own opener). "
                "The pattern covers the three facts the route rests on: arg3 saved, reader at ctx+0x418, "
                "inline reader fallback at ctx+0x50.",
    },
    {
        "name": "gfx_global_heap_slot",
        "pattern": "48 8B 0D ?? ?? ?? ?? 48 8B 01 45 33 C0 41 8D 50 30 FF 50 50",
        "slot": (3, 7),
        "critical": False,
        "refs": ["goblin_gfx_probe.cpp"],
        "note": "The Scaleform global MemoryHeap* slot (2.6.2: exe+0x4593250), read from a "
                "unique Alloc(0x30) call site (mov rcx,[slot]; mov rax,[rcx]; call [rax+0x50]). "
                "gfx_heap_alloc used the raw slot literal until 2026-08-05; scanned at runtime "
                "now so downpatched exes (2.2.3 keeps the slot at +0x20) still allocate the "
                "engine-freed frame array from the right heap.",
    },
    {
        "name": "game_crt_malloc",
        "pattern": "40 53 48 83 EC 20 48 8B D9 48 83 F9 E0 77 ?? 48 85 C9 B8 01 00 00 00 48 0F 44 D8 EB ?? E8 ?? ?? ?? ?? 85 C0 74 ?? 48 8B CB E8 ?? ?? ?? ?? 85 C0",
        "slot": None,
        "critical": True,
        "refs": ["goblin_gfx_probe.cpp:169"],
        "note": "_malloc_base (game static-CRT malloc). We allocate Scaleform-owned buffers (frame array/tag arrays/tags) here so the game's _free_base frees them on the same _crtheap; a foreign heap there = corruption (v2.0.4 crashes).",
    },
    {
        "name": "game_crt_aligned_malloc",
        "pattern": "48 89 5C 24 08 57 48 83 EC 20 33 DB 48 85 D2 74 ?? 48 8D 42 FF 48 85 C2 75 ?? 8D 43 08 48 3B D0 48 0F 47 C2",
        "slot": None,
        "critical": True,
        "refs": ["goblin_gfx_probe.cpp:238"],
        "note": "_aligned_malloc_base. FMG slot buffers MUST come from here: the engine releases a "
                "MsgRepository slot through DLKRD::HeapAllocator<Win32RuntimeHeapImpl>::Free, which is "
                "_aligned_free - and that frees the back-pointer stored at (p & ~7) - 8, not the pointer "
                "itself. A plain _malloc_base buffer has no back-pointer there, so the engine fed the "
                "XOR-encoded _HEAP_ENTRY to RtlFreeHeap and the process died with 0xC0000374 on ten of "
                "ten quits (traced 2026-08-04). Distinct from game_crt_malloc, which stays correct for "
                "the Scaleform tag objects - nothing ever frees those. Since 2026-08-12 this is the "
                "FALLBACK: dl_owner_lookup below is tried first (reports 25/27/28/37).",
    },
    {
        "name": "dl_owner_lookup",
        "pattern": "48 8B 5F 20 48 C7 47 20 00 00 00 00 48 8B 7C 24 38 48 85 DB 75 26 "
                   "48 8B CE E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 75 16 4C 8D 05 ?? ?? ?? ?? "
                   "8D 50 64 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 4C 8B 03 48 8B D6 "
                   "48 8B CB 41 FF 50 68",
        "slot": None,
        "critical": False,
        "refs": ["goblin_gfx_probe.cpp"],
        "note": "Slice of DLNew's operator delete (v2.6.2 0x1EBA020) around `call <owning-allocator "
                "lookup>` (0x1EC5DE0 resolved via the call's rel32, offsets {0x1A,0x1E}); anchored by "
                "the DLNew.cpp line-100 assert setup (8D 50 64) and the vtable free (41 FF 50 68). The "
                "lookup's own prologue is a generic singleton-getter shape (4 matches) - do NOT pattern "
                "it directly. dl_alloc_like() allocates handed-over buffers (expanded WMP/TutorialParam "
                "tables, PlaceName FMG) from the arena owning the ORIGINAL buffer, so the DL range "
                "lookup succeeds at release and the mod-host-SUBSTITUTED fallback allocator is never "
                "consulted (me3_mod_host AV / ME2 0xC0000374, reports 25/27/28/37). Non-critical: on a "
                "miss the callers fall back to game_crt_aligned_malloc (status quo).",
    },
    # ---- Hooks that used to be hardcoded RVAs (report 20) - a wrong address CORRUPTS CODE ----
    {
        "name": "rm2_addsnapshot",
        "pattern": "48 89 5C 24 18 56 41 56 41 57 48 83 EC 20 48 8B 01",
        "slot": None,
        "critical": True,
        "refs": ["goblin_gfx_probe.cpp:2416"],
        "note": "RemoveObject2::AddToTimelineSnapshot. Was the literal RVA 0x11BDE10, measured on game "
                "build 2.6.2.0. Report 20: a player on 2.6.1.0 crashed because that function sits 0x20 "
                "higher there, so the RVA landed on the second byte of `movzx edx,word[rcx+8]` in the "
                "neighbour, MinHook wrote its jmp over it, and the orphaned 0F plus the jmp decoded as "
                "`psubsw mm7,[rdi+0x30]` - AV reading 0x30. Present since v2.0.1, so it killed every "
                "player not on 2.6.2.0. Pattern is 17 bytes of pure opcode/ModRM with no wildcards.",
    },
    {
        "name": "po3_addsnapshot",
        "pattern": "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20 "
                   "48 8B 01 48 8B EA 48 8D 15 ?? ?? ?? ?? 45 8B F0 48 8B D9 FF 50 38 80 7B 08 00 "
                   "BE 01 00 00 00 B9 09 00 00 00 8B C6 0F 4C C1 0F B6 4B 08 33 FF 0F B6 54 18 0A "
                   "0F B6 44 18 09 66 C1 E2 08 66",
        "slot": None,
        "critical": False,
        "refs": ["goblin_gfx_probe.cpp:capture_tag_vtables"],
        "note": "PlaceObject3::AddToTimelineSnapshot, 0x11BDB40 on 2.6.2.0. NOT hooked - scanned once "
                "at setup so capture_tag_vtables can confirm a candidate tag vtable by its +0x30 slot "
                "CONTENT on any exe build (the mod+RVA literal compare failed on every shifted exe, "
                "report 30). It shares its first 0x4C bytes with PlaceObject2's snapshot fn except the "
                "RIP displacement (wildcarded); the tail bytes are the tag-BODY reads that tell the two "
                "apart - PO3 reads depth at body+2 (0F B6 54 18 0A), PO2 at body+1 - which is layout, "
                "not address, so it holds across builds (verified unique on 2.6.2/2.6.0/2.2.3). A miss "
                "only costs the vtable capture its confirmation (falls back to the baked-RVA compare), "
                "hence critical=False. The rm2 side needs no scan: the rm2exec hook already resolves "
                "that function, and its return value is what the compare uses.",
    },
    {
        "name": "menu_row_path",
        "pattern": "4C 8B DC 57 48 81 EC 90 00 00 00 49 C7 43 90 FE FF FF FF 49 89 5B 20 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 80 00 00 00 48 8B FA 48 8B D9 49 89 53 98 C7 44 24 20 00 00 00 00 45 8B 08 45 8B 40 04",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp:9337"],
        "note": "The menu row-slot path builder, hooked so row icons find their clip. Was the literal "
                "0x736FC0 - same landmine as rm2_addsnapshot. Needs 66 bytes for uniqueness; the four "
                "wildcards cover one RIP-relative displacement (the stack-cookie load). A miss only "
                "costs the menu row icons, hence critical=False.",
    },
    {
        "name": "input_action_test",
        "pattern": "4C 8B DC 48 81 EC 88 00 00 00 49 C7 43 98 FE FF FF FF 49 8D 43 B0 49 89 43 20 41 89 53 18 41 0F B6 00",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp:9357"],
        "note": "The input-action predicate, hooked only when debug_logging is on (the ESC action probe). "
                "Was the literal 0x758500. Diagnostics-only, but it was still a code-corrupting hook on "
                "any other game build, so it goes through an AOB like the rest.",
    },
    # ---- Active save slot (per-character manual hides) - non-critical ----
    {
        "name": "game_man_slot",
        "pattern": "48 8B 05 ?? ?? ?? ?? 80 B8 ?? ?? ?? ?? 0D 0F 94 C0 C3",
        "slot": (3, 7),
        "critical": False,
        "refs": ["goblin_inject.cpp"],
        "note": "GameMan singleton getter; GameMan+0xAC0 = active Save Slot (profile "
                "index). Scopes manual marker-hides per character. A miss just disables "
                "per-slot scoping (hides fall back to none), not core icons.",
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
        "retired":
            "replaced by RTTI (goblin_kindling.cpp resolves .?AVEcTestDistance@CS@@ by name). "
            "The pattern was unique only because it ran past the function ret into padding, so "
            "it MISSED on 2.6.1/2.2.3/2.2.0 and silently switched spirit tracking off there; "
            "trimmed to the function itself it matches 4 times on every build.",
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
        # The TWIN of map_callsite_dispatcher: the handler directly above it calls the same
        # refresh fn with a byte-identical sequence except the tail restores rbx from +0x38
        # (the main site restores from +0x30). Optional at runtime: a build without it keeps
        # the single-site Patch D skip.
        "name": "map_callsite_dispatcher_twin",
        "pattern": "48 8B 89 18 01 00 00 E8 ?? ?? ?? ?? 33 D2 48 8B CF E8 ?? ?? ?? ?? "
                   "BA 01 00 00 00 48 8B CF E8 ?? ?? ?? ?? BA 03 00 00 00 48 8B CF "
                   "E8 ?? ?? ?? ?? 48 8B 5C 24 38 B0 01",
        "slot": None,
        "critical": False,
        "refs": ["goblin_map_timing.cpp"],
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
        "name": "map_wmd_dtor_hook",
        # Was ending on the raw rip-relative displacement B7 A3 16 02, which made it a
        # 2.6.2-only pattern. Wildcarded + 4 bytes longer: single match on all five builds.
        "pattern": "48 89 4C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 30 "
                   "48 C7 45 F0 FE FF FF FF 48 89 9C 24 88 00 00 00 48 8B F1 48 8D 05 "
                   "?? ?? ?? ?? 48 89 01 48",
        "slot": None,
        "critical": False,
        "refs": ["goblin_map_timing.cpp:340"],
        "note": "Vtable lea disp wildcarded (it shifted on 1.17); the vtable store "
                "after it (48 89 01 48) keeps it unique.",
    },
    # ---- Stall-probe cost counters (debug_logging diagnostics) - non-critical ----
    {
        "name": "stallprobe_widget_a",
        "pattern": "48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8D 6C 24 C9 48 81 EC "
                   "A0 00 00 00 48 8B",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Per-marker map-widget virtual (v1.16 entry 0x1410dbb70, vtable slot "
                "0x142cbc840). Pass-through cost counter only; a miss disables that counter.",
    },
    {
        "name": "stallprobe_widget_b",
        "pattern": "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8D "
                   "6C 24 D1 48 81 EC 90 00 00 00 48 8B 41 08",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Per-marker map-widget virtual (v1.16 entry 0x1410dbea0, vtable slot "
                "0x142cbc848) - the reopen pin-construction hot path. Counter only.",
    },
    {
        "name": "stallprobe_widget_c",
        "pattern": "48 89 5C 24 20 4C 89 44 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D AC",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Per-marker map-widget virtual (v1.16 entry 0x1410dc260, vtable slot "
                "0x142cbc850). Counter only.",
    },
    {
        "name": "stallprobe_typed_find",
        "pattern": "40 53 41 55 41 57 48 83 EC 30 33 DB 4C 8B F9 89 5C 24 58 4C 8B EA 48 8B",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Typed-find walk (v1.16 entry 0x14113feb0, TRUE entry - never the "
                "0x14113feda continuation). Counter only.",
    },
    {
        "name": "stallprobe_child_step",
        "pattern": "48 89 6C 24 18 48 89 74 24 20 41 56 48 83 EC 20 8B A9 B8 00 00 00 4C 8B F1 "
                   "48 8B B1 E0 00 00 00 C1 ED 03 40 80 E5 01 48",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Per-frame child-step loop (v1.16 entry 0x1411d3980) - walks every "
                "marker widget per frame; the while-map-open fps cost. Counter only.",
    },
    {
        "name": "stallprobe_v3_native_insert_core",
        "pattern": "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 "
                   "48 C7 44 24 28 FF FF FF FF 49 8B D9 48 89 5C 24 20 49 8B F0 48 8B EA 48 8B F9",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Scaleform DisplayObjContainer insert core (v1.16 entry 0x14113e970). "
                "Pass-through spy records the +0xd8 child-vector count, sorted index, "
                "and child during debug-logging sessions.",
    },
    {
        "name": "stallprobe_v3_custom_placeobject",
        "pattern": "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 "
                   "41 54 41 56 41 57 48 83 EC 20 45 8B 70 4C 4C 8B FA 4C 8B 61 08 "
                   "41 8B D6 49 8B D9 49 8B E8 48 8B F9 E8",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Scaleform PlaceObject add path (DisplayList::Add, v1.16 entry 0x14113e7e0). "
                "The detour matches the marker factory's outstanding (depth, charId) slots - "
                "charIds chosen per world-map parse - and captures the ready DisplayObject.",
    },
    {
        "name": "stallprobe_v3_attach_movie_bridge",
        "pattern": "4C 8B DC 4D 89 4B 20 4D 89 43 18 55 56 41 57 49 8D 6B D8 "
                   "48 81 EC 10 01 00 00 48 8B 41 08 49 8B F1 48 8B 4A 28 4C 8B 78 18 "
                   "8B 81 90 00 00 00 83 E8 1F 83 F8 05",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Scaleform attachMovie DAPI bridge (v1.16 entry 0x1410e00c0). "
                "Debug-only spy correlates export/init parameters with the actual "
                "display-list parent observed by the nested high-level attach hook.",
    },
    {
        "name": "stallprobe_v3_high_level_attach",
        "pattern": "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 40 "
                   "48 8B DA 45 8B F0 48 8B 51 18 48 8B E9 48 8B 4B 38 8B 82 E0 00 00 00",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "High-level Scaleform display-object move/attach API (v1.16 entry "
                "0x1410c8440). One-shot V3 visual experiment uses its trampoline so "
                "old-parent removal, refcounts and child state remain engine-owned.",
    },
    {
        "name": "stallprobe_xform_get",
        "pattern": "48 89 74 24 10 57 48 83 EC 20 48 8B FA 48 8B F1 48 8B 51 50 48 85 D2 0F",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Widget transform getter (v1.16 entry 0x14117e140): full matrix "
                "decompose when cache [this+0x50] is null, cheap copy when cached. "
                "Counter tests the refresh-fills-the-cache hypothesis.",
    },
    {
        "name": "stallprobe_item_proc",
        "pattern": "40 53 56 57 48 83 EC 20 48 8B F9 83 CA FF 48 81 C1 88 00 00 00",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "GFx batch per-movie-slot processor (v1.16 entry 0x140d716a0), runs "
                "on a worker. Counter only.",
    },
    {
        "name": "stallprobe_next_capture",
        "pattern": "48 89 5C 24 20 57 41 56 41 57 48 83 EC 20 48 8B 19 4D 8B F8 4C 8B F2 48",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "GFx render next-capture / display-tree changelist apply (v1.16 entry "
                "0x1411577b0). Counter only.",
    },
    {
        "name": "stallprobe_movie_display",
        "pattern": "48 89 5C 24 18 48 89 74 24 20 57 41 56 41 57 48 81 EC 80 00 00 00 48 8B",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "GFx movie display/draw (v1.16 entry 0x14115cde0). Counter only.",
    },
    {
        "name": "stallprobe_batch_job",
        "pattern": "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 49 8B F8 48 8B F2 "
                   "E8 01 C4 FE FF 83",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "GFx render batch job entry (v1.16 0x140d7d720, worker thread). "
                "Counter only. Contains a build-specific rel32 (E8 01 C4 FE FF) - "
                "expected to shift on a game update; non-critical by design.",
    },
    {
        "name": "stallprobe_job_poll",
        "ambiguous_ok":
            "two byte-identical copies of the same poll on every build, and the scan is "
            "compiled only under MFG_STALL_PROFILER - first match lands on the same code either "
            "way",
        # Baked rel32 -> wildcard. Two byte-identical copies exist on every build; first-match
        # picks the same code either way, so the AMBIGUOUS warning here is expected.
        "pattern": "48 83 EC 28 48 8B 09 48 85 C9 74 16 48 83 C1 10 E8 ?? ?? ?? ?? 85 C0 0F",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Generic 'async job done?' poll (v1.16 entry 0x140e811c0); the map UI "
                "thread waits in it ~140ms after close. Spy logs (caller, job vtable) "
                "during capture windows. Contains a build-specific rel32 (E8 FB A6 08 "
                "01) - expected to shift on a game update; non-critical by design.",
    },
    {
        "name": "map_placename_update",
        "pattern": "40 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 D9 48 81 EC B0 00 00 00 "
                   "48 C7 45 B7 FE FF FF FF 48 89 9C 24 08 01 00 00 48 8B 05 ?? ?? ?? ?? "
                   "48 33 C4 48 89 45 1F 49 8B F8",
        "slot": None,
        "critical": False,
        "refs": ["goblin_maphover.cpp"],
        "note": "Map dialog per-frame 'name the focused pin' fn; RDX = hovered "
                "WorldMapPointPinData. Drives marker hover-detect (manual hide / hover "
                "overlay). Non-critical: a miss just disables hover, not core icons.",
    },
    # ---- REMOVED 2026-07-31: signatures whose code is gone from src/ ----
    # Twenty-four entries were resolved against eldenring.exe on every build for code that no longer
    # exists. Eight of them belonged to the F11 settings-tab subsystem retired the same day
    # (settings_build_job, opt_build_category, opt_append_tab, opt_show_page, opt_populate_page5,
    # opt_dtor_category, option_top_dialog_ctor, option_top_dialog_dtor). The other sixteen: the six DrawingContext primitives of the solid-fill spike (dc_begin, dc_beginfill_solid,
    # dc_moveto, dc_lineto, dc_endfill, dc_shapereset), the six memo-dialog prototype patterns, and
    # gfx_readtaginfo / gfx_tagalign / gfx_chardef_registrar / gfx_name_resolver from the re-host
    # cluster. check_drift() below only walks src/ -> this list, never the other way, so a stale
    # entry can never fail a build - it just costs a scan and reads as live documentation.
    {
        "name": "settings_refmove_job",
        "pattern": "48 89 54 24 10 53 48 83 EC 30 48 C7 44 24 28 FE FF FF FF 48 8B DA "
                   "C7 44 24 20 00 00 00 00 48 8B 09 48 89 0A 48 85 C9 74",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Ref-move a job handle (v2.6.x 0x7a7b60), from the confirm-dialog machinery. "
                "Used in the F11 settings-open push sequence. Dev proto.",
    },
    {
        "name": "settings_push_job",
        "pattern": "4C 89 4C 24 20 48 89 54 24 10 55 56 57 41 56 41 57 48 81 EC A0 00 00 00 "
                   "48 C7 44 24 40 FE FF FF FF 48 89 9C 24 D0 00 00",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "pushJob (v2.6.x 0x7edfa0): push a menu job onto the active menu = open it. "
                "Reused from the confirm-dialog work for the F11 settings open. Dev proto.",
    },
    {
        "name": "map_subdialog_job_step",
        "retired":
            "no scan for this pattern is left in src/",
        "pattern": "4C 89 44 24 18 55 53 56 57 41 54 41 56 41 57 48 8D 6C 24 D9 "
                   "48 81 EC A0 00 00 00 48 C7 45 A7 FE FF FF FF 4D 8B F0 4C 8B",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Map sub-dialog job step (v2.6.x 0x7ad1c0): creates the sub-dialog's movie "
                "(Path B, desc @job+0x58) + factory + registers render (FUN_140733ef0(job+0x50)). "
                "The map's own 'menu movie over the map' mechanism. Dev diagnostic hook.",
    },
    {
        "name": "settings_movie_job_builder",
        "pattern": "40 55 56 57 41 56 41 57 48 8D 6C 24 C0 48 81 EC 40 01 00 00 "
                   "48 C7 44 24 50 FE FF FF FF 48 89 9C 24 88 01 00 00 48 8B 05 ?? ?? ?? ?? "
                   "48 33 C4 48 89 45 30 4D 8B F0 48 8B F2 48 8B F9",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "Generic settings movie-job builder (v2.6.x 0x808630): (out, owner, DESC, flag). "
                "All movie sub-opens (Graphic/Brightness) funnel through it; F11's 0x8087e0 is its "
                "sibling. Task #9 capture hook to find the sub-page push target/site. Dev proto.",
    },
    {
        "name": "game_heap_alloc",
        "retired":
            "no refs and no scan in src/ - never had a consumer",
        # The old pattern ran 18 bytes past the thunk's tail jump, into whatever the linker
        # put next - so it existed only on 2.6.2. Cut to the thunk itself (6 instructions,
        # ending on its `jmp qword ptr [rax+0x50]`). Two byte-identical copies then match on
        # every build and the first is the documented one on all five.
        "pattern": "49 8B 00 4D 8B C8 4C 8B C2 48 8B D1 49 8B C9 48 FF 60 50",
        "slot": None,
        "critical": False,
        "refs": [],
        "note": "Game heap allocator thunk FUN_141eb9ed0(rcx=size, rdx=align, r8=allocObj) -> "
                "allocObj->vtable[0x50](allocObj, size, align). Was used to allocate an "
                "OptionSettingTopDialog (0x18a0) for the Route A settings-over-map experiment; "
                "no source calls it today, the entry is kept as the documented address.",
    },
    {
        "name": "alloc_singleton_global",
        "retired":
            "no scan for this pattern is left in src/",
        "pattern": "4C 8B 05 ?? ?? ?? ?? 4C 89 40 18 8D 53 08 B9 A0 18 00 00",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "mov r8,[rip+DAT_143d87350] in the settings build-job factory: loads the heap "
                "allocator singleton ptr passed to the FUN_141eb9ed0 thunk. relative_offsets {{3,7}} "
                "resolves the global's address. Dev proto (Route A settings-over-map).",
    },
    {
        "name": "menu_man_update_task",
        "pattern": "48 8B C4 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 68 A1 "
                   "48 81 EC 98 00 00 00 48 C7 45 A7 FE FF FF FF 0F 29 70 A8 "
                   "0F 29 78 98 44 0F 29 40 88 44 0F 29 4C 24 50 44 0F 29 54 24 40 "
                   "48 8B FA 48 8B D9 0F 57 FF F3 0F 10 05 ?? ?? ?? ?? 0F 2E C7 "
                   "7A 16 75 14 B9 29 0A 00 00",
        "slot": None,
        "critical": False,
        "refs": ["goblin_stall_probe.cpp"],
        "note": "CSMenuMan::updateTask (v2.6.x 0x766980), per-frame menu UI-thread entry. "
                "Detour reads the active menu (*(this+0x80)) for the dev menu-movie "
                "graphics probe; also the UI-thread beachhead for future native announce/"
                "dialogs. Non-critical: a miss just disables the probe.",
    },
    {
        "name": "gfx_placeobject2_exec",
        "retired":
            "dev-only Path-A experiment; no scan left in src/ (matched 47 places)",
        "pattern": "48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 E8 ?? ?? ?? ?? "
                   "F6 C3 01 74 0D BA 10 00 00 00",
        "slot": None,
        "critical": False,
        "refs": ["goblin_gfx_probe.cpp"],
        "note": "PlaceObject2::Execute (v2.6.x 0x140EE3D10): (thisTag, ctx, frame). Dev-only Path-A "
                "hook (debug_logging) - records the map _root's live DisplayObjContext in a TLS so the "
                "settings-root placement can Execute against it. Contains a build-specific rel32 (E8 "
                "wildcarded). Non-critical: a miss disables the Task-4 experiment only.",
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
