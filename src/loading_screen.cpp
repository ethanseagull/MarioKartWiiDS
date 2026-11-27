#include <mkw/loading_screen.hpp>
#include <mkw/loading_wheel_pixels.hpp>
#include <mkw/renderer.hpp>
#include <mkw/asset_pack.hpp>
#include <mkw/stereo.hpp>
#include <mkw/console_model.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <vector>
#include <atomic>
#include <cmath>
#include <memory>
#include <new>
#include <cstdio>
#include <cstdarg>
#include <malloc.h>
namespace mkw {
namespace {
// A byte buffer whose allocation may fail without abort(): a failed new in a
// release build exits to the HOME Menu with no crash dump.
struct Bytes{
 std::unique_ptr<uint8_t[]> p;
 uint8_t* data() const {return p.get();}
 bool alloc(size_t n){p.reset(new(std::nothrow) uint8_t[n]);return bool(p);}
 void clear(){p.reset();}
};
#ifndef MKW_RELEASE
// DIAG: the loading picture's steps in runtime.log (main thread only: the timer
// thread must not touch the SD card).
void pictureLog(const char* format,...){
 FILE* f=std::fopen("sdmc:/3ds/MKWiiDS/runtime.log","a");if(!f)return;
 va_list args;va_start(args,format);std::vfprintf(f,format,args);va_end(args);std::fputc('\n',f);std::fclose(f);
}
#else
void pictureLog(const char*,...){}
#endif
constexpr unsigned Size=32,Visible=24;
Renderer* renderer_=nullptr;
std::unique_ptr<Texture> wheel;
std::array<uint8_t,Size*Size*4> pixels{};
std::array<uint8_t,320*240*3> background{};
Thread spinner=nullptr;std::atomic<bool> stop{false};unsigned depth=0;
uint8_t* framebuffer=nullptr;unsigned width=320;
bool shown=false;
uint64_t started=0;
unsigned scopes=0;
// RaceLoadingPicture (one at a time; tools/cook_loading_picture.py).
struct PictureRect{uint16_t x,y,w,h;};
enum PictureLayer:unsigned{LogoLayer,TextLayer,PeriodLayer};
std::array<PictureRect,3> pictureRects{};
Bytes pictureStill;   // background, logo and LOADING; freed once both buffers hold the picture
Bytes periodLoop;     // the periods' loop, grey + alpha per frame
std::array<Bytes,2> periodBase; // per eye: the picture under the periods' area
std::atomic<unsigned> picturePresents{0}; // DIAG: refreshes the timer thread presented
// DIAG: CPU time the timer threads spend drawing (the picture's periods, LoadingScope's wheel), and their refreshes.
std::atomic<uint64_t> pictureWork{0},wheelWork{0};std::atomic<unsigned> wheelDraws{0};
unsigned pictureWidth=0,loopFrames=0,loopFps=60;
size_t logoAt=0,textAt=0;
std::array<int,2> eyeShift{};        // per eye: the logo's and LOADING's x shift (3D: out of the screen)
Thread pictureThread=nullptr;std::atomic<bool> pictureStop{false};
bool pictureStereo=false;
std::array<uint8_t*,2> pictureShown{}; // per eye: the buffer on screen
constexpr unsigned PictureFadeFrames=10;
unsigned pictureEyes(){return pictureStereo?2u:1u;}
// The buffer libctru shows after the next swap.
uint8_t* topBack(unsigned eye){return gfxGetFramebuffer(GFX_TOP,eye?GFX_RIGHT:GFX_LEFT,nullptr,nullptr);}
size_t pictureBytes(){return size_t(pictureWidth)*240*3;}
// Framebuffer columns run bottom row first: screen rows y..y+h-1 are rows 240-y-h..239-y.
uint8_t* fbColumn(uint8_t* fb,int x,const PictureRect& r){return fb+3*(size_t(x)*240+(240-r.y-r.h));}
// A premultiplied layer (columns, bottom row first; Channels 4 = BGRA, 2 = grey
// and alpha) over the framebuffer at its place plus `shift`, scaled by k/256.
template<unsigned Channels> void blendLayer(uint8_t* fb,const PictureRect& r,int shift,const uint8_t* src,unsigned k){
 for(unsigned cx=0;cx<r.w;++cx){
  const int x=int(r.x)+int(cx)+shift;if(x<0||x>=int(pictureWidth))continue;
  uint8_t* out=fbColumn(fb,x,r);const uint8_t* in=src+size_t(Channels)*cx*r.h;
  for(unsigned i=0;i<r.h;++i,out+=3,in+=Channels){
   const unsigned a=in[Channels-1];if(!a)continue;
   const unsigned keep=255-a;
   for(unsigned c=0;c<3;++c){const unsigned s=Channels==4?in[c]:in[0];out[c]=uint8_t(((s*k)>>8)+(out[c]*keep+127)/255);}
  }
 }
}
const uint8_t* periodFrame(unsigned frame){const auto& r=pictureRects[PeriodLayer];return periodLoop.data()+size_t(frame)*r.w*r.h*2;}
// The whole picture for one eye at brightness k/256, its periods at rest if asked.
void composeStill(uint8_t* fb,unsigned eye,unsigned k,bool periods){
 const uint8_t* background=pictureStill.data();const size_t bytes=pictureBytes();
 if(k>=256)std::memcpy(fb,background,bytes);else for(size_t i=0;i<bytes;++i)fb[i]=uint8_t((background[i]*k)>>8);
 blendLayer<4>(fb,pictureRects[LogoLayer],eyeShift[eye],pictureStill.data()+logoAt,k);
 blendLayer<2>(fb,pictureRects[TextLayer],eyeShift[eye],pictureStill.data()+textAt,k);
 if(periods)blendLayer<2>(fb,pictureRects[PeriodLayer],eyeShift[eye],periodFrame(0),k);
}
// The periods' area: keep what lies under it, or put that back and draw a loop frame.
void savePeriodBase(uint8_t* fb,unsigned eye){
 const auto& r=pictureRects[PeriodLayer];auto& base=periodBase[eye];
 for(unsigned cx=0;cx<r.w;++cx){const int x=int(r.x)+int(cx)+eyeShift[eye];if(x<0||x>=int(pictureWidth))continue;
  std::memcpy(base.data()+size_t(3)*cx*r.h,fbColumn(fb,x,r),size_t(3)*r.h);}
}
void drawPeriods(uint8_t* fb,unsigned eye,unsigned frame){
 const auto& r=pictureRects[PeriodLayer];const auto& base=periodBase[eye];
 for(unsigned cx=0;cx<r.w;++cx){const int x=int(r.x)+int(cx)+eyeShift[eye];if(x<0||x>=int(pictureWidth))continue;
  std::memcpy(fbColumn(fb,x,r),base.data()+size_t(3)*cx*r.h,size_t(3)*r.h);}
 blendLayer<2>(fb,r,eyeShift[eye],periodFrame(frame),256);
 const int x0=std::max(0,int(r.x)+eyeShift[eye]),x1=std::min(int(pictureWidth),int(r.x)+int(r.w)+eyeShift[eye]);
 if(x1>x0)GSPGPU_FlushDataCache(fb+size_t(3)*x0*240,size_t(3)*(x1-x0)*240);
}
// Shows the buffers just drawn at the next refresh, and waits for it.
void presentPicture(){
 for(unsigned eye=0;eye<pictureEyes();++eye)pictureShown[eye]=topBack(eye);
 gfxScreenSwapBuffers(GFX_TOP,pictureStereo);gspWaitForVBlank();
 picturePresents.fetch_add(1,std::memory_order_relaxed);
}
void pictureTimer(void*){
 // Fade in from the black the menus left, one step per refresh.
 for(unsigned f=1;f<=PictureFadeFrames;++f){
  for(unsigned eye=0;eye<pictureEyes();++eye){uint8_t* fb=topBack(eye);composeStill(fb,eye,f*256/PictureFadeFrames,true);GSPGPU_FlushDataCache(fb,pictureBytes());}
  presentPicture();
 }
 // The other buffer gets the whole picture too; from then on only the
 // periods' area is redrawn, into the hidden buffer, one loop frame per refresh.
 for(unsigned eye=0;eye<pictureEyes();++eye){
  uint8_t* fb=topBack(eye);composeStill(fb,eye,256,false);savePeriodBase(fb,eye);
  blendLayer<2>(fb,pictureRects[PeriodLayer],eyeShift[eye],periodFrame(0),256);GSPGPU_FlushDataCache(fb,pictureBytes());
 }
 presentPicture();
 pictureStill.clear();
 const uint64_t start=svcGetSystemTick();
 while(!pictureStop.load(std::memory_order_acquire)){
  // The frame due at the next refresh, so the bounce keeps its tempo.
  const uint64_t w0=svcGetSystemTick();
  const unsigned frame=unsigned((w0-start)*loopFps/SYSCLOCK_ARM11+1)%loopFrames;
  for(unsigned eye=0;eye<pictureEyes();++eye)drawPeriods(topBack(eye),eye,frame);
  pictureWork.fetch_add(svcGetSystemTick()-w0,std::memory_order_relaxed);
  presentPicture();
 }
}
// The course reveal (tools/cook_loading_reveal.py), both screens while a race
// loads. The main thread reads its files and composes both stills; the timer
// thread fades them in and then redraws only what moves: the map pixels the pen
// has just passed (from a copy of the map's box), the pen, the gold edges and
// the wheel. It touches no file and no Citro3D state.
namespace reveal {
constexpr float Pen0=.3f,Draw=1.7f,Speed=52.f,RestFade=.5f; // the pen sets off at 0.3 s, closes the lap at 2.0 s, then laps at 52 px/s; the rest fades in 0.5 s
constexpr int EdgeY=222,E=4,MapX=226,MapY=4,WheelX=320-24-8,WheelY=240-24-8;
constexpr int FrameX=4,FrameW=312,FrameH=34;
constexpr unsigned Fade=10,WheelFrames=60,MaxShots=4;
constexpr std::array<int,4> RaceRows{{57,99,141,183}};
constexpr std::array<int,5> ArenaRows{{38,75,112,149,186}};
struct Image{unsigned w=0,h=0,ch=0;float capTop=0,capBottom=0;const uint8_t* px=nullptr;};
// An image block: u16 w, h, u8 channels, u8 0, i16 cap top, cap bottom (1/16 px), then rows.
const uint8_t* readImage(const uint8_t* p,const uint8_t* end,Image& im){
 if(!p||end-p<10)return nullptr;
 uint16_t w,h;int16_t top,bottom;std::memcpy(&w,p,2);std::memcpy(&h,p+2,2);std::memcpy(&top,p+6,2);std::memcpy(&bottom,p+8,2);
 im.w=w;im.h=h;im.ch=p[4];im.capTop=top/16.f;im.capBottom=bottom/16.f;
 const size_t n=size_t(w)*h*im.ch;
 if((im.ch!=2&&im.ch!=4)||size_t(end-p-10)<n)return nullptr;
 im.px=p+10;return p+10+n;
}
struct File{Bytes data;size_t size=0;const uint8_t* begin()const{return data.data();}const uint8_t* end()const{return data.data()+size;}};
enum class Read{Ok,Missing,NoMemory};
// Packed (menu.pak) or loose on the SD card: fopen serves both (asset_pack.hpp).
Read load(const char* path,File& f){
 FILE* in=std::fopen(path,"rb");if(!in)return Read::Missing;
 long n=-1;if(std::fseek(in,0,SEEK_END)==0)n=std::ftell(in);
 if(n<=0||std::fseek(in,0,SEEK_SET)!=0){std::fclose(in);return Read::Missing;}
 if(!f.data.alloc(size_t(n))){std::fclose(in);return Read::NoMemory;}
 const bool ok=std::fread(f.data.data(),1,size_t(n),in)==size_t(n);std::fclose(in);
 if(!ok){f.data.clear();return Read::Missing;}
 f.size=size_t(n);return Read::Ok;
}
// A framebuffer-order picture (columns of 240 BGR pixels, bottom row first).
struct Canvas{
 uint8_t* p=nullptr;int w=0;
 uint8_t* at(int x,int y)const{return p+3*(size_t(x)*240+size_t(239-y));}
 // A premultiplied colour over the pixel.
 void blend(int x,int y,unsigned r,unsigned g,unsigned b,unsigned a)const{
  if(!a||x<0||y<0||x>=w||y>=240)return;
  uint8_t* o=at(x,y);const unsigned k=255-a;
  o[0]=uint8_t(std::min(255u,b+(o[0]*k+127)/255));o[1]=uint8_t(std::min(255u,g+(o[1]*k+127)/255));o[2]=uint8_t(std::min(255u,r+(o[2]*k+127)/255));
 }
};
void blendInto(uint8_t* o,unsigned r,unsigned g,unsigned b,unsigned a){
 const unsigned k=255-a;
 o[0]=uint8_t(std::min(255u,b+(o[0]*k+127)/255));o[1]=uint8_t(std::min(255u,g+(o[1]*k+127)/255));o[2]=uint8_t(std::min(255u,r+(o[2]*k+127)/255));
}
// The background: the course's Standings colour shaded down the screen, 1 px
// diagonal lines running down to the right (x - y a multiple of `step`; user,
// Nov 27: the approved concept's lines, not horizontal ones) mixed toward its
// tint, and a +-0.5 level noise dither (interleaved gradient noise: every row
// and column has the same average), so the slow gradient shows no steps and no
// beat with the lines.
struct Backdrop{float row[240][3],line[240][3];int step=1;};
float noise(int x,int y){float v=.06711056f*float(x)+.00583715f*float(y);v-=std::floor(v);v*=52.9829189f;return v-std::floor(v);}
void makeBackdrop(Backdrop& b,const uint8_t colour[3],float top,float bottom,unsigned step,float k){
 for(unsigned y=0;y<240;++y)for(unsigned c=0;c<3;++c){
  const float v0=std::min(255.f,colour[c]*top),v1=std::min(255.f,colour[c]*bottom),tint=std::min(255.f,colour[c]*1.42f+14.f);
  const float v=v0+(v1-v0)*float(y)/239.f;
  b.row[y][c]=v;b.line[y][c]=v+(tint-v)*k;
 }
 b.step=int(step);
}
void backdropAt(const Backdrop& b,int x,int y,uint8_t bgr[3]){
 const float o=noise(x,y);const float* v=((x-y)%b.step+b.step)%b.step==0?b.line[y]:b.row[y];
 for(unsigned c=0;c<3;++c)bgr[2-c]=uint8_t(std::min(255.f,std::floor(v[c]+o)));
}
void fillBackdrop(const Canvas& c,const Backdrop& b){for(int x=0;x<c.w;++x)for(int y=0;y<240;++y)backdropAt(b,x,y,c.at(x,y));}
void drawImage(const Canvas& c,const Image& im,int x,int y){
 for(unsigned r=0;r<im.h;++r)for(unsigned q=0;q<im.w;++q){
  const uint8_t* p=im.px+size_t(im.ch)*(size_t(r)*im.w+q);
  if(im.ch==4)c.blend(x+int(q),y+int(r),p[0],p[1],p[2],p[3]);else c.blend(x+int(q),y+int(r),p[0],p[0],p[0],p[1]);
 }
}
// Lettering: the race font's white fill takes a colour from top to bottom of
// its capitals; the black edge stays black.
struct Fill{float top[3],bottom[3];};
void letter(const Canvas& c,const uint8_t* px,unsigned w,unsigned h,int x,int y,float capTop,float capBottom,const Fill& f){
 const float span=std::max(1.f,capBottom-capTop);
 for(unsigned r=0;r<h;++r){
  const float t=std::clamp((float(r)+.5f-capTop)/span,0.f,1.f);float col[3];
  for(unsigned k=0;k<3;++k)col[k]=f.top[k]+(f.bottom[k]-f.top[k])*t;
  for(unsigned q=0;q<w;++q){const uint8_t* p=px+2*(size_t(r)*w+q);if(!p[1])continue;const float g=p[0]/255.f;
   c.blend(x+int(q),y+int(r),unsigned(col[0]*g+.5f),unsigned(col[1]*g+.5f),unsigned(col[2]*g+.5f),p[1]);}
 }
}
// A cooked text picture with its capitals centred on row cy.
void drawText(const Canvas& c,const Image& im,int x,int cy,const Fill& f){
 letter(c,im.px,im.w,im.h,x,int(std::lround(float(cy)-(im.capTop+im.capBottom)*.5f)),im.capTop,im.capBottom,f);
}
// font-SS.bin: u16 cell px, i16 cap top, cap bottom (1/16 px), u16 glyphs; per
// glyph u16 code, i8 dx, u8 w, u16 advance (1/16 px), u32 offset; then pictures.
struct Font{File file;unsigned px=0,count=0;float capTop=0,capBottom=0;const uint8_t* table=nullptr;const uint8_t* data=nullptr;};
bool parseFont(Font& f){
 const uint8_t* p=f.file.begin();if(f.file.size<8)return false;
 uint16_t px,count;int16_t top,bottom;std::memcpy(&px,p,2);std::memcpy(&top,p+2,2);std::memcpy(&bottom,p+4,2);std::memcpy(&count,p+6,2);
 f.px=px;f.count=count;f.capTop=top/16.f;f.capBottom=bottom/16.f;f.table=p+8;f.data=f.table+10*size_t(count);
 if(f.data>f.file.end())return false;
 for(unsigned i=0;i<count;++i){uint32_t at;std::memcpy(&at,f.table+10*i+6,4);if(f.data+at+size_t(f.table[10*i+3])*px*2>f.file.end())return false;}
 return true;
}
const uint8_t* glyph(const Font& f,char c){
 for(unsigned i=0;i<f.count;++i){uint16_t code;std::memcpy(&code,f.table+10*i,2);if(code==uint8_t(c))return f.table+10*i;}
 return nullptr;
}
float advance(const uint8_t* g){uint16_t a;std::memcpy(&a,g+4,2);return a/16.f;}
float textWidth(const Font& f,const char* s,float spacing){float w=0;for(;*s;++s)if(const uint8_t* g=glyph(f,*s))w+=advance(g)+spacing;return w;}
void drawRun(const Canvas& c,const Font& f,const char* s,float x,int cy,float spacing,const Fill& fill){
 const int top=int(std::lround(float(cy)-(f.capTop+f.capBottom)*.5f));float pen=x+1;
 for(;*s;++s){const uint8_t* g=glyph(f,*s);if(!g)continue;
  uint32_t at;std::memcpy(&at,g+6,4);const unsigned w=g[3];
  if(w)letter(c,f.data+at,w,f.px,int(std::lround(pen))+int(int8_t(g[2])),top,f.capTop,f.capBottom,fill);
  pen+=advance(g)+spacing;}
}
struct MapPixel{uint8_t x,y,g,b,a,pad;uint16_t arc;};
struct PathPoint{float x,y,cum;};
struct Rect{
 int x0=1<<20,y0=1<<20,x1=-(1<<20),y1=-(1<<20);
 bool empty()const{return x0>=x1||y0>=y1;}
 void add(int x,int y){x0=std::min(x0,x);y0=std::min(y0,y);x1=std::max(x1,x+1);y1=std::max(y1,y+1);}
 void add(const Rect& r){if(r.empty())return;x0=std::min(x0,r.x0);y0=std::min(y0,r.y0);x1=std::max(x1,r.x1);y1=std::max(y1,r.y1);}
 void clip(int w,int h){x0=std::max(x0,0);y0=std::max(y0,0);x1=std::min(x1,w);y1=std::min(y1,h);}
};
struct State{
 File art,course,medal,faces;Font footer,times;
 bool stereo=false,wide=false,arena=false,mapOn=false;unsigned scale=1;std::array<int,2> shift{};
 Backdrop topBg,lowBg;
 Bytes topStill[2],lowStill,lowBase,edgeBase,layer,wheelCache;
 // art.bin
 float period=1,stripStart=0;unsigned phases=30;Image logo,strips[3],sides[60];
 // the map: its box on the top screen, pixels by reveal order, the pen's path
 int x0=0,y0=0,bw=0,bh=0;const MapPixel* pixels=nullptr;uint32_t count=0,firstRest=0,done=0;bool restDone=false;Rect restBox;
 const PathPoint* path=nullptr;uint32_t pathCount=0;float length=1;
 int frameY=-1;   // the lower screen's gold frame (its top row), or none
 uint64_t wheelReady=0;
 Thread thread=nullptr;std::atomic<bool> stop{false};
 std::atomic<unsigned> presents{0};std::atomic<uint64_t> workTicks{0},workMax{0};std::atomic<float> lastT{0};
 std::array<uint8_t*,2> topShown{};uint8_t* lowShown=nullptr;
 // DIAG: frames kept at the given times (loading-shot.txt), written by the main thread
 unsigned shots=0;std::atomic<unsigned> shotsTaken{0};float shotAt[MaxShots]{};Bytes shotTop[MaxShots],shotLow[MaxShots];
};
State* S=nullptr;
std::atomic<bool> ownsBottom{false}; // LoadingScope leaves the lower screen alone
unsigned topColumns(){return 400*S->scale;}
size_t topBytes(){return size_t(topColumns())*240*3;}
uint8_t* lowBack(){return gfxGetFramebuffer(GFX_BOTTOM,GFX_LEFT,nullptr,nullptr);}
float penAt(float t){
 const float T=t-Pen0;if(T<0)return -1;
 if(T<Draw){const float u=T/Draw;return Speed*T+(S->length-Speed*Draw)*u*u*(3-2*u);}
 return S->length+Speed*(T-Draw);
}
void pointAt(float s,float& x,float& y){
 const auto* P=S->path;const uint32_t n=S->pathCount;
 s=std::fmod(s,S->length);if(s<0)s+=S->length;
 uint32_t lo=0,hi=n-1;while(hi-lo>1){const uint32_t m=(lo+hi)/2;if(P[m].cum<=s)lo=m;else hi=m;}
 const float span=P[hi].cum-P[lo].cum,f=span>0?(s-P[lo].cum)/span:0;
 x=P[lo].x+(P[hi].x-P[lo].x)*f;y=P[lo].y+(P[hi].y-P[lo].y)*f;
}
uint8_t* layerAt(int x,int y){return S->layer.data()+3*(size_t(x)*S->bh+size_t(S->bh-1-y));}
// One map pixel at v/256 of its colour over the background, into the box copy.
void showPixel(const MapPixel& p,unsigned v){
 uint8_t bg[3];backdropAt(S->topBg,S->x0+p.x,S->y0+p.y,bg);
 const unsigned a=p.a*v>>8,g=p.g*v>>8,b=p.b*v>>8,k=255-a;uint8_t* o=layerAt(p.x,p.y);
 o[0]=uint8_t(std::min(255u,b+(bg[0]*k+127)/255));o[1]=uint8_t(std::min(255u,g+(bg[1]*k+127)/255));o[2]=uint8_t(std::min(255u,g+(bg[2]*k+127)/255));
}
// The pixels whose value changes at pen position s and time t; their box.
Rect advanceMap(float s,float t){
 Rect r;
 if(s>=0){
  const int32_t s16=int32_t(s*16);
  for(uint32_t i=S->done;i<S->firstRest&&int32_t(S->pixels[i].arc)<=s16;++i){
   const int32_t d=s16-int32_t(S->pixels[i].arc);showPixel(S->pixels[i],d>=32?256u:unsigned(d*8));r.add(S->pixels[i].x,S->pixels[i].y);}
  while(S->done<S->firstRest&&int32_t(S->pixels[S->done].arc)+32<=s16)++S->done;
 }
 const float u=(t-Pen0-Draw)/RestFade;
 if(u>0&&!S->restDone&&S->firstRest<S->count){
  const float k=std::min(1.f,u);const unsigned v=unsigned(256.f*k*k*(3-2*k));
  for(uint32_t i=S->firstRest;i<S->count;++i)showPixel(S->pixels[i],v);
  r.add(S->restBox);S->restDone=k>=1;
 }
 return r;
}
// The box copy's rows y0..y1 of columns x0..x1 (box px) into a top framebuffer.
void copyLayer(uint8_t* fb,Rect r){
 r.clip(S->bw,S->bh);if(r.empty())return;
 const size_t run=3*size_t(r.y1-r.y0);
 for(int x=r.x0;x<r.x1;++x){const uint8_t* from=layerAt(x,r.y1-1);
  for(unsigned c=0;c<S->scale;++c)std::memcpy(fb+3*(size_t((S->x0+x)*int(S->scale)+int(c))*240+size_t(239-(S->y0+r.y1-1))),from,run);}
}
void putTop(uint8_t* fb,int x,int y,const uint8_t bgr[3]){
 for(unsigned c=0;c<S->scale;++c)std::memcpy(fb+3*(size_t(x*int(S->scale)+int(c))*240+size_t(239-y)),bgr,3);
}
Rect penBox(float s){
 Rect r;if(s<0||!S->mapOn)return r;float x,y;pointAt(s,x,y);
 r.x0=int(std::floor(x-4.7f));r.y0=int(std::floor(y-4.7f));r.x1=int(std::ceil(x+4.7f))+1;r.y1=int(std::ceil(y+4.7f))+1;return r;
}
// The pen: a gold dot with a dark rim.
void drawPen(uint8_t* fb,float s){
 if(s<0)return;float px,py;pointAt(s,px,py);Rect r=penBox(s);r.clip(S->bw,S->bh);
 for(int x=r.x0;x<r.x1;++x)for(int y=r.y0;y<r.y1;++y){
  const float d=std::hypot(float(x)+.5f-px,float(y)+.5f-py);
  const float dark=std::clamp(4.1f-d,0.f,1.f)*.85f,gold=std::clamp(3.f-d,0.f,1.f);if(dark<=0)continue;
  uint8_t o[3];std::memcpy(o,layerAt(x,y),3);
  blendInto(o,unsigned(20*dark+.5f),unsigned(14*dark+.5f),0,unsigned(255*dark+.5f));
  if(gold>0)blendInto(o,unsigned(255*gold+.5f),unsigned(206*gold+.5f),unsigned(32*gold+.5f),unsigned(255*gold+.5f));
  putTop(fb,S->x0+x,S->y0+y,o);
 }
}
// A long gold edge along a row: its place in the loop period in 1/256 px, a
// whole px a step, its sample (4 per px) without a division per pixel.
struct StripWalk{
 uint32_t u,period;
 StripWalk(float start){period=uint32_t(S->period*256.f);float v=std::fmod(start,S->period);if(v<0)v+=S->period;u=std::min<uint32_t>(period-1,uint32_t(v*256.f));}
 const uint8_t* at(const Image& s,unsigned row)const{return s.px+4*(size_t(row)*s.w+std::min<uint32_t>(s.w-1,u>>6));}
 void step(){u+=256;if(u>=period)u-=period;}
};
float phaseStart(unsigned phase){return std::fmod(S->stripStart+float(phase)*S->period/float(S->phases),S->period);}
// The top screen's bottom edge: the info box's top edge across the screen.
void drawEdge(const Canvas& c,const uint8_t* base,unsigned phase){
 const Image& s=S->strips[0];StripWalk walk(phaseStart(phase));
 for(int x=0;x<400;++x,walk.step())for(int r=0;r<E;++r){
  uint8_t o[3];std::memcpy(o,base+3*(size_t(x)*E+r),3);const uint8_t* p=walk.at(s,r);blendInto(o,p[0],p[1],p[2],p[3]);
  if(c.w==400)std::memcpy(c.at(x,EdgeY+r),o,3);else putTop(c.p,x,EdgeY+r,o);
 }
}
// The lower screen's gold frame (its long edges, its gold line, its short edges) over the still without it.
void drawFrame(const Canvas& c,const uint8_t* base,unsigned phase){
 if(S->frameY<0)return;
 const int oy=S->frameY;const float u0=phaseStart(phase);
 auto put=[&](int x,int y,const uint8_t* p){if(y<0||y>=240)return;uint8_t* o=c.at(x,y);std::memcpy(o,base+3*(size_t(x)*240+size_t(239-y)),3);blendInto(o,p[0],p[1],p[2],p[3]);};
 const int rows[3][2]={{oy,E},{oy+FrameH-E-3,3},{oy+FrameH-E,E}};
 for(unsigned k=0;k<3;++k){StripWalk walk(u0);for(int x=FrameX+E;x<FrameX+FrameW-E;++x,walk.step())for(int r=0;r<rows[k][1];++r)put(x,rows[k][0]+r,walk.at(S->strips[k],r));}
 for(unsigned right=0;right<2;++right){const Image& s=S->sides[std::min(phase,S->phases-1)*2+right];const int x0=right?FrameX+FrameW-E:FrameX;
  for(unsigned y=0;y<s.h&&int(y)<FrameH;++y)for(unsigned x=0;x<s.w;++x)put(x0+int(x),oy+int(y),s.px+4*(size_t(y)*s.w+x));}
}
// The wheel (loading_wheel_pixels.hpp, as LoadingScope draws it), one turn a second; its 60 frames kept once drawn.
void drawWheel(const Canvas& c,const uint8_t* base,unsigned frame){
 frame%=WheelFrames;uint8_t* cache=S->wheelCache.data()+size_t(frame)*24*24*2;
 if(!(S->wheelReady>>frame&1)){
  for(unsigned y=0;y<24;++y)for(unsigned x=0;x<24;++x){uint8_t p[4];loadingWheelPixel(float(x),float(y),24.f,float(frame)*6.283185307f/WheelFrames,p);cache[2*(y*24+x)]=p[0];cache[2*(y*24+x)+1]=p[3];}
  S->wheelReady|=uint64_t(1)<<frame;
 }
 for(unsigned y=0;y<24;++y)for(unsigned x=0;x<24;++x){
  const int X=WheelX+int(x),Y=WheelY+int(y);uint8_t* o=c.at(X,Y);const uint8_t* b=base+3*(size_t(X)*240+size_t(239-Y));const uint8_t* p=cache+2*(y*24+x);
  for(unsigned k=0;k<3;++k)o[k]=loadingWheelBlend(p[0],p[1],b[k]);
 }
}
void scaleTop(uint8_t* fb,const uint8_t* still,unsigned k){
 for(unsigned x=0;x<400;++x){const uint8_t* from=still+size_t(3)*240*x;
  for(unsigned c=0;c<S->scale;++c){uint8_t* to=fb+size_t(3)*240*(x*S->scale+c);
   if(k>=256)std::memcpy(to,from,3*240);else for(unsigned i=0;i<3*240;++i)to[i]=uint8_t((from[i]*k)>>8);}}
}
void scaleLow(uint8_t* fb,const uint8_t* still,unsigned k){
 constexpr size_t n=320*240*3;if(k>=256)std::memcpy(fb,still,n);else for(size_t i=0;i<n;++i)fb[i]=uint8_t((still[i]*k)>>8);
}
void present(){
 for(unsigned eye=0;eye<(S->stereo?2u:1u);++eye)S->topShown[eye]=topBack(eye);
 S->lowShown=lowBack();
 gfxScreenSwapBuffers(GFX_TOP,S->stereo);gfxScreenSwapBuffers(GFX_BOTTOM,false);gspWaitForVBlank();
 S->presents.fetch_add(1,std::memory_order_relaxed);
}
void timer(void*){
 const uint64_t start=svcGetSystemTick();const unsigned eyes=S->stereo?2u:1u;
 // Fade in from the black the menus left, one step per refresh; then the other buffers get the whole picture.
 for(unsigned f=1;f<=Fade+1;++f){
  const unsigned k=std::min(256u,f*256/Fade);
  for(unsigned eye=0;eye<eyes;++eye){uint8_t* fb=topBack(eye);scaleTop(fb,S->topStill[eye].data(),k);GSPGPU_FlushDataCache(fb,topBytes());}
  uint8_t* low=lowBack();scaleLow(low,S->lowStill.data(),k);GSPGPU_FlushDataCache(low,320*240*3);
  present();
 }
 for(auto& still:S->topStill)still.clear();
 S->lowStill.clear();
 Rect last;Rect pens[2];unsigned shot=0;
 while(!S->stop.load(std::memory_order_acquire)){
  const uint64_t w0=svcGetSystemTick();
  // The state due at the next refresh.
  const float t=float(double(w0-start)/SYSCLOCK_ARM11)+1.f/60.f;
  const unsigned phase=unsigned(t*30.f)%S->phases,wheel=unsigned(t*60.f);
  float s=-1;Rect copy;
  if(S->mapOn){
   s=penAt(t);const Rect changed=advanceMap(s,t);
   // This buffer last showed the state of two refreshes ago.
   copy=changed;copy.add(last);copy.add(pens[1]);const Rect pen=penBox(s);copy.add(pen);
   last=changed;pens[1]=pens[0];pens[0]=pen;
  }
  for(unsigned eye=0;eye<eyes;++eye){
   uint8_t* fb=topBack(eye);
   if(S->mapOn){copyLayer(fb,copy);drawPen(fb,s);}
   drawEdge(Canvas{fb,int(topColumns())},S->edgeBase.data(),phase);
   GSPGPU_FlushDataCache(fb,topBytes());
  }
  uint8_t* low=lowBack();const Canvas lc{low,320};
  drawFrame(lc,S->lowBase.data(),phase);drawWheel(lc,S->lowBase.data(),wheel);
  GSPGPU_FlushDataCache(low,320*240*3);
  const uint64_t work=svcGetSystemTick()-w0;S->workTicks.fetch_add(work,std::memory_order_relaxed);
  if(work>S->workMax.load(std::memory_order_relaxed))S->workMax.store(work,std::memory_order_relaxed);
  S->lastT.store(t,std::memory_order_relaxed);
  present();
  if(shot<S->shots&&t>=S->shotAt[shot]){
   if(S->shotTop[shot].data())std::memcpy(S->shotTop[shot].data(),S->topShown[0],topBytes());
   if(S->shotLow[shot].data())std::memcpy(S->shotLow[shot].data(),S->lowShown,320*240*3);
   S->shotsTaken.store(++shot,std::memory_order_release);
  }
 }
}
void writePpm(const char* path,const uint8_t* fb,unsigned w){
 FILE* f=std::fopen(path,"wb");if(!f)return;
 std::fprintf(f,"P6\n%u 240\n255\n",w);std::vector<uint8_t> row(size_t(w)*3);
 for(unsigned y=0;y<240;++y){for(unsigned x=0;x<w;++x){const uint8_t* p=fb+3*(x*240+(239-y));row[3*x]=p[2];row[3*x+1]=p[1];row[3*x+2]=p[0];}std::fwrite(row.data(),1,row.size(),f);}
 std::fclose(f);
}
enum class Start{Started,Missing,Skipped};
// Reads the files and composes both stills (main thread), then starts the timer thread.
Start start(const RaceLoadingInfo& info,Renderer& renderer){
 if(info.course>=42)return Start::Missing;
 std::unique_ptr<State> state(new(std::nothrow) State);
 if(!state){pictureLog("loading reveal: FAIL no memory for its state, skipped");return Start::Skipped;}
 S=state.get();auto fail=[&](Start why){S=nullptr;return why;};
 char path[48];
 Read r=Read::Ok;
 auto need=[&](const char* p,File& f){if(r==Read::Ok)r=load(p,f);return r==Read::Ok;};
 std::snprintf(path,sizeof path,"loading/reveal/course-%02u.bin",info.course);
 need(path,S->course);need("loading/reveal/art.bin",S->art);
 need("loading/reveal/font-30.bin",S->footer.file);
 if(info.mode==2&&info.records){need("loading/reveal/font-44.bin",S->times.file);need("loading/reveal/faces.bin",S->faces);}
 const uint8_t* c=S->course.begin();
 if(r==Read::Ok&&(S->course.size<12||std::memcmp(c,"MKRC",4)))r=Read::Missing;
 const unsigned cup=r==Read::Ok?c[5]:0;
 if(r==Read::Ok){std::snprintf(path,sizeof path,"loading/reveal/medal-%02u.bin",cup);need(path,S->medal);}
 if(r!=Read::Ok){
  pictureLog(r==Read::NoMemory?"loading reveal: FAIL no memory for its files, skipped":"loading reveal: files missing (%s)",path);
  return fail(r==Read::NoMemory?Start::Skipped:Start::Missing);
 }
 // The course: kind, cup, slot, members, colour; its name, cup name and member names; a race's map.
 const unsigned kind=c[4],slot=c[6],members=c[7];const uint8_t colour[3]={c[8],c[9],c[10]};
 S->arena=kind!=0;
 Image name,cupName,member[5],medal;const uint8_t* p=c+12;const uint8_t* end=S->course.end();
 p=readImage(p,end,name);p=readImage(p,end,cupName);
 for(unsigned i=0;i<members&&i<5;++i)p=readImage(p,end,member[i]);
 bool ok=p&&members<=5&&slot<members&&readImage(S->medal.begin(),S->medal.end(),medal)&&medal.ch==4;
 if(ok&&!S->arena){
  p=c+((size_t(p-c)+3)&~size_t(3)); // the map starts on a whole word
  ok=p<=end&&size_t(end-p)>=36;
  if(ok){
   uint16_t box[4],rest[4];uint32_t n[2];float lengths[2];
   std::memcpy(box,p,8);std::memcpy(n,p+8,8);std::memcpy(rest,p+16,8);std::memcpy(&S->pathCount,p+24,4);std::memcpy(lengths,p+28,8);p+=36;
   S->bw=box[2];S->bh=box[3];S->x0=MapX+box[0];S->y0=MapY+box[1];S->count=n[0];S->firstRest=n[1];S->length=lengths[0];
   S->restBox.x0=rest[0];S->restBox.y0=rest[1];S->restBox.x1=rest[0]+rest[2];S->restBox.y1=rest[1]+rest[3];
   ok=S->bw>0&&S->bh>0&&S->x0+S->bw<=400&&S->y0+S->bh<=EdgeY&&S->firstRest<=S->count&&S->pathCount>=2&&S->length>0&&
      size_t(end-p)==size_t(S->count)*sizeof(MapPixel)+size_t(S->pathCount)*sizeof(PathPoint);
   if(ok){
    auto* px=reinterpret_cast<MapPixel*>(const_cast<uint8_t*>(p));auto* pts=reinterpret_cast<PathPoint*>(const_cast<uint8_t*>(p)+size_t(S->count)*sizeof(MapPixel));
    for(uint32_t i=0;i<S->count&&ok;++i)ok=px[i].x<S->bw&&px[i].y<S->bh;
    // Mirror races: the map as the Standings shows it, mirrored in its box.
    if(ok&&info.engine==3){
     for(uint32_t i=0;i<S->count;++i)px[i].x=uint8_t(S->bw-1-px[i].x);
     for(uint32_t i=0;i<S->pathCount;++i)pts[i].x=float(S->bw)-pts[i].x;
     const int x0=S->bw-S->restBox.x1;S->restBox.x1=S->bw-S->restBox.x0;S->restBox.x0=x0;
    }
    S->pixels=px;S->path=pts;S->mapOn=true;
   }
  }
 }
 // art.bin: "MKRA", period, strip start, phases; the logo, three strips, the short edges per phase.
 const uint8_t* a=S->art.begin();
 ok=ok&&S->art.size>16&&!std::memcmp(a,"MKRA",4);
 if(ok){uint16_t phases;std::memcpy(&S->period,a+4,4);std::memcpy(&S->stripStart,a+8,4);std::memcpy(&phases,a+12,2);S->phases=phases;ok=phases>=1&&phases<=30&&S->period>1;}
 const uint8_t* q=ok?a+16:nullptr;
 q=readImage(q,S->art.end(),S->logo);
 for(auto& s:S->strips)q=readImage(q,S->art.end(),s);
 for(unsigned i=0;q&&i<2*S->phases;++i)q=readImage(q,S->art.end(),S->sides[i]);
 ok=ok&&q&&parseFont(S->footer)&&(!(info.mode==2&&info.records)||(parseFont(S->times)&&S->faces.size==24*(10+24*24*4)));
 for(const auto& s:S->strips)ok=ok&&s.ch==4&&s.w>=unsigned(S->period*4)&&s.h<=unsigned(E);
 if(!ok){pictureLog("loading reveal: FAIL files for course %u are not as expected",info.course);return fail(Start::Missing);}
 // Memory: both stills (one top still per eye), the lower picture under its moving parts, the map box, the wheel's frames.
 S->wide=gfxIsWide();S->scale=S->wide?2:1;S->stereo=!S->wide&&gfxIs3D();
 const size_t top=size_t(400)*240*3,low=size_t(320)*240*3;
 bool mem=S->topStill[0].alloc(top)&&(!S->stereo||S->topStill[1].alloc(top))&&S->lowStill.alloc(low)&&S->lowBase.alloc(low)&&
          S->edgeBase.alloc(400*E*3)&&S->wheelCache.alloc(WheelFrames*24*24*2)&&(!S->mapOn||S->layer.alloc(size_t(S->bw)*S->bh*3));
#ifndef MKW_RELEASE
 // DIAG loading-shot.txt: up to four times (s) at which the screens are kept, besides the last frame.
 if(mem)if(FILE* f=std::fopen("sdmc:/3ds/MKWiiDS/loading-shot.txt","rb")){
  float at;while(S->shots<MaxShots&&std::fscanf(f,"%f",&at)==1){if(!S->shotTop[S->shots].alloc(top*S->scale)||!S->shotLow[S->shots].alloc(low))break;S->shotAt[S->shots++]=at;}
  std::fclose(f);
 }
#endif
 const struct mallinfo heap=mallinfo();
 const size_t bytes=S->course.size+S->art.size+S->medal.size+S->faces.size+S->footer.file.size+S->times.file.size+top*(S->stereo?2:1)+2*low+400*E*3+WheelFrames*24*24*2+size_t(S->bw)*S->bh*3;
 pictureLog("loading reveal: course %u mode %u, %u KB, heap used %lu KB free %lu KB",info.course,info.mode,unsigned(bytes/1024),(unsigned long)heap.uordblks/1024,(unsigned long)heap.fordblks/1024);
 if(!mem){pictureLog("loading reveal: FAIL no memory for its %u KB, skipped",unsigned(bytes/1024));return fail(Start::Skipped);}

 // The top still: the background, the info box's see-through inside under the
 // bottom edge with the race's line in it (screen plane); the logo, medallion,
 // cup and name at the race HUD's layer (3D: out of the screen).
 const float tint[3]={std::min(255.f,colour[0]*1.42f+14),std::min(255.f,colour[1]*1.42f+14),std::min(255.f,colour[2]*1.42f+14)};
 makeBackdrop(S->topBg,colour,1.f,.76f,6,.32f);makeBackdrop(S->lowBg,colour,.5f,.38f,4,.16f);
 const Canvas t0{S->topStill[0].data(),400};
 fillBackdrop(t0,S->topBg);
 for(int x=0;x<400;++x)for(int y=EdgeY+E;y<240;++y)t0.blend(x,y,6,8,9,160);
 for(int x=0;x<400;++x)for(int y=0;y<E;++y)std::memcpy(S->edgeBase.data()+3*(size_t(x)*E+y),t0.at(x,EdgeY+y),3);
 {
  static constexpr const char* classes[4]={"50cc","100cc","150cc","Mirror"};
  static constexpr const char* modes[3]={"Grand Prix","VS","Time Trial"};
  char left[40],right[40];right[0]=0;
  if(info.mode==3)std::snprintf(left,sizeof left,"%s",info.coinRunners?"Coin Runners":"Balloon Battle");
  else std::snprintf(left,sizeof left,"%s %s",modes[std::min(info.mode,2u)],classes[std::min(info.engine,3u)]);
  if(info.mode==3)std::snprintf(right,sizeof right,"Battle %u",info.race);
  else if(info.mode==2){for(unsigned i=0;i<info.records;++i)if(info.record[i].mine){const unsigned ms=info.record[i].ms;
   std::snprintf(right,sizeof right,"Best %u:%02u.%03u",ms/60000,ms/1000%60,ms%1000);}}
  else std::snprintf(right,sizeof right,"Race %u/%u",info.race,info.races);
  const Fill credit{{235,225,170},{206,196,146}};constexpr int cy=EdgeY+E+(240-EdgeY-E)/2;
  drawRun(t0,S->footer,left,12,cy,.3f,credit);
  if(right[0])drawRun(t0,S->footer,right,387.f-textWidth(S->footer,right,.3f)-1,cy,.3f,credit);
 }
 if(S->stereo){
  std::memcpy(S->topStill[1].data(),S->topStill[0].data(),top);
  const float parallax=renderer.stereoLayer(stereo::RaceHudLayer);
  S->shift[0]=int(std::lround(-.5f*parallax));S->shift[1]=S->shift[0]+int(std::lround(parallax));
 }
 const Fill selected{{255,255,255},{214,206,185}},others{{150,140,128},{138,128,116}};
 Fill light;for(unsigned k=0;k<3;++k){light.top[k]=tint[k]+(255-tint[k])*.7f;light.bottom[k]=light.top[k]*.86f;}
 for(unsigned eye=0;eye<(S->stereo?2u:1u);++eye){
  const Canvas t{S->topStill[eye].data(),400};const int d=S->shift[eye];
  drawImage(t,S->logo,12+d,6);
  if(S->arena){
   drawImage(t,medal,308-int(medal.w)/2+d,122-int(medal.h)/2);
   drawText(t,cupName,13+d,97,light);drawText(t,name,13+d,125,selected);
  }else{
   drawImage(t,medal,14+d,72);
   drawText(t,cupName,85+d,104,light);drawText(t,name,13+d,164,selected);
  }
  drawEdge(t,S->edgeBase.data(),0);
 }
 if(S->mapOn)for(int x=0;x<S->bw;++x)for(int y=0;y<S->bh;++y)backdropAt(S->topBg,S->x0+x,S->y0+y,layerAt(x,y));
 // The lower still: the cup's courses (the coming one in the info box's frame),
 // or in Time Trials the course's records with the license's best framed.
 const Canvas l{S->lowBase.data(),320};
 fillBackdrop(l,S->lowBg);
 auto inside=[&](int cy){S->frameY=cy-FrameH/2;for(int x=FrameX+E;x<FrameX+FrameW-E;++x)for(int y=S->frameY+E;y<S->frameY+FrameH-E-3;++y)l.blend(x,y,6,8,9,160);};
 if(info.mode==2&&info.records){
  for(unsigned i=0;i<info.records&&i<4;++i){
   const auto& rec=info.record[i];const int cy=RaceRows[i];
   if(rec.mine)inside(cy);
   if(rec.character<24){Image face;if(readImage(S->faces.begin()+rec.character*(10+24*24*4),S->faces.end(),face))drawImage(l,face,15,cy-1-12);}
   char time[16];std::snprintf(time,sizeof time,"%u:%02u.%03u",rec.ms/60000,rec.ms/1000%60,rec.ms%1000);
   drawRun(l,S->times,time,46,cy-1,.4f,rec.mine?selected:others);
  }
 }else{
  for(unsigned i=0;i<members&&i<5;++i){
   const int cy=members>4?ArenaRows[i]:RaceRows[i];
   if(i==slot)inside(cy);
   drawText(l,member[i],17,cy-1,i==slot?selected:others);
  }
 }
 std::memcpy(S->lowStill.data(),S->lowBase.data(),low);
 const Canvas ls{S->lowStill.data(),320};drawFrame(ls,S->lowBase.data(),0);drawWheel(ls,S->lowBase.data(),0);
 // Nothing may draw either screen while the timer thread owns them; the lower
 // screen's wheel (LoadingScope) stops here and stays off until the end.
 renderer.waitForGpu();
 ownsBottom.store(true);
 if(spinner){stop.store(true,std::memory_order_release);threadJoin(spinner,U64_MAX);threadFree(spinner);spinner=nullptr;}
 shown=false;++scopes;
 s32 priority=0x30;svcGetThreadPriority(&priority,CUR_THREAD_HANDLE);
 S->thread=threadCreate(timer,nullptr,16*1024,std::max<int>(priority-1,0x18),-2,false);
 if(!S->thread){ownsBottom.store(false);pictureLog("loading reveal: FAIL thread not created");return fail(Start::Skipped);}
 pictureLog("loading reveal: shown (%s, %s%s, map %s %u px, path %.0f px)",S->wide?"800px":"400px",S->stereo?"3D":"2D",info.engine==3?", mirrored":"",
  S->mapOn?"on":"off",unsigned(S->count),double(S->length));
 state.release();return Start::Started;
}
void end(){
 if(!S)return;
 S->stop.store(true,std::memory_order_release);
 threadJoin(S->thread,U64_MAX);threadFree(S->thread);S->thread=nullptr;
 const unsigned presents=S->presents.load(),loops=presents>Fade+1?presents-Fade-1:0;
 pictureLog("loading reveal: load done after %u refreshes (%.2f s), work per refresh %.0f us average %.0f us most, map %u of %u px",presents,double(S->lastT.load()),
  loops?double(S->workTicks.load())*1e6/SYSCLOCK_ARM11/loops:0.,double(S->workMax.load())*1e6/SYSCLOCK_ARM11,unsigned(S->done),unsigned(S->count));
 const unsigned eyes=S->stereo?2u:1u;
#ifndef MKW_RELEASE
 // DIAG loading-shot.txt: the screens as shown before the fade out (loading-reveal-NN-top.ppm,
 // -right.ppm in 3D, -bottom.ppm), and at the times the file lists (-tN.NN-).
 if(FILE* probe=std::fopen("sdmc:/3ds/MKWiiDS/loading-shot.txt","rb")){
  std::fclose(probe);static unsigned shot=0;char path[80];
  for(unsigned i=0;i<S->shotsTaken.load(std::memory_order_acquire);++i){
   std::snprintf(path,sizeof path,"sdmc:/3ds/MKWiiDS/loading-reveal-%02u-t%.2f-top.ppm",shot,double(S->shotAt[i]));writePpm(path,S->shotTop[i].data(),topColumns());
   std::snprintf(path,sizeof path,"sdmc:/3ds/MKWiiDS/loading-reveal-%02u-t%.2f-bottom.ppm",shot,double(S->shotAt[i]));writePpm(path,S->shotLow[i].data(),320);
  }
  for(unsigned eye=0;eye<eyes;++eye)if(S->topShown[eye]){std::snprintf(path,sizeof path,"sdmc:/3ds/MKWiiDS/loading-reveal-%02u-%s.ppm",shot,eye?"right":"top");writePpm(path,S->topShown[eye],topColumns());}
  if(S->lowShown){std::snprintf(path,sizeof path,"sdmc:/3ds/MKWiiDS/loading-reveal-%02u-bottom.ppm",shot);writePpm(path,S->lowShown,320);}
  ++shot;
 }
#endif
 // Fade both screens out to black, one step per refresh (each drawn into the
 // hidden buffer from the one on screen), then leave every buffer black.
 const size_t bytes=topBytes(),low=320*240*3;
 for(unsigned f=1;f<=Fade;++f){
  const unsigned left=Fade-f,k=left*256/(left+1);
  for(unsigned eye=0;eye<eyes;++eye){uint8_t* to=topBack(eye);const uint8_t* from=S->topShown[eye];
   if(!left||!from)std::memset(to,0,bytes);else for(size_t i=0;i<bytes;++i)to[i]=uint8_t((from[i]*k)>>8);GSPGPU_FlushDataCache(to,bytes);}
  uint8_t* to=lowBack();const uint8_t* from=S->lowShown;
  if(!left||!from)std::memset(to,0,low);else for(size_t i=0;i<low;++i)to[i]=uint8_t((from[i]*k)>>8);GSPGPU_FlushDataCache(to,low);
  present();
 }
 for(unsigned eye=0;eye<eyes;++eye){uint8_t* fb=topBack(eye);std::memset(fb,0,bytes);GSPGPU_FlushDataCache(fb,bytes);}
 {uint8_t* fb=lowBack();std::memset(fb,0,low);GSPGPU_FlushDataCache(fb,low);}
 ++scopes;ownsBottom.store(false);
 delete S;S=nullptr;
 pictureLog("loading reveal: closed");
}
}
float angle(uint64_t start){return float(double(svcGetSystemTick()-start)/SYSCLOCK_ARM11*6.283185307);}
void raster(){
 for(unsigned y=0;y<Size;++y)for(unsigned x=0;x<Size;++x)
  loadingWheelPixel(float(x),float(y),float(Size),0,&pixels[(y*Size+x)*4]);
}
void cpuFrame(float a){
 const unsigned x0=width-Visible-8,y0=240-Visible-8;
 for(unsigned x=0;x<Visible;++x)for(unsigned y=0;y<Visible;++y){
  uint8_t p[4];loadingWheelPixel(float(x),float(y),float(Visible),a,p);
  const unsigned at=3*((x0+x)*240+(239-y0-y));auto* out=framebuffer+at;
  for(unsigned k=0;k<3;++k)out[k]=loadingWheelBlend(p[2-k],p[3],background[at+k]);
 }
 GSPGPU_FlushDataCache(framebuffer+3*x0*240,3*Visible*240);
}
void timer(void*){
 // Avoid flashing a loading screen for an operation completing immediately.
 while(!stop.load(std::memory_order_acquire)){
  if(svcGetSystemTick()-started>SYSCLOCK_ARM11/2){
   if(!shown){std::memcpy(framebuffer,background.data(),width*240*3);GSPGPU_FlushDataCache(framebuffer,width*240*3);}
   const uint64_t w0=svcGetSystemTick();cpuFrame(angle(started));
   wheelWork.fetch_add(svcGetSystemTick()-w0,std::memory_order_relaxed);wheelDraws.fetch_add(1,std::memory_order_relaxed);
   if(!shown){gfxScreenSwapBuffers(GFX_BOTTOM,false);shown=true;}
  }
  svcSleepThread(16666667);
 }
}
}
void initializeLoadingScreen(Renderer& renderer){
 renderer_=&renderer;raster();wheel=std::make_unique<Texture>();
 if(!wheel->initRgba8(Size,Size,pixels.data(),pixels.size()))wheel.reset();started=svcGetSystemTick();
}
void shutdownLoadingScreen(){
 stop.store(true);if(spinner){threadJoin(spinner,U64_MAX);threadFree(spinner);spinner=nullptr;}
 wheel.reset();renderer_=nullptr;depth=0;
}
LoadingScope::LoadingScope(){
 if(!renderer_)return;
 ++depth;owner_=true;if(depth!=1)return;
 ++scopes;
 // The race loading screen draws the lower screen itself (its own wheel).
 if(reveal::ownsBottom.load()){shown=false;return;}
 renderer_->waitForGpu();
 u16 w=0,h=0;framebuffer=gfxGetFramebuffer(GFX_BOTTOM,GFX_LEFT,&w,&h);
 width=h; // libctru returns the rotated lower LCD buffer (240 x 320).
 if(!framebuffer||w!=240||h!=320)return;
 if(!renderer_->copyPresentedBottom(background.data()))background.fill(0);
 started=svcGetSystemTick();shown=false;stop.store(false);
 s32 priority=0x30;svcGetThreadPriority(&priority,CUR_THREAD_HANDLE);
 spinner=threadCreate(timer,nullptr,8*1024,std::max<int>(priority-1,0x18),-2,false);
}
LoadingScope::~LoadingScope(){
 if(!owner_||!depth||--depth)return;
 stop.store(true,std::memory_order_release);
 if(spinner){threadJoin(spinner,U64_MAX);threadFree(spinner);spinner=nullptr;}
 // Retire the software icon even when the next scene does not redraw this LCD.
 if(shown&&framebuffer){
  const unsigned x0=width-Visible-8,y0=240-Visible-8;
  for(unsigned x=0;x<Visible;++x)for(unsigned y=0;y<Visible;++y){
   const unsigned at=3*((x0+x)*240+(239-y0-y));
   std::memcpy(framebuffer+at,background.data()+at,3);
  }
  GSPGPU_FlushDataCache(framebuffer+3*x0*240,3*Visible*240);
 }
 shown=false;framebuffer=nullptr;
}
unsigned loadingScopes(){return scopes;}
bool loadingScreenOwnsBottom(){return reveal::ownsBottom.load();}
RaceLoadingPicture::RaceLoadingPicture(const RaceLoadingInfo* info){
 if(!renderer_||pictureThread||reveal::S)return;
#ifndef MKW_RELEASE
 // DIAG no-loading-picture.txt: races load behind the black screen, as before 1.2.0.
 if(FILE* off=std::fopen("sdmc:/3ds/MKWiiDS/no-loading-picture.txt","rb")){std::fclose(off);pictureLog("loading picture: off (no-loading-picture.txt)");return;}
#endif
 // The course reveal, New 3DS only (Old 3DS keeps the loading picture). Short
 // of memory it is skipped (black); without its files the picture shows.
 if(info&&isNew3ds()){
  bool on=true;
#ifndef MKW_RELEASE
  // DIAG no-loading-reveal.txt: the loading picture instead (A/B).
  if(FILE* off=std::fopen("sdmc:/3ds/MKWiiDS/no-loading-reveal.txt","rb")){std::fclose(off);on=false;pictureLog("loading reveal: off (no-loading-reveal.txt)");}
#endif
  if(on)switch(reveal::start(*info,*renderer_)){
   case reveal::Start::Started:active_=reveal_=true;return;
   case reveal::Start::Skipped:return;
   case reveal::Start::Missing:break;
  }
 }
 // The display mode the menus left: 800px Mode shows 800 columns, 3D two eyes.
 const bool wide=gfxIsWide();
 const char* path=wide?"loading/race-800.bin":"loading/race-400.bin";
 const long size=pack::size(path);
 uint8_t head[36];if(size<36||!pack::readRange(path,0,head,sizeof head)||std::memcmp(head,"MKLQ",4))return;
 uint16_t value[4];std::memcpy(value,head+4,8);std::memcpy(pictureRects.data(),head+12,24);
 pictureWidth=value[0];loopFrames=value[1];loopFps=value[2];
 if(pictureWidth!=(wide?800u:400u)||!loopFrames||!loopFps)return;
 for(const auto& r:pictureRects)if(!r.w||!r.h||r.x+r.w>pictureWidth||r.y+r.h>240)return;
 const auto& logo=pictureRects[LogoLayer];const auto& text=pictureRects[TextLayer];const auto& area=pictureRects[PeriodLayer];
 logoAt=pictureBytes();textAt=logoAt+size_t(logo.w)*logo.h*4;
 const size_t still=textAt+size_t(text.w)*text.h*2,loop=size_t(loopFrames)*area.w*area.h*2;
 if(36+still+loop!=size_t(size)){pictureLog("loading picture: %s size %ld does not match its header",path,size);return;}
 auto release=[](){pictureStill.clear();periodLoop.clear();for(auto& base:periodBase)base.clear();};
 const size_t baseBytes=size_t(area.w)*area.h*3;
 const struct mallinfo heap=mallinfo();
 pictureLog("loading picture: %s still %u KB loop %u KB, heap used %lu KB free %lu KB unclaimed %lu KB",path,unsigned(still/1024),unsigned(loop/1024),
  (unsigned long)heap.uordblks/1024,(unsigned long)heap.fordblks/1024,(unsigned long)(envGetHeapSize()>size_t(heap.arena)?envGetHeapSize()-size_t(heap.arena):0)/1024);
 bool ok=pictureStill.alloc(still)&&periodLoop.alloc(loop);
 for(auto& base:periodBase)if(ok){ok=base.alloc(baseBytes);if(ok)std::memset(base.data(),0,baseBytes);}
 if(!ok){pictureLog("loading picture: FAIL no memory for its %u KB, skipped",unsigned((still+loop+2*baseBytes)/1024));release();return;}
 if(!pack::readRange(path,36,pictureStill.data(),still)||!pack::readRange(path,36+still,periodLoop.data(),loop)){pictureLog("loading picture: FAIL read %s",path);release();return;}
 // 3D: the logo and LOADING... come out of the screen with the race HUD
 // (as Renderer's screen quads: left eye -parallax/2, right eye +parallax/2).
 pictureStereo=!wide&&gfxIs3D();eyeShift={};
 if(pictureStereo){const float parallax=renderer_->stereoLayer(stereo::RaceHudLayer);
  eyeShift[0]=int(std::lround(-.5f*parallax));eyeShift[1]=eyeShift[0]+int(std::lround(parallax));}
 // Nothing may draw the top screen while the timer thread owns it.
 renderer_->waitForGpu();
 pictureShown={};pictureStop.store(false);picturePresents.store(0);pictureWork.store(0);wheelWork.store(0);wheelDraws.store(0);
 s32 priority=0x30;svcGetThreadPriority(&priority,CUR_THREAD_HANDLE);
 pictureThread=threadCreate(pictureTimer,nullptr,8*1024,std::max<int>(priority-1,0x18),-2,false);
 if(!pictureThread){pictureLog("loading picture: FAIL thread not created");release();return;}
 pictureLog("loading picture: shown (%s, %s, thread priority %ld)",wide?"800px":"400px",pictureStereo?"3D":"2D",long(std::max<int>(priority-1,0x18)));
 active_=true;
}
RaceLoadingPicture::~RaceLoadingPicture(){
 if(!active_)return;
 if(reveal_){reveal::end();return;}
 pictureStop.store(true,std::memory_order_release);
 threadJoin(pictureThread,U64_MAX);threadFree(pictureThread);pictureThread=nullptr;
 {const unsigned loops=picturePresents.load()>PictureFadeFrames+1?picturePresents.load()-PictureFadeFrames-1:0;
  pictureLog("loading picture: load done after %u refreshes, periods %.0f us a refresh; lower wheel %u draws, %.0f us each",picturePresents.load(),
   loops?double(pictureWork.load())*1e6/SYSCLOCK_ARM11/loops:0.,wheelDraws.load(),wheelDraws.load()?double(wheelWork.load())*1e6/SYSCLOCK_ARM11/wheelDraws.load():0.);}
 const size_t bytes=pictureBytes();
#ifndef MKW_RELEASE
 // DIAG loading-shot.txt: the picture as shown before its fade out, as
 // loading-shot-NN.ppm (3D: -NN-right.ppm too). The main thread may use the
 // SD card; the timer thread can't.
 if(FILE* probe=std::fopen("sdmc:/3ds/MKWiiDS/loading-shot.txt","rb")){
  std::fclose(probe);static unsigned shot=0;
  for(unsigned eye=0;eye<pictureEyes();++eye){
   char path[64];std::snprintf(path,sizeof path,"sdmc:/3ds/MKWiiDS/loading-shot-%02u%s.ppm",shot,eye?"-right":"");
   if(const uint8_t* fb=pictureShown[eye])if(FILE* f=std::fopen(path,"wb")){
    std::fprintf(f,"P6\n%u 240\n255\n",pictureWidth);std::vector<uint8_t> row(size_t(pictureWidth)*3);
    for(unsigned y=0;y<240;++y){for(unsigned x=0;x<pictureWidth;++x){const uint8_t* p=fb+3*(x*240+(239-y));row[3*x]=p[2];row[3*x+1]=p[1];row[3*x+2]=p[0];}std::fwrite(row.data(),1,row.size(),f);}
    std::fclose(f);
   }
  }
  ++shot;
 }
#endif
 // Fade out to black, one step per refresh (each drawn into the hidden buffer
 // from the one on screen), then leave both buffers black for the race's
 // first frames (drawn black under the loading cover).
 for(unsigned f=1;f<=PictureFadeFrames;++f){
  const unsigned left=PictureFadeFrames-f,scale=left*256/(left+1);
  for(unsigned eye=0;eye<pictureEyes();++eye){
   uint8_t* to=topBack(eye);const uint8_t* from=pictureShown[eye];
   if(!left||!from)std::memset(to,0,bytes);else for(size_t i=0;i<bytes;++i)to[i]=uint8_t((from[i]*scale)>>8);
   GSPGPU_FlushDataCache(to,bytes);
  }
  presentPicture();
 }
 for(unsigned eye=0;eye<pictureEyes();++eye){uint8_t* fb=topBack(eye);std::memset(fb,0,bytes);GSPGPU_FlushDataCache(fb,bytes);}
 pictureStill.clear();periodLoop.clear();
 for(auto& base:periodBase)base.clear();
 pictureShown={};
 pictureLog("loading picture: closed");
}
void drawLoadingWheel(Renderer& renderer,uint64_t waitStarted){
 if(!wheel||!wheel->ready||!waitStarted||svcGetSystemTick()-waitStarted<=SYSCLOCK_ARM11/2)return;
 const float a=angle(waitStarted),c=std::cos(a),s=std::sin(a);
 // Call only on the lower LCD while an actual resource wait is pending.
 constexpr float cx=300,cy=220,h=12;
 float q[4][2];const float local[4][2]={{-h,-h},{h,-h},{-h,h},{h,h}};
 for(unsigned i=0;i<4;++i){q[i][0]=cx+c*local[i][0]-s*local[i][1];q[i][1]=cy+s*local[i][0]+c*local[i][1];}
 constexpr float uv[4][2]={{0,0},{1,0},{0,1},{1,1}};constexpr uint32_t color[4]={~0u,~0u,~0u,~0u};
 renderer.drawScreenMaterialQuad(wheel.get(),q,uv,color,0,~0u,1);
}
}
