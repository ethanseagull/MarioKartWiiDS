// The race loading screen's map draw-in (tools/cook_loading_reveal.py): for one course's Standings
// silhouette and enemy route, the pen's path (resampled, with its length) and the place along that
// path at which each map pixel appears.
//   node loading_reveal.mjs <in.bin> <out.bin>
// in:  u32 route points, then the route as f32 x,y pairs (silhouette texture px), then the
//      silhouette as 256x256 RGBA8 (grey already shaded as the Standings draws it).
// out: u32 path samples, f32 length, f32 arc max, then the path as f32 x,y,cum triples, then the
//      arc per pixel (f32, -1 = none) and the rest flag per pixel (u8).
import {readFileSync,writeFileSync} from 'node:fs';
const clamp=(v,a=0,b=1)=>Math.max(a,Math.min(b,v));
const EDGE=2,REST=.5,WAKE=.7;    // reveal edge (px of path); fade of the unclaimed rest (s); wake: px of path per px aside
const NEAR=40,TIE=2,FLIGHT=30,BAND=14; // claim reach; near-tie margin; flight segments; reach beyond the road's half-width (px)
const SOLID=250,AIRGAP=12,SKIRT=6,HOLE=1; // road and its black edge = alpha >= SOLID (the shadow is softer); route up to AIRGAP px off its road; shadow skirt; holes in a road
// Centripetal Catmull-Rom: no loops or overshoot where route points are unevenly spaced.
function cr(p0,p1,p2,p3,u){
 const d=(a,b)=>Math.max(1e-3,Math.sqrt(Math.hypot(b[0]-a[0],b[1]-a[1])));
 const t1=d(p0,p1),t2=t1+d(p1,p2),t3=t2+d(p2,p3),t=t1+(t2-t1)*u;
 const L=(a,b,ta,tb)=>[(a[0]*(tb-t)+b[0]*(t-ta))/(tb-ta),(a[1]*(tb-t)+b[1]*(t-ta))/(tb-ta)];
 const A1=L(p0,p1,0,t1),A2=L(p1,p2,t1,t2),A3=L(p2,p3,t2,t3);return L(L(A1,A2,0,t2),L(A2,A3,t1,t3),t1,t2);
}
// The pen's path: the closed enemy route resampled about every px. Curves between close points; a long segment
// (a cannon, a glide, a gap between route points) stays a straight line, crossed at the same steady speed. Samples
// on segments over FLIGHT px are in the air (cannons, glides): the pen flies over whatever is under them and they
// claim no map pixels.
function penPath(route){
 const n=route.length,at=i=>route[((i%n)+n)%n],P=[],air=[];
 for(let i=0;i<n;i++){const p0=at(i-1),p1=at(i),p2=at(i+1),p3=at(i+2),len=Math.hypot(p2[0]-p1[0],p2[1]-p1[1]);
  const straight=len>24||Math.hypot(p1[0]-p0[0],p1[1]-p0[1])>24||Math.hypot(p3[0]-p2[0],p3[1]-p2[1])>24;
  for(let k=0,m=Math.max(1,Math.ceil(len));k<m;k++){const u=k/m;P.push(straight?[p1[0]+(p2[0]-p1[0])*u,p1[1]+(p2[1]-p1[1])*u]:cr(p0,p1,p2,p3,u));air.push(len>FLIGHT&&k>0)}}
 // round the corners a little (two passes of a 5-sample average on the closed path): an even glide through
 // 90-degree turns instead of a snap
 for(let pass=0;pass<2;pass++){const n2=P.length,Q=P.map(p=>p.slice());
  for(let i=0;i<n2;i++){let x=0,y=0;for(let j=-2;j<=2;j++){const q=P[((i+j)%n2+n2)%n2];x+=q[0];y+=q[1]}Q[i]=[x/5,y/5]}P.splice(0,n2,...Q)}
 P.push(P[0].slice());air.push(false);const cum=new Float64Array(P.length);
 for(let i=1;i<P.length;i++)cum[i]=cum[i-1]+Math.hypot(P[i][0]-P[i-1][0],P[i][1]-P[i-1][1]);
 return {P,air,cum,L:cum[P.length-1]};
}
function pointAt(M,s){s=((s%M.L)+M.L)%M.L;let lo=0,hi=M.cum.length-1;while(hi-lo>1){const m=(lo+hi)>>1;if(M.cum[m]<=s)lo=m;else hi=m}
 const f=(s-M.cum[lo])/((M.cum[hi]-M.cum[lo])||1),a=M.P[lo],b=M.P[hi];return [a[0]+(b[0]-a[0])*f,a[1]+(b[1]-a[1])*f]}
// Can a path sample claim a pixel? Walking from the pixel to the sample may cross at most a SKIRT of shadow at the
// pixel's end, then road (small HOLEs allowed), then at most AIRGAP px of air at the sample's end (a route drawn
// beside its road). So a pass claims only its own road: never a neighbouring road across a gap, never a road it
// flies over.
function sees(A,px,py,sx,sy){
 const n=Math.max(1,Math.ceil(Math.hypot(sx-px,sy-py)));let phase=0,run=0;
 for(let i=0;i<=n;i++){const x=Math.floor(px+(sx-px)*i/n),y=Math.floor(py+(sy-py)*i/n),solid=x>=0&&y>=0&&x<256&&y<256&&A[(y*256+x)*4+3]>=SOLID;
  if(phase===0){if(solid)phase=1;else if(++run>SKIRT)return false}
  else if(phase===1){if(!solid){phase=2;run=1}}
  else if(phase===2){if(solid){if(run>HOLE)return false;phase=1}else if(++run>AIRGAP)return false}}
 return true;
}
// Each path sample's road half-width: the distance from the sample to the road's edge (a chamfer distance
// transform of the solid road), the smallest over 12 px of path either side, so a crossing doesn't widen it.
function halfWidths(M,A){
 const D=new Float32Array(65536);for(let k=0;k<65536;k++)D[k]=A[k*4+3]>=SOLID?1e9:0;
 const a=1,b=Math.SQRT2;
 for(let y=0;y<256;y++)for(let x=0;x<256;x++){const k=y*256+x;if(!D[k])continue;let v=D[k];
  if(x>0)v=Math.min(v,D[k-1]+a);if(y>0){v=Math.min(v,D[k-256]+a);if(x>0)v=Math.min(v,D[k-257]+b);if(x<255)v=Math.min(v,D[k-255]+b)}D[k]=v}
 for(let y=255;y>=0;y--)for(let x=255;x>=0;x--){const k=y*256+x;if(!D[k])continue;let v=D[k];
  if(x<255)v=Math.min(v,D[k+1]+a);if(y<255){v=Math.min(v,D[k+256]+a);if(x<255)v=Math.min(v,D[k+257]+b);if(x>0)v=Math.min(v,D[k+255]+b)}D[k]=v}
 const n=M.P.length,raw=new Float32Array(n),hw=new Float32Array(n);
 for(let i=0;i<n;i++){const x=Math.round(M.P[i][0]),y=Math.round(M.P[i][1]);raw[i]=x>=0&&y>=0&&x<256&&y<256?Math.min(D[y*256+x],40):0}
 for(let i=0;i<n;i++){let m=1e9;for(let j=-12;j<=12;j++)m=Math.min(m,raw[((i+j)%n+n)%n]);hw[i]=m}
 return hw;
}
// Each map pixel's place along the pen's path. If the pixel lies inside the road band of one or more passes
// (within that sample's road half-width + 1.5 px, at most 12: a crossing's square, overlapping layers) the earliest of those
// passes takes it, so a shared piece appears with the first pass, whole, while the other road's arms wait for
// their own pass. Otherwise the nearest sample that can claim it, within its road's
// half-width + BAND px, at most 24 (wide plazas beyond that band are left to the end). Pixels no pass can claim (side
// areas, ramps and branches off the route) are the rest: they fade in once the lap is closed.
// Cooked per course on the 3DS build: a 16-bit arc per map pixel, 0xFFFF for the rest.
// The road body: solid pixels, except the dark ones at the silhouette's border (its black edge). Dark pixels inside
// (a road's shadow cast across another, lane marks) are body.
function bodyMask(A){const b=new Uint8Array(65536);
 for(let y=0;y<256;y++)for(let x=0;x<256;x++){const k=y*256+x;if(A[k*4+3]<SOLID)continue;if(A[k*4]>=128){b[k]=1;continue}
  let border=false;for(let dy=-2;dy<=2&&!border;dy++)for(let dx=-2;dx<=2;dx++){const u=x+dx,v=y+dy;if(u<0||v<0||u>255||v>255||A[(v*256+u)*4+3]<SOLID){border=true;break}}
  if(!border)b[k]=1}
 return b}
function arcMap(M,A){
 const body=bodyMask(A),hw=halfWidths(M,A),arc=new Float32Array(65536).fill(-1),rest=new Uint8Array(65536),C=NEAR+1,G=Math.ceil(256/C)+2,cells=Array.from({length:G*G},()=>[]);
 for(let i=0;i<M.P.length-1;i++){if(M.air[i])continue;const cx=Math.floor(M.P[i][0]/C)+1,cy=Math.floor(M.P[i][1]/C)+1;if(cx>=0&&cy>=0&&cx<G&&cy<G)cells[cy*G+cx].push(i)}
 for(let y=0;y<256;y++)for(let x=0;x<256;x++){const k=y*256+x;if(!body[k])continue; // the road body
  const px=x+.5,py=y+.5,cx=Math.floor(px/C)+1,cy=Math.floor(py/C)+1,near=[];
  for(let j=cy-1;j<=cy+1;j++)for(let i=cx-1;i<=cx+1;i++){if(i<0||j<0||i>=G||j>=G)continue;
   for(const s of cells[j*G+i]){const d=Math.hypot(M.P[s][0]-px,M.P[s][1]-py);if(d<=NEAR)near.push([d,s])}}
  near.sort((a,b)=>a[0]-b[0]);let core=1e9,cd=0;
  for(const [d,s] of near)if(hw[s]>0&&d<=Math.min(hw[s]+1.5,12)&&M.cum[s]<core){core=M.cum[s];cd=d}
  if(core<1e9){arc[k]=core+WAKE*cd;continue}
  let dmin=-1,best=1e9;
  for(const [d,s] of near){if(dmin>=0&&d>dmin+TIE)break;if(d>Math.min(Math.max(hw[s],8)+BAND,24))continue;if(dmin<0&&!sees(A,px,py,M.P[s][0],M.P[s][1]))continue;
   if(dmin<0)dmin=d;if(M.cum[s]<best&&sees(A,px,py,M.P[s][0],M.P[s][1]))best=M.cum[s]}
  // a pixel aside of the path appears a little after the pen passes (WAKE px per px aside): wide areas open
  // out behind the pen in a smooth wake instead of all at once
  if(dmin<0)rest[k]=1;else arc[k]=best+WAKE*dmin}
 // The black edge goes with the road body it bounds (the nearest body pixel within 4 px), so an edge never
 // waits for a farther pass or shows without its road.
 for(let y=0;y<256;y++)for(let x=0;x<256;x++){const k=y*256+x;if(A[k*4+3]<SOLID||body[k])continue;
  let bd=1e9,bk=-1;for(let dy=-4;dy<=4;dy++)for(let dx=-4;dx<=4;dx++){const u=x+dx,v=y+dy;if(u<0||v<0||u>255||v>255)continue;const q=v*256+u;
   if(!body[q]||(arc[q]<0&&!rest[q]))continue;const d=dx*dx+dy*dy;if(d<bd){bd=d;bk=q}}
  if(bk<0||rest[bk])rest[k]=1;else{let m=arc[bk];for(let dy=-2;dy<=2;dy++)for(let dx=-2;dx<=2;dx++){const u=x+dx,v=y+dy;if(u<0||v<0||u>255||v>255)continue;const q=v*256+u;
   if(body[q]&&arc[q]>=0&&arc[q]<m)m=arc[q]}arc[k]=m}} // the edge shows as soon as any road beside it does
 // The soft pixels (drop shadow, anti-aliased rim) go with the road that casts them: the nearest road pixel to
 // their up-left (the shadow falls down-right), so a road's shadow never appears with a road it merely touches.
 for(let y=0;y<256;y++)for(let x=0;x<256;x++){const k=y*256+x;if(A[k*4+3]<4||A[k*4+3]>=SOLID)continue;
  let bd=1e9,bk=-1;for(let dy=-SKIRT;dy<=2;dy++)for(let dx=-SKIRT;dx<=2;dx++){const u=x+dx,v=y+dy;if(u<0||v<0||u>255||v>255)continue;const q=v*256+u;
   if(A[q*4+3]<SOLID)continue;const d=Math.hypot(dx+1.5,dy+1.5);if(d<bd){bd=d;bk=q}}
  if(bk<0||rest[bk])rest[k]=1;else arc[k]=arc[bk]}
 // No islands: a pixel that would appear before all of its neighbours waits for the earliest of them (4 rounds),
 // so stray specks and slivers cut off by a thin gap appear with the road around them, not ahead of it.
 for(let it=0;it<4;it++){const nx=arc.slice();
  for(let y=0;y<256;y++)for(let x=0;x<256;x++){const k=y*256+x;if(arc[k]<0)continue;let m=1e9;
   for(let dy=-1;dy<=1;dy++)for(let dx=-1;dx<=1;dx++){if(!dx&&!dy)continue;const u=x+dx,v=y+dy;if(u<0||v<0||u>255||v>255)continue;const a2=arc[v*256+u];if(a2>=0&&a2<m)m=a2}
   if(m<1e9&&m>arc[k])nx[k]=m}
  arc.set(nx)}
 let max=0;for(let k=0;k<65536;k++)if(arc[k]>max)max=arc[k];
 return {arc,rest,max};
}

const input=readFileSync(process.argv[2]);
const view=new DataView(input.buffer,input.byteOffset,input.byteLength);
const n=view.getUint32(0,true),route=[];
for(let i=0;i<n;i++)route.push([view.getFloat32(4+8*i,true),view.getFloat32(8+8*i,true)]);
const SIL=new Uint8Array(input.buffer,input.byteOffset+4+8*n,65536*4);
const M=penPath(route),R=arcMap(M,SIL);
const out=Buffer.alloc(12+12*M.P.length+65536*4+65536),o=new DataView(out.buffer,out.byteOffset,out.byteLength);
o.setUint32(0,M.P.length,true);o.setFloat32(4,M.L,true);o.setFloat32(8,R.max,true);
let at=12;
for(let i=0;i<M.P.length;i++){o.setFloat32(at,M.P[i][0],true);o.setFloat32(at+4,M.P[i][1],true);o.setFloat32(at+8,M.cum[i],true);at+=12}
for(let k=0;k<65536;k++){o.setFloat32(at,R.arc[k],true);at+=4}
for(let k=0;k<65536;k++)out[at++]=R.rest[k];
writeFileSync(process.argv[3],out);
