"""Quick verification of piece positions database."""
import json
import config

with open(str(config.DATA_DIR / 'rune_pieces.json')) as f:
    pieces = json.load(f)

# Known test position
test_x, test_y, test_z = 90.423, 244.471, -29.212

nearest = sorted(pieces,
    key=lambda p: (p['x']-test_x)**2 + (p['y']-test_y)**2 + (p['z']-test_z)**2)

print("Nearest pieces to test position (90.423, 244.471, -29.212) on m60_34_45_00:")
for p in nearest[:5]:
    dist = ((p['x']-test_x)**2 + (p['y']-test_y)**2 + (p['z']-test_z)**2)**0.5
    print(f"  {p['map']:20s} {p['name']:20s} ({p['x']:>10.3f}, {p['y']:>10.3f}, {p['z']:>10.3f})  dist={dist:.2f}")

# Count by area
from collections import Counter
areas = Counter()
for p in pieces:
    area = p['map'].split('_')[0]
    areas[area] += 1
print(f"\nPieces by area:")
for area, cnt in areas.most_common():
    print(f"  {area}: {cnt}")
