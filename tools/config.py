"""
Shared configuration for MapForGoblins tools.

Reads paths from tools/config.ini. Copy config.ini.example to config.ini
and fill in your local paths before running any tool scripts.

Local project paths (SoulsFormats DLL, paramdefs) are resolved automatically.
"""

import configparser
import sys
from pathlib import Path

TOOLS_DIR = Path(__file__).parent
PROJECT_DIR = TOOLS_DIR.parent
DATA_DIR = PROJECT_DIR / "data"

# Local project resources (no user config needed)
LIB_DIR = TOOLS_DIR / "lib"
SOULSFORMATS_DLL = LIB_DIR / "Andre.SoulsFormats.dll"
PARAMDEF_DIR = TOOLS_DIR / "paramdefs"
OO2CORE_DLL = None  # resolved from GAME_DIR below

# User-configured paths
ERR_MOD_DIR = None
GAME_DIR = None
SMITHBOX_DIR = None

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

    _sb = _cfg.get("paths", "smithbox_dir", fallback="").strip()
    if _sb:
        SMITHBOX_DIR = Path(_sb)

if GAME_DIR:
    OO2CORE_DLL = GAME_DIR / "oo2core_6_win64.dll"


def require_err_mod_dir():
    """Return ERR_MOD_DIR or exit with a helpful message."""
    if ERR_MOD_DIR and ERR_MOD_DIR.exists():
        return ERR_MOD_DIR
    print("ERROR: ERR mod directory not configured or not found.")
    print(f"  Create {_config_path} from config.ini.example and set err_mod_dir.")
    sys.exit(1)


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
