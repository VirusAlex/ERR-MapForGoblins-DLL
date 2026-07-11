#!/usr/bin/env python3
"""
Stage a merged game-data view for overlay-mod profiles (convergence).

ModEngine2 mods ship a PARTIAL file overlay: at runtime the game sees the
mod's file when one exists, else the vanilla file. The Convergence overlays
only ~608 of 1347 MSBs and ~240 of 589 EMEVDs, so reading the overlay dir
alone would silently drop every map the mod didn't touch. This script
reproduces the runtime view on disk:

    data/<profile>/merged_src/
        regulation.bin                 (overlay)
        msg/engus/item_dlc02.msgbnd.dcx, menu_dlc02.msgbnd.dcx
        map/MapStudio/*.msb.dcx        (overlay file if present, else vanilla)
        event/*.emevd.dcx              (overlay file if present, else vanilla)

Files are HARDLINKED when source and target share a volume (instant, zero
disk cost, and the link's size/mtime automatically track the source, so the
pipeline's mtime-based stage cache invalidates correctly when the mod
updates). Falls back to copying. A manifest records which source each entry
came from; entries are re-staged when the desired source path changes
(e.g. a mod update adds an overlay file that previously fell back to
vanilla). Stale links are replaced via os.replace (no bare deletes).

Run via build_pipeline.py (first stage of the convergence profile), or
standalone:  py prepare_merged_src.py
"""
import json
import os
import shutil
import sys
import tempfile
from pathlib import Path

import config

# What the pipeline actually reads. Each entry: (relative dir, glob).
MERGE_SETS = [
    ("regulation.bin", None),                      # single file
    ("msg/engus/item_dlc02.msgbnd.dcx", None),
    ("msg/engus/menu_dlc02.msgbnd.dcx", None),
    ("map/MapStudio", "*.msb.dcx"),
    ("event", "*.emevd.dcx"),
]


def _stage_file(src: Path, dst: Path) -> str:
    """Hardlink src to dst (copy fallback). Returns 'link' or 'copy'.
    Existing dst is atomically replaced, never bare-deleted."""
    dst.parent.mkdir(parents=True, exist_ok=True)
    tmp = Path(tempfile.mktemp(dir=str(dst.parent), prefix=".staging_"))
    try:
        os.link(str(src), str(tmp))
        how = "link"
    except OSError:
        shutil.copy2(str(src), str(tmp))
        how = "copy"
    os.replace(str(tmp), str(dst))
    return how


def _same_file(a: Path, b: Path) -> bool:
    try:
        sa, sb = a.stat(), b.stat()
        return (sa.st_dev, sa.st_ino) == (sb.st_dev, sb.st_ino)
    except OSError:
        return False


def collect_plan(overlay: Path, base: Path):
    """relpath -> desired source Path (overlay wins)."""
    plan = {}
    for rel, glob in MERGE_SETS:
        if glob is None:
            for root in (base, overlay):          # overlay overwrites base
                # tolerate case differences (map/MapStudio vs map/mapstudio)
                cand = root / rel
                if cand.exists():
                    plan[rel] = cand
            if rel not in plan:
                print(f"  WARNING: {rel} missing from both overlay and game")
        else:
            names = {}
            for root in (base, overlay):
                d = root / rel
                if d.is_dir():
                    for f in d.glob(glob):
                        names[f.name] = f
            for name, src in names.items():
                plan[f"{rel}/{name}"] = src
    return plan


def main():
    base = config.GAME_DIR
    if config.PROFILE not in ("convergence2", "convergence3", "erte", "goldenage", "goldenage363", "vins", "reborn", "graceborne"):
        print(f"NOTE: profile is '{config.PROFILE}' - merged staging only applies "
              f"to overlay profiles; nothing to do.")
        return 0
    overlay = getattr(config, config.PROFILE.upper() + "_MOD_DIR", None)  # *_MOD_DIR per profile
    if not overlay or not overlay.exists():
        print(f"ERROR: {config.PROFILE}_mod_dir not set in tools/config.ini or not found.")
        return 1
    if not base or not base.exists():
        print("ERROR: game_dir not set in tools/config.ini or not found "
              "(must be UXM-unpacked).")
        return 1

    merged = config.DATA_SRC_DIR
    merged.mkdir(parents=True, exist_ok=True)
    manifest_path = merged / ".merge_manifest.json"
    manifest = {}
    if manifest_path.exists():
        try:
            manifest = json.load(open(manifest_path, encoding="utf-8"))
        except Exception:
            manifest = {}

    plan = collect_plan(overlay, base)
    staged = skipped = 0
    counts = {"link": 0, "copy": 0}
    new_manifest = {}
    for rel, src in sorted(plan.items()):
        dst = merged / rel
        new_manifest[rel] = str(src)
        if manifest.get(rel) == str(src) and dst.exists() and _same_file(src, dst):
            skipped += 1
            continue
        # copies (non-hardlinks) can't be cheaply verified -> re-stage only
        # when source path changed or size/mtime differ
        if (manifest.get(rel) == str(src) and dst.exists()
                and dst.stat().st_size == src.stat().st_size
                and dst.stat().st_mtime_ns == src.stat().st_mtime_ns):
            skipped += 1
            continue
        counts[_stage_file(src, dst)] += 1
        staged += 1

    # Orphans (file no longer in plan, e.g. a map removed from the overlay AND
    # the game) are left in place but dropped from the manifest; they are
    # extremely unlikely and harmless for vanilla-rooted sets. Report them.
    orphans = [r for r in manifest if r not in new_manifest]
    if orphans:
        print(f"  NOTE: {len(orphans)} stale staged files no longer have a source "
              f"(left in place): {orphans[:5]}{'...' if len(orphans) > 5 else ''}")

    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(new_manifest, f, indent=1)

    ov_count = sum(1 for s in plan.values() if str(s).startswith(str(overlay)))
    print(f"Merged source staged: {merged}")
    print(f"  total {len(plan)} files ({ov_count} from overlay, "
          f"{len(plan) - ov_count} from vanilla); "
          f"{staged} (re)staged ({counts['link']} linked, {counts['copy']} copied), "
          f"{skipped} up-to-date")
    return 0


if __name__ == "__main__":
    sys.exit(main())
