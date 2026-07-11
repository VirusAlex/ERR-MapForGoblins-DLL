"""
Shared configuration for MapForGoblins tools.

Reads paths from tools/config.ini. Copy config.ini.example to config.ini
and fill in your local paths before running any tool scripts.

Local project paths (SoulsFormats DLL, paramdefs) are resolved automatically.
"""

import configparser
import os
import sys
from pathlib import Path

TOOLS_DIR = Path(__file__).parent
PROJECT_DIR = TOOLS_DIR.parent

# ── Build profile ────────────────────────────────────────────────────────
# Selected via the MFG_PROFILE env var (set by build_pipeline.py / build.bat).
#   'err'         -> data source = the ERR mod (default; behaves exactly as before)
#   'vanilla'     -> data source = the vanilla game files (UXM-unpacked GAME_DIR)
#   'convergence2'-> data source = a MERGED view of The Convergence 2.x mod
#                    overlay over the vanilla game (the overlay is partial -
#                    608/1347 MSBs - so tools/prepare_merged_src.py stages
#                    overlay-over-vanilla into data/convergence2/merged_src,
#                    reproducing what ModEngine2 serves the game at runtime).
#                    Convergence 2.x ships on ModEngine2 (external_dlls).
#   'convergence3'-> same staging for The Convergence 3.x. Identical pipeline;
#                    3.x ships on ModEngine3 (a .me3 profile [[natives]] list),
#                    which only changes how our DLL is installed, not the data.
# Each non-default profile scopes all generated artifacts under its own dirs
# so the builds never clobber each other.
#   'erte'        -> data source = a MERGED view of the ERTE overhaul's mod
#                    overlay over the vanilla game (partial overlay, same
#                    staging as convergence; ERTE ships no worldmap gfx so its
#                    icon-frame offset is 0, like vanilla)
#   'goldenage'   -> data source = a MERGED view of the Golden Age overhaul's mod
#                    overlay over the vanilla game (partial overlay, same staging
#                    as convergence/erte; ME2 mod, Chinese overhaul w/ engus text)
#   'vins'        -> data source = a MERGED view of the Elden Vins overhaul's mod
#                    overlay over the vanilla game (partial overlay - 61 MSBs;
#                    same staging as convergence/erte/goldenage; ME2 mod, engus text)
#   'reborn'      -> data source = a MERGED view of the Elden Ring Reborn overhaul's
#                    mod overlay over vanilla (near-complete overlay - 1471 MSBs;
#                    same staging as the other overlay profiles; ME2 mod, engus text)
#   'graceborne'  -> data source = a MERGED view of the Graceborne (Bloodborne-inspired)
#                    overhaul's mod overlay over vanilla (same staging as convergence3;
#                    ME3 mod - launch.me3 [[natives]]; fully localized incl. engus/rusru)
PROFILE = os.environ.get("MFG_PROFILE", "err").strip().lower()
if PROFILE not in ("err", "vanilla", "convergence2", "convergence3", "erte", "goldenage", "goldenage363", "vins", "reborn", "graceborne"):
    PROFILE = "err"

# Profile-scoped intermediate/generated data dir.
if PROFILE == "err":
    DATA_DIR = PROJECT_DIR / "data"
    GENERATED_DIR = PROJECT_DIR / "src" / "generated"
else:
    DATA_DIR = PROJECT_DIR / "data" / PROFILE
    GENERATED_DIR = PROJECT_DIR / "src" / ("generated_" + PROFILE)

# Sprite-171 icon frame offset for the active profile. Our custom icon frames
# occupy 349-440 on the ERR/vanilla worldmap gfx. The Convergence's own
# 02_120_worldmap.gfx ALREADY extends sprite 171 to 756 frames (408 icons of
# its own), so there our frames are appended after theirs and every baked
# (Removed OUR_ICON_RANGE / ICON_FRAME_OFFSET.) Pure-DLL injects icon frames at
# runtime (compute_safe_base) and rewrites each marker's iconId to the injected
# frame, so baked iconIds are value-agnostic lookup keys - no vanilla-frame offset
# is needed on any profile. Numbering is owned by tools/icon_registry.py.

# Local project resources (no user config needed)
LIB_DIR = TOOLS_DIR / "lib"
SOULSFORMATS_DLL = LIB_DIR / "Andre.SoulsFormats.dll"
PARAMDEF_DIR = TOOLS_DIR / "paramdefs"
OO2CORE_DLL = None  # resolved from GAME_DIR below

# User-configured paths
ERR_MOD_DIR = None
GAME_DIR = None
CONVERGENCE2_MOD_DIR = None  # The Convergence 2.x ME2 'mod' overlay dir
CONVERGENCE3_MOD_DIR = None  # The Convergence 3.x ME3 'mod' overlay dir
ERTE_MOD_DIR = None         # ERTE overhaul's mod overlay dir
GOLDENAGE_MOD_DIR = None    # Golden Age overhaul's ME2 'mod' overlay dir
GOLDENAGE363_MOD_DIR = None # Golden Age 3.6.3 overhaul's ME2 'mod' overlay dir
VINS_MOD_DIR = None         # Elden Vins overhaul's ME2 'mod' overlay dir
REBORN_MOD_DIR = None       # Elden Ring Reborn overhaul's ME2 'mod' overlay dir
GRACEBORNE_MOD_DIR = None   # Graceborne overhaul's ME3 'mod' overlay dir
SMITHBOX_DIR = None
DARKSCRIPT_RESOURCES = None  # path to <DarkScript3>/Resources/ (optional)

_config_path = TOOLS_DIR / "config.ini"

if _config_path.exists():
    _cfg = configparser.ConfigParser()
    _cfg.read(str(_config_path), encoding="utf-8")

    _err = _cfg.get("paths", "err_mod_dir", fallback="").strip()
    if _err:
        ERR_MOD_DIR = Path(_err)

    _game = _cfg.get("paths", "game_dir", fallback="").strip()
    if _game:
        GAME_DIR = Path(_game)

    _conv2 = _cfg.get("paths", "convergence2_mod_dir", fallback="").strip()
    if _conv2:
        CONVERGENCE2_MOD_DIR = Path(_conv2)

    _conv3 = _cfg.get("paths", "convergence3_mod_dir", fallback="").strip()
    if _conv3:
        CONVERGENCE3_MOD_DIR = Path(_conv3)

    _erte = _cfg.get("paths", "erte_mod_dir", fallback="").strip()
    if _erte:
        ERTE_MOD_DIR = Path(_erte)

    _ga = _cfg.get("paths", "goldenage_mod_dir", fallback="").strip()
    if _ga:
        GOLDENAGE_MOD_DIR = Path(_ga)

    _ga363 = _cfg.get("paths", "goldenage363_mod_dir", fallback="").strip()
    if _ga363:
        GOLDENAGE363_MOD_DIR = Path(_ga363)

    _vins = _cfg.get("paths", "vins_mod_dir", fallback="").strip()
    if _vins:
        VINS_MOD_DIR = Path(_vins)

    _reborn = _cfg.get("paths", "reborn_mod_dir", fallback="").strip()
    if _reborn:
        REBORN_MOD_DIR = Path(_reborn)

    _grace = _cfg.get("paths", "graceborne_mod_dir", fallback="").strip()
    if _grace:
        GRACEBORNE_MOD_DIR = Path(_grace)

    _sb = _cfg.get("paths", "smithbox_dir", fallback="").strip()
    if _sb:
        SMITHBOX_DIR = Path(_sb)

    _ds = _cfg.get("paths", "darkscript_resources", fallback="").strip()
    if _ds:
        DARKSCRIPT_RESOURCES = Path(_ds)

if GAME_DIR:
    OO2CORE_DLL = GAME_DIR / "oo2core_6_win64.dll"

# Active game-data source for the selected profile. The pipeline reads
# regulation.bin / map / event / msg from here. For 'vanilla' this is the
# UXM-unpacked vanilla game; for 'err' it is the ERR mod overlay; for
# the overlay profiles (convergence2/3, erte, ...) it is the staged merged dir
# (overlay-over-vanilla, built by tools/prepare_merged_src.py as the first stage).
if PROFILE == "vanilla":
    DATA_SRC_DIR = GAME_DIR
elif PROFILE in ("convergence2", "convergence3", "erte", "goldenage", "goldenage363", "vins", "reborn", "graceborne"):
    DATA_SRC_DIR = DATA_DIR / "merged_src"
else:
    DATA_SRC_DIR = ERR_MOD_DIR

# In the non-ERR profiles the "mod" IS the active source, so repoint
# ERR_MOD_DIR to it. This makes the many scripts that read
# `config.ERR_MOD_DIR` directly (regulation/MSB/msg) profile-correct without
# touching each of them. In the err profile it is unchanged.
if PROFILE != "err":
    ERR_MOD_DIR = DATA_SRC_DIR


def require_data_src_dir():
    """Return the active profile's game-data source dir or exit.

    'err' -> ERR_MOD_DIR, 'vanilla' -> GAME_DIR. This is what every extractor
    should read regulation/MSB/event/msg from."""
    if DATA_SRC_DIR and DATA_SRC_DIR.exists():
        return DATA_SRC_DIR
    if PROFILE == "vanilla":
        print("ERROR: vanilla profile needs a UXM-unpacked game_dir.")
        print(f"  Set game_dir in {_config_path} (must contain loose regulation.bin, map/, event/, msg/).")
    elif PROFILE in ("convergence2", "convergence3", "erte", "goldenage", "goldenage363", "vins", "reborn", "graceborne"):
        key = PROFILE + "_mod_dir"
        print(f"ERROR: {PROFILE} merged source dir not staged yet.")
        print(f"  Set {key} in {_config_path}, then run tools/prepare_merged_src.py")
        print("  (build_pipeline.py runs it automatically as the first stage).")
    else:
        print("ERROR: ERR mod directory not configured or not found.")
        print(f"  Create {_config_path} from config.ini.example and set err_mod_dir.")
    sys.exit(1)


def require_err_mod_dir():
    """Return the active profile's game-data source dir or exit.

    Profile-aware alias of require_data_src_dir() (kept for the many callers
    that predate the vanilla profile). For 'err' this is ERR_MOD_DIR; for
    'vanilla' it transparently returns the vanilla GAME_DIR."""
    return require_data_src_dir()


def require_game_dir():
    """Return GAME_DIR or exit with a helpful message."""
    if GAME_DIR and GAME_DIR.exists():
        return GAME_DIR
    print("ERROR: Game directory not configured or not found.")
    print(f"  Create {_config_path} from config.ini.example and set game_dir.")
    sys.exit(1)


def require_smithbox_dir():
    """Return SMITHBOX_DIR or exit with a helpful message."""
    if SMITHBOX_DIR and SMITHBOX_DIR.exists():
        return SMITHBOX_DIR
    print("ERROR: SmithBox directory not configured or not found.")
    print(f"  Set smithbox_dir in {_config_path} (only needed by this script).")
    sys.exit(1)


def require_oo2core():
    """Return path to oo2core_6_win64.dll or exit."""
    if OO2CORE_DLL and OO2CORE_DLL.exists():
        return OO2CORE_DLL
    print("ERROR: oo2core_6_win64.dll not found.")
    print("  Set game_dir in config.ini (the DLL ships with Elden Ring).")
    sys.exit(1)


def find_eldenring_pid():
    """Return PID of a running eldenring.exe or None."""
    try:
        import psutil
    except ImportError:
        return None
    for p in psutil.process_iter(['pid', 'name']):
        name = (p.info.get('name') or '').lower()
        if name == 'eldenring.exe':
            return p.info['pid']
    return None


def require_eldenring_pid():
    """Return a running eldenring.exe PID or exit."""
    pid = find_eldenring_pid()
    if pid: return pid
    print("ERROR: eldenring.exe is not running.")
    sys.exit(1)


def find_active_save():
    """Return path to ER0000.err from %APPDATA%/EldenRing/<steamId>/ or None.
    Returns the most recently modified .err save (excludes 'backup' and 'err'
    subdirs which hold snapshots)."""
    import os
    appdata = os.environ.get('APPDATA')
    if not appdata: return None
    root = Path(appdata) / 'EldenRing'
    if not root.exists(): return None
    candidates = []
    for steam_dir in root.iterdir():
        if not steam_dir.is_dir(): continue
        if not steam_dir.name.isdigit(): continue
        for p in steam_dir.glob('ER0000.err'):
            candidates.append(p)
    if not candidates: return None
    return max(candidates, key=lambda p: p.stat().st_mtime)


def require_active_save():
    """Return path to current ER0000.err or exit."""
    p = find_active_save()
    if p and p.exists(): return p
    print("ERROR: no ER0000.err found in %APPDATA%/EldenRing/<steamId>/")
    sys.exit(1)
