from pathlib import Path
import sys,json,struct,re,subprocess,os
from course_catalog import ROOT as root
# --out DIR: write the header and the audit there instead of the work tree (a cook check).
out=Path(sys.argv[sys.argv.index('--out')+1]) if '--out' in sys.argv else None
w=out or root/'build/draw-policies';w.mkdir(parents=True,exist_ok=True)
header_path=(out or root/'include/mkw')/'course_phase_ranges.hpp'

from course_catalog import courses
from model_sources import LocalModels,model
pack=LocalModels()
rows=[];table=[];ztable=[]
def key(vs):return tuple(sorted(tuple(round(float(x),2) for x in v[:3]) for v in vs))
for c in sorted(courses(),key=lambda c:c['id']):
 cid=c['id'];k=c['key'];paths=[root/'build/startline-source'/k,root/'build/course-exports'/k/'course']
 src=next((p for p in paths if (p/'model.json').exists() and (p/'materials.json').exists()),None)
 if src is None:
  src=root/'build/phase-source'/k;src.mkdir(parents=True,exist_ok=True)
  env={**os.environ,'MKW_FREEZE_NONLINEAR_UV':'1','MKW_COURSE_MATERIALS':'1','MKW_RETAIL_LIGHTING':'1'}
  subprocess.run(['node',str(root/'build/export_brres.mjs'),str(root/'assets/courses'/k/'course_model.brres'),str(src)],env=env,check=True,stdout=subprocess.DEVNULL)
 mats={m['name']:m for group in json.loads((src/'materials.json').read_text()) for m in group['materials']}
 lookup={};eligible=set()
 # Source materials that blend AND write depth: DrawOpa ones stay in NW4R's opaque list (opaque order,
 # below); DrawXlu ones keep the late pass. Kept apart so a DrawXlu shape on the same triangles never matches.
 zlookup={};ordered=set();zothers=set()
 for group in json.loads((src/'model.json').read_text())['models']:
  for shape in group['shapes']:
   m=mats.get(shape['material'])
   if m is None:continue  # extra program passes (<material>#<pass>, export_brres.ts) are translucent overlays: never eligible
   g=m['gxMaterial']['ropInfo']
   if not m['translucent'] and g['blendMode']==1 and g['depthTest'] and not g['depthWrite']:
    eligible.add(m['name'])
    v=shape['vertices']
    for i in range(0,len(v)-2,3):lookup.setdefault(key(v[i:i+3]),set()).add(m['name'])
   elif g['blendMode']==1 and g['depthWrite']:
    (zothers if m['translucent'] else ordered).add(m['name'])
    v=shape['vertices']
    for i in range(0,len(v)-2,3):zlookup.setdefault(key(v[i:i+3]),set()).add(m['name'])
 selected=[];unmatched=0;zselected=[];zunmatched=0
 for name in sorted(n for n in pack.entries if re.fullmatch(fr'models/course_{cid:02d}_\d+\.gmdl',n)):
  raw=pack.read(name);version,subs,names,verts,indices=model(raw)
  _,_,nv,ni,ns,nt=struct.unpack_from('<4s5I',raw)
  for sub in subs:
   first,count,tex,flags=sub[:4]
   if flags&2 and flags&(1<<24) and (ordered or zothers):
    # Cooked blended + depth-write submesh: in NW4R's DrawOpa list when every triangle is a DrawOpa one.
    zfound=[zlookup.get(key([verts[j] for j in indices[i:i+3]]),set()) for i in range(first,first+count,3)]
    if zfound and all(f and f<=ordered for f in zfound):
     material=sorted(set().union(*zfound))
     # Rainbow Road's z2_road keeps its own early pass (rainbow_shadow.hpp, bit 27), on the retail course and its copies.
     bit=27 if material==['z2_road'] else 30
     zselected.append({'mesh':name,'first':first,'count':count,'materials':material,'bit':bit})
     ztable.append((Path(name).name,nv,ni,first,count,bit))
    elif any(f&ordered for f in zfound):zunmatched+=1
   if not flags&2 or flags&(1<<24):continue
   found=[lookup.get(key([verts[j] for j in indices[i:i+3]]),set()) for i in range(first,first+count,3)]
   if found and all(found):
    material=sorted(set().union(*found));selected.append({'mesh':name,'first':first,'count':count,'materials':material})
    table.append((Path(name).name,nv,ni,first,count))
   elif any(found):unmatched+=1
 rows.append({'course':cid,'key':k,'eligible_source_materials':sorted(eligible),'selected':selected,'partially_matched_submeshes':unmatched,
              'opaque_order_source_materials':sorted(ordered),'opaque_order_selected':zselected,'opaque_order_partially_matched':zunmatched})
 assert not unmatched,(k,unmatched)
header=['// Generated against packaged HW81 meshes and original DrawOpa materials.','#pragma once','#include <cstring>','namespace mkw::coursePhase {','struct Range {const char* name;unsigned vertices,indices,first,count;};','inline constexpr Range Ranges[]={']
header += ['{"%s",%d,%d,%d,%d},'%r for r in table]
header += ['};','template<class Model> void apply(Model& model,const char* path){','const char* name=std::strrchr(path,\'/\');name=name?name+1:path;','for(const auto& r:Ranges)if(model.vertexCount==r.vertices&&model.indexCount==r.indices&&!std::strcmp(name,r.name))','for(auto& s:model.submeshes)if(s.first==r.first&&s.count==r.count)s.lighting|=1u<<26;','}','}']
# Source DrawOpa materials that blend and write depth: drawn in the opaque layer in authored order
# (course_opaque_order.hpp), so later DrawOpa shapes and racer shadow volumes see their depth.
header += ['namespace mkw::courseOpaqueOrder {','struct Range {const char* name;unsigned vertices,indices,first,count,bit;};','inline constexpr Range Ranges[]={']
header += ['{"%s",%d,%d,%d,%d,%d},'%r for r in ztable] or ['{"",0,0,0,0,0},']
header += ['};','}']
header_path.write_text('\n'.join(header)+'\n')
(w/'course-phase-audit.json').write_text(json.dumps({'courses':rows,'selected_submeshes':len(table),'opaque_order_submeshes':len(ztable),'limitations':'Read-only source/pack audit; no gameplay traversal'},ensure_ascii=False,indent=2)+'\n')
print('PASS',len(rows),'courses;',len(table),'source DrawOpa blended submeshes matched exactly;',len(ztable),'DrawOpa blend + depth write (opaque order);',sum(r['opaque_order_partially_matched'] for r in rows),'partially matched')
