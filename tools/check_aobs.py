"""Build-time AOB signature check against the target eldenring.exe.

For every AOB our DLL scans for at runtime (tools/aob_signatures.py), verify it
resolves UNIQUELY in the game's .text. A game update that shifts code silently
breaks the runtime scans -> map injection just stops working with no diagnostic.
This catches it at build time instead.

One vanilla eldenring.exe (config game_dir) backs every profile via ModEngine
(data overlays only, no exe patch), so ONE static scan validates all profiles.

Exit codes:
  0  all critical signatures unique (non-critical misses -> warnings only)
  1  a critical signature is MISSING or AMBIGUOUS, or a drift-guard failure,
     or the exe/config could not be read.

Usage:
  py tools/check_aobs.py [--json <path>] [--exe <path>]
"""
import sys, io, struct, re, argparse
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8")

from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS_DIR))
import config  # noqa: E402
from aob_signatures import SIGNATURES, check_drift  # noqa: E402


def to_regex(aob):
    """Translate an AOB string ('??' = wildcard) to a byte-matching regex."""
    out = b""
    for tok in aob.split():
        out += b"." if tok == "??" else re.escape(bytes([int(tok, 16)]))
    return re.compile(out, re.DOTALL)


def load_text_section(exe_path):
    """Return (code_bytes, text_rva) for the lowest-VA .text section - the
    original MSVC code, identical on disk and in memory."""
    import pefile
    pe = pefile.PE(str(exe_path), fast_load=True)
    texts = [s for s in pe.sections if s.Name.rstrip(b"\x00") == b".text"]
    if not texts:
        raise RuntimeError("no .text section in exe")
    texts.sort(key=lambda s: s.VirtualAddress)
    t = texts[0]
    code = pe.__data__[t.PointerToRawData: t.PointerToRawData + t.SizeOfRawData]
    return code, t.VirtualAddress, t.Misc_VirtualSize


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", help="write resolved RVAs to this JSON path")
    ap.add_argument("--exe", help="override eldenring.exe path (default: config game_dir)")
    args = ap.parse_args()

    # ---- Drift guard: every AOB literal in src/ must be in the signature list ----
    src_dir = config.PROJECT_DIR / "src"
    uncovered = check_drift(src_dir)
    if uncovered:
        print("ERROR: AOB drift - source has hex-pattern literals NOT in tools/aob_signatures.py:")
        for loc, lit in uncovered:
            print(f"  {loc}\n    {lit}")
        print("\nAdd each new AOB to tools/aob_signatures.py (name/pattern/critical) then rebuild.")
        return 1

    # ---- Locate the exe ----
    if args.exe:
        exe = Path(args.exe)
    else:
        game_dir = config.GAME_DIR
        if not game_dir:
            print("ERROR: game_dir not set in tools/config.ini (needed to locate eldenring.exe).")
            return 1
        exe = Path(game_dir) / "eldenring.exe"
    if not exe.exists():
        print(f"ERROR: eldenring.exe not found at {exe}")
        return 1

    try:
        code, text_rva, text_size = load_text_section(exe)
    except Exception as e:
        print(f"ERROR: could not read {exe}: {e}")
        return 1

    print(f"AOB check vs {exe}")
    print(f".text RVA 0x{text_rva:X}, size 0x{text_size:X}, {len(SIGNATURES)} signatures\n")
    print(f"  {'status':10} {'name':28} {'RVA':>10}  {'slot RVA':>10}")
    print(f"  {'-'*10} {'-'*28} {'-'*10}  {'-'*10}")

    results = {}
    critical_fail = False
    noncritical_warn = False

    for sig in SIGNATURES:
        rx = to_regex(sig["pattern"])
        hits = [m.start() for m in rx.finditer(code)]
        crit = sig["critical"]
        tag = "CRIT" if crit else "warn"
        entry = {"critical": crit, "matches": len(hits)}

        if len(hits) == 1:
            off = hits[0]
            rva = text_rva + off
            entry["rva"] = rva
            slot_str = ""
            if sig.get("slot"):
                disp_off, instr_len = sig["slot"]
                disp = struct.unpack_from("<i", code, off + disp_off)[0]
                slot_rva = rva + instr_len + disp
                entry["slot_rva"] = slot_rva
                slot_str = f"0x{slot_rva:X}"
            print(f"  {'[OK]':10} {sig['name']:28} {'0x%X'%rva:>10}  {slot_str:>10}")
        elif len(hits) == 0:
            entry["status"] = "MISSING"
            print(f"  {'[MISS-'+tag+']':10} {sig['name']:28} {'MISSING':>10}")
            if crit:
                critical_fail = True
            else:
                noncritical_warn = True
        else:
            entry["status"] = "AMBIGUOUS"
            print(f"  {'[AMB-'+tag+']':10} {sig['name']:28} {('x%d'%len(hits)):>10}")
            if crit:
                critical_fail = True
            else:
                noncritical_warn = True

        results[sig["name"]] = entry

    print()
    if args.json:
        import json
        Path(args.json).write_text(json.dumps(results, indent=2), encoding="utf-8")
        print(f"Wrote resolved RVAs -> {args.json}\n")

    if critical_fail:
        print("BUILD-BLOCKING: a CRITICAL eldenring.exe signature is missing or ambiguous.")
        print("The game may have updated. Re-find the AOB (Ghidra: scratch/ghidra_proj/eldenring)")
        print("and update the source + tools/aob_signatures.py before shipping.")
        return 1
    if noncritical_warn:
        print("WARNING: a non-critical signature missed (cosmetic/niche feature may be off).")
        print("Build continues.")
    else:
        print("All signatures resolved uniquely.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
