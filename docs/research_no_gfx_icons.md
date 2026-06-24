# Research: showing our map icons WITHOUT shipping/modifying 02_120_worldmap.gfx

Goal: stop building+shipping a per-profile modified `02_120_worldmap.gfx`. Add our extra
icons at runtime so the game uses its STOCK gfx untouched. Two paths were researched in
depth (live RE on a running game, PID-attached read-only, plus static disasm). All file
VAs assume ImageBase 0x140000000; live addr = module_base + (fileVA - 0x140000000).
Engine = Scaleform GFx 3.x (evidence: `GFx_PlaceObject2Loader`, `GFx_DefineBitsJpeg*Loader`,
`GFxZlibState`, `MovieDef`, `CSScaleformSwfPlayer`). Deep per-function notes live in
`docs/research_worldmap_internals.md`; live scripts in `scratch/re_gfx_*.py`,
`scratch/_screenpos_*.py`.

## DECISION: Path A (native frame injection). Path B kept as documented fallback.

Rationale (user): a second render layer (self-draw overlay) duplicates parity work
(per-zoom scale, map clip, underground layer, cull for ~7000 quads) and adds risk; let the
engine render natively.

---

## Path A - inject our icon frames into the live DefineSprite at runtime (CHOSEN)

### What our icons are
- Each iconId = a frame in `DefineSprite cid=171` of 02_120_worldmap (iconId = 1-based
  frame index). Stock vanilla = 348 frames; our build appends ~92 -> 440 (per-profile,
  see `tools/build_vanilla_gfx.py`, `ICON_FRAME_OFFSET` in `tools/config.py`).
- Each frame = `RemoveObject2(depth) + PlaceObject2/3(depth, charId, matrix, cxform)`,
  composing a background (MENU_MAP_MemoCursor blue drop) + an overlay (MENU_Tab_* /
  MENU_ItemIcon_*). Textures are referenced by NAME via DefineExternalImage2, resolved
  from the shared image repository at load - NOT embedded pixels (except the 3 embedded
  rasters: anon "?", cleared-badge, logo, which are DefineBitsLossless2).

### Frame storage model = (B) parsed object graph (CONFIRMED)
Not (A) retained-SWF-byte-range replay. The SWF tag loaders (`PO2 loader 0x1411e23b0`)
convert each tag into a heap C++ object with a vtable at parse time. Confirmed statically
(loader: allocate -> store vtable -> copy body -> append) and live (objects in heap).

### Texture/resource verdict = SETTLED, tractable (HIGH)
- Stock worldmap movie imports NONE of our overlay/background textures (MENU_MAP_MemoCursor,
  MENU_Tab_*, MENU_ItemIcon_* belong to inventory movies). So we cannot reuse existing
  charIds for those layers.
- BUT DefineExternalImage2 resolves by NAME/path at load and builds the texture itself -
  no bitmap shipping, no manual ScaleformTexRepository poke. Live proof: our own
  DefineExternalImage2 records dumped fully resolved (e.g. path `...\Game\menu\MENU_Tab_00.tga`
  + a loaded 160x160 texture). 
- Net addition for Path A = ~17 DefineExternalImage2 name-ref entries (charIds 1000-1024);
  the 9 vanilla charIds the frames also use (10/21/53/57/105/109/112/113/162) are already
  in the stock map dict -> reuse those.
- OPEN sub-question: confirm MENU_Tab_*.tga / MENU_ItemIcon_*.tga resolve from the VANILLA
  game VFS (so we ship nothing), not from loose files only the mod loader provides.

### Tag object layout = byte-mapped (MEDIUM-HIGH)
- Tags are ~0x20-byte intrusively-linked heap objects (NOT the flat `Array<ExecuteTag*>`
  first assumed). Real vtables: PO2 = `exe+0x2C65CC0`, PO3 = `exe+0x2C65D20`
  (the `0x142cc9360` variant seen earlier is unused in this build).
- Execute (PO2 vtable slot6 `0x1411bcce0`): `flags=byte[tag+8]; base=(flags<0)?9:1;
  depth=u16[tag+8+base]`; flag bits 0/1 = place-vs-modify; binary-search display list by
  depth; place = copy tag into display object; modify = apply matrix/cxform via `0x1411bfd50`.
- Implication: CLONE existing PO2/PO3 tag objects (alloc via the movie heap, memcpy 0x20
  body, patch depth/charId/matrix/cxform) rather than hand-encode the variable body.

### The unresolved crux (the one remaining live poke)
Locating the live `SpriteDef cid=171` `this` and deriving its REAL frame-storage + append
path. Two RE passes DISAGREE:
- Pass 1: flat per-frame `{ptr,count,cap}` slots at `spriteDef+(f*3+0x79)*8`, append via
  `Array<ExecuteTag*>::PushBack 0x140ee3e90`, realloc `0x140ee4480`.
- Pass 2: storage is intrusive-linked ~0x20 objects -> the Pass-1 formula/PushBack are
  LIKELY WRONG for this build and must be re-derived once the sprite is located.
Frame count is NOT a literal u32 reachable from the def (441/440 hits were the GFx JIT
arena `0x7fff0...`, false positives). payload+0x20 = GFx context binding (file opener /
image creator singletons); payload+0x108 = name->id export table. The per-movie
charId->SpriteDef node was not located read-only.

### RESOLVED (static disasm pass, 2026-06-20) - the crux is closed, no running game needed
Full cite-ready dump: `scratch/re_sprite/RESULTS.md` + `RESULTS_task45.md` (every claim tied to
an instruction RVA). Frame storage = **flat GArray of `ExecuteTag*`** (Pass-1 correct; Pass-2
"intrusive-linked" WRONG). The subtlety prior LIVE passes missed: the per-frame tag-list array
does NOT live on the SpriteDef - it lives on the **MovieDataDef load context (CDEF)** at +0x3c8.

Layout (file VAs):
- **SpriteDef** (ctor `0x1411be1f0`): `charId@+0x18`, **`frameCount@+0x30`**, frame-LABEL GArray
  @+0x38/+0x40/+0x48 (NOT the tag lists), back-ptr to CDEF `@+0x20` (= `[charDef+0x40]`).
  Primary vtable `Resource@GFx 0x142bb7650`; iface vtable `0x142cc2e58` (seal slot+0x80 = `0x1411bfcb0`).
- **CDEF (load ctx)**: `curFrameIndex(i32)@+0x3b8`, `currentSpriteDef@+0x3c0`, **tag-slot base @+0x3c8**:
  `slot[f] = CDEF + (3*f + 0x79)*8` = `CDEF+0x3c8 + f*0x18`, each `{data_ptr@0, count@8, cap@0x10}`
  of `ExecuteTag*`. Triple-confirmed (append `0x140ee3eaa`, ShowFrame `0x1411be5ce`, finalize `0x1411cf99c`).
- **Append** `0x140ee3e90`(rcx=CDEF, rdx=tag), realloc `0x140ee4480` (1.25x, 8-byte elems). Tag
  objects from the movie page-pool `0x1411685d0` (over GMemoryHeap @CDEF+0x18). PO2 vtable
  `0x142c65cc0` (short-tag variant `0x142cc9360`), PO3 `0x142c65d20`, body @tag+0x10, execute slot6 `0x1411bcce0`.
- **Frame seal** (ShowFrame): `0x1411cf930` builds the frame-range + calls SpriteDef vtbl+0x80
  `0x1411bfcb0 -> 0x140f88f20` into the sealed timeline at `[SpriteDef+0x20]`. Play path reads the
  SEALED timeline (NOT the CDEF slots) - only the 2 load-side sites use the (3f+0x79) formula.
  => a runtime-appended frame MUST be sealed + `frameCount@+0x30` raised to be selectable by iconId.
- **Resource dict** (2 containers!): WRITE = hashmap @`DictOwner([movieDef+0x38])+0x180` (node 0x20:
  link@+0x10[-2=empty], charId@+0x18, resource@+0x28), register `0x141169d90`/`0x141169ce0`,
  hash `(id>>8)^id`. READ (queried at play by AddDisplayObject `0x140f01b10`) = SORTED array
  @`movieDef+0xd8` (slot stride 16, charId@node+0x2c, flag byte@node+0x6a>>6), lookup `0x14113fd90`.
  Common registrar `0x1411cf250` (writes BOTH). DefineExternalImage2 loader `0x1411e3a20` (custom
  tag 1009; record builder `0x1411e57e0`, 0x38 bytes, creator vtable `0x142cc8288`, path GString@+0x30).
  DefineBitsLossless2 loader `0x1411e2670` (raster builder `0x1411e59d0`) - the "?" embedded type.

### Implementation = strategy (A): re-drive the engine's own load+seal (NO further live RE)
Path A is FULLY specified statically. Recipe (run once, guarded, at worldmap-build/load time):
1. From the live MovieDataDef get CDEF = `[charDef+0x40]` (== `SpriteDef171+0x20`); MovieDataDef is
   refcount-cached across map close (prior live research). Resolve it via the CSMD vtable `0x142bbb310`
   scan -> url "02_120_worldmap" match (see below).
2. **Register our ~17 new image charIds (1000-1024)** via the common registrar `0x1411cf250`
   (writes BOTH dict containers) - do it in the LOAD phase before `movieDef+0xd8` is first queried,
   else the read index is stale (engine rebuilds it only internally). Reuse the 9 vanilla charIds
   (10/21/53/57/105/109/112/113/162) directly. Resource record must be a real Resource subclass
   (built like the ExternalImage2 builder) so AddDisplayObject's `[res]+0x198/0x1a0/0x1a8/0x38` virtuals resolve.
3. For each new frame f (349..440): set `CDEF+0x3c0 = SpriteDef171`, `CDEF+0x3b8 = f`, zero the slot
   count, alloc/clone PO2/PO3 tag objects via page-pool `0x1411685d0` + patch body, `append 0x140ee3e90`.
4. **Seal** frame f via `0x1411cf930(CDEF)` (writes the sealed timeline correctly - why strategy A
   needs no live confirmation), and raise `frameCount@+0x30` >= f.
Caveat (unchanged): mutates a shared refcounted cross-thread MovieDataDef; once, guarded, objects
from the movie's own allocator; patch-fragile (many RVAs).

(Alt strategy B = hand-write the SEALED container at `[SpriteDef+0x20]` via the `0x140f88f20`
insert path - needs ONE live read of that container's element layout (behind a runtime vtable).
Not recommended; A avoids it.)

--- historical (how the crux was approached) ---
Read-only heap scans repeatedly stumbled navigating to the live sprite (slow, one agent wedged at
the harness level). The anon-"?" beacon (the "?" is OUR frame 440/441 + embedded raster
ANON_QMARK_SHAPE 1099; `anonymous_loot=true` puts frame 441 on the display list) located SpriteDef-171
live but its exact dump was lost when killed early. The STATIC disasm pass above then derived the
whole layout from the loader code - durable, game-independent, the right tool. A DLL instrumentation
hook (capture sprite `this` from `AddDisplayObject 0x140f01b10`) remains an option to VALIDATE in
vivo but is not required to write the injector under strategy (A).

### Path A recipe (once the sprite/append is pinned)
1. Resolve MovieDataDef live: scan RW for CSMD vtable `0x142bbb310` -> payload `[h+0x10]`
   -> pick the handle whose OWN url buffer (hop <=1) holds "02_120_worldmap" (deep BFS
   gives false candidates that reach the url at hop 3 via a shared atlas).
   (Last session: handle `0x7ff2e7f8bca0` -> payload `0x7ff2dffd60a0`; re-resolve per run.)
2. Resolve SpriteDef-171 (REMAINING).
3. Register ~17 DefineExternalImage2 name-ref entries (charIds 1000-1024); reuse vanilla
   charIds 10/21/53/57/105/109/112/113/162.
4. Build frames: clone PO2/PO3 tag objects via the movie GMemoryHeap, patch body.
5. Append + bump frame count on the SpriteDef.

Risk: this mutates a shared, refcounted, cross-thread MovieDataDef; objects MUST come from
the movie's own allocator (page-pool `0x1411685d0` over GMemoryHeap at charDef+0x18) so
they free with the movie. Patch-fragile (many RVAs).

---

## Path B - self-draw via our DX12 overlay (FALLBACK, documented)

We already ship a DX12 Present-hook ImGui overlay + an icon atlas (`goblin_overlay.cpp`:
Present hook :1346, on_present :1207, atlas :107/:1149/:1168). Keep injecting our
WorldMapPointParam markers (for free behavior) or use our own marker data; compute each
on-screen position ourselves and draw our PNG visual.

### Projection (static-confirmed fields; one scalar needs a live calibration)
Marker map-coords: pin objects in vector at `dialog+0x3940`; map-space (X,Z) float pair at
`pin+0x10` (CONFIRMED constant under pan/zoom). Dialog transform fields:
- view center (map): `+0x0A38` / `+0x0A3C` (CONFIRMED live pan)
- eased current zoom: `+0x0A40` (use for lockstep; target zoom = `+0x2B58`)
- logical viewport rect `{0,0,1920,768}` at `+0x0D88..+0x0D94` (FromSoft logical canvas is
  1920x768; `+0x0D94` holds runtime device/aspect scale)
- NOT zoom: `+0x2F28` = ease duration; `+0x2F24/2C/30` = smoothstep params; 2.25 = a
  hardcoded constant (`.rdata 0x142B2BFA0`), not a field.
```
cx,cz = float[dlg+0x0A38], float[dlg+0x0A3C]
zoom  = float[dlg+0x0A40]
logical_x = (pin_mapX - cx)/zoom + 960
logical_z = (pin_mapZ - cz)/zoom + 384
screen    = logical * (device_h / 768)
```
RVAs: Update (ease cur<-tgt) `0x1409C32F0`; transform broadcast (scale=1/zoom) `0x1409C38D0`;
pan-clamp `0x1409CD1C0`; rect leaf `0x1409CE190`; ease helper `0x1409E6650`.
Open: the `/zoom` vs `*zoom` direction + sign + exact device-scale source need ONE live pin
(map-coord + center + zoom vs the marker's actual on-screen pixel). Projection is distributed
across Scaleform widget layers, so the formula is a reconstructed equivalent, not one
instruction.

### Why not chosen
Worst case is mispositioned icons (vs Path A worst case = crash/corruption), but visual
parity (per-zoom scale, clip, underground layer, cull) becomes our job for ~7000 quads, and
the single-formula approximation may drift at some zooms/aspect ratios. A second render
layer is extra maintenance. Kept here in case Path A's frame-append proves intractable.

---

## Phase-1a live verify (2026-06-20) - signature locator did NOT converge
Tried to confirm the static model live by scanning for SpriteDef-171 via vtable signature
(`scratch/verify_spritedef171*.py`, read-only). Result: the assumed PRIMARY vtable
`0x142bb7650` has ~zero live heap instances; the `0x142cc2e58` vtable matches 3251 objects
which are **0xA0-byte per-charId DICT ENTRIES** (charId@+0x18, packed 171,172,...), NOT
SpriteDefs. Following their pointer fields (+0x08/+0x20/+0x38/+0x60/+0x70) to hunt for a u32
frameCount (440/441/348/349) found nothing in the first 0x200 of any target. Opening vs not
opening the map did not change the hits. CONCLUSION: the static field offsets came from loader
CODE (plausible) but the vtable-based OBJECT LOCATOR is not validated - we cannot yet reliably
find the live SpriteDef-171 by signature. Do NOT write the injector on this.
Proven locator route (use next, NOT signature scan): resolve worldmap MovieDataDef by URL
("02_120_worldmap") via CSMD vtable `0x142bbb310` scan (works - see `scratch/re_gfx_live12.py`),
then its resource dict (read array @movieDef+0xd8, lookup `0x14113fd90`) for charId 171 ->
SpriteDef; OR an instrumentation hook on `AddDisplayObject 0x140f01b10` / DefineSprite loader
to capture the real live sprite `this`. Resume injector work via that, in a focused pass.

## Phase-1b live probe (2026-06-20) - dict node structure CONFIRMED, static SpriteDef model NOT
Built a read-only DLL probe (`src/goblin_gfx_probe.cpp`, ini `experimental_gfx_probe`) hooking the
GFx resource-dict lookup `0x14113fd90`. Live findings on running ERR (gfx = our `_new`, cid 171 =
441 frames per file parse):
- **Resource-dict NODE structure CONFIRMED**: vtable `0x..2E343108` @+0, secondary vtable @+0x18,
  `+0x28` = resource id/type (83 for the sprite node, 449/466/413 for texture nodes), **charId @+0x2c**.
- At map DISPLAY time the game dict-looks-up the icon sprite's CHILD texture/shape charIds
  (saw 9, 23, 37...), NOT charId 171 - the sprite container is resolved once at LOAD. So a
  display-time hook never sees charId 171; the 171 node IS in the dict (registered at load).
- Found 7 charId-171 dict nodes (multiple movies). BFS (depth<=4, 0x400/obj) from them found a
  u32 440 only at a 4-hop object `+0x2F4` whose layout does NOT match the static SpriteDef model
  (frameCount@+0x30, CDEF@+0x20, slot `(3f+0x79)*8` all gave garbage) - likely coincidental.
- **CONCLUSION: the static SpriteDef frame-model offsets do NOT validate live on this build.**
  3 DLL-probe versions + 6 python scans did not pin the live SpriteDef frame storage. Read-only
  scanning has hit diminishing returns.

## Phase-1c (2026-06-20) - EXHAUSTED the tractable read-only/load-hook approaches
Six DLL-probe versions + 12+ python memory scans. Every approach hit a fundamental wall:
- **Static model offsets** (frameCount@SpriteDef+0x30, CDEF@+0x20, slot `(3f+0x79)*8`): do NOT
  validate live (multiple methods, no 440/441 where expected).
- **Memory scan by charId@+0x2c**: matched `CSWorldGeomStaticIns` WORLD GEOMETRY objects
  (RTTI confirmed) - the 171/1099/1000 there are coincidental geom fields, NOT GFx charIds.
- **Byte-search for our frame-440 tag body** (charId 1099 + matrix): 0 hits - GFx parses tags
  into structs, does NOT retain raw SWF bytes.
- **Hook `dict_lookup 0x14113fd90`**: at display time resolves the sprite's CHILD textures
  (charId 9/23/37), never the sprite container 171; container-iteration for 171 found nothing.
- **Hook SpriteDef ctor `0x1411be1f0`** (AOB UNIQUE, correct fn): fired **0 times** the whole
  session incl. after map open -> the worldmap movie loads BEFORE our injected DLL arms the hook
  (injector injects post-menu), so the construction is never observed. Load-time hook is DEAD
  for this delivery (injector).
CONCLUSION: read-only scanning + load-time instrumentation are exhausted. The live GFx SpriteDef
frame structure is NOT cracked.

## SOLVED (2026-06-20) - live SpriteDef + frame model CONFIRMED via SpriteDef-ctor hook
The ctor hook `0x1411be1f0` (AOB-unique) fires correctly ONLY when our DLL loads before the
worldmap movie -> the dev INJECTOR loads too late (0 fires), but the OFFLINE loader (with
load_delay=0) loads at process start and catches it (1203->3061 ctor fires). Shipping builds
use the offline/ME loader, so timing is fine for release; the dev injector is the only thing
that misses it. With that, we found the REAL SpriteDef and read the full live model:

LIVE-CONFIRMED MODEL (ERR _err gfx, sprite cid 171 = 441 frames):
- **SpriteDef**: `charId @+0x18`, `frameCount @+0x30`, vtable `0x142CC2E58`.
- **Sealed frame array ON THE SpriteDef**: `data @+0x38`, `count @+0x40` (==441),
  `cap @+0x48` (==552). NOT the CDEF `(3f+0x79)*8` slots - those are LOAD scratch on
  CDEF=[SpriteDef+0x20] and are reset/garbage post-load (curFrameIndex=0). The static
  model put the array on CDEF; live it is the SEALED GArray on the SpriteDef.
- **Frame element stride 0x10** = `{ ExecuteTag** tags @+0, u32 tagCount @+8 }`. Live:
  frame0 count1, frame1 count3, frame440('?') count3.
- **Tag object**: vtable @+0 (live PlaceObject `0x142C65D80`, PlaceObject3 `0x142C65D20`),
  then `+0x10` = an 8-byte field + the PARSED tag body (flags, depth, charId, matrix).
  Frame 440 tag[1] body = `0e 10 01 00 4b 04...` (flags/depth1/charId 0x44b=1099 = our '?'),
  tag[2] = the matrix - matches our gfx bytes exactly. (So GFx DOES keep tag bytes, split
  per tag object after an 8-byte prefix - why a contiguous byte-search missed them.)
- **cap(552) > count(441)** => spare frame slots; on a stock sprite the injector appends
  frames into spare cap (or reallocs the +0x38 GArray if cap is exceeded).

INJECTOR RECIPE (live-confirmed, ready to implement):
1. Locate SpriteDef-171: hook SpriteDef ctor `0x1411be1f0` early (offline loader), record the
   `this` whose `charId@+0x18==171` (after frame parse, `frameCount@+0x30` set). DLL must load
   before the movie (offline/ME loader - shipping default).
2. For each of our ~92 new frames: build its tag objects by CLONING an existing PlaceObject/
   PlaceObject3 tag (alloc via the movie allocator, copy, patch body@+0x10: depth/charId/matrix/
   cxform), assemble an `ExecuteTag*[]` array.
3. Append a `{tags, tagCount}` element (stride 0x10) to the SpriteDef+0x38 frame GArray (use
   spare cap or realloc), and bump BOTH `frameCount@+0x30` and array `count@+0x40`.
4. Register our ~17 DefineExternalImage2 image charIds (1000-1024) in the resource dict (static
   recipe) OR confirm the textures resolve; the '?' embedded raster path is a separate item.

## (obsolete) Remaining strategy (heavier, not attempted): DISPLAY-time instance hook
The only path that sidesteps the load-timing problem: hook a function that runs EVERY time the
worldmap renders a marker icon (the per-marker icon/frame update on a live GFx sprite INSTANCE),
get the instance -> its SpriteDef def-ptr. This fires continuously (no load-timing dependency).
The static RE flagged this update as `thunk_FUN_1457cd3df` (a thunk toward the VMProtect-adjacent
region) - deeper RE, no guarantee. This is a fresh, focused effort, not a quick cycle.

## (historical) Earlier idea: instrument the DefineSprite LOADER at movie load
The unambiguous ground truth = hook the **DefineSprite loader `0x1411e2bc0`** (or seal `0x1411cf930`)
which BUILDS sprite 171 at gfx parse, and capture the real SpriteDef `this` + dump its true layout
as the engine writes it - no scanning, no guessing. Probe must arm before the worldmap movie's
first load and filter to charId 171. This is the clean phase-1b and should be a focused fresh pass.

## Status
- Movie resolution (CSMD vtable + URL) and resource-dict NODE layout: CONFIRMED live.
- SpriteDef-171 frame model: **NOT confirmed** - static offsets don't hold live; needs the
  DefineSprite-loader instrumentation above before any injector. Do NOT write the mutation yet. Textures settled, tags mapped, SpriteDef
  layout + append + seal + resource-dict all derived from loader disasm (see RESOLVED section +
  scratch/re_sprite/RESULTS*.md). No further RE needed to write the injector; optional in-vivo
  validation only. Research phase DONE - next is implementation (a guarded worldmap-load hook).
- Path B (fallback): fully mapped except one projection-scalar live calibration.

## Cleanup (2026-06-22) - dead/PoC code removed from src/goblin_gfx_probe.cpp
Removed (superseded by the live resinj path; RE knowledge retained in this doc + memory
project_no_gfx_icons, so nothing is lost):
- `inject_frames` - the clone-every-frame-to-grace PoC (experimental_icon_inject). Frame GArray grow/cap +
  cross-allocator-free caveat: see the SOLVED frame model + strategy A above. Superseded by
  `append_icon_frame` (per-icon synthesized frame).
- `inject_resource_test` (R1) - dict-alias charId registration PoC (never called). The proven RE it
  encoded: worldmap READ dict @movieDef+0xd8 = {base@+0, count@+8, cap@+0x10, dirty@+0x2a}, slot stride 16,
  node charId@+0x2c, aliased resource @+0x48; clearing +0x2a forces the resolver to rebuild from base. See
  the "R1 PROVEN LIVE" section above.
- `builder_detour` / BuilderFn - the descriptor builder hook (0x141264fb0, rcx=alloc rdx=mgr r8=pixsrc
  r9=&record{w,h,fmt}); never installed. Builder signature documented in the RESOLVED section.
- `patch_tag_first_charid` - `?? 10 01 00 <charId>` body-signature charId patch; only used by the dead R1.
- Constants CLONE_SRC(0)/STOCK_FRAMES(348)/TARGET_FRAMES(440) and `goblin::apply_anon_icon` (superseded by
  remap_injected_icons). 348/440 were per-build frame counts - the live path is frame-count-agnostic.
Live path keeps: registrar/ctor/lossless/spriteloader/adddisp/lookup hooks, capture_tag_vtables,
inject_all_icons, append_icon_frame, build_clean_place_tag (synth), build_remove_tag (synth),
inject_logo_into_plaque, and the read-only diagnostics (dump_*, collision detector) under the dev flags.
