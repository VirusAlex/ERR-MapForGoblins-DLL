# GeomNonActiveBlockManager - reverse-engineering notes

Full RE of `CS::GeomNonActiveBlockManagerImp`, done to explain a stream of
first-chance `0xc0000005` access violations reported from MapForGoblins'
background worker on **ELDEN RING 2.6.2.0** (646 faults in ~2 min, same site,
never crashing - caught by our `safe_read` SEH, surfaced only because the user
ran a VEH-based crash logger). All facts below are confirmed against the live
game (RTTI + memory dump) and the on-disk `eldenring.exe` (disassembly), exe
**FileVersion 2.6.2.0**, image base `0x7FF70B950000` in the captured session.

## TL;DR

`GeomNonActiveBlockManager` is **real**, the static pointer RVA we use
(`0x3D69D98`) is **correct**, and its RTTI name is `GeomNonActiveBlockManagerImp@CS`.
But its **memory layout is completely different** from `GeomFlagSaveDataManager`
(`0x3D69D18`). Our `read_singleton_entries()` was written for the GeomFlag layout
- an inline `(tile_id, ptr)` table starting at `+0x08` - and blindly applied the
same parse to the NonActive manager. NonActive has no such table, so the scan
walks ~126 KB past the end of a 0x820-byte object into unrelated heap, producing
garbage "entries" and the AV storm. It never returned valid data on any game
version. **We don't need it** - collected-geometry state comes from
`GeomFlagSaveDataManager` (persistent flags) + `CSWorldGeomMan` (loaded tiles).

## How the three managers were identified (live RTTI)

Read `obj = *(base + rva)`, then `vtable = *obj`, `COL = *(vtable-8)`,
`TypeDescriptor = imagebase + COL.pTypeDescriptor`, name at `TypeDescriptor+0x10`:

| static RVA | RTTI class | our name |
|---|---|---|
| `0x3D69D18` | `GeomFlagSaveDataManagerImp@CS` | RVA_GEOM_FLAG (works) |
| `0x3D69D98` | `GeomNonActiveBlockManagerImp@CS` | RVA_GEOM_NONACTIVE (faults) |
| `0x3D69BA8` | `CSWorldGeomManImp@CS` | RVA_WORLD_GEOM_MAN (read by a different parser) |

All three RVAs are correct on 2.6.2.0 - the game's `.data` did **not** shift
wholesale (GeomFlag still parses 206 clean tiles, 0 AV).

## GeomFlagSaveDataManager layout (the one our parser DOES match)

Live dump of the object body (qwords from `+0x08`):
```
+0x08: 0x0A000000   tile_id     +0x10: 0x27F93296200  ptr
+0x18: 0x0A010000   tile_id     +0x20: 0x27EA78517C0  ptr
+0x28: 0x0B0A0000   tile_id     +0x30: 0x27E816D5B80  ptr   ...
```
i.e. an **inline array of `(u32 tile_id, _, u64 ptr)` 16-byte pairs starting at
`+0x08`**, terminated by zero pairs. `read_singleton_entries` is correct for this:
tile_id low byte is always `0x00` (it is `area<<24 | gridX<<16 | gridZ<<8`), and
`ptr` points to a per-tile geom-flag block (`countA @+8` / entries follow).

## GeomNonActiveBlockManager layout (RE'd from code)

Confirmed object size **`0x820` bytes (2080)** three independent ways:
- lazy getter: `mov ecx, 0x820 ; call <alloc>`
- constructor stores at `+0x818`
- scalar-deleting destructor: `mov edx, 0x820 ; call <free>`

Constructor (`eldenring.exe+0x6EA270`) initializes only:
```
[obj+0x00]  = vtable (0x2A87DE8)
[obj+0x08]  = 0        ; ONE byte - "has entries / active" flag
[obj+0x818] = 0        ; qword - element COUNT
```
The vtable (`0x2A87DE8`, `.rdata`) has a **single entry** (the scalar-deleting
destructor) - no other virtuals; the class is a data holder driven by member
functions. One such member, `+0x6EAAD0`, is just `mov byte [rcx+8], 1 ; ret`
(sets the active flag; this is the `1` seen live at `+0x08`).

The real layout comes from the lookup member at `eldenring.exe+0x6EA310`:
```
cmp byte [rcx+8], 0        ; flag@+0x08 == 0  -> empty, return
lea rdi, [rcx+0x10]        ; +0x10 = base of the record array
mov rcx, [rdi+0x808]       ; [obj+0x818] = element COUNT
shl rcx, 5                 ; COUNT * 0x20      -> record stride = 0x20 (32) bytes
add rcx, <aligned base>    ; end = base + COUNT*0x20
... loop, rbx += 0x20:
  mov rax, [rbx+8]         ; record+0x08 = pointer to a keyed object
  cmp dword [rax], edx     ; match key (a u32) against *(record+0x08)
  ... on hit, reads record+0x00 (ptr, 16 bytes via movups) and byte record+0x19
```

So the body is a **fixed inline array of up to ~64 records × `0x20` bytes at
`+0x10`**, with the **count at `+0x818`** and an **active flag (byte) at `+0x08`**:

```
struct GeomNonActiveBlockManagerImp {       // 0x820 bytes
    void*    vtable;            // +0x000
    uint8_t  active;            // +0x008  (1 once it holds entries)
    // +0x009 .. +0x00F : padding
    Record   entries[~64];      // +0x010  (stride 0x20)
    // ...
    uint64_t count;             // +0x818
};                                           // sizeof = 0x820
struct Record {                 // 0x20 bytes
    void*    blockPtr;          // +0x00  (geom/block object; 16 bytes read on hit)
    void*    keyed;             // +0x08  (-> first u32 is the lookup key/id)
    // +0x10 .. +0x1F : flags/data, incl. a byte at +0x19
};
```

This is **transient streaming bookkeeping** for map blocks that are currently
*non-active* (streamed out), keyed by a block/asset id - **not** a per-tile
collected-geometry flag store. Live, with the player standing still after load,
it was essentially empty (`active=1`, one stray `0x0E` at `+0xC8`, count ~0).

## Why our scan faulted

`read_singleton_entries(base, RVA_GEOM_NONACTIVE, …)` assumes the **GeomFlag**
layout and:
- starts at `+0x08` reading `(u64 id, u64 ptr)` pairs,
- walks `off = 0x08 .. 0x20000` (128 KB),
- accepts any `ptr` in `[0x10000, 0x7FFF…]` and `safe_read`s 16 bytes from it.

But the object is only `0x820` (2 KB). Past `+0x820` the scan reads **~126 KB of
unrelated heap**. Random qwords pass the loose `area 0x0A..0x3D` and pointer-range
checks, so we `safe_read` arbitrary addresses - e.g. floats misread as pointers
(`0x3F800000` = `1.0f`). Those that hit an uncommitted page raise `0xc0000005`,
caught by `safe_read`'s `__except` (hence no crash), but logged first-chance by a
VEH crash logger. On older game builds the adjacent heap happened to be readable
more often, so faults were rare/invisible - the read was **never valid**, only
quieter.

GeomFlag does **not** fault because its real `(tile_id, ptr)` table ends in zero
pairs well before 128 KB, so the scan terminates legitimately.

## Decision

Stop scanning `GeomNonActiveBlockManager`. It is the wrong source for collected
pieces (that is `GeomFlagSaveDataManager`), our parse of it was always invalid,
and it holds no `(tile_id, ptr)` table we can use. Removing the
`read_singleton_entries(game_base, RVA_GEOM_NONACTIVE, result)` call eliminates
the AV storm at the root on every game version and loses nothing real
(collected-piece hiding works via GeomFlag + CSWorldGeomMan + the immediate
per-AEG `+0x26B` flag). See `src/goblin_collected.cpp`.

## Repro / scripts (scratch, gitignored)

- `_rtti_probe.py` - RTTI class name of each manager pointer.
- `_nonactive_dump.py` - live object dump / vector-triple scan.
- `_nonactive_xref.py` - RIP-relative xrefs to the singleton global + vtable.
- `_nonactive_full_re.py` - vtable dump + member disassembly.
- `_probe_geom_rva.py` - replicates `read_singleton_entries` and counts plausible
  vs. unreadable (AV) reads per manager.
