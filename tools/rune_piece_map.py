"""
Elden Ring Reforged — Rune Piece Map Builder
Combines save file GEOM/GEOF data with JSON coordinates to produce a full map
of all Rune Pieces with picked-up status.

Usage:
  python rune_piece_map.py <save_file> --slot <N> --json <rune_pieces.json> [--out map.json] [--html map.html]
"""

import struct
import json
import sys
import os
from pathlib import Path
from collections import defaultdict

RUNE_PIECE_GEOM_IDX_MIN = 0x1194
RUNE_PIECE_GEOM_IDX_MAX = 0x11A6


def decode_map_id(raw_bytes):
    return f"m{raw_bytes[3]:02d}_{raw_bytes[2]:02d}_{raw_bytes[1]:02d}_{raw_bytes[0]:02d}"


def parse_geom_section(slot_data, off):
    size = struct.unpack_from('<i', slot_data, off)[0]
    if size <= 0:
        return {}, off + 4
    chunks = {}
    chunk_off = off + 4 + 8
    end = off + 4 + size
    while chunk_off < end:
        map_raw = slot_data[chunk_off:chunk_off + 4]
        entry_size = struct.unpack_from('<i', slot_data, chunk_off + 4)[0]
        if entry_size <= 0 or map_raw == b'\xff\xff\xff\xff':
            break
        payload = slot_data[chunk_off + 8:chunk_off + entry_size]
        count = struct.unpack_from('<I', payload, 0)[0] if len(payload) >= 4 else 0
        total = struct.unpack_from('<I', payload, 4)[0] if len(payload) >= 8 else 0
        map_name = decode_map_id(map_raw)

        rp_entries = []
        for i in range(count):
            eo = 8 + i * 8
            if eo + 8 <= len(payload):
                e = payload[eo:eo + 8]
                flags = e[1]
                geom_idx = struct.unpack_from('<H', e, 2)[0]
                inst_hash = struct.unpack_from('<I', e, 4)[0]
                if RUNE_PIECE_GEOM_IDX_MIN <= geom_idx <= RUNE_PIECE_GEOM_IDX_MAX:
                    rp_entries.append({
                        'geom_idx': geom_idx,
                        'flags': flags,
                        'instance_hash': inst_hash,
                    })

        if map_name not in chunks:
            chunks[map_name] = {'picked': [], 'total_geom': total}
        chunks[map_name]['picked'].extend(rp_entries)
        chunk_off += entry_size
    return chunks, off + 4 + size


def extract_pickup_data(filepath, slot=2):
    """Extract per-tile Rune Piece pickup counts from save file."""
    with open(filepath, 'rb') as f:
        data = f.read()

    slot_start = 0x300 + slot * 0x280010 + 0x10
    b = data[slot_start:slot_start + 0x280000]

    version = struct.unpack_from('<I', b, 0)[0]
    if version == 0:
        return None

    geom_off = b.find(b'MOEG')
    if geom_off < 0:
        return None
    geom_off -= 4

    geom_size = struct.unpack_from('<i', b, geom_off)[0]
    _, _ = parse_geom_section(b, geom_off)  # skip GEOM, use size to find GEOF
    geof_off = geom_off + 4 + geom_size

    # Parse GEOF (primary pickup data)
    geof_data, _ = parse_geom_section(b, geof_off)

    # Also parse GEOM for any entries not in GEOF
    geom_data, _ = parse_geom_section(b, geom_off)

    # Merge: GEOF is primary, add GEOM-only tiles
    merged = dict(geof_data)
    for map_name, data in geom_data.items():
        if map_name not in merged:
            merged[map_name] = data
    return merged


def load_dungeon_mapping():
    script_dir = Path(__file__).parent
    mapping_path = script_dir.parent / "data" / "dungeon_to_world.json"
    if mapping_path.exists():
        with open(mapping_path) as f:
            return json.load(f)
    return {}

_DUNGEON_MAP = None

def to_world_coords(piece):
    import re
    global _DUNGEON_MAP
    if _DUNGEON_MAP is None:
        _DUNGEON_MAP = load_dungeon_mapping()

    m = re.match(r'm60_(\d+)_(\d+)_(\d+)', piece['map'])
    if m:
        aa, bb = int(m.group(1)), int(m.group(2))
        return aa * 256 + piece['x'], bb * 256 + piece['z'], True

    # Dungeons: WorldMapLegacyConvParam mapping (e.g. m10_00_00_00 -> key m10_00)
    dm = re.match(r'm(\d+)_(\d+)_', piece['map'])
    if dm:
        key = f"m{int(dm.group(1)):02d}_{int(dm.group(2)):02d}"
        if key in _DUNGEON_MAP:
            mp = _DUNGEON_MAP[key]
            wx = mp['world_x'] + (piece['x'] - mp['src_x'])
            wz = mp['world_z'] + (piece['z'] - mp['src_z'])
            return wx, wz, True

    return piece['x'], piece['z'], False


def build_map(save_file, slot, json_file, out_json=None, out_html=None):
    with open(json_file) as f:
        all_pieces = json.load(f)

    pieces_by_map = defaultdict(list)
    for p in all_pieces:
        pieces_by_map[p['map']].append(p)

    for m in pieces_by_map:
        pieces_by_map[m].sort(key=lambda p: p['name'])

    pickup_data = extract_pickup_data(save_file, slot)
    if pickup_data is None:
        print(f"Slot {slot} is empty or not found")
        return

    # slot = (geom_idx - 0x1194) * 2 + (flags >> 7), maps to name-sorted index per tile
    result = []
    total_pieces = 0

    geof_slots = defaultdict(set)
    if pickup_data:
        for map_name, tile_data in pickup_data.items():
            for entry in tile_data.get('all_rp_entries', tile_data.get('picked', [])):
                slot = (entry['geom_idx'] - 0x1194) * 2 + (entry['flags'] >> 7)
                geof_slots[map_name].add(slot)

    for map_name in sorted(pieces_by_map.keys()):
        pieces = pieces_by_map[map_name]
        slots = geof_slots.get(map_name, set())

        for i, piece in enumerate(pieces):
            status = "picked" if i in slots else "remaining"
            wx, wz, is_overworld = to_world_coords(piece)
            result.append({
                'map': map_name, 'name': piece['name'],
                'x': piece['x'], 'y': piece['y'], 'z': piece['z'],
                'world_x': wx, 'world_z': wz, 'overworld': is_overworld,
                'status': status, 'tile_picked': len(slots & set(range(len(pieces)))),
                'tile_total': len(pieces),
            })
            total_pieces += 1

    total_picked = sum(1 for e in result if e['status'] == 'picked')
    total_remaining = sum(1 for e in result if e['status'] == 'remaining')

    print(f"Total Rune Pieces in data: {total_pieces}")
    print(f"Collected (exact slot match): {total_picked}")
    print(f"Marked as picked: {total_picked}")
    print(f"Remaining: {total_remaining}")
    print(f"Tiles visited: {sum(1 for m in pieces_by_map if m in pickup_data and len(pickup_data[m]['picked']) > 0)}")
    print(f"Tiles fully collected: {sum(1 for m in pieces_by_map if m in pickup_data and len(pickup_data[m]['picked']) >= len(pieces_by_map[m]))}")

    if out_json:
        with open(out_json, 'w') as f:
            json.dump(result, f, indent=2)
        print(f"\nJSON map saved to: {out_json}")

    if out_html:
        script_dir = Path(__file__).parent
        map_b64_path = script_dir / "map_b64.txt"
        generate_html_map(result, total_picked, total_remaining, out_html, str(map_b64_path))
        print(f"HTML map saved to: {out_html}")

    return result


def generate_html_map(entries, picked, remaining, filepath, map_b64_path=None):
    maps_data = defaultdict(lambda: {'picked': [], 'remaining': []})
    for e in entries:
        maps_data[e['map']][e['status']].append(e)

    map_b64 = ""
    if map_b64_path and os.path.exists(map_b64_path):
        with open(map_b64_path) as f:
            map_b64 = f.read().strip()

    html = f"""<!DOCTYPE html>
<html><head>
<meta charset="utf-8">
<title>Rune Piece Map — ERR</title>
<style>
body {{ font-family: 'Segoe UI', sans-serif; margin: 0; background: #1a1a2e; color: #e0e0e0; }}
.header {{ padding: 15px 20px; background: #16213e; border-bottom: 1px solid #0f3460; }}
.header h1 {{ margin: 0; font-size: 22px; color: #e94560; }}
.stats {{ display: flex; gap: 20px; margin-top: 8px; font-size: 14px; }}
.stat {{ padding: 4px 12px; border-radius: 4px; }}
.stat-picked {{ background: #1b4332; color: #95d5b2; }}
.stat-remaining {{ background: #4a1942; color: #e0aaff; }}
.container {{ display: flex; height: calc(100vh - 80px); }}
.sidebar {{ width: 340px; overflow-y: auto; padding: 10px; background: #16213e; border-right: 1px solid #0f3460; }}
.map-tile {{ margin-bottom: 4px; padding: 6px 10px; background: #1a1a2e; border-radius: 4px; cursor: pointer; font-size: 13px; }}
.map-tile:hover {{ background: #0f3460; }}
.map-tile .count {{ float: right; font-weight: bold; }}
.complete {{ border-left: 3px solid #2d6a4f; }}
.partial {{ border-left: 3px solid #e9c46a; }}
.untouched {{ border-left: 3px solid #e76f51; }}
.canvas-wrap {{ flex: 1; position: relative; overflow: hidden; }}
canvas {{ position: absolute; top: 0; left: 0; }}
.filter {{ padding: 8px; display: flex; gap: 8px; }}
.filter label {{ font-size: 12px; cursor: pointer; }}
.filter input {{ cursor: pointer; }}
.piece-list {{ max-height: 300px; overflow-y: auto; margin-top: 4px; }}
.piece {{ padding: 3px 8px; font-size: 12px; border-radius: 2px; margin: 1px 0; }}
.piece-picked {{ background: #1b4332; color: #95d5b2; }}
.piece-remaining {{ background: #4a1942; color: #e0aaff; }}
#tooltip {{ position: absolute; background: #16213e; border: 1px solid #0f3460; padding: 8px; border-radius: 4px; font-size: 12px; pointer-events: none; display: none; z-index: 100; }}
</style>
</head><body>
<div class="header">
  <h1>Elden Ring Reforged — Rune Piece Tracker</h1>
  <div class="stats">
    <span class="stat stat-picked">Picked: {picked}</span>
    <span class="stat stat-remaining">Remaining: {remaining}</span>
    <span class="stat" style="background:#0f3460;color:#a2d2ff">Total: {picked + remaining}</span>
  </div>
</div>
<div class="container">
  <div class="sidebar">
    <div class="filter">
      <label><input type="checkbox" id="fPicked" checked onchange="filterTiles()"> Picked</label>
      <label><input type="checkbox" id="fRemaining" checked onchange="filterTiles()"> Remaining</label>
      <input type="text" id="searchBox" placeholder="Search map..." oninput="filterTiles()" style="flex:1;background:#1a1a2e;border:1px solid #0f3460;color:#e0e0e0;padding:4px;border-radius:3px;">
    </div>
    <div class="filter">
      <button onclick="switchView('ow')" id="btnOW" style="flex:1;padding:4px;background:#0f3460;color:#a2d2ff;border:1px solid #0f3460;border-radius:3px;cursor:pointer;font-weight:bold">Overworld</button>
      <button onclick="switchView('dg')" id="btnDG" style="flex:1;padding:4px;background:#1a1a2e;color:#888;border:1px solid #0f3460;border-radius:3px;cursor:pointer">Dungeons</button>
    </div>
    <div id="calibDiv" style="padding:4px 8px;font-size:11px;color:#888;">
      <details><summary style="cursor:pointer">Map calibration</summary>
      <label>Opacity <input type="range" id="cOpacity" min="10" max="100" value="55" step="5" oninput="draw()"></label>
      </details>
    </div>
    <div id="tileList"></div>
  </div>
  <div class="canvas-wrap">
    <canvas id="mapCanvas"></canvas>
    <div id="tooltip"></div>
  </div>
</div>
<script>
const pieces = {json.dumps([{
    'm': e['map'], 'n': e['name'],
    'x': round(e['x'], 1), 'y': round(e['y'], 1), 'z': round(e['z'], 1),
    'wx': round(e['world_x'], 1), 'wz': round(e['world_z'], 1),
    'ow': e['overworld'], 's': e['status'],
    'tp': e['tile_picked'], 'tt': e['tile_total']
} for e in entries])};

const tileData = {{}};
pieces.forEach(p => {{
  if (!tileData[p.m]) tileData[p.m] = {{picked:[], remaining:[]}};
  tileData[p.m][p.s].push(p);
}});

// Build sidebar
function filterTiles() {{
  const showPicked = document.getElementById('fPicked').checked;
  const showRemaining = document.getElementById('fRemaining').checked;
  const search = document.getElementById('searchBox').value.toLowerCase();
  const list = document.getElementById('tileList');
  list.innerHTML = '';
  const sortedMaps = Object.keys(tileData).sort();
  for (const m of sortedMaps) {{
    const isOW = m.startsWith('m60');
    if (showOverworld && !isOW) continue;
    if (!showOverworld && isOW) continue;
    const t = tileData[m];
    const pc = t.picked.length, rc = t.remaining.length;
    if (!search || m.toLowerCase().includes(search)) {{
      if ((showPicked && pc > 0) || (showRemaining && rc > 0) || (!showPicked && !showRemaining)) {{
        const cls = rc === 0 ? 'complete' : pc === 0 ? 'untouched' : 'partial';
        const div = document.createElement('div');
        div.className = 'map-tile ' + cls;
        div.innerHTML = m + '<span class="count" style="color:' + (rc===0?'#95d5b2':'#e0aaff') + '">' + pc + '/' + (pc+rc) + '</span>';
        div.onclick = () => showTileDetail(m);
        list.appendChild(div);
      }}
    }}
  }}
}}

function showTileDetail(m) {{
  const t = tileData[m];
  const all = [...t.picked, ...t.remaining];
  let html = '<div style="padding:6px;font-weight:bold;border-bottom:1px solid #0f3460">' + m + '</div><div class="piece-list">';
  for (const p of all) {{
    const cls = p.s === 'picked' ? 'piece-picked' : 'piece-remaining';
    html += '<div class="piece ' + cls + '">' + (p.s==='picked'?'✓':'○') + ' ' + p.n + ' (' + p.x + ', ' + p.z + ')</div>';
  }}
  html += '</div>';
  document.getElementById('tileList').innerHTML = html + '<div class="map-tile" onclick="filterTiles()" style="text-align:center;margin-top:8px">← Back</div>';
  // Highlight on canvas
  if (!m.startsWith('m60') && !showOverworld) {{
    currentDungeon = m;
    bounds = getBounds();
    scale = 1; offsetX = 0; offsetY = 0;
  }}
  highlightTile(m);
}}

// Canvas 2D map (x, z coordinates)
const canvas = document.getElementById('mapCanvas');
const ctx = canvas.getContext('2d');
const wrap = canvas.parentElement;
let scale = 1, offsetX = 0, offsetY = 0;

function resize() {{
  canvas.width = wrap.clientWidth;
  canvas.height = wrap.clientHeight;
  draw();
}}
window.addEventListener('resize', resize);

// Overworld mode by default; use world coords (wx, wz)
let showOverworld = true;
let currentDungeon = null;

function getVisiblePieces() {{
  if (showOverworld) return pieces.filter(p => p.ow);
  if (currentDungeon) return pieces.filter(p => p.m === currentDungeon);
  return pieces.filter(p => !p.ow);
}}

function getBounds() {{
  const vis = getVisiblePieces();
  if (vis.length === 0) return {{minX:0,maxX:1,minZ:0,maxZ:1}};
  let minX=Infinity, maxX=-Infinity, minZ=Infinity, maxZ=-Infinity;
  vis.forEach(p => {{
    const px = showOverworld ? p.wx : p.x;
    const pz = showOverworld ? p.wz : p.z;
    if (px < minX) minX = px; if (px > maxX) maxX = px;
    if (pz < minZ) minZ = pz; if (pz > maxZ) maxZ = pz;
  }});
  const padX = (maxX-minX)*0.05 || 50, padZ = (maxZ-minZ)*0.05 || 50;
  return {{minX: minX-padX, maxX: maxX+padX, minZ: minZ-padZ, maxZ: maxZ+padZ}};
}}

let bounds = getBounds();

function toScreen(px, pz) {{
  // Use uniform scale (aspect ratio 1:1), flip Z so north is up
  const rangeX = bounds.maxX - bounds.minX;
  const rangeZ = bounds.maxZ - bounds.minZ;
  const uniformScale = Math.min(canvas.width / rangeX, canvas.height / rangeZ);
  const sx = (px - bounds.minX) * uniformScale * scale + offsetX;
  const sy = (bounds.maxZ - pz) * uniformScale * scale + offsetY;  // flipped Z
  return [sx, sy];
}}

let highlightMap = null;
function highlightTile(m) {{ highlightMap = m; draw(); }}

// Map background image
const mapImg = new Image();
const mapB64 = '{map_b64}';
if (mapB64) mapImg.src = 'data:image/jpeg;base64,' + mapB64;
let mapLoaded = false;
mapImg.onload = () => {{ mapLoaded = true; draw(); }};

// Calibration: map image world-coord bounds
// These define which world coords the image corners correspond to
// Adjust with sliders until the map aligns with points
let calMinX = 7030, calMaxX = 16550, calMinZ = 7449, calMaxZ = 16491;

function draw() {{
  ctx.fillStyle = '#1a1a2e';
  ctx.fillRect(0, 0, canvas.width, canvas.height);

  // Draw map background (overworld only)
  if (mapLoaded && showOverworld) {{
    const rangeX = bounds.maxX - bounds.minX;
    const rangeZ = bounds.maxZ - bounds.minZ;
    const uniformScale2 = Math.min(canvas.width / rangeX, canvas.height / rangeZ);
    // Convert calibration corners to screen coords
    // Image maps: left=calMinX, right=calMaxX, top=calMaxZ (north), bottom=calMinZ (south)
    const [imgLeft, imgTop] = toScreen(calMinX, calMaxZ);
    const [imgRight, imgBot] = toScreen(calMaxX, calMinZ);
    ctx.globalAlpha = (+document.getElementById('cOpacity').value) / 100;
    ctx.drawImage(mapImg, imgLeft, imgTop, imgRight - imgLeft, imgBot - imgTop);
    ctx.globalAlpha = 1.0;
  }}

  const vis = getVisiblePieces();
  for (const p of vis) {{
    const px = showOverworld ? p.wx : p.x;
    const pz = showOverworld ? p.wz : p.z;
    const [sx, sy] = toScreen(px, pz);
    if (sx < -10 || sx > canvas.width+10 || sy < -10 || sy > canvas.height+10) continue;
    const r = (highlightMap === p.m ? 6 : 4) * Math.min(scale, 3);
    ctx.beginPath();
    ctx.arc(sx, sy, r, 0, Math.PI*2);
    if (p.s === 'picked') {{
      ctx.fillStyle = highlightMap === p.m ? '#2d6a4f' : '#2d6a4faa';
    }} else {{
      ctx.fillStyle = highlightMap === p.m ? '#e94560' : '#e94560aa';
    }}
    ctx.fill();
    if (highlightMap === p.m) {{
      ctx.strokeStyle = '#fff'; ctx.lineWidth = 1.5; ctx.stroke();
    }}
  }}
}}

// Pan & zoom
let dragging = false, lastX, lastY;
canvas.addEventListener('mousedown', e => {{ dragging=true; lastX=e.clientX; lastY=e.clientY; }});
canvas.addEventListener('mousemove', e => {{
  if (dragging) {{ offsetX += e.clientX-lastX; offsetY += e.clientY-lastY; lastX=e.clientX; lastY=e.clientY; draw(); }}
  // Tooltip
  const rect = canvas.getBoundingClientRect();
  const mx = e.clientX - rect.left, my = e.clientY - rect.top;
  let found = null;
  const vis = getVisiblePieces();
  for (const p of vis) {{
    const px = showOverworld ? p.wx : p.x;
    const pz = showOverworld ? p.wz : p.z;
    const [sx, sy] = toScreen(px, pz);
    const dist = Math.sqrt((sx-mx)**2 + (sy-my)**2);
    if (dist < 8 * Math.min(scale, 3)) {{ found = p; break; }}
  }}
  const tip = document.getElementById('tooltip');
  if (found) {{
    tip.style.display = 'block';
    tip.style.left = (e.clientX - rect.left + 12) + 'px';
    tip.style.top = (e.clientY - rect.top + 12) + 'px';
    tip.innerHTML = '<b>' + found.n + '</b><br>' + found.m + '<br>pos: (' + found.x + ', ' + found.y + ', ' + found.z + ')<br>' + (found.s==='picked'?'<span style="color:#95d5b2">✓ Picked up</span>':'<span style="color:#e0aaff">○ Remaining</span>');
  }} else {{ tip.style.display = 'none'; }}
}});
canvas.addEventListener('mouseup', () => dragging=false);
canvas.addEventListener('wheel', e => {{
  e.preventDefault();
  const rect = canvas.getBoundingClientRect();
  const mx = e.clientX - rect.left, my = e.clientY - rect.top;
  const oldScale = scale;
  scale *= e.deltaY < 0 ? 1.15 : 0.87;
  scale = Math.max(0.1, Math.min(50, scale));
  offsetX = mx - (mx - offsetX) * scale / oldScale;
  offsetY = my - (my - offsetY) * scale / oldScale;
  draw();
}});

function updateCal() {{
  calMinX = +document.getElementById('cMinX').value;
  calMaxX = +document.getElementById('cMaxX').value;
  calMinZ = +document.getElementById('cMinZ').value;
  calMaxZ = +document.getElementById('cMaxZ').value;
  document.getElementById('vMinX').textContent = calMinX;
  document.getElementById('vMaxX').textContent = calMaxX;
  document.getElementById('vMinZ').textContent = calMinZ;
  document.getElementById('vMaxZ').textContent = calMaxZ;
  document.getElementById('calValues').textContent = 'calMinX='+calMinX+' calMaxX='+calMaxX+' calMinZ='+calMinZ+' calMaxZ='+calMaxZ;
  draw();
}}

function switchView(mode) {{
  showOverworld = mode === 'ow';
  currentDungeon = null;
  highlightMap = null;
  document.getElementById('btnOW').style.background = showOverworld ? '#0f3460' : '#1a1a2e';
  document.getElementById('btnOW').style.color = showOverworld ? '#a2d2ff' : '#888';
  document.getElementById('btnDG').style.background = !showOverworld ? '#0f3460' : '#1a1a2e';
  document.getElementById('btnDG').style.color = !showOverworld ? '#a2d2ff' : '#888';
  bounds = getBounds();
  scale = 1; offsetX = 0; offsetY = 0;
  filterTiles();
  resize();
}}

filterTiles();
resize();
</script></body></html>"""

    with open(filepath, 'w', encoding='utf-8') as f:
        f.write(html)


def main():
    import argparse
    parser = argparse.ArgumentParser(description="Rune Piece Map Builder")
    parser.add_argument("save_file", help="Path to .err save file")
    parser.add_argument("--slot", type=int, default=2, help="Character slot (0-9)")
    parser.add_argument("--json", required=True, help="Path to rune_pieces.json with coordinates")
    parser.add_argument("--out", default=None, help="Output JSON file path")
    parser.add_argument("--html", default=None, help="Output HTML map file path")

    args = parser.parse_args()
    build_map(args.save_file, args.slot, args.json, args.out, args.html)


if __name__ == "__main__":
    main()
