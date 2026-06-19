# Crash-dump diagnostics (no cdb/WinDbg)

How to find out *what* and *where* an Elden Ring crash is, by parsing the Windows minidump in plain
Python. Used when a player reports a crash and we need to know if MapForGoblins (or which subsystem)
is involved. The throwaway scripts live in `scratch/_dmp*.py` (gitignored) - this doc preserves the
recipe so they can be recreated.

## 1. Find the dump
Windows Error Reporting writes local dumps to:

```
C:\Users\<user>\AppData\Local\CrashDumps\eldenring.exe.<pid>.dmp
```

~50-60 MB **triage** dumps = thread stacks + module list + thread contexts, **no heap**. (ER's own
crash log is elsewhere and usually unhelpful.) List recent ones:

```powershell
Get-ChildItem "$env:LOCALAPPDATA\CrashDumps" -Filter *.dmp |
  Sort-Object LastWriteTime -Descending | Select FullName,LastWriteTime,Length -First 5
```

Also worth a glance at the mod logs around the crash time:
`...\ERRv2.2.1.2\tools\injector\logs\MapForGoblins_<date>.log` (look for the last lines, `[SANITIZE]`,
`SEH exception in init step`, `Injection complete`).

## 2. Minidump layout (parse with `struct`)
- Header: `MDMP` at 0; at +8 `NumberOfStreams u32`, `StreamDirRva u32`.
- Dir entry (12 B): `Type u32, Size u32, Rva u32`. Types: **3** ThreadList, **4** ModuleList,
  **6** Exception, **9** Memory64List, **5** MemoryList.

### Exception stream (6)
`ThreadId u32, align u32`, then `MINIDUMP_EXCEPTION { Code u32, Flags u32, Record u64, Address u64,
NumParams u32, align u32, Info[15] u64 }`, then a `MINIDUMP_LOCATION_DESCRIPTOR (DataSize u32,
Rva u32)` → the **thread CONTEXT at fault** (use this, not the threadlist context which is mid-WER
unwind in ntdll).
- `Code` 0xC0000005 = access violation. `Info[0]` 0=read/1=write/8=exec, `Info[1]` = faulting address.

### x64 CONTEXT offsets (from context Rva)
`Rax 0x78, Rcx 0x80, Rdx 0x88, Rbx 0x90, Rsp 0x98, Rbp 0xA0, Rsi 0xA8, Rdi 0xB0, R8 0xB8 … R15 0xF0,
Rip 0xF8`.

### Module list (4)
`count u32`, then `MINIDUMP_MODULE` (108 B each): `Base u64, Size u32, …, ModuleNameRva u32` at
module-offset **+0x14** (name = `u32 len` + UTF-16LE). Map `addr → module` by `base <= addr < base+size`
to get the **faulting module**.

### Thread list (3)
`count u32`, then `MINIDUMP_THREAD` (48 B): `ThreadId u32 @+0`; Stack desc `StartOfMemoryRange u64
@+0x18, DataSize u32 @+0x20, Rva u32 @+0x24`; ThreadContext `DataSize u32 @+0x28, Rva u32 @+0x2C`.
Stack bytes are in the dump: `fileoff = stack_rva + (addr - stack_start)`.

### Memory64List (9)
`count u64, baseRva u64`, then `count × (Start u64, DataSize u64)`; data concatenated from baseRva.
Triage dumps frequently **lack stream 9** (heap absent) → register pointers read as "<not in dump>".
Stacks still work via stream 3.

## 3. What to extract
1. **Exception**: code, access type, faulting address, and the faulting **module+offset** (map the
   fault address through the module list).
2. **Stack walk** (poor-man, no symbols): from the exception-context `Rsp`, scan each qword up the
   stack; print any value that lands in a loaded module as `module+offset`. A chain of
   `eldenring.exe+…` frames with **no mod-DLL frame** = crash is in game code called from game code.
3. **Registers** at fault (esp. the deref'd one = `Info[1]`). With no heap stream you can't follow
   heap pointers, but stack-resident objects are readable.
4. **Disassemble** the faulting RVA: `py scratch/scripts/disfn.py <rva>` (capstone, reads
   eldenring.exe off disk). ER lower MSVC `.text` = RVA 0x1000..~0x29A3000; VMP-added `.text` =
   0x4C0E000+ (disassemble only the lower one).

## 4. Interpreting "game crash, no mod frame"
A fault in game code with no mod frame on the stack can **still be the mod's fault** if the mod fed
the game malformed data the game later reads. Precedents in this project:
- the 16-align bug in the expanded `WorldMapPointParam` table (game crashed on an id-lookup);
- a marker `textId` that resolves to no FMG string → `GetMessage` returns null → the game builds a
  `std::wstring` from null → null-deref in a string SSO routine on load
  (see `docs/` and the `reference_fmg_textid_system` memory).

So treat "no mod frame" as *necessary-not-sufficient* evidence. Confirm with an empirical **without
the mod** repro before concluding either way.
