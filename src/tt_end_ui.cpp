#include <mkw/tt_end_ui.hpp>
#include <mkw/race_flow_draw.hpp>
#include <mkw/race_flow_layout.hpp>
#include <mkw/race_flow_ui.hpp>
#include <mkw/race_results.hpp>
#include <mkw/flow_text.hpp>
#include <mkw/renderer.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace mkw {
namespace {
// The race overlays' mapping (race_flow_draw.cpp): 456 layout units span the 240-pixel
// height, about the screen centre, y up.
constexpr float Unit=240.f/456.f;
// TTSplits' rows as the page shows them on the Wii: the total at the top left and the laps on
// the right, their digits ending at about -67 and 351 layout units (time_number's 16:9
// CtrlRaceTime_1 / CtrlRaceTimeLapN say x 188 for both; the rows' y and scales are the brctr's).
// Origins for game_image_number's panes below.
constexpr float TotalX=-168.f,LapX=261.f;
// game_image_number.brlyt: race_null's own offset in the control.
constexpr float RaceNullX=-81.f,RaceNullY=-.4f;

uint32_t packColor(const std::array<float,4>& c){
 auto b=[](float v){return uint32_t(std::clamp(std::lround(v),0l,255l));};
 return (b(c[0])<<24)|(b(c[1])<<16)|(b(c[2])<<8)|b(c[3]);
}
uint32_t withAlpha(uint32_t rgba,float alpha){return (rgba&0xffffff00u)|uint32_t(std::lround(std::clamp(alpha,0.f,1.f)*float(rgba&0xff)));}
const FlowGlyph* glyph(const FlowFont& font,char16_t c){
 for(unsigned i=0;i<font.glyphCount;++i)if(font.glyphs[i].code==c)return &font.glyphs[i];
 return nullptr;
}
struct Placement {float x,y,sx,sy;};
struct Batch {std::array<FlowQuad,96> quads;unsigned count=0;FlowQuad* add(){return count<quads.size()?&quads[count++]:nullptr;}};

// The helpers below follow race_flow_draw.cpp's (NW4R pane placement, picture panes,
// LEQUAL-occluded button plates, one-line text boxes).
void place(const FlowPanePose& pane,const Placement& at,float lx,float ly,float& sx,float& sy){
 const float x=pane.m[0]*lx+pane.m[1]*ly+pane.m[2],y=pane.m[3]*lx+pane.m[4]*ly+pane.m[5];
 sx=200.f+(at.x+x*at.sx)*Unit;sy=120.f-(at.y+y*at.sy)*Unit;
}
template<class B>
void pictureQuad(B& batch,const FlowPane& def,const FlowPanePose& pane,const FlowMaterialPose& material,const Placement& at){
 auto* q=batch.add();if(!q)return;
 const float left=-float(def.origin%3)*pane.w*.5f,top=float(def.origin/3)*pane.h*.5f;
 const float lx[4]={left,left+pane.w,left,left+pane.w},ly[4]={top,top,top-pane.h,top-pane.h};
 const auto& srt=material.srt;
 const float r=srt[2]*3.14159265358979323846f/180.f,c=std::cos(r),s=std::sin(r);
 for(unsigned k=0;k<4;++k){
  place(pane,at,lx[k],ly[k],q->x[k],q->y[k]);
  const float du=def.uv[k*2]-.5f,dv=def.uv[k*2+1]-.5f;
  q->u[k]=srt[3]*c*du-srt[4]*s*dv+srt[0]+.5f;q->v[k]=srt[3]*s*du+srt[4]*c*dv+srt[1]+.5f;
  q->color[k]=withAlpha(pane.vertex[k],pane.alpha);
 }
}
template<class B>
void occludedPicture(B& batch,const FlowPane& def,const FlowPanePose& pane,const FlowMaterialPose& material,const Placement& at,
                     const FlowOccluder* occluders,unsigned count,float z,FlowOccluder& self){
 struct Whole {FlowQuad quad;unsigned count=0;FlowQuad* add(){return count?nullptr:(++count,&quad);}} whole;
 pictureQuad(whole,def,pane,material,at);
 const auto& q=whole.quad;
 const float x0=std::min(q.x[0],q.x[1]),x1=std::max(q.x[0],q.x[1]),y0=std::min(q.y[0],q.y[2]),y1=std::max(q.y[0],q.y[2]);
 self={x0,y0,x1,y1,z};
 const bool axisAligned=std::fabs(q.x[0]-q.x[2])<.01f&&std::fabs(q.y[0]-q.y[1])<.01f;
 struct Box {float x0,y0,x1,y1;};
 std::array<Box,16> parts{};unsigned n=0;parts[n++]={x0,y0,x1,y1};
 for(unsigned o=0;o<count&&axisAligned;++o){
  const auto& c=occluders[o];if(c.z<=z)continue;
  std::array<Box,16> next{};unsigned m=0;
  for(unsigned i=0;i<n;++i){
   const Box b=parts[i];
   if(c.x1<=b.x0||c.x0>=b.x1||c.y1<=b.y0||c.y0>=b.y1){if(m<next.size())next[m++]=b;continue;}
   const float ix0=std::max(b.x0,c.x0),ix1=std::min(b.x1,c.x1),iy0=std::max(b.y0,c.y0),iy1=std::min(b.y1,c.y1);
   const Box pieces[4]={{b.x0,b.y0,b.x1,iy0},{b.x0,iy1,b.x1,b.y1},{b.x0,iy0,ix0,iy1},{ix1,iy0,b.x1,iy1}};
   for(const auto& piece:pieces)if(piece.x1-piece.x0>.01f&&piece.y1-piece.y0>.01f&&m<next.size())next[m++]=piece;
  }
  parts=next;n=m;
 }
 const bool flipX=q.x[1]<q.x[0],flipY=q.y[2]<q.y[0];
 auto lerp4=[&](const float* v,float s,float t){return v[0]*(1-s)*(1-t)+v[1]*s*(1-t)+v[2]*(1-s)*t+v[3]*s*t;};
 for(unsigned i=0;i<n;++i){
  auto* out=batch.add();if(!out)return;
  const Box& b=parts[i];
  const float xs[4]={b.x0,b.x1,b.x0,b.x1},ys[4]={b.y0,b.y0,b.y1,b.y1};
  for(unsigned k=0;k<4;++k){
   float s=x1>x0?(xs[k]-x0)/(x1-x0):0,t=y1>y0?(ys[k]-y0)/(y1-y0):0;
   if(flipX)s=1-s;
   if(flipY)t=1-t;
   out->x[k]=xs[k];out->y[k]=ys[k];out->u[k]=lerp4(q.u,s,t);out->v[k]=lerp4(q.v,s,t);
   uint32_t colour=0;
   for(unsigned shift:{24u,16u,8u,0u}){
    const float c[4]={float((q.color[0]>>shift)&255),float((q.color[1]>>shift)&255),float((q.color[2]>>shift)&255),float((q.color[3]>>shift)&255)};
    colour|=uint32_t(std::clamp(std::lround(lerp4(c,s,t)),0l,255l))<<shift;
   }
   out->color[k]=colour;
  }
 }
}
// One line in a text box (fontSize / BRFNT size per glyph, the box's text position).
template<class B>
void textQuads(B& batch,const FlowPane& def,const FlowPanePose& pane,const Placement& at,const char* text,const FlowFont& font){
 if(!text||!*text)return;
 const float scaleX=def.fontW/float(font.width),scaleY=def.fontH/float(font.height);
 float width=0;unsigned n=0;
 for(const char* p=text;*p;){if(const auto* g=glyph(font,nextFlowGlyph(p))){width+=float(g->advance)*scaleX;++n;}}
 if(n>1)width+=def.charSpace*float(n-1);
 const float lineH=float(font.linefeed)*scaleY,cellH=float(font.cellH)*scaleY;
 const float left=-float(def.origin%3)*pane.w*.5f,top=float(def.origin/3)*pane.h*.5f;
 static constexpr float Align[3]={0,.5f,1};
 float x=left+(pane.w-width)*Align[def.textPosition%3];const float y=top-(pane.h-lineH)*Align[std::min(def.textPosition/3,2)];
 const uint32_t topColor=withAlpha(pane.vertex[0],pane.alpha),bottomColor=withAlpha(pane.vertex[2],pane.alpha);
 for(const char* p=text;*p;){
  const auto* g=glyph(font,nextFlowGlyph(p));if(!g)continue;
  if(g->w>0){
   auto* q=batch.add();if(!q)return;
   const float gx=x+float(g->left)*scaleX,gw=float(g->w)*scaleX;
   const float lx[4]={gx,gx+gw,gx,gx+gw},ly[4]={y,y,y-cellH,y-cellH};
   const float u0=float(g->x)/float(font.atlasW),u1=float(g->x+g->w)/float(font.atlasW);
   const float v0=float(g->y)/float(font.atlasH),v1=float(g->y+font.cellH)/float(font.atlasH);
   const float us[4]={u0,u1,u0,u1},vs[4]={v0,v0,v1,v1};
   for(unsigned k=0;k<4;++k){place(pane,at,lx[k],ly[k],q->x[k],q->y[k]);q->u[k]=us[k];q->v[k]=vs[k];q->color[k]=k<2?topColor:bottomColor;}
  }
  x+=float(g->advance)*scaleX+def.charSpace;
 }
}
}

struct TtEndUi::Impl {
 std::array<Texture,FlowTtTextures.size()> textures;unsigned loaded=0;bool failed=false;
 Renderer* renderer=nullptr;RaceFlowUi* flowUi=nullptr;
 Batch batch;FlowPose pose;
 std::array<std::array<FlowPlay,4>,RaceResultsFlow::TtButtons> buttons{};bool menuStarted=false;unsigned lastCursor=0;int chosen=-1;
 std::array<FlowOccluder,FlowPose::MaxPanes> occluders{};
 unsigned splitsFrame=0;FlowPlay record{};
 unsigned* counter=nullptr;
 void flush(int texture,uint8_t wrapS,uint8_t wrapT,uint32_t c0,uint32_t c1){
  if(!batch.count)return;
  if(counter)*counter+=batch.count;
  if(texture>=100){
   const unsigned i=unsigned(texture-100);
   if(i<textures.size()&&textures[i].ready)
    renderer->drawScreenMaterialQuads(&textures[i],reinterpret_cast<const Renderer::MaterialQuad*>(batch.quads.data()),batch.count,c0,c1,GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE);
  }else{
   const FlowBatch b{texture,wrapS,wrapT,c0,c1,batch.quads.data(),batch.count};
   flowUi->drawBatch(*renderer,b);
  }
  batch.count=0;
 }
 // One CtrlRaceTime row (game_image_number): the label (TIME / Lap n), minutes, ':' seconds '.' milliseconds.
 void row(const Placement& at,unsigned label,uint32_t ms,uint32_t c1){
  const unsigned minutes=unsigned(std::min<uint32_t>(ms/60000,99u)),seconds=unsigned(ms/1000%60),milli=unsigned(ms%1000);
  // CtrlRaceTime_refresh: race_null and time_01 the minutes, time_02..03 the seconds, time_04..06 the
  // milliseconds; coron01 = coron_00, coron00 = coron.
  const FlowTtRect* art[10]={&FlowTtDigits[minutes/10],&FlowTtDigits[minutes%10],&FlowTtColon,&FlowTtDigits[seconds/10],&FlowTtDigits[seconds%10],
                             &FlowTtDot,&FlowTtDigits[milli/100],&FlowTtDigits[milli/10%10],&FlowTtDigits[milli%10],&FlowTtLabels[std::min<size_t>(label,FlowTtLabels.size()-1)]};
  for(unsigned i=0;i<10;++i){
   const auto& p=FlowTtNumberPanes[i];auto* q=batch.add();if(!q)break;
   const float cx=200.f+(at.x+(RaceNullX+p[0])*at.sx)*Unit,cy=120.f-(at.y+(RaceNullY+p[1])*at.sy)*Unit;
   const float hw=p[2]*at.sx*Unit*.5f,hh=p[3]*at.sy*Unit*.5f;
   const auto& r=*art[i];
   const float u0=float(r.x)/FlowTtAtlas[0],u1=float(r.x+r.w)/FlowTtAtlas[0],v0=float(r.y)/FlowTtAtlas[1],v1=float(r.y+r.h)/FlowTtAtlas[1];
   const float xs[4]={cx-hw,cx+hw,cx-hw,cx+hw},ys[4]={cy-hh,cy-hh,cy+hh,cy+hh},us[4]={u0,u1,u0,u1},vs[4]={v0,v0,v1,v1};
   for(unsigned k=0;k<4;++k){q->x[k]=xs[k];q->y[k]=ys[k];q->u[k]=us[k];q->v[k]=vs[k];q->color[k]=0xffffffffu;}
  }
  flush(100,0,0,0x00000000u,c1);
 }
};

TtEndUi::TtEndUi():impl_(std::make_unique<Impl>()){}
TtEndUi::~TtEndUi()=default;
bool TtEndUi::loadStep(){
 auto& m=*impl_;if(m.failed)return false;
 if(m.loaded<m.textures.size()){if(!m.textures[m.loaded].load(FlowTtTextures[m.loaded])){m.failed=true;return false;}++m.loaded;}
 return m.loaded==m.textures.size();
}
bool TtEndUi::loaded()const{return impl_->loaded==impl_->textures.size();}
bool TtEndUi::failed()const{return impl_->failed;}
unsigned TtEndUi::bytes()const{unsigned n=0;for(const auto& t:impl_->textures)n+=t.bytes();return n;}
void TtEndUi::unload(){
 auto& m=*impl_;for(auto& t:m.textures)t.reset();
 m.loaded=0;m.failed=false;m.menuStarted=false;m.chosen=-1;m.splitsFrame=0;m.record={};
 splitsQuads=menuQuads=0;
}

void TtEndUi::step(const RaceResultsFlow& flow){
 auto& m=*impl_;
 if(flow.ttSplitsVisible()){
  if(!m.splitsFrame&&data_.newRecord)m.record=flowPlay(FlowTtRecordLayout,"Fade","Fadein");
  ++m.splitsFrame;flowAdvance(FlowTtRecordLayout,m.record,1);
  // Fadein ends at alpha 0 and continues into Hide, which animates no alpha: NW4R keeps the
  // panes' last values (invisible); FlowPose starts from the layout's, so stop drawing it.
  if(m.record.clip>=0&&std::strcmp(FlowTtRecordLayout.clips[m.record.clip].name,"Fadein"))m.record={};
 }
 if(!flow.ttMenuVisible()){m.menuStarted=false;m.chosen=-1;return;}
 // RaceMenu::OnInit: PushButton::SelectInitial(0); then as FlowDrawer::step's buttons.
 const auto& def=FlowButtonLayout;const unsigned count=flow.buttonCount(),cursor=flow.cursor();
 if(!m.menuStarted){
  for(unsigned b=0;b<count;++b){auto& p=m.buttons[b];const bool selected=b==cursor;
   p[0]=flowPlay(def,"Loop","Loop");p[1]=flowPlay(def,"Select",selected?"FreeToSelect":"Free");
   p[2]=flowPlay(def,"SelectIn",selected?"SelectIn":"SelectStop");p[3]=flowPlay(def,"OK","OKStop");}
  m.menuStarted=true;m.lastCursor=cursor;m.chosen=-1;
 }else if(cursor!=m.lastCursor&&cursor<count){
  auto& old=m.buttons[m.lastCursor];auto& now=m.buttons[cursor];
  old[1]=flowPlay(def,"Select","SelectToFree");old[2]=flowPlay(def,"SelectIn","SelectStop");
  now[1]=flowPlay(def,"Select","FreeToSelect");now[2]=flowPlay(def,"SelectIn","SelectIn");
  m.lastCursor=cursor;
 }
 if(flow.chosen()>=0&&m.chosen<0&&unsigned(flow.chosen())<count){m.chosen=flow.chosen();m.buttons[m.chosen][3]=flowPlay(def,"OK","OK");}
 for(unsigned b=0;b<count;++b)for(auto& p:m.buttons[b])flowAdvance(def,p,1);
}

void TtEndUi::draw(Renderer& renderer,RaceFlowUi& flowUi,const RaceResultsFlow& flow){
 auto& m=*impl_;
 if(!flow.ttEnd()||!loaded())return;
 m.renderer=&renderer;m.flowUi=&flowUi;
 if(flow.ttSplitsVisible()){
  m.counter=&splitsQuads;
  // TTSplits: the best lap's game_image_number_best_lap (C1 green/blue 255 <-> 100), the total too
  // with a new record; the others game_image_number_normal_rap (C1 white).
  const float g=flowSample(FlowTtBestKeys,unsigned(std::size(FlowTtBestKeys)),std::fmod(float(m.splitsFrame),FlowTtBestFrames),false);
  const uint32_t gb=uint32_t(std::clamp(std::lround(g),0l,255l));
  const uint32_t flash=0xff0000ffu|gb<<16|gb<<8;
  m.row({TotalX,FlowTtRows[0].y,FlowTtRows[0].sx,FlowTtRows[0].sy},0,data_.totalMs,data_.newRecord?flash:0xffffffffu);
  for(unsigned lap=1;lap<=data_.laps&&lap<FlowTtRows.size();++lap){
   const auto& c=FlowTtRows[lap];
   m.row({LapX,c.y,c.sx,c.sy},lap,data_.lapMs[lap-1],lap==data_.bestLap?flash:0xffffffffu);
  }
  // NEW RECORD! (newrecord.brctr NewRecord on new_record.brlyt, Fade / Fadein).
  if(m.record.clip>=0){
   auto& pose=m.pose;pose.evaluate(FlowTtRecordLayout,&m.record,1);
   const Placement at{FlowTtRecordPlace.x,FlowTtRecordPlace.y,FlowTtRecordPlace.sx,FlowTtRecordPlace.sy};
   for(unsigned p=0;p<FlowTtRecordLayout.paneCount&&p<FlowPose::MaxPanes;++p){
    const auto& pane=FlowTtRecordLayout.panes[p];if(pane.kind!=2||pane.material<0)continue;
    const auto& pp=pose.panes[p];if(!pp.visible||pp.alpha<=0)continue;
    const auto& mat=pose.materials[pane.material];
    textQuads(m.batch,pane,pp,at,FlowTtRecordText,FlowTtRecordFont);m.flush(FlowTtRecordFont.texture,0,0,packColor(mat.c0),packColor(mat.c1));
   }
  }
  m.counter=nullptr;
 }
 if(flow.ttMenuVisible()&&m.menuStarted){
  m.counter=&menuQuads;
  // TTEnd: AfterMenuTimeAttack's buttons on common_w004_menu (as FlowDrawer's VS buttons).
  const auto& def=FlowButtonLayout;
  static const int fuchi=flowMaterialIndex(def,"fuchi_pattern"),base=flowMaterialIndex(def,"color_base");
  for(unsigned b=0;b<flow.buttonCount()&&b<FlowTtButtons.size();++b){
   const auto& c=FlowTtButtons[b];
   const Placement at{c.x+flow.slideOffset(c.delay),c.y,c.sx,c.sy};
   auto& pose=m.pose;pose.evaluate(def,m.buttons[b].data(),4);
   const auto& palette=FlowButtonPalette[b==flow.cursor()?0:1];
   for(unsigned k=0;k<4;++k){
    if(fuchi>=0){pose.materials[fuchi].c0[k]=palette[0][k];pose.materials[fuchi].c1[k]=palette[1][k];}
    if(base>=0){pose.materials[base].c0[k]=palette[2][k];pose.materials[base].c1[k]=palette[2][k];}
   }
   unsigned occluded=0;
   for(unsigned p=0;p<def.paneCount&&p<FlowPose::MaxPanes;++p){
    const auto& pane=def.panes[p];const auto& pp=pose.panes[p];
    if(pane.kind==0||!pp.visible||pp.alpha<=0)continue;
    const auto& material=pose.materials[pane.material];
    if(pane.kind==1){occludedPicture(m.batch,pane,pp,material,at,m.occluders.data(),occluded,pp.z,m.occluders[occluded]);++occluded;
     m.flush(def.materials[pane.material].texture,def.materials[pane.material].wrapS,def.materials[pane.material].wrapT,packColor(material.c0),packColor(material.c1));}
    else{textQuads(m.batch,pane,pp,at,FlowTtButtonText[b],FlowTtKanjiFont);m.flush(FlowTtKanjiFont.texture,0,0,packColor(material.c0),packColor(material.c1));}
   }
  }
  m.counter=nullptr;
 }
}
}
