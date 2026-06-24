"""Generate the "all-on" preset MapForGoblins.ini for a release.

Takes the shipped (stock) MapForGoblins.ini and produces a copy with every
`show_* = false` flipped to `show_* = true`, leaving all non-icon settings
(require_map_fragments, ERR Markers patches, Debug hotkeys, etc.)
exactly as shipped. A short header explaining the preset is prepended.

This is the file that ships next to the release README; users drop it into
dll/offline/ (replacing the default) to render every icon category at once.

Usage:
    py tools/make_allon_ini.py <stock_ini> <output_ini> <version>

Example:
    py tools/make_allon_ini.py "ERR - MapForGoblins - DLL - v1.0.13/dll/offline/MapForGoblins.ini" \
                               "ERR - MapForGoblins - DLL - v1.0.13/MapForGoblins.ini" 1.0.13
"""
import sys
import re

HEADER = """; All-on preset for Map For Goblins v{version}.
; Every show_* option is enabled. Copy this file into {target_dir}
; (replacing the default MapForGoblins.ini) to render every icon
; category at once.
;
; The non-icon settings (hotkeys, fast_map_open, etc.)
; are kept at the same defaults as the shipped MapForGoblins.ini.

"""

# Match `show_xxx = false` on a config line (not a comment). Captures the
# `key = ` prefix so we can re-emit it verbatim with `true`.
SHOW_FALSE = re.compile(r'^(\s*show_[a-z0-9_]+\s*=\s*)false\s*$', re.IGNORECASE)


def main():
    if len(sys.argv) not in (4, 5):
        print(__doc__)
        sys.exit(2)
    stock_path, out_path, version = sys.argv[1], sys.argv[2], sys.argv[3]
    target_dir = sys.argv[4] if len(sys.argv) == 5 else 'dll/offline/' 

    # newline='' preserves the file's original line endings on read.
    with open(stock_path, 'r', encoding='utf-8', newline='') as f:
        text = f.read()

    eol = '\r\n' if '\r\n' in text else '\n'
    flipped = 0
    out = []
    for raw in text.split(eol):
        m = SHOW_FALSE.match(raw)
        if m:
            out.append(f"{m.group(1)}true")
            flipped += 1
        else:
            out.append(raw)

    with open(out_path, 'w', encoding='utf-8', newline='') as f:
        f.write(HEADER.format(version=version, target_dir=target_dir).replace('\n', eol))
        f.write(eol.join(out))

    print(f"[make_allon_ini] {flipped} show_* options flipped to true")
    print(f"[make_allon_ini] wrote {out_path}")


if __name__ == '__main__':
    main()
