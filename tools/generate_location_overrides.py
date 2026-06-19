"""Generate src/generated/goblin_location_alt.{cpp,hpp} - the PRIMARY loot-location naming.

The HYBRID sub-area resolver: MSB MapPoint sub-volume -> MapNameOverride super-volume ->
nearest authored anchor in 3D (WorldMapPoint place-pin UNION grace). At runtime the DLL
OVERWRITES each marker's textId2 with this name (goblin_inject.cpp), so this is the main
location shown. The coarse tile/grace baseline baked by massedit_common.resolve_location_id_at
is the FALLBACK - kept for rows this resolver is silent on (overworld / no volume / no anchor).

Emits:
  - LOCATION_ALT[]    : row_id -> hybrid textId2, only where the hybrid name DIFFERS from the
                        baked baseline (rows absent from it keep the fallback).
  - LOCATION_COMPOSE[]: synthetic ids for duplicate-named sub-zones ("Hallowhorn Grounds
                        (Nokron)") - goblin_messages composes the FMG string at runtime.

Reads the baked entries straight from src/generated/goblin_map_data.cpp (row_id + areaNo/grid +
posX/Y/Z + current textId2; posY recovered from source data where the cpp omits it). Overworld
(areas 60/61) is left as-is. Must run AFTER generate_data.

Usage: py generate_location_overrides.py
"""
import os, sys, io, re, json, tempfile, math
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8")
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import config
from pythonnet import load; load("coreclr")
import clr
from System import Array, Object, Type as SysType
from System.IO import File as SysFile
from System.Reflection import Assembly, BindingFlags
asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL)); clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats
_str = SysType.GetType("System.String")
_read = asm.GetType("SoulsFormats.MSBE").GetMethod("Read", BindingFlags.Public|BindingFlags.Static|BindingFlags.FlattenHierarchy, None, Array[SysType]([_str]), None)
D = os.path.join(HERE, "..", "data")          # repo data root (ERR-only artifacts)
PROF = str(config.DATA_DIR)                    # profile data dir (data/ or data/vanilla/)
PLACE = json.load(open(os.path.join(PROF, "PlaceName_engus.json"), encoding="utf-8"))
VALID = set(int(k) for k in PLACE.keys())
WMPP = {int(w["ID"]): w for w in json.load(open(os.path.join(PROF, "WorldMapPointParam.json"), encoding="utf-8"))}
WP = json.load(open(os.path.join(D, "WorldMapPointParam.json"), encoding="utf-8"))
GR = json.load(open(os.path.join(PROF, "grace_position_index.json"), encoding="utf-8"))
SENTINELS = {5000, 5100, 5300, 8800}  # textId2/3/4 logic sentinels - never override
from massedit_common import OVERWORLD_AREAS as OVERWORLD  # {60, 61}: no location subtitle, keep baked

# ---- MSB volume cache ----
def _prop(o,n):
    p=o.GetType().GetProperty(n); return p.GetValue(o) if p else None
def _vec3(v): return None if v is None else (float(v.X),float(v.Y),float(v.Z))
def _shp(sh):
    if sh is None: return None
    t=sh.GetType().Name; d={}
    for n in ("Width","Height","Depth","Radius"):
        p=sh.GetType().GetProperty(n)
        if p: d[n]=float(p.GetValue(sh))
    ch=[]
    if t=="Composite":
        c=sh.GetType().GetProperty("Children").GetValue(sh)
        for i in range(c.Length):
            rn=c[i].GetType().GetProperty("RegionName").GetValue(c[i])
            if rn: ch.append(str(rn))
    return {"type":t,"dims":d,"children":ch}
def _wmp_textid(wid):
    w=WMPP.get(int(wid))
    if not w: return None
    t=w.get("textId1")
    return int(t) if t is not None and int(t)>=0 else None

_VOL_CACHE={}
def load_vols(mapname):
    if mapname in _VOL_CACHE: return _VOL_CACHE[mapname]
    path=config.require_err_mod_dir()/"map"/"MapStudio"/(mapname+".msb.dcx")
    subs=[]; sups=[]
    if path.exists():
        try:
            data=SoulsFormats.DCX.Decompress(str(path))
            t=os.path.join(tempfile.gettempdir(),str(os.getpid()) + "_lo.msb"); SysFile.WriteAllBytes(t,data.ToArray())
            msb=_read.Invoke(None,Array[Object]([t])); os.unlink(t)
            byname={}
            for r in list(msb.Regions.GetEntries()):
                byname[str(_prop(r,"Name"))]={"kind":type(r).__name__,"pos":_vec3(_prop(r,"Position")),
                    "rot":_vec3(_prop(r,"Rotation")),"shape":_shp(_prop(r,"Shape")),
                    "wmp":_prop(r,"WorldMapPointParamID"),"textId":_prop(r,"TextID")}
            def prims(rec):
                sh=rec["shape"]
                if not sh: return []
                if sh["type"]=="Composite":
                    o=[]
                    for cn in sh["children"]:
                        ch=byname.get(cn)
                        if ch and ch["shape"] and ch["shape"]["type"]!="Composite":
                            o.append((ch["shape"]["type"],ch["pos"],ch["rot"],ch["shape"]["dims"]))
                    return o
                return [(sh["type"],rec["pos"],rec["rot"],sh["dims"])]
            for rec in byname.values():
                if rec["kind"]=="MapPoint" and rec["wmp"] is not None and int(rec["wmp"])>=0:
                    tid=_wmp_textid(rec["wmp"])
                    if tid and tid in VALID: subs.append((tid,int(rec["wmp"]),prims(rec)))
                elif rec["kind"]=="MapNameOverride" and rec["textId"] is not None and int(rec["textId"])>=0:
                    tid=int(rec["textId"])
                    if tid in VALID: sups.append((tid,None,prims(rec)))
        except Exception as e:
            pass
    _VOL_CACHE[mapname]=(subs,sups)
    return _VOL_CACHE[mapname]

_ANCH_CACHE={}
def anchors_for(area,gx,gz):
    key=(area,gx,gz)
    if key in _ANCH_CACHE: return _ANCH_CACHE[key]
    out=[]
    for r in WP:
        if r.get("areaNo")!=area or r.get("gridXNo")!=gx or r.get("gridZNo")!=gz: continue
        t=r.get("textId1")
        # 3xxxxxxx = boss/enemy markers (e.g. 30120200 Crucible Knight, 32100100 Grafted Scion) - not places
        if t is None or int(t)<0 or 30000000<=int(t)<40000000: continue
        if int(t) not in VALID: continue
        out.append((int(t),float(r["posX"]),float(r["posY"]),float(r["posZ"]),"wmp-pin"))
    for g in GR:
        if g.get("areaNo")!=area or g.get("gridX")!=gx or g.get("gridZ")!=gz: continue
        sc=g.get("subCategoryId",0)
        # only graces whose region belongs to THIS area's numbering (avoids cross-area
        # warp graces, e.g. an overworld 'Stormhill' 61001 indexed under area 10)
        if sc and int(sc) in VALID and int(sc)//1000==area:
            out.append((int(sc),float(g["x"]),float(g["y"]),float(g["z"]),"grace"))
    _ANCH_CACHE[key]=out
    return out

# Vertical bounds follow the bottom-anchor semantics (Position.Y = the area's FLOOR):
# a volume must NEVER claim points below its floor (small slope tolerance only) - e.g. a
# riverbank node 13u under the Hallowhorn Grounds floor is Siofra River, not Hallowhorn.
def _v_ok(dy,H):
    return -max(3.0,0.15*H)<=dy<=1.25*H

def _h_excess(px,py,pz,pos,rotY,dims,kind):
    """Horizontal distance OUTSIDE the shape footprint (0 = inside), or None if the
    vertical band rejects the point. Hand-drawn boxes undershoot the real named area,
    so sub-areas may claim within a horizontal margin - but only on their own floor."""
    if kind=="Sphere":
        R=dims.get("Radius",0)
        d=math.sqrt((px-pos[0])**2+(py-pos[1])**2+(pz-pos[2])**2)
        return max(0.0,d-R)
    if kind=="Cylinder":
        R=dims.get("Radius",0); H=dims.get("Height",0); dy=py-pos[1]
        if not _v_ok(dy,H): return None
        return max(0.0,math.sqrt((px-pos[0])**2+(pz-pos[2])**2)-R)
    W,H,Dp=dims.get("Width",0),dims.get("Height",0),dims.get("Depth",0)
    a=-math.radians(rotY or 0); ca,sa=math.cos(a),math.sin(a)
    dx,dy,dz=px-pos[0],py-pos[1],pz-pos[2]; lx=dx*ca+dz*sa; lz=-dx*sa+dz*ca
    if not _v_ok(dy,H): return None
    ex=max(0.0,abs(lx)-W/2); ez=max(0.0,abs(lz)-Dp/2)
    return math.sqrt(ex*ex+ez*ez)

SUB_MARGIN=60.0   # horizontal reach beyond the authored box (same-floor only)

def _claim(vols,x,y,z,margin):
    best=None; bd=margin+1e-9
    for tid,wmp,prs in vols:
        for k,pos,rot,dims in prs:
            if not pos: continue
            ex=_h_excess(x,y,z,pos,(rot[1] if rot else 0),dims,k)
            if ex is not None and ex<bd: bd=ex; best=(tid,wmp)
    return best

def _nearest_grace(area,gx,gz,x,y,z):
    anch=[a for a in anchors_for(area,gx,gz) if a[4]=="grace"]
    best=None; bd=1e30
    for t,ax,ay,az,_ in anch:
        d=(x-ax)**2+(y-ay)**2+(z-az)**2
        if d<bd: bd=d; best=t
    return best

# Duplicate-name sub-zone disambiguation: when one PlaceName labels SEVERAL distinct
# volumes in a block (e.g. the two Hallowhorn Grounds - Siofra-level and Nokron-level),
# loot claimed by each instance gets a COMPOSED label "<sub> (<super>)". The DLL builds
# the text at runtime from the game's own PlaceName FMG strings (localization-safe).
COMPOSE={}        # (subTextId, superTextId) -> composeId
COMPOSE_BASE=999000001
def compose_id(sub_tid,super_tid):
    key=(sub_tid,super_tid)
    if key not in COMPOSE: COMPOSE[key]=COMPOSE_BASE+len(COMPOSE)
    return COMPOSE[key]

_DUP_CACHE={}
def dup_super_for(mapname,area,gx,gz):
    """For blocks with duplicate-named MapPoint volumes: wmpId -> superTextId (or None)."""
    if mapname in _DUP_CACHE: return _DUP_CACHE[mapname]
    subs,sups=load_vols(mapname)
    byname_count={}
    for tid,wmp,prs in subs: byname_count[tid]=byname_count.get(tid,0)+1
    out={}
    for tid,wmp,prs in subs:
        if byname_count.get(tid,0)<2 or wmp is None: continue
        # volume center -> super context (strict super containment, else nearest grace)
        pts=[p[1] for p in prs if p[1]]
        if not pts: continue
        cx=sum(p[0] for p in pts)/len(pts); cy=sum(p[1] for p in pts)/len(pts); cz=sum(p[2] for p in pts)/len(pts)
        sup=_claim(sups,cx,cy,cz,0.0)
        sup_tid=sup[0] if sup else _nearest_grace(area,gx,gz,cx,cy,cz)
        if sup_tid and sup_tid!=tid: out[wmp]=sup_tid
    _DUP_CACHE[mapname]=out
    return out

def hybrid_id(area,gx,gz,x,y,z):
    """Return PlaceName textId via hybrid model, or None to keep baked."""
    if area in OVERWORLD: return None
    mapname=f"m{area:02d}_{gx:02d}_{gz:02d}_00"
    subs,sups=load_vols(mapname)
    hit=_claim(subs,x,y,z,SUB_MARGIN)         # sub-area: volume + margin, floor-gated
    if hit is not None:
        tid,wmp=hit
        dup=dup_super_for(mapname,area,gx,gz)
        if wmp in dup: return compose_id(tid,dup[wmp])   # duplicated name -> "<sub> (<super>)"
        return tid
    hit=_claim(sups,x,y,z,0.0)                # super override: strict containment
    if hit is not None: return hit[0]
    # fallback: nearest GRACE only (3D). WorldMapPoint pins are deliberately NOT free
    # Voronoi anchors - their authority is their volume; open-range pins over-claim
    # (e.g. Hallowhorn pin 65u grabbing a Siofra riverbank node from a 90u grace).
    return _nearest_grace(area,gx,gz,x,y,z)

# ---- name-keyed source index for pieces: (area,gx,gz,object_name) -> (x,y,z) MSB-local ----
# Bypasses cpp coords entirely - also fixes maps whose baked coords are display-shifted
# (generate_data.py COORD_SHIFTS, e.g. (11,10) Roundtable Hold x-2195 z-352).
_NIDX={}
for fn,key in (("rune_pieces.json","map"),("ember_pieces.json","map")):
    if not os.path.exists(os.path.join(D,fn)): continue  # ERR-only artifact
    for r in json.load(open(os.path.join(D,fn),encoding="utf-8")):
        mp=r.get(key,"")
        if len(mp)<9: continue
        _NIDX[(int(mp[1:3]),int(mp[4:6]),int(mp[7:9]),r.get("name",""))]=(float(r["x"]),float(r["y"]),float(r["z"]))
# COORD_SHIFTS mirror of generate_data.py - unshift baked coords for spatial lookup
COORD_SHIFTS={(11,10):(-2195.0,-352.0)}

# ---- parse baked cpp ----
CPP=str(config.GENERATED_DIR / "goblin_map_data.cpp")
txt=open(CPP,encoding="utf-8").read()
# split per entry: "{<id>ull, {" ... "}, Category::<cat>, <geom>, <suffix>, "name"|nullptr, <lotId>u, <lotType>},"
# (the trailing lotId/lotType pair is optional for backward compat)
ENTRY=re.compile(r"\{(\d+)ull,\s*\{(.*?)\},\s*Category::(\w+),\s*(-?\d+),\s*(-?\d+),\s*(?:nullptr|\"([^\"]*)\")(?:,\s*\d+u?,\s*\d+)?\}", re.DOTALL)
def fget(body,name):
    m=re.search(r"\."+name+r"\s*=\s*(-?[\d.]+)f?", body)
    return m.group(1) if m else None

entries=[]
for m in ENTRY.finditer(txt):
    rid=int(m.group(1)); body=m.group(2)
    def gi(n):
        v=fget(body,n); return int(float(v)) if v is not None else None
    def gf(n):
        v=fget(body,n); return float(v) if v is not None else 0.0
    yv=fget(body,"posY")  # MASSEDIT generators omit posY when 0 - absent != 0 for our 3D math!
    area=gi("areaNo") or 0; gx=gi("gridXNo") or 0; gz=gi("gridZNo") or 0
    x=gf("posX"); z=gf("posZ"); y=float(yv) if yv is not None else None
    # undo display-space shift so spatial lookup/volumes work in MSB-local space
    sh=COORD_SHIFTS.get((area,gx))
    if sh: x-=sh[0]; z-=sh[1]
    # pieces etc.: exact (tile, object_name) join beats coords entirely
    oname=m.group(6)
    if oname:
        src=_NIDX.get((area,gx,gz,oname))
        if src: x,y,z=src
    # Find the LOCATION slot. Per-marker the layout varies: plain loot has the
    # location in textId2, but enemy-drop loot is textId1=item / textId2=enemy /
    # textId3=LOCATION. The location id is always a PlaceName id < 50,000,000
    # (item names are 100M+ offsets, enemy names 700M/900M/1.6B), and never a
    # logic sentinel - so the only textId in [1,50M) non-sentinel is the location.
    SENT={5000,5100,5300,8800}
    loc_slot=0; loc_val=None
    for s in range(2,9):
        v=gi(f"textId{s}")
        if v is not None and 0<v<50000000 and v not in SENT:
            loc_slot=s; loc_val=v; break
    entries.append({"row_id":rid,"area":area,"gx":gx,"gz":gz,"x":x,"y":y,"z":z,
                    "loc_slot":loc_slot,"loc_val":loc_val,"cat":m.group(3)})
print(f"parsed {len(entries)} baked entries from goblin_map_data.cpp "
      f"({sum(1 for e in entries if e['y'] is None)} without posY)")

# ---- Y-recovery index: (area,gx,gz) -> {(round x, round z): [(x,z,y), ...]} ----
# Sources carry the true 3D position the markers were generated from.
_YIDX={}
def _yidx_add(area,gx,gz,x,z,y):
    blk=_YIDX.setdefault((area,gx,gz),{})
    blk.setdefault((round(x),round(z)),[]).append((x,z,y))
for r in json.load(open(os.path.join(PROF,"items_database.json"),encoding="utf-8")):
    if r.get("x") is None or r.get("y") is None: continue
    _yidx_add(int(r.get("areaNo",0)),int(r.get("gridX",0)),int(r.get("gridZ",0)),
              float(r["x"]),float(r["z"]),float(r["y"]))
for r in json.load(open(os.path.join(PROF,"all_gathering_nodes_final.json"),encoding="utf-8")):
    mp=r.get("map","")
    if len(mp)<9: continue
    _yidx_add(int(mp[1:3]),int(mp[4:6]),int(mp[7:9]),float(r["x"]),float(r["z"]),float(r["y"]))
# Rune/Ember pieces + graces: categories whose MASSEDIT rows often omit posY
for r in (json.load(open(os.path.join(D,"all_pieces.json"),encoding="utf-8"))["pieces"]
          if os.path.exists(os.path.join(D,"all_pieces.json")) else []):
    mp=r.get("tile","")
    if len(mp)<9: continue
    _yidx_add(int(mp[1:3]),int(mp[4:6]),int(mp[7:9]),float(r["x"]),float(r["z"]),float(r["y"]))
for r in (json.load(open(os.path.join(D,"ember_pieces.json"),encoding="utf-8"))
          if os.path.exists(os.path.join(D,"ember_pieces.json")) else []):
    mp=r.get("map","")
    if len(mp)<9: continue
    _yidx_add(int(mp[1:3]),int(mp[4:6]),int(mp[7:9]),float(r["x"]),float(r["z"]),float(r["y"]))
for g in GR:
    _yidx_add(int(g.get("areaNo",0)),int(g.get("gridX",0)),int(g.get("gridZ",0)),
              float(g["x"]),float(g["z"]),float(g["y"]))
def recover_y(area,gx,gz,x,z,tol=1.5):
    blk=_YIDX.get((area,gx,gz))
    if not blk: return None
    best=None; bd=tol*tol
    rx,rz=round(x),round(z)
    for kx in (rx-1,rx,rx+1):
        for kz in (rz-1,rz,rz+1):
            for cx,cz,cy in blk.get((kx,kz),()):
                d=(x-cx)**2+(z-cz)**2
                if d<bd: bd=d; best=cy
    return best

# ---- compute hybrid overrides ----
# Each override is (row_id, slot, new_textId): the DLL overwrites textId<slot> with
# new_textId. slot is the marker's LOCATION slot (textId2 for plain loot, textId3 for
# enemy-drops); slot 0 means "no baseline location line - add one (textId2 if free)".
from collections import Counter
recs=[]; changed=0; kept=0; by_area=Counter()
nameffmt=lambda t: PLACE.get(str(t),"?")
no_y=0
for e in entries:
    if e["cat"]=="WorldGraces": continue  # graces ARE location markers - no location line
    cur=e["loc_val"]                       # baseline location id (None if marker has none)
    y=e["y"]
    if y is None:
        y=recover_y(e["area"],e["gx"],e["gz"],e["x"],e["z"])
        if y is None:
            no_y+=1; continue           # can't trust 3D math without a real Y -> keep baked
    nid=hybrid_id(e["area"],e["gx"],e["gz"],e["x"],y,e["z"])
    if nid is None: continue            # no hybrid opinion -> keep baked baseline
    # skip if same id OR same displayed PlaceName text (avoids cosmetic id-only churn)
    if cur is not None and (nid==cur or PLACE.get(str(nid))==PLACE.get(str(cur))):
        kept+=1; continue
    recs.append((e["row_id"], e["loc_slot"], nid))   # loc_slot 0 => DLL adds the line
    changed+=1
    by_area[e["area"]]+=1

# Bake as a generated C++ table (sorted by row_id for binary search) - no external file.
GEN=str(config.GENERATED_DIR)
os.makedirs(GEN, exist_ok=True)
trips=sorted((rid,slot,tid) for rid,slot,tid in recs)
with open(os.path.join(GEN,"goblin_location_alt.hpp"),"w",encoding="utf-8") as f:
    f.write("""#pragma once

#include <cstddef>
#include <cstdint>

namespace goblin::generated
{

// Hybrid sub-area loot-location names (PRIMARY): rows whose location differs from the baked
// baseline under the hybrid model (MSB volume containment + height-aware nearest anchor in 3D).
// The DLL OVERWRITES textId<slot> with this value at injection time; rows absent from this
// table keep their baked baseline (the tile/grace fallback).
//   slot = the marker's LOCATION slot - textId2 for plain loot, textId3 for enemy-drop loot
//          (textId1=item / textId2=enemy / textId3=location). slot 0 = no baseline location
//          line; the DLL adds one (textId2 if free, else textId3).
struct LocationAlt
{
    uint64_t row_id;   // ORIGINAL (pre-remap) row id, matches MAP_ENTRIES[].row_id
    uint8_t  slot;     // location textId slot to overwrite (2/3...), 0 = add a new line
    int32_t  textId2;  // hybrid PlaceName id (may be a COMPOSED id, see below)
};

// Duplicate-named sub-zones (e.g. the two Hallowhorn Grounds) get a synthetic PlaceName id
// whose text the DLL composes at runtime from the game's own FMG strings: "<sub> (<super>)".
// Localization-safe: both parts are vanilla PlaceName ids looked up in the live FMG.
struct LocationCompose
{
    int32_t id;        // synthetic PlaceName id (999000001+)
    int32_t subId;     // vanilla PlaceName id of the sub-zone name
    int32_t superId;   // vanilla PlaceName id of the disambiguating super-zone
};

extern const LocationAlt LOCATION_ALT[];  // sorted by row_id
extern const size_t LOCATION_ALT_COUNT;
extern const LocationCompose LOCATION_COMPOSE[];
extern const size_t LOCATION_COMPOSE_COUNT;

} // namespace goblin::generated
""")
with open(os.path.join(GEN,"goblin_location_alt.cpp"),"w",encoding="utf-8") as f:
    f.write("// AUTO-GENERATED FILE - DO NOT EDIT\n")
    f.write("// Generated by tools/generate_location_overrides.py from goblin_map_data.cpp + MSB/param data\n\n")
    f.write('#include "goblin_location_alt.hpp"\n\n')
    f.write("namespace goblin::generated\n{\n\n")
    f.write(f"const size_t LOCATION_ALT_COUNT = {len(trips)};\n\n")
    f.write("const LocationAlt LOCATION_ALT[] = {\n")
    for rid,slot,tid in trips:
        f.write(f"    {{{rid}ull, {slot}, {tid}}},\n")
    f.write("};\n\n")
    comp=sorted((cid,sub,sup) for (sub,sup),cid in COMPOSE.items())
    f.write(f"const size_t LOCATION_COMPOSE_COUNT = {len(comp)};\n\n")
    f.write("const LocationCompose LOCATION_COMPOSE[] = {\n")
    for cid,sub,sup in comp:
        f.write(f"    {{{cid}, {sub}, {sup}}},  // \"{PLACE.get(str(sub),'?')} ({PLACE.get(str(sup),'?')})\"\n")
    if not comp:
        f.write("    {0, 0, 0},  // none generated - keep array non-empty for MSVC\n")
    f.write("};\n\n} // namespace goblin::generated\n")
if comp:
    print(f"{len(comp)} composed duplicate-zone labels:")
    for cid,sub,sup in comp:
        print(f"   {cid}: \"{PLACE.get(str(sub),'?')} ({PLACE.get(str(sup),'?')})\"")
print(f"\nwrote {changed} alt locations ({kept} already matched baked, "
      f"{no_y} skipped: no Y recoverable) -> {config.GENERATED_DIR}/goblin_location_alt.cpp")
print("overrides by area:", dict(by_area))
# show a few human-readable samples
print("\nsample changes (row_id slot: baked -> hybrid):")
shown=0
for e in entries:
    if shown>=12: break
    if e["cat"]=="WorldGraces": continue
    cur=e["loc_val"]
    y=e["y"] if e["y"] is not None else recover_y(e["area"],e["gx"],e["gz"],e["x"],e["z"])
    if y is None: continue
    nid=hybrid_id(e["area"],e["gx"],e["gz"],e["x"],y,e["z"])
    if nid is None or nid==cur or PLACE.get(str(nid))==PLACE.get(str(cur)): continue
    print(f"   {e['row_id']} textId{e['loc_slot']}: {cur} '{nameffmt(cur)}' -> {nid} '{nameffmt(nid)}'")
    shown+=1
