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


def require_oo2core():
    """Return path to oo2core_6_win64.dll or exit."""
    if OO2CORE_DLL and OO2CORE_DLL.exists():
        return OO2CORE_DLL
    print("ERROR: oo2core_6_win64.dll not found.")
    print("  Set game_dir in config.ini (the DLL ships with Elden Ring).")
    sys.exit(1)
