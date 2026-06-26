# ME2/ME3 no-gfx failure: root cause (looks_heap) + research toward a loader-timing-independent late inject

Status: **root cause SOLVED 2026-06-25** (one-line `looks_heap` fix). This doc preserves the full research so the
NEXT release can build a loader-timing-independent ("late inject") path and stop depending on catching the worldmap
parse. All addresses are file VAs for `eldenring.exe` (image base `0x140000000`); the exe is the 2026-05-29 build and
is fully analyzed in the Ghidra project (see below / memory `reference_ghidra_re`).

## 1. Root cause (the actual bug)

Symptom: no-gfx icons render as DEFAULT on ME2/ME3 loaders; ERR (offline loader) works. Looked like a load-order
/ timing problem (and a whole "DLL loads too late, worldmap parsed at startup, need a late anchor" theory was built).
**That theory was WRONG - a red herring.** The real cause:

`looks_heap()` in `src/goblin_gfx_probe.cpp` was `v > 0x10000 && v < 0x7FF000000000`. The game's GFx heap under
**ME2/ME3 sits at `0x7FF2_xxxxxxxx`** (above that bound); ERR's heap is lower (passed). So `looks_heap` REJECTED
every valid pointer -> the sprite-171 locate, the inject gate, and the whole injection silently no-op'd. Because
locate/diagnostics all funnel through `looks_heap`, it ALSO masked every probe (made it look like hooks "never fired"
/ "saw no 171").

Discovery: a temporary `[ctorhook]` log printed the ctor's `self` = `0x7FF2D5F8D060` (a valid SpriteDef) and showed
it failing `looks_heap`.

**Fix:** widen to `v < 0x7FFFFFFFFFFFull` (x64 user-mode max). The EXISTING load-time `spriteloader_detour` injection
then works under ME2 - live-confirmed: `[icons] worldmap sprite-171 (frameCount=348) loading; appending 65 icon
frames`, `registered 65/65 bitmaps`, `remapped 6875 markers`, logo ok. So there is **no timing problem** - the worldmap
sprite-171 DOES load through our hook at map-open under ME2.

Implication: the `looks_heap` fix is in SHARED code -> needed by all 8 profiles; the pre-fix v2.0.1 release would have
shown default icons on all 7 ME2/ME3 builds. Rebuild + re-VT-scan all 8 before publishing.

## 1b. SECOND root cause: ME2 startup-parse HEAP CORRUPTION (FIXED 2026-06-25)

The `looks_heap` fix ENABLED injection under ME2, which then crashed the game intermittently at STARTUP (the worldmap
SWF preloads ~14s in under ME2/ME3, vs at map-open under ERR). Crash signature across today's dumps was MIXED - several
faulted at different game RIPs (`eldenring+0x11E1DAB` ShapeLoader `this`=null READ 0x0; `eldenring+0x10DE7EC` READ 0x40)
and FIVE were ntdll heap fast-fails (`ntdll+0x112265`). Mixed sites + heap fast-fails = **heap corruption**, not OOM and
not a single deterministic bug. The crashing thread's full stack was **pure eldenring.exe - MapForGoblins.dll was NOT on
the stack** (we had already returned), i.e. the SWF parse CONTINUED past our injection and tripped over smashed heap.

Root cause, pinned via Ghidra decompile of the window-refill `FUN_1411bba10`: the SWF reader's read window is
`buf@reader+0x60`, `capacity@reader+0x68` (**default 0x200**; when there is no file source the buffer is INLINE at
`reader+0x6c`). The refill streams chunks INTO that buffer in place (never reallocates - so `+0x60`/`+0x68` are stable).
`inject_lossless_tag` saved+restored the window content with a **hardcoded `0x1000`** byte copy. Since the real capacity
is 0x200, the write-back stamped **0xE00 bytes PAST the buffer** into adjacent memory (the reader's own tail when the
buffer is inline, or neighbor heap). And because our lossless load allocates 65 bitmaps BETWEEN the save and the restore,
the restore wrote a STALE snapshot over now-live neighbor heap -> corrupted heap metadata. Under ERR that overspill region
was benign; under ME2's heap layout it was live -> crash. Intermittent / first-launch-only because it depends on what the
0xE00 overspill happens to overwrite.

Fix (`inject_lossless_tag`): read `wincap = [reader+0x68]` and copy exactly that many bytes (clamped to the snapshot
buffer, now 0x4000) for both save and restore; also save/restore `+0x54` (stream counter) and `+0x58` (refill flag) the
loader advances. Reader window structure (from `FUN_1411bba10`): `+0x4c`=pos, `+0x50`=fill, `+0x54`=stream pos counter,
`+0x58`=byte flag, `+0x60`=buf ptr, `+0x68`=capacity (u32, default 0x200), `+0x6c`=inline 0x200 buffer.

## 2. Confirmed addresses / structures (current exe)

- **SpriteDef ctor** `0x1411be1f0` (rcx = new SpriteDef `self`, rdx = CDEF/load-ctx). Sets vtable `0x142cc2e58` at
  `[rcx]` (so vt is 0 if read at hook ENTRY, before the body runs). `charId@+0x18` (init 0x40000), `frameCount@+0x30`.
  Sealed frame GArray `@SpriteDef+0x38` = {data ptr, count@+0x40, cap@+0x48}, element stride `0x10` = {ExecuteTag**
  tags@+0, u32 tagCount@+8}. CDEF = `[SpriteDef+0x20]`.
- **DefineSprite-loader** `0x1411e2bc0` (rcx = load ctx / movieContext). reader = `[rcx+0x418]` (fallback `rcx+0x50`);
  on reader: pos `@+0x4c`, window buf `@+0x60`, MemoryFile source `@+0x20`. This is the SWF-tag parse path; only runs
  during an active parse. It is dispatched from a vtable slot (`0x14498fbd0`).
- **Dict registrar** `0x14011cf250` (RVA `0x11CF250`; rcx = movieDef, dictOwner = `[movieDef+0x38]`). Writes both dicts.
- **+0x180 WRITE hashmap** (the authoritative charId->character dict): lives at `movieDef+0x180`. GET = `FUN_141169f70`
  (`0x141169f70`, param_1 = movieDef). INSERT = `FUN_141169d90` -> `FUN_14116ffc0`. Hash = `(charId>>8) ^ charId`.
  Slot stride `0x20`: chain `@+0x10` (`-2`=empty, `-1`=end of chain, else next index), key u32 `@+0x18`, value (the
  character/SpriteDef ptr) `@+0x28`. To find sprite-171 from a movieDef: walk this from `idx = (171>>8^171)&mask`,
  `mask = [hdr+8]`, `slots = hdr+0x10`, `hdr = [movieDef+0x180]`.
- **+0xd8 READ sorted array**: container `@movieDef+0xd8` {base@+0, count@+8}. Lookup = `0x14113fd90` (rcx =
  container = movieDef+0xd8). Slots are an array of node POINTERS (stride 16); node charId `@+0x2c`. (Resource-binding
  map; may NOT contain the sprite-171 node - prefer the +0x180 dict for locating sprites.)
- **AddDisplayObject** `0x140f01b10` (movieDef = `*(rcx - 0x10)`).
- **Image manager** = `[[movieDef+0x18]+0x40]` (= `[[loadctx+0x18]+0x40]`). PERSISTS post-load (the load-time bitmap
  registration succeeded post-arm under ME2 -> the manager is warm/usable when the map opens).
- **Lossless bitmap creation** (`inject_lossless_tag`): clone the native MemoryFile at `reader+0x20` (one-time, cached),
  point its data@+0x18/size@+0x20/pos@+0x24 at our DefineBitsLossless2 tag bytes, swap `reader+0x20` to our MemoryFile,
  empty the window (reader+0x4c/0x50/0x54=0), call the native loader with tagInfo {[+0]=36, [+8]=len}, restore. Comment
  warns it must run while the image manager is "warm" - but under ME2 the load-time path runs at map-open and it works.
- **Worldmap dialog anchor** (no hook): `*(eldenring.exe + 0x3D5DF38) -> +0x68` = the worldmap dialog (holds
  beacons `+0x118` / stamps `+0x1B8`; used by `goblin_markers.cpp` F9 dump). RVA `0x3D5DF38` is valid in the current
  exe (Ghidra: `140a3910d READ -> 143d5df38`). Live value seen: dlg = `0x7FF3D35460C0` when the map is open.
- **WMP marker-build / copy-ctor anchor** `0x140a390c0` (see `scratch/ghidra_out.c`, `scratch/wmp_eval_disasm.txt`).
- Menu-movie name table: `"02_120_WorldMap"` string `@0x142ab7960`, registered in a startup init table; map-open
  funcs `FUN_1407ee1f0` / `FUN_1407fb2e0` look the movie up BY NAME (singletons `DAT_143d83148` menu mgr,
  `DAT_143d5ae60` CS::WorldMap).

## 3. Late-anchor findings (the next-release building blocks)

Goal next release: inject WITHOUT depending on catching the parse, so loader timing never matters. Confirmed under ME2
at map-open (with looks_heap fixed, these probes are valid):

- The **+0xd8 lookup `0x14113fd90` FIRES** at map-open (~85k+ calls/open) and the **+0x180 GET `FUN_141169f70`
  FIRES** (~148k+ calls/open). Both receive the worldmap movieDef (lookup: rcx = movieDef+0xd8; get: rcx = movieDef).
  (Earlier "lookup never fires" was a looks_heap artifact - it fires plenty.)
- **AddDisplayObject fired 0** for the worldmap at map-open (worldmap uses a different display caller, e.g.
  `0x1410c98af` per old notes) - not a good anchor.
- A hook-free **static anchor** exists: poll `*(exe+0x3D5DF38)->+0x68` (worldmap dialog) from the background tick;
  it becomes non-null when the map opens. The dialog->movieDef offset is NOT yet pinned (a scan of dlg[0..0x1000] in
  the diagnostic did not isolate it - revisit; or reach the movie via the menu-mgr singleton `DAT_143d83148` +
  `FUN_140d6b0c0(mgr, &{flags,"02_120_WorldMap"})`).
- **Post-hoc locator** (proven design): `find_sprite171_in_movie(movieDef)` = walk `movieDef+0x180` for charId 171
  (returns the SpriteDef, validate vtable `0x142cc2e58` + charId@+0x18==171).
- Already PROVEN post-load by prior work (see `project_no_gfx_icons` memory): R1 charId registration, frame append,
  and (this session) bitmap registration via the load-time path with a persistent image manager.

## 4. NEXT-RELEASE plan: loader-timing-independent late inject

1. Find the worldmap movieDef ONCE, hook-timing-independent: hook the +0x180 GET (`FUN_141169f70`) and/or +0xd8
   lookup (`0x14113fd90`) - both fire at map-open - and on each call run `find_sprite171_in_movie(movieDef)`; capture
   the movieDef the FIRST time it yields sprite-171, then DISARM/early-out (the diagnostic version ran the walk on
   EVERY call and killed FPS - find-once + stop is mandatory). Alternative: a static-anchor poll from the bg tick.
2. Once captured: if the load-time path already injected (g_found_sd set), do nothing. Else run the full inject
   post-hoc on the captured movieDef: register bitmaps (image manager `[[movieDef+0x18]+0x40]`), append the 65 frames
   to sprite-171's GArray, remap the WorldMapPointParam iconIds. All proven feasible post-load.
3. Keep the load-time `spriteloader_detour` path too (it works when our hooks catch the parse, e.g. ERR); the late
   path is the fallback for loaders that load us after the parse. Net: timing never matters.
4. PERF: never run per-call dict walks on the hot path. The map's dict resolvers fire 100k+ times per open; do the
   locate at most a handful of times then disarm.

## 5. Ghidra material

Project: `scratch/ghidra_proj/eldenring` (analyzeHeadless, `.java` scripts only - see `reference_ghidra_re`). Dumps:
`scratch/ghidra_re_dump.c` (core funcs + callers), `ghidra_re_dictfns.c` (+0x180 INSERT/hash), `ghidra_re_findfns.c`
(`FUN_141169f70` GET + others), `ghidra_re_xrefs.txt`, `ghidra_re_strings.txt`, `ghidra_re_globals.txt`,
`scratch/ghidra_out.c`, `scratch/wmp_eval_disasm.txt`.
