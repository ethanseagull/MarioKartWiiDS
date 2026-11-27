// Race bottom screen: see include/mkw/race_bottom_screen.hpp and docs/BOTTOM_SCREEN.md.
#include <mkw/race_bottom_screen.hpp>
#include <mkw/asset_pack.hpp>
#include <mkw/render_quality.hpp>
#include <mkw/native_mii_portrait.hpp>
#include <mkw/race_map_assets.hpp>
#include <mkw/battle_map_assets.hpp>
#include <mkw/map_object_assets.hpp>
#include <mkw/course_catalog.hpp>
#include <mkw/flyover_info_assets.hpp>
#include <mkw/race_standings_assets.hpp>
#include <mkw/race_flow_layout.hpp>
#include <mkw/flow_text.hpp>
#include <game/field/ObjectDrivableDirector.hh>
#include <game/field/obj/ObjectTownBridge.hh>
#include <game/field/obj/ObjectTwistedWay.hh>
#include <game/field/obj/ObjectEscalator.hh>
#include <game/field/obj/ObjectVolcanoPiece.hh>
#include <mkw/race_grid.hpp>
#include <mkw/race_items.hpp>
#include <mkw/renderer.hpp>
#include <game/kart/KartMove.hh>
#include <game/kart/KartObjectManager.hh>
#include <game/system/RaceConfig.hh>
#include <game/system/RaceManager.hh>
#include <3ds.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mkw/hud_feedback.hpp>
#include <mkw/diag.hpp>
#include <mkw/loading_screen.hpp>
#include <atomic>
#include <cstring>
#include <game/kart/KartAction.hh>
#include <new>
namespace mkw {
namespace {
constexpr float ScreenW=320.f,ScreenH=240.f;
// The map fills the screen, centred on the player (Mario Kart 7's framing).
constexpr float MapCY=ScreenH*.5f;
// Standings: 12 boxes over the map's left edge (RaceMapUi rows: the menus'
// selected-button frame, 19 px), 1 px of map above the first and below the
// last, the rest shared out as 0-1 px gaps; each row snaps to a whole pixel.
// They slide in from RowSlide px left of the screen at GO.
// DIAG rows-margin.txt overrides the margin (pixels, e.g. 0.5).
constexpr float RowX=2.f,RowSlide=86.f;
static float rowMargin=1.f;
inline float rowY(float row){
    const float pitch=(ScreenH-2*rowMargin-RaceMapUi.rowH)/11.f;
    return std::floor(rowMargin+row*pitch+.5f);
}
// Retail CtrlRace2DMapCharacter (USA 0x807E1620/0x807E1B20): the face pane is
// 32 x 1.6 layout units, scaled 0.8 for the local racer and 0.6 for the
// others; the two black shadow copies 2.0/1.6 of it, 1.5 units either side;
// the others are drawn at alpha 160. One layout unit is 240/456 px here.
constexpr float Unit=240.f/456.f;
constexpr float FaceUnits=32.f*1.6f;
constexpr float LocalScale=.8f,LocalShadowScale=.85f,OtherScale=.6f,OtherShadowScale=.65f;
// All racer portraits are opaque (Dad's HW09 layout policy).
// Searchlight (light pane, origin bottom centre): 60x64 x 1.2, pane scale 0.8, alpha 0xB4.
constexpr float LightW=60.f*1.2f*.8f*Unit,LightH=64.f*1.2f*.8f*Unit;
constexpr uint32_t LightColor=0xffff00b4u;
// CtrlRaceTime's digits at the top right, the retail layout uniformly scaled:
// 21/32 draws its 32-unit digits at the atlas's 21 px; no TIME label.
constexpr float TimerRight=316.f,TimerTop=4.f;
// DIAG timer-scale.txt overrides it for comparisons.
static float TimerScale=21.f/32.f;
// fn_1_2DBB6C (USA 0x807E2674) with flag 0: the HUD slot colour on top, then
// +(40,130,40) clamped per lower corner, cumulatively.
constexpr uint32_t TimerGold[4]={0xe8d400ffu,0xe8d400ffu,0xffff28ffu,0xffff50ffu};
constexpr float MarkerSize=13.f;
float smooth(float t){t=std::clamp(t,0.f,1.f);return t*t*(3.f-2.f*t);}
// The course intro's info box (tools/cook_flyover_info.py): Mario Kart 7's flyover box (race.szs
// 0002ae00: 320 x 64 over the map's top, cup and course) in the standings rows' frame, with a cup
// medallion pinned on its top edge. Box at (InfoX, InfoY), FlyBoxW wide; lines on whole-pixel baselines: InfoPad from the inner edge to
// the first line's cap top, InfoGap from a line's descender to the next one's cap top, the bottom gold
// as far below the last line as the first is below the top gold (layoutInfo); text InfoTextX in from
// the inner edge, the first line ending InfoRingGap left of the ring.
constexpr float InfoX=4.f,InfoY=20.f,InfoPad=6.f,InfoGap=2.f,InfoTextX=9.f;
// The medallion: its centre InfoRingRight px left of the box's right end and InfoRingY below its top,
// the ring's radius (FlyRing drawn 1:1 at rest), its shadow's offset and alpha.
constexpr float InfoRingRight=32.f,InfoRingY=1.f,InfoRingGap=6.f,InfoShadowX=1.f,InfoShadowY=2.f;
constexpr uint32_t InfoShadowColour=0x00000073u;
// The top edge's gold ends 3 px below the box's top (the button's rows 2-4 of the corner's 8 at half size).
constexpr float InfoGoldTop=3.f;
// White names, the cups pale gold.
constexpr uint32_t InfoNameColour=0xffffffffu,InfoCreditColour=0xebe1aaffu;
// The entrance, InfoInTicks long (the exit plays it backwards): the medallion pops with an overshoot
// (InfoPopTicks), the box unrolls leftwards from behind it (InfoUnrollTicks from InfoUnrollAt), then the
// text fades in (from InfoTextAt). The DIAG test switch flyinfo-mk7-entrance.txt:
// Mario Kart 7's box instead, which appears and goes with no movement. The gold runs round once every InfoLoopTicks (the menus'
// buttons: 40 frames), the ring's bright arc with it.
constexpr unsigned InfoInTicks=24,InfoLoopTicks=60;
constexpr float InfoPopTicks=10.f,InfoUnrollAt=6.f,InfoUnrollTicks=16.f,InfoTextAt=16.f;
float backOut(float t){t=std::clamp(t,0.f,1.f)-1.f;return 1.f+t*t*(3.2f*t+2.2f);}
float cubicOut(float t){t=1.f-std::clamp(t,0.f,1.f);return 1.f-t*t*t;}
const FlyGlyph* flyGlyph(unsigned size,char16_t c){
    const auto& f=FlyFonts[size];const FlyGlyph* first=FlyGlyphs+f.first;const FlyGlyph* last=first+f.count;
    const auto* g=std::lower_bound(first,last,c,[](const FlyGlyph& a,char16_t b){return a.c<b;});
    return g!=last&&g->c==c?g:nullptr;
}
float flyWidth(unsigned size,const char* text){
    float w=0;while(*text){const char16_t c=nextFlowGlyph(text);if(const auto* g=flyGlyph(size,c))w+=float(g->advance)/16.f;}
    return w;
}
// The Standings view (tools/cook_race_standings.py): Mario Kart DS's lower screen in the Wii's look. A fixed column of the 12 rank numbers (x 0..21, 20 px a
// slot), each racer's row beside it (x 22: the rows' frame, face, name, item), the course silhouette
// at its best-fit angle over a per-track colour and a drifting emblem tile. In the close-up the rows
// are short tabs (face and item) that slide between the fixed numbers; the 12-tick switch widens them
// into rows (names fade in over its second half) and cross-fades the map into the Standings picture.
constexpr float DsSlot=20.f,DsRowX=22.f,DsTab=40.f,DsTabLocal=46.f,DsRow=116.f,DsRowLocal=124.f,DsRowSlide=150.f;
// Inside a row (from its left edge): the face (15 px) and its top, the names' left edge, the item
// centred 11 px in from the right end; everything centred on the frame's inside (rows 3-13, line 8).
constexpr float DsFace=15.f,DsFaceX=2.f,DsIconY=1.f,DsNameX=21.f,DsItemRight=18.f,DsCentre=8.f;
// Standings racer faces: others 15 px, the player 18 px (the close-up's 16.2 / 21.6).
constexpr float DsOtherFace=15.f/(32.f*1.6f*OtherScale*Unit),DsLocalFace=18.f/(32.f*1.6f*LocalScale*Unit);
// The background drift: Mario Kart DS's speed (1/6 DS px a frame on each axis of its 45-degree path,
// at 1.25x) in a direction picked per race, sub-pixel smooth, one 64 px tile period.
constexpr float DsDrift=10.f*1.41421356f*1.25f/60.f;
// The names texture: racer i's name on rows i*16..+13, at most 126 px.
constexpr unsigned DsNamesW=128,DsNamesH=256,DsNameRow=16;
}
static_assert(RaceBottomScreen::QuadCapacity==292&&RaceBottomScreen::QuadCapacity<=unsigned(Renderer::BottomQuadCapacity),
              "every quad a frame can add fits the renderer's bottom sprite region");
static_assert(RaceBottomScreen::QuadCapacity+RaceBottomScreen::InfoCapacity<=unsigned(Renderer::BottomQuadCapacity),"the info box fits beside them");

struct RaceBottomScreen::Impl {
    Texture map,overview,atlas,objects,mii;
    // The UI atlas's rectangles.
    const RaceMapAtlas* ui=&RaceMapUi;
    // The local outline drawn from the face in a team colour: its own draw
    // (colour0 white), at this index of the overlay batch.
    unsigned outline=~0u;
    NativeMii miiIdentity{};
    Renderer::MaterialQuad miiQuads[2][2]{};bool miiMap=false,miiRow=false;
    unsigned course=~0u;
    bool target=false;
    std::unique_ptr<Renderer::MaterialQuad[]> quads; // 2 x QuadCapacity, alternating per frame
    // Animation state (60 Hz ticks).
    uint32_t lastTick=0;bool started=false;
    float blend=0;bool overviewWanted=false;     // 0 detailed .. 1 whole course
    float timerSlide=-1.f;
    float standings=0;                           // slide-in 0..1
    std::array<float,12> row{};bool rowsSet=false;
    unsigned roulette=0;
    unsigned skippedFrames=0; // DIAG: frames left alone at 30 Hz (skipRace)
    // Race frames whose quads equal the last shown frame's keep the lower
    // LCD's picture: no draw, no clear, no transfer (A/B: no-race-bottom-keep.txt).
    // The race's 2D lower screen binds no depth buffer (A/B: no-bottom-depth-off.txt).
    bool depthOff=true;
    // Its depth buffer only exists as the top LCD's, borrowed by the intro's
    // 3D lower view (A/B: no-bottom-depth-free.txt).
    bool depthFree=true;
    bool keep=true,shownValid=false;uint64_t shownHash=0;uint32_t shownSerial=0;unsigned shownApt=0,shownScopes=0;
    unsigned keptFrames=0,drawnFrames=0;uint32_t miiGeneration=0;
    // The info box: its glyph atlas and frame art, its quads (2 x InfoCapacity, alternating per
    // frame: frame art from 0, glyphs from InfoArtCapacity), entrance ticks 0..InfoInTicks (MK7-style:
    // 0 or InfoInTicks), gold loop ticks, the emblem in the art's cell (-1: none, the flag), and the
    // laid-out lines of one course (box px from its top left; sx: a squeezed line's width scale).
    Texture infoFont,infoArt;std::unique_ptr<Renderer::MaterialQuad[]> infoQuads;
    unsigned infoIn=0;uint32_t infoTicks=0;unsigned infoArtCount=0,infoTextCount=0;bool infoMk7=false;int infoEmblem=-1;
    struct InfoGlyph{float x,y,sx;uint16_t glyph;uint32_t colour;};
    std::array<InfoGlyph,InfoCapacity-InfoArtCapacity> infoGlyph{};unsigned infoGlyphs=0,infoLaidCourse=~0u;
    float infoH=0;int infoCup=-1;
    unsigned infoFrames=0,infoQuadPeak=0; // DIAG: composed frames with the box, its most quads
    // The Standings view: on for this race (overviewWanted then means Standings), its course record,
    // textures (silhouette LA4, emblem tile L8, atlas RGBA8, the racers' names LA8), each racer's name
    // width and left ink in the names texture, the drift (direction, ticks since load).
    bool ds=false;const StandingsCourse* dsCourse=nullptr;
    Texture dsSil,dsTile,dsAtlas,dsNames;unsigned dsNamesGeneration=0;
    std::array<float,12> nameW{},nameInk{};
    float driftCos=1,driftSin=0;uint32_t driftTicks=0;uint16_t driftAngle=0;
    unsigned dsFrames=0,dsQuadPeak=0; // DIAG: composed frames with the Standings picture, the most quads
    unsigned dsToggleEvery=0,dsToggleClock=0; // DIAG map-toggle.txt: switch views every N race ticks (captures)
    void layoutInfo(unsigned course){
        infoLaidCourse=course;infoGlyphs=0;infoCup=-1;infoH=0;
        if(course>=std::size(Courses))return;
        // Its cup over its name (Mario Kart 7's two lines); a course in no cup: its name.
        struct Line{const char* text;unsigned size;uint32_t colour;};
        Line lines[2];unsigned n=0;
        const unsigned cup=course<std::size(FlyCup)?FlyCup[course]:255u;
        if(cup<10){infoCup=int(cup);lines[n++]={FlyCupNames[cup],1,InfoCreditColour};}
        lines[n++]={Courses[course].name,0,InfoNameColour};
        // Every line from the same left; the first ends left of the medallion (its cup is in the ring).
        const float edge=float(FlyEdge),left=edge+InfoTextX;
        const float room=float(FlyBoxW)-edge-InfoTextX-left,firstRoom=float(FlyBoxW)-InfoRingRight-float(FlyRing.w)*.5f-InfoRingGap-left;
        float base=edge+InfoPad,descent=0;
        for(unsigned i=0;i<n;++i){
            const auto& f=FlyFonts[lines[i].size];
            base+=(i?descent+InfoGap:0.f)-float(f.capTop);descent=float(f.descent);
            const float width=flyWidth(lines[i].size,lines[i].text),r=i?room:firstRoom,sx=width>r?std::max(.72f,r/width):1.f;
            float pen=0;
            for(const char* t=lines[i].text;*t&&infoGlyphs<infoGlyph.size();){
                const char16_t c=nextFlowGlyph(t);
                const auto* g=flyGlyph(lines[i].size,c);if(!g)continue;
                // Whole-pixel pens (the cook keeps each glyph's fractional ink offset).
                if(g->w)infoGlyph[infoGlyphs++]={left+std::floor(pen*sx+.5f),base,sx,uint16_t(g-FlyGlyphs),lines[i].colour};
                pen+=float(g->advance)/16.f;
            }
        }
        // Even gold to text gaps: the top edge's gold ends InfoGoldTop px down; the bottom gold
        // (FlyLineTop of the middle) starts as far below the last line's ink (the glyphs' outline
        // is one row past the baseline) as the first line's ink starts below the top gold.
        const float gap=edge+InfoPad+1.f-InfoGoldTop;
        infoH=std::ceil(2*edge+(base+1.f+gap-edge)/FlyLineTop);
    }
    // Race load, after the info box's textures: the course's cup emblem into the frame art's cell.
    void setInfoCup();
    // The composed frame's draws (compose -> submit).
    struct Batches {unsigned fills=0,mapFirst=0,mapCount=0,objectFirst=0,objectCount=0,overlayFirst=0,overlayCount=0,
                    topFirst=0,topCount=0,fadeFirst=0,mii=0,outline=~0u;const Texture* mapTexture=nullptr;bool covered=false,atlas=false;
                    // The Standings view's draws: background tile (colours c0 -> c1; opaque once it covers the
                    // screen), its shade, the silhouette, the pointer, the number boxes, the names.
                    unsigned bgFirst=0,bgCount=0,shadeFirst=0,shadeCount=0,silFirst=0,silCount=0,pointerFirst=0,pointerCount=0,
                             numberFirst=0,numberCount=0,nameFirst=0,nameCount=0;uint32_t bg0=0,bg1=0;bool bgOpaque=false;} b;
    // One frame's quads, in three in-place batches.
    Renderer::MaterialQuad* base=nullptr;unsigned count=0;
    // recordingFrame()'s parity: this frame's half of quads and miiQuads.
    unsigned half=0;
    bool add(const RaceMapRect& r,float x0,float y0,float x1,float y1,uint32_t color=0xffffffffu,bool mirrorU=false,bool mirrorV=false){
        if(count>=QuadCapacity||!r.w)return false;
        const float aw=ui->size[0],ah=ui->size[1];
        float u0=r.x/aw,u1=(r.x+r.w)/aw,v0=r.y/ah,v1=(r.y+r.h)/ah;
        if(mirrorU)std::swap(u0,u1);
        if(mirrorV)std::swap(v0,v1);
        auto& q=base[count++];
        q.x[0]=x0;q.y[0]=y0;q.x[1]=x1;q.y[1]=y0;q.x[2]=x0;q.y[2]=y1;q.x[3]=x1;q.y[3]=y1;
        q.u[0]=u0;q.v[0]=v0;q.u[1]=u1;q.v[1]=v0;q.u[2]=u0;q.v[2]=v1;q.u[3]=u1;q.v[3]=v1;
        for(auto& c:q.color)c=color;
        return true;
    }
    // A quad with free corners (TL, TR, BL, BR) over atlas rectangle r.
    // A quad over texel rectangle (u0, v0)-(u1, v1) of a w x h texture.
    bool addTexels(float u0,float v0,float u1,float v1,float w,float h,float x0,float y0,float x1,float y1,uint32_t color){
        if(count>=QuadCapacity)return false;
        auto& q=base[count++];
        q.x[0]=x0;q.y[0]=y0;q.x[1]=x1;q.y[1]=y0;q.x[2]=x0;q.y[2]=y1;q.x[3]=x1;q.y[3]=y1;
        q.u[0]=u0/w;q.v[0]=v0/h;q.u[1]=u1/w;q.v[1]=v0/h;q.u[2]=u0/w;q.v[2]=v1/h;q.u[3]=u1/w;q.v[3]=v1/h;
        for(auto& c:q.color)c=color;
        return true;
    }
    bool addCorners(const RaceMapRect& r,const float (&x)[4],const float (&y)[4],uint32_t color){
        if(count>=QuadCapacity||!r.w)return false;
        const float aw=ui->size[0],ah=ui->size[1];
        const float u0=r.x/aw,u1=(r.x+r.w)/aw,v0=r.y/ah,v1=(r.y+r.h)/ah;
        auto& q=base[count++];
        for(unsigned k=0;k<4;++k){q.x[k]=x[k];q.y[k]=y[k];q.color[k]=color;}
        q.u[0]=u0;q.v[0]=v0;q.u[1]=u1;q.v[1]=v0;q.u[2]=u0;q.v[2]=v1;q.u[3]=u1;q.v[3]=v1;
        return true;
    }
};

const Texture* RaceBottomScreen::miiPortrait() const{return impl_->mii.ready?&impl_->mii:nullptr;}

namespace {
// Assets the DIAG may also take as loose SD files (the next full build packs them).
bool readAsset(const char* path,std::vector<uint8_t>& out){
    if(const long n=pack::size(path);n>=0){out.resize(size_t(n));return pack::readInto(path,out.data(),out.size());}
    FILE* f=std::fopen(path,"rb");if(!f)return false;
    std::fseek(f,0,SEEK_END);const long n=std::ftell(f);std::fseek(f,0,SEEK_SET);
    out.resize(n>0?size_t(n):0);const bool ok=n>0&&std::fread(out.data(),1,out.size(),f)==out.size();std::fclose(f);return ok;
}
}
void RaceBottomScreen::setRacerNames(Renderer& renderer,const std::array<uint8_t,12>& characters,
                                     const std::array<std::array<char,32>,12>& miiNames,unsigned racers){
    auto& m=*impl_;
    // The drift direction: a hash of the race (course and lineup; captures and benchmarks fix their
    // lineups, a restart keeps it), Knuth multiplicative then an xor-shift mix, as a 16-bit angle.
    uint32_t seed=2166136261u;auto mix=[&](uint32_t v){seed=(seed^v)*16777619u;};
    mix(m.course);for(unsigned i=0;i<12;++i){mix(i<racers?characters[i]:0xffu);mix(0u);}
    uint32_t h=seed*2654435761u;h^=h>>16;h*=0x45d9f3bu;h^=h>>16;m.driftAngle=uint16_t(h&0xffffu);
    const float angle=float(m.driftAngle)*(6.2831853f/65536.f);m.driftCos=std::cos(angle);m.driftSin=std::sin(angle);
    if(!m.ds)return;
    // The font: the menus' quarter-size name glyphs (8-bit coverage), by character code.
    std::vector<uint8_t> font;
    if(!readAsset(StandingsFont,font)||font.size()<8||std::memcmp(font.data(),"STNF",4)){std::fprintf(stderr,"standings names font missing\n");return;}
    const unsigned glyphs=font[4]|font[5]<<8,cell=font[6]|font[7]<<8;
    if(font.size()<8+size_t(glyphs)*8||cell>DsNameRow)return;
    const uint8_t* table=font.data()+8;const uint8_t* data=table+glyphs*8;const size_t dataBytes=font.size()-8-size_t(glyphs)*8;
    auto glyph=[&](char16_t c)->const uint8_t*{
        unsigned lo=0,hi=glyphs;
        while(lo<hi){const unsigned mid=(lo+hi)/2;const unsigned code=table[mid*8]|table[mid*8+1]<<8;if(code<c)lo=mid+1;else hi=mid;}
        return lo<glyphs&&(table[lo*8]|table[lo*8+1]<<8)==c?table+lo*8:nullptr;
    };
    // Each name (a Mii's name, else the character's) as the menus write it: coverage composed
    // (advance = width - 1, an unknown character 3 px),
    // then white over a 1 px black outline (its 3x3 dilation): L = fill share, A = coverage.
    std::vector<uint8_t> la(DsNamesW*DsNamesH*2,0);
    for(unsigned i=0;i<racers&&i<12;++i){
        const char* text=miiNames[i][0]?miiNames[i].data():characters[i]<FlowCharacterNames.size()?FlowCharacterNames[characters[i]]:"Mii";
        float cov[DsNameRow][DsNamesW]{};unsigned x=0;
        for(const char* s=text;*s&&x<DsNamesW-2;){
            const char16_t c=nextFlowGlyph(s);const uint8_t* g=glyph(c);
            if(!g){x+=3;continue;}
            const unsigned w=g[2],gh=g[3],off=g[4]|g[5]<<8|g[6]<<16|unsigned(g[7])<<24;
            if(!w||size_t(off)+size_t(w)*gh>dataBytes)continue;
            for(unsigned y=0;y<gh&&y<cell;++y)for(unsigned k=0;k<w&&x+k<DsNamesW-1;++k){
                float& v=cov[y][x+k];const float a=float(data[off+y*w+k])/255.f;v=v+a*(1-v);}
            x+=w-1;
        }
        const unsigned width=std::min(DsNamesW-1,x+2);float ink=0;bool inked=false;
        for(unsigned y=0;y<DsNameRow;++y)for(unsigned k=0;k<width;++k){
            float d=0;for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){const int yy=int(y)+dy,xx=int(k)+dx;
                if(yy>=0&&yy<int(DsNameRow)&&xx>=0&&xx<int(DsNamesW))d=std::max(d,cov[yy][xx]);}
            const float c=cov[y][k],a=c+d*(1-c);
            uint8_t* o=&la[((i*DsNameRow+y)*DsNamesW+k)*2];o[0]=uint8_t(std::lround((a>0?c/a:0)*255.f));o[1]=uint8_t(std::lround(a*255.f));
        }
        for(unsigned k=0;k<width&&!inked;++k)for(unsigned y=0;y<DsNameRow;++y)if(la[((i*DsNameRow+y)*DsNamesW+k)*2+1]>40){ink=float(k);inked=true;break;}
        m.nameW[i]=float(width);m.nameInk[i]=ink;
    }
    if(m.dsNames.ready)renderer.waitForGpu();
    m.dsNames.reset();
    if(!C3D_TexInit(&m.dsNames.gpu,DsNamesW,DsNamesH,GPU_LA8))return;
    auto* out=static_cast<uint8_t*>(m.dsNames.gpu.data);
    for(unsigned y=0;y<DsNamesH;++y)for(unsigned x=0;x<DsNamesW;++x){
        unsigned morton=0;for(unsigned bit=0;bit<3;++bit)morton|=((x>>bit)&1u)<<(2*bit)|((y>>bit)&1u)<<(2*bit+1);
        uint8_t* q=out+2*(((y>>3)*(DsNamesW>>3)+(x>>3))*64+morton);const uint8_t* p=&la[(y*DsNamesW+x)*2];
        q[0]=p[1];q[1]=p[0]; // GPU_LA8: u16 L << 8 | A
    }
    C3D_TexFlush(&m.dsNames.gpu);C3D_TexSetFilter(&m.dsNames.gpu,GPU_LINEAR,GPU_LINEAR);C3D_TexSetWrap(&m.dsNames.gpu,GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE);
    m.dsNames.ready=true;++m.dsNamesGeneration;
}
bool RaceBottomScreen::standingsChoice(bool& view) const{view=impl_->overviewWanted;return impl_->ds;}

bool RaceBottomScreen::setMiiPortrait(Renderer& renderer,const NativeMii& identity){
 if(impl_->mii.ready&&impl_->miiIdentity==identity)return true;
 renderer.waitForGpu();impl_->mii=Texture{};std::vector<uint8_t> rgba;
 ++impl_->miiGeneration;
 if(!composeNativeMiiPortrait(identity,64,rgba)||!impl_->mii.initRgba8(64,64,rgba.data(),rgba.size()))return false;
 impl_->miiIdentity=identity;return true;
}

void fillBottomView(BottomView& v,const RaceGrid& grid,const RaceItems& items,bool introActive,uint32_t tick,float leaveFade){
    namespace K=Kinoko;
    const auto previousObjects=v.mapObject;
    const unsigned previousCount=v.mapObjects,previousCourse=v.mapCourse,previousTick=v.tick;
    v=BottomView{};
    auto* race=K::System::RaceManager::Instance();
    const auto& scenario=K::System::RaceConfig::Instance()->raceScenario();
    v.mirror=scenario.mirror;
    v.timeTrial=scenario.gameMode==K::System::RaceConfig::Scenario::GameMode::Time_Trial;
    // Snapshot live physical transforms, independent of the upper camera's culling.
    // No scene pointers survive this call (restart/unload cannot leave stale actors).
    unsigned mapCourse=~0u;
    for(unsigned i=0;i<std::size(Courses);++i)if(Courses[i].id==scenario.course){mapCourse=i;break;}
    v.mapCourse=mapCourse;
    const auto objectArt=mapObjectAtlas(mapCourse);
    const bool reuseMap=previousCourse==mapCourse&&tick>=previousTick&&tick/3==previousTick/3;
    if(reuseMap){v.mapObject=previousObjects;v.mapObjects=previousCount;}
    else if(objectArt.count)if(const auto* director=K::Field::ObjectDrivableDirector::Instance()){
        for(const auto* object:director->objects()){
            const auto* geo=object->mapObj();
            const unsigned id=unsigned(object->id());
            // DCPillar also registers a stationary collision base with the same id.
            if(id==0x208&&!(object->loadFlags()&1))continue;
            if((id==0x1fc||id==0x20d)&&!static_cast<const K::Field::ObjectVolcanoPiece*>(object)->mapRoadPresent())continue;
            const bool bridge=id==0x1ff;
            unsigned state=0;
            auto matrix=object->transform();
            if(bridge&&geo){
                const float frame=static_cast<const K::Field::ObjectTownBridge*>(object)->animationFrame();
                state=unsigned(std::clamp(std::lround(frame*15.f/89.f),0l,15l))+(geo->rot().y<0?16:0);
                matrix.makeRT(geo->rot()*(3.14159265358979323846f/180.f),object->pos());
            }
            const bool racing= race&&race->isStageReached(K::System::RaceManager::Stage::Race);
            const unsigned timer=race?race->timer():0;
            if(id==0x1fe){
                const auto* road=static_cast<const K::Field::ObjectTwistedWay*>(object);
                const unsigned frame=racing?timer:road->introTimer()-1;
                state=(frame%120)/3;
            }else if(id==0x202){state=((racing?timer:0)%160)/3;}
            else if(id==0x25e){
                const float phase=static_cast<const K::Field::ObjectEscalator*>(object)->stepFactor();
                state=std::min(19u,unsigned(std::max(0.f,phase)));
                // The atlas animates within the fixed landing footprint. Remove
                // the tread-loop offset so its wrap cannot move the whole tile.
                const auto shift=matrix.multVector33({0.f,10.f*phase,-30.f*phase});
                matrix.makeRT(object->rot(),object->pos()-shift);
            }
            else if(id==0x208&&geo){
                state=unsigned(std::clamp(std::lround(object->rot().x*(180.f/3.14159265358979323846f)/5.f),0l,24l));
                matrix.makeRT(geo->rot()*(3.14159265358979323846f/180.f),object->pos());
            }else if(id==0x209&&geo){
                const float rise=object->pos().y-geo->pos().y;
                state=unsigned(std::clamp(std::lround(rise*15.f/std::max(1u,unsigned(geo->setting(1)))),0l,15l));
            }
            for(unsigned a=0;a<objectArt.count;++a){
                const auto& art=objectArt.art[a];
                if(state!=art.state||unsigned(object->id())!=art.id||(art.variant>=0&&(!geo||geo->setting(id==0x209?1:0)!=unsigned(art.variant))))continue;
                if(v.mapObjects>=v.mapObject.size())break;
                auto& out=v.mapObject[v.mapObjects++];out.art=a;
                const auto& scale=object->scale();
                for(unsigned k=0;k<4;++k){
                    const K::EGG::Vector3f local{(k&1?art.x1:art.x0)*scale.x,art.height*scale.y,(k&2?art.z1:art.z0)*scale.z};
                    const auto p=matrix.multVector(local);
                    out.x[k]=p.x;out.z[k]=p.z;out.height+=p.y*.25f;
                }
                break;
            }
        }
        std::sort(v.mapObject.begin(),v.mapObject.begin()+v.mapObjects,[](const auto& a,const auto& b){return a.height<b.height;});
    }

    v.racers=std::min(grid.count(),12u);
    for(unsigned i=0;i<v.racers;++i){
        const auto& p=K::Kart::KartObjectManager::Instance()->object(i)->pos();
        auto& r=v.racer[i];r.x=p.x;r.z=p.z;r.local=i==0;
        const auto* kart=K::Kart::KartObjectManager::Instance()->object(i);const auto* action=kart->action();
        const int damage=int(action->currentAction());
        if(kart->status().onBit(K::Kart::eStatus::InAction)&&((damage>=0&&damage<=11)||damage==17))r.hitAngle=mapHitAngle(action->frame());
        r.team=scenario.teams?RaceTeam(scenario.players[i].team):RaceTeam::None;
        r.character=uint8_t(scenario.players[i].character);
        r.rank=uint8_t(std::clamp(grid.rank(i),1u,12u));
        // The item window's picture (CtrlRaceItemWindow): a spinning roulette's
        // icon, else the held item; a Triple Mushroom shows its count (19 = two).
        const auto& h=items.hud(i);
        if(h.roulette)r.item=uint8_t(h.rouletteIcon);
        else{
            const auto picture=itemWindowPicture(h);
            r.item=picture.kind==ItemKind::TripleMushroom&&picture.variant==2?19:
                   picture.kind==ItemKind::TripleMushroom&&picture.variant==1?uint8_t(ItemKind::Mushroom):uint8_t(picture.kind);
        }
    }
    {const auto& f=K::Kart::KartObjectManager::Instance()->object(0)->move()->smoothedForward();v.headingX=f.x;v.headingZ=f.z;}
    items.forEachActorSlotRead([&](unsigned slot){
        const auto& a=items.actors()[slot];
        if(!a.active||v.items>=v.item.size())return true;
        int marker=-1;
        if(a.explosion)marker=a.kind==ItemKind::BlueShell?int(BottomMarker::BlastBlue):a.kind==ItemKind::Bobomb?int(BottomMarker::BlastRed):-1;
        else if(a.kind==ItemKind::Banana)marker=int(BottomMarker::Banana);
        else if(a.kind==ItemKind::GreenShell)marker=int(BottomMarker::GreenShell);
        else if(a.kind==ItemKind::RedShell)marker=int(BottomMarker::RedShell);
        else if(a.kind==ItemKind::BlueShell)marker=int(BottomMarker::SpinyShell);
        else if(a.kind==ItemKind::Bobomb)marker=int(BottomMarker::Bobomb);
        else if(a.kind==ItemKind::FakeBox)marker=int(BottomMarker::FakeBox);
        if(marker<0)return true;
        const auto p=a.visualPosition();auto& it=v.item[v.items++];
        it.x=p.x;it.z=p.z;it.marker=uint8_t(marker);it.radius=a.radius; // a blast's live retail radius
        return true;
    });
    const unsigned stage=unsigned(race->stage());
    const auto& me=race->player(0);
    const auto& timer=me.finished()?me.raceTimer():race->timerManager().currentTimer();
    v.milliseconds=uint32_t(timer.min)*60000u+uint32_t(timer.sec)*1000u+uint32_t(timer.mil);
    // CtrlRaceTime shows with the HUD from the countdown; Mario Kart 7 shows
    // its standings once the race has started.
    v.timerVisible=!introActive&&stage>=1;v.racerIconsVisible=!introActive;v.standingsVisible=!v.timeTrial&&!introActive&&stage>=2;v.raceStarted=!introActive&&stage>=2;v.tick=tick;
    // Both screens go black together when leaving after the race (Mario Kart 7).
    v.fade=std::clamp(leaveFade,0.f,1.f);
}

namespace {
// The HOME Menu, sleep and the like may change what the lower LCD shows.
std::atomic<unsigned> aptEvents{0};
void countAptEvent(APT_HookType,void*){aptEvents.fetch_add(1,std::memory_order_relaxed);}
aptHookCookie aptCookie;
}

RaceBottomScreen::RaceBottomScreen():impl_(std::make_unique<Impl>()){aptHook(&aptCookie,countAptEvent,nullptr);}
RaceBottomScreen::~RaceBottomScreen(){aptUnhook(&aptCookie);}
bool RaceBottomScreen::active() const {return impl_->target;}

static bool sdFile(const char* name){
    char path[96];std::snprintf(path,sizeof path,"sdmc:/3ds/MKWiiDS/%s",name);
    FILE* f=std::fopen(path,"rb");if(f)std::fclose(f);return f!=nullptr;
}

bool RaceBottomScreen::load(Renderer& renderer,unsigned course){
    auto& m=*impl_;
    rowMargin=1.f;TimerScale=21.f/32.f;
#ifndef MKW_RELEASE
    if(FILE* f=std::fopen("sdmc:/3ds/MKWiiDS/timer-scale.txt","r")){float v;if(std::fscanf(f,"%f",&v)==1&&v>=.3f&&v<=1.f)TimerScale=v;std::fclose(f);}
    if(FILE* f=std::fopen("sdmc:/3ds/MKWiiDS/rows-margin.txt","r")){float v;if(std::fscanf(f,"%f",&v)==1&&v>=0&&v<=8)rowMargin=v;std::fclose(f);}
#endif
    // A/B files are read once a session (three SD opens per race load otherwise).
    static const bool noKeep=sdFile("no-race-bottom-keep.txt"),noDepthOff=sdFile("no-bottom-depth-off.txt"),noDepthFree=sdFile("no-bottom-depth-free.txt");
    const bool whole=course>=32;
    endRace();m.keep=!noKeep;m.depthOff=!noDepthOff;m.depthFree=!noDepthFree;
    const auto* art=raceMapCourse(course);
    if(!art){unload(renderer);return false;}
    // The Standings view (A/B: no-ds-standings.txt brings back the whole-course map and today's rows).
    // DIAG map-standings.txt: races start in Standings (bottom-screen captures).
    static const bool noDs=sdFile("no-ds-standings.txt");
#ifndef MKW_RELEASE
    static const bool startStandings=sdFile("map-standings.txt");
    static const unsigned toggleEvery=[]{unsigned n=0;if(FILE* f=std::fopen("sdmc:/3ds/MKWiiDS/map-toggle.txt","r")){if(std::fscanf(f,"%u",&n)!=1)n=0;std::fclose(f);}return n;}();
    m.dsToggleEvery=toggleEvery;m.dsToggleClock=0;
#else
    constexpr bool startStandings=false;
#endif
    const StandingsCourse* dsCourse=StandingsAvailable&&!noDs&&course<32&&course<StandingsCourses.size()&&StandingsCourses[course].silhouette?&StandingsCourses[course]:nullptr;
    // It starts in the close-up and turns to the chosen view after the course intro.
    auto startView=[&]{
        if(m.ds){m.blend=0.f;m.overviewWanted=standingsRemembered||startStandings;}
        else{m.blend=whole?1.f:0.f;m.overviewWanted=whole;}
        m.driftTicks=0;
    };
    if(m.target&&m.course==course){
        // A restart of the same race: reset the presentation, keep the textures.
        m.started=false;startView();m.standings=0;m.timerSlide=-1.f;m.rowsSet=false;
        m.infoIn=0;m.infoTicks=0;
        return true;
    }
    unload(renderer);
    if(!renderer.enableBottom())return false;
    // Before the course textures take the VRAM that is left.
    renderer.shareBottomDepth(m.depthFree);
    m.target=true;m.course=course;
    m.quads.reset(new(std::nothrow) Renderer::MaterialQuad[2*QuadCapacity]);
    if(!m.quads){unload(renderer);return false;}
    const auto& c=*art;
    m.ui=&RaceMapUi;
    renderer.loadVramTexture(m.atlas,RaceMapUi.texture,GPU_CLAMP_TO_EDGE,renderQuality().bottomRgba4);
    if(course<32)renderer.loadVramTexture(m.map,c.texture);
    // The Standings view's textures: LINEAR loads (the DIAG also takes loose SD files), then VRAM
    // where it leaves vramReserve free. Without them the race keeps the whole-course map.
    m.ds=false;m.dsCourse=nullptr;
    if(dsCourse){
        m.ds=m.dsSil.load(dsCourse->silhouette,0)&&dsCourse->tile<StandingsTiles.size()&&m.dsTile.load(StandingsTiles[dsCourse->tile],0)&&m.dsAtlas.load(StandingsAtlas,0);
        if(m.ds){
            C3D_TexSetWrap(&m.dsTile.gpu,GPU_REPEAT,GPU_REPEAT);C3D_TexSetWrap(&m.dsSil.gpu,GPU_CLAMP_TO_BORDER,GPU_CLAMP_TO_BORDER);m.dsSil.gpu.border=0;
            renderer.waitForGpu();
            for(auto* t:{&m.dsSil,&m.dsTile,&m.dsAtlas})renderer.moveToVram(*t,RenderQuality::vramReserve);
            m.dsCourse=dsCourse;
        }else{m.dsSil.reset();m.dsTile.reset();m.dsAtlas.reset();std::fprintf(stderr,"standings textures missing: course %u\n",course);}
    }
    // The whole-course map: not while Standings replaces it.
    if(!m.ds)renderer.loadVramTexture(m.overview,c.overview);
    if(course>=32&&(!m.atlas.ready||!m.overview.ready)){unload(renderer);return false;}
    const auto objectArt=mapObjectAtlas(course);
    if(objectArt.texture&&!renderer.loadVramTexture(m.objects,objectArt.texture))
        std::fprintf(stderr,"minimap object atlas load failed: %s\n",objectArt.texture);
    m.started=false;startView();m.standings=0;m.timerSlide=-1.f;m.rowsSet=false;
    // The course intro's info box (A/B: no-flyinfo.txt). Its textures stay in LINEAR: about 30k
    // pixels a drawn frame, during the intro only. Mario Kart 7's plain appear/go is a DIAG test switch.
    static const bool noInfo=sdFile("no-flyinfo.txt");
#ifndef MKW_RELEASE
    static const bool mk7Entrance=sdFile("flyinfo-mk7-entrance.txt");
#else
    constexpr bool mk7Entrance=false;
#endif
    m.infoIn=0;m.infoTicks=0;m.infoLaidCourse=~0u;m.infoMk7=mk7Entrance;m.infoEmblem=-1;
    if(FlyAvailable&&!noInfo){
        m.infoQuads.reset(new(std::nothrow) Renderer::MaterialQuad[2*InfoCapacity]);
        if(!m.infoQuads||!m.infoFont.load(FlyFontTexture)||!m.infoArt.load(FlyArtTexture)){m.infoFont.reset();m.infoArt.reset();m.infoQuads.reset();}
    }
    m.setInfoCup();
    return true;
}

void RaceBottomScreen::unload(Renderer& renderer){
    auto& m=*impl_;
    endRace();
    if(!m.target&&!m.map.ready&&!m.overview.ready&&!m.atlas.ready&&!m.objects.ready&&!m.quads&&!m.infoFont.ready&&!m.infoArt.ready&&!m.infoQuads&&
       !m.dsSil.ready&&!m.dsTile.ready&&!m.dsAtlas.ready&&!m.dsNames.ready)return;
    // Nothing recorded may still sample these textures or read the quads.
    renderer.waitForGpu();
    m.map.reset();m.overview.reset();m.atlas.reset();m.objects.reset();m.infoFont.reset();m.infoArt.reset();m.infoQuads.reset();
    m.dsSil.reset();m.dsTile.reset();m.dsAtlas.reset();m.dsNames.reset();m.ds=false;m.dsCourse=nullptr;
    m.quads.reset();
    renderer.releaseBottom();
    m.target=false;m.course=~0u;
}

void RaceBottomScreen::toggleView(){
    if(impl_->course>=32)return; // Battle is always the whole arena, including touch input.
    impl_->overviewWanted=!impl_->overviewWanted;
}

void RaceBottomScreen::describe(char* text,unsigned size) const {
    const auto& m=*impl_;
    // Standings: silhouette, tile, atlas, names bytes, each v (VRAM) or l (LINEAR).
    auto where=[](const Texture& t){return t.ready&&t.gpu.data&&osConvertVirtToPhys(t.gpu.data)>=0x18000000u&&osConvertVirtToPhys(t.gpu.data)<0x18600000u?'v':'l';};
    std::snprintf(text,size,"bottom screen course=%d target=%d map=%lu overview=%lu atlas=%lu objects=%lu info=%lu+%lu emblem=%d%s standings=%d sil=%lu%c tile=%lu%c ui=%lu%c names=%lu%c drift=%u vram_free=%lu",
                  m.course==~0u?-1:int(m.course),int(m.target),(unsigned long)m.map.bytes(),(unsigned long)m.overview.bytes(),
                  (unsigned long)m.atlas.bytes(),(unsigned long)m.objects.bytes(),(unsigned long)m.infoFont.bytes(),(unsigned long)m.infoArt.bytes(),m.infoEmblem,m.infoMk7?" mk7":"",int(m.ds),
                  (unsigned long)m.dsSil.bytes(),where(m.dsSil),(unsigned long)m.dsTile.bytes(),where(m.dsTile),(unsigned long)m.dsAtlas.bytes(),where(m.dsAtlas),
                  (unsigned long)m.dsNames.bytes(),where(m.dsNames),unsigned(m.driftAngle)*360u/65536u,(unsigned long)vramSpaceFree());
}
bool RaceBottomScreen::infoReady() const{return impl_->infoFont.ready&&impl_->infoArt.ready&&impl_->infoQuads;}
unsigned RaceBottomScreen::infoLeadTicks() const{return impl_->infoMk7?0u:InfoInTicks;}
void RaceBottomScreen::Impl::setInfoCup(){
    auto& m=*this;
    // The emblem: the course's cup's menu icon (0-9, FlyCup); none (or an unreadable file): the flag.
    const unsigned retail=m.course<std::size(FlyCup)?FlyCup[m.course]:255u;
    const int emblem=retail<FlyEmblemCount?int(retail):-1;
    if(emblem==m.infoEmblem||!m.infoArt.ready)return; // a restart: already in the cell
    m.infoEmblem=-1;
    // An emblem file: tex3ds RGBA8, 32 x 32, one subtexture, uncompressed (a 21-byte header), then its
    // 8 x 8 tiles in rows (row-major, Morton order inside: Texture::shrinkRgba8). The cell is on whole
    // tiles of the art (LINEAR: the race's VRAM placement comes after), so each tile row is one copy.
    constexpr unsigned Cell=32,Head=21,Bytes=Head+Cell*Cell*4,RowBytes=Cell*8*4;
    const uintptr_t at=reinterpret_cast<uintptr_t>(m.infoArt.gpu.data);
    if(emblem<0||FlyEmblem.w!=Cell||FlyEmblem.h!=Cell||(FlyEmblem.x|FlyEmblem.y)%8||m.infoArt.gpu.fmt!=GPU_RGBA8||
       m.infoArt.gpu.width!=FlyArtSize[0]||m.infoArt.gpu.height!=FlyArtSize[1]||(at>=0x1F000000u&&at<0x1F600000u))return;
    char path[64];std::snprintf(path,sizeof path,FlyEmblemPath,unsigned(emblem));
    std::array<uint8_t,Bytes> file;bool read=false;
    if(pack::size(path)==long(Bytes))read=pack::readInto(path,file.data(),Bytes);
    else if(FILE* f=std::fopen(path,"rb")){read=std::fread(file.data(),1,Bytes,f)==Bytes&&std::fgetc(f)==EOF;std::fclose(f);}
    if(!read||file[0]!=1||file[1]||file[2]!=0x12||file[3]!=GPU_RGBA8||file[4]||file[17]||unsigned(file[18]|file[19]<<8|file[20]<<16)!=Cell*Cell*4)return;
    auto* out=static_cast<uint8_t*>(m.infoArt.gpu.data);
    for(unsigned row=0;row<Cell/8;++row)
        std::memcpy(out+((FlyEmblem.y/8+row)*(FlyArtSize[0]/8)+FlyEmblem.x/8)*64*4,file.data()+Head+row*RowBytes,RowBytes);
    C3D_TexFlush(&m.infoArt.gpu);
    m.infoEmblem=emblem;
}

bool RaceBottomScreen::compose(Renderer& renderer,const BottomView& view){
    auto& m=*impl_;m.miiMap=m.miiRow=false;m.outline=~0u;
    if(!m.target||!m.quads)return false;
    const auto& course=*raceMapCourse(m.course);
    // Animations advance by simulation ticks (presentation runs at 30 or 60 Hz).
    const unsigned elapsed=m.started?std::min<unsigned>(view.tick-m.lastTick,30u):0u;
    m.lastTick=view.tick;m.started=true;
    // Standings: held at the close-up during the course intro; its column also shows a Time Trial's
    // player and ghosts.
#ifndef MKW_RELEASE
    if(m.ds&&m.dsToggleEvery&&view.raceStarted&&(m.dsToggleClock+=elapsed)>=m.dsToggleEvery){m.dsToggleClock=0;m.overviewWanted=!m.overviewWanted;}
#endif
    const bool wanted=m.overviewWanted&&!(m.ds&&!view.racerIconsVisible);
    const bool rowsShown=view.standingsVisible||(m.ds&&view.timeTrial&&view.raceStarted);
    for(unsigned i=0;i<elapsed;++i){
        m.blend=std::clamp(m.blend+(wanted?1.f:-1.f)/12.f,0.f,1.f);
        m.standings=std::clamp(m.standings+(rowsShown?1.f:-1.f)/16.f,0.f,1.f);
        ++m.roulette;
    }
    m.driftTicks+=elapsed;
    if(view.timeTrial&&!m.ds)m.standings=0.f; // Never retain sliding rows from another mode.
    // The rows hide no strip of the screen: the map is centred on its full width.
    const float mapWidth=ScreenW,mapCenterX=ScreenW*.5f;
    // CtrlRaceBase's authored 20-tick entrance: -1 -> 0 in .05 steps.
    m.timerSlide=view.timerVisible?std::min(0.f,m.timerSlide+float(elapsed)*.05f):-1.f;
    m.half=renderer.recordingFrame()&1;m.base=m.quads.get()+m.half*QuadCapacity;m.count=0;
    const bool atlas=m.atlas.ready;

    // 1. Opaque fills: the whole screen, then the standings column (no clear).
    m.add(m.ui->white,0,0,ScreenW,ScreenH,0x0b0f17ffu);
    const unsigned fills=m.count;

    // 2. The map: the view's world rectangle over the course texture. Clamped
    // sampling repeats the baked surround past the course's edges.
    const BottomRacer* local=nullptr;
    for(unsigned i=0;i<view.racers;++i)if(view.racer[i].local)local=&view.racer[i];
    const float frameW=course.x1-course.x0,frameH=course.z1-course.z0;
    const float detailed=course.unitsPerPixel,whole=std::max(frameW/mapWidth,frameH/ScreenH);
    // b: the zoom towards the whole course; sb: the cross-fade into Standings (the close-up keeps its zoom).
    const float b=m.course>=32?1.f:m.ds?0.f:smooth(m.blend),sb=m.ds?smooth(m.blend):0.f;
    const float upp=std::exp(std::log(detailed)*(1-b)+std::log(std::max(whole,detailed))*b);
    // Mario Kart 7 turns the close-up map with the kart so its heading points
    // up; the whole-course view stays north-up, and the turn fades with the zoom.
    const float s=view.mirror?-1.f:1.f;
    float turn=0.f;
    {
        const float hx=s*view.headingX,hz=view.headingZ;
        // Shortest way round, so the blend into the overview never spins a full turn.
        if(hx*hx+hz*hz>1e-8f)turn=std::remainder(-1.5707963f-std::atan2(hz,hx),6.2831853f)*(1-b);
    }
    const float rc=std::cos(turn),rs=std::sin(turn);
    float cx=local?local->x:(course.x0+course.x1)*.5f,cz=local?local->z:(course.z0+course.z1)*.5f;
    // North-up, Mario Kart 7 keeps its view inside the map: the player leaves
    // the centre near the course's edge instead of the view running off it.
    // Turning, the player stays centred (clamped sampling repeats the baked
    // surround past the course's edges).
    auto clampAxis=[](float c,float half,float lo,float hi){return hi-lo<=2*half?(lo+hi)*.5f:std::clamp(c,lo+half,hi-half);};
    {
        const float kx=clampAxis(cx,mapWidth*.5f*upp,course.x0,course.x1),kz=clampAxis(cz,ScreenH*.5f*upp,course.z0,course.z1);
        cx=kx+(cx-kx)*(1-b);cz=kz+(cz-kz)*(1-b);
    }
    cx+=((course.x0+course.x1)*.5f-cx)*b;cz+=((course.z0+course.z1)*.5f-cz)*b;
    // World (x,z) to screen: mirror x, turn about the player, scale.
    auto screenAt=[&](float x,float z,float& outX,float& outY){
        const float dx=s*(x-cx),dz=z-cz;
        outX=mapCenterX+(rc*dx-rs*dz)/upp;outY=MapCY+(rs*dx+rc*dz)/upp;
    };
    const Texture* mapTexture=b>.5f&&m.overview.ready?&m.overview:m.map.ready?&m.map:nullptr;
    const unsigned mapFirst=m.count;
    if(mapTexture&&sb<1.f&&m.count<QuadCapacity){
        auto& q=m.base[m.count++];
        // The screen's corners mapped back to the world (the inverse of
        // screenAt), so in Mirror the map flips about mapCenterX like the
        // markers do, and turns with them about the player. It starts at
        // the screen's left edge.
        for(unsigned k=0;k<4;++k){
            q.x[k]=(k&1)?ScreenW:0.f;q.y[k]=(k&2)?ScreenH:0;
            const float ox=(q.x[k]-mapCenterX)*upp,oy=(q.y[k]-MapCY)*upp;
            const float wx=cx+s*(rc*ox+rs*oy),wz=cz+(-rs*ox+rc*oy);
            q.u[k]=(wx-course.x0)/frameW;q.v[k]=(wz-course.z0)/frameH;
            q.color[k]=0xffffffffu;
        }
    }
    const unsigned mapCount=m.count-mapFirst;

    // Live drivable-object silhouettes sit above the baked map, below markers.
    const unsigned objectFirst=m.count;
    const auto objectArt=mapObjectAtlas(m.course);
    if(m.objects.ready&&sb<.5f)for(unsigned i=0;i<std::min(view.mapObjects,BottomView::MaxMapObjects);++i){
        const auto& item=view.mapObject[i];if(item.art>=objectArt.count)continue;
        const auto& art=objectArt.art[item.art];
        float x[4],y[4];
        for(unsigned k=0;k<4;++k)screenAt(item.x[k],item.z[k],x[k],y[k]);
        if(*std::max_element(x,x+4)<0||*std::min_element(x,x+4)>ScreenW||
           *std::max_element(y,y+4)<0||*std::min_element(y,y+4)>ScreenH)continue;
        if(m.count>=QuadCapacity)break;
        auto& q=m.base[m.count++];
        for(unsigned k=0;k<4;++k){
            q.x[k]=x[k];q.y[k]=y[k];q.color[k]=0xffffffffu;
            q.u[k]=float(art.x+((k&1)?art.w:0))/objectArt.width;
            q.v[k]=float(art.y+((k&2)?art.h:0))/objectArt.height;
        }
    }
    unsigned objectCount=m.count-objectFirst;

    // The Standings picture over the close-up map as the view turns: the emblem tile in the track's
    // colours (lerp(colour, tint, level)) drifting by the race's direction, the shade (up to 24% darker
    // at the bottom), the silhouette (its grey from the vertex colours, 247 at its top to 216).
    const unsigned bgFirst=m.count;unsigned shadeFirst=bgFirst,silFirst=bgFirst;uint32_t bg0=0,bg1=0;
    const float boxMid=(StandingsBox[0]+StandingsBox[2])*.5f;
    // World (x, z) to the Standings screen: the cooked affine, Mirror flipping it about the map area's middle.
    auto standAt=[&](float x,float z,float& outX,float& outY){
        const auto& c=*m.dsCourse;outX=c.a*x+c.b*z+c.e;outY=c.c*x+c.d*z+c.f;if(view.mirror)outX=2*boxMid-outX;};
    if(m.ds&&sb>0){
        const auto& c=*m.dsCourse;const uint32_t alpha=uint32_t(std::lround(sb*255.f));
        const float ox=std::fmod(float(m.driftTicks)*DsDrift*m.driftCos,64.f),oy=std::fmod(float(m.driftTicks)*DsDrift*m.driftSin,64.f);
        m.addTexels(-ox,-oy,ScreenW-ox,ScreenH-oy,64.f,64.f,0,0,ScreenW,ScreenH,0xffffff00u|alpha);
        auto tint=[](unsigned v){return unsigned(std::min(255.f,float(v)*StandingsTintScale+StandingsTintAdd));};
        bg0=uint32_t(c.r)<<24|uint32_t(c.g)<<16|uint32_t(c.b_)<<8|0xffu;bg1=tint(c.r)<<24|tint(c.g)<<16|tint(c.b_)<<8|0xffu;
        shadeFirst=m.count;
        if(atlas&&m.add(m.ui->white,0,0,ScreenW,ScreenH,0)){auto& q=m.base[m.count-1];q.color[2]=q.color[3]=uint32_t(std::lround(60.f*sb));}
        silFirst=m.count;
        const float sx0=StandingsSilX,sy0=StandingsSilY,sx1=ScreenW,sy1=ScreenH,tw=sx1-sx0,th=sy1-sy0;
        if(m.addTexels(view.mirror?tw:0.f,0,view.mirror?0.f:tw,th,StandingsSilSize,StandingsSilSize,view.mirror?2*boxMid-sx1:sx0,sy0,view.mirror?2*boxMid-sx0:sx1,sy1,0)){
            auto& q=m.base[m.count-1];
            auto grey=[&](float y){const unsigned g=unsigned(std::lround(250.f-34.f*y/ScreenH));return g<<24|g<<16|std::min(255u,g+4)<<8|alpha;};
            q.color[0]=q.color[1]=grey(sy0);q.color[2]=q.color[3]=grey(sy1);
        }
        ++m.dsFrames;
    }
    const unsigned bgCount=shadeFirst-bgFirst,shadeCount=silFirst-shadeFirst,silCount=m.count-silFirst;
    const bool bgOpaque=bgCount&&sb>=1.f&&!mapCount;

    // 3. Everything else from the atlas, in drawing order.
    unsigned overlayFirst=m.count;
    struct {float x,y,hx,hy;} pointer{};bool hasPointer=false;
    auto onMap=[&](float x,float y,float margin){return x>-margin&&x<ScreenW+margin&&y>-margin&&y<ScreenH+margin;};
    if(atlas){
        // Start line (KMP checkpoint 0): Mario Kart 7's checker marker across the road.
        {
            float ax,ay,bx,by;
            if(sb>=.5f){
                // Standings: the checker trimmed to the road's middle 13 px.
                standAt(course.startLeft[0],course.startLeft[1],ax,ay);standAt(course.startRight[0],course.startRight[1],bx,by);
                const float mx=(ax+bx)*.5f,my=(ay+by)*.5f,l=std::sqrt((bx-ax)*(bx-ax)+(by-ay)*(by-ay)),half=std::min(l*.5f,6.5f);
                if(l>1e-3f){const float ux=(bx-ax)/l,uy=(by-ay)/l;ax=mx-ux*half;ay=my-uy*half;bx=mx+ux*half;by=my+uy*half;}
            }else{screenAt(course.startLeft[0],course.startLeft[1],ax,ay);screenAt(course.startRight[0],course.startRight[1],bx,by);}
            const float dx=bx-ax,dy=by-ay,length=std::sqrt(dx*dx+dy*dy);
            if(sb>=.5f&&length>1.f){
                // Standings: the minimap's start checker, two rows of 2.6 px cells in a black frame.
                const float ux=dx/length,uy=dy/length,nx=-uy,ny=ux,half=length*.5f,mx=(ax+bx)*.5f,my=(ay+by)*.5f;
                auto cell=[&](float s0,float s1,float o0,float o1,uint32_t colour){
                    const float x[4]={mx+ux*s0+nx*o0,mx+ux*s1+nx*o0,mx+ux*s0+nx*o1,mx+ux*s1+nx*o1};
                    const float y[4]={my+uy*s0+ny*o0,my+uy*s1+ny*o0,my+uy*s0+ny*o1,my+uy*s1+ny*o1};
                    m.addCorners(m.ui->white,x,y,colour);
                };
                cell(-half-1,half+1,-3.6f,3.6f,0x000000ffu);
                const unsigned n=std::max(3u,unsigned(2*half/2.6f));
                for(unsigned k=0;k<n&&k<10;++k)for(unsigned row=0;row<2;++row)
                    cell(-half+2*half*float(k)/float(n),-half+2*half*float(k+1)/float(n),(float(row)-1)*2.6f,float(row)*2.6f,(k+row)%2?0x181818ffu:0xffffffffu);
            }else if(length>1.f&&(onMap(ax,ay,8)||onMap(bx,by,8))){
                const float thick=std::clamp(length/8.f,3.f,6.f);
                const unsigned tiles=std::clamp(unsigned(std::lround(length/thick)),1u,24u);
                const float ux=dx/length,uy=dy/length,nx=-uy*thick*.5f,ny=ux*thick*.5f;
                for(unsigned t=0;t<tiles;++t){
                    const float s0=length*float(t)/float(tiles),s1=length*float(t+1)/float(tiles);
                    const float x[4]={ax+ux*s0-nx,ax+ux*s1-nx,ax+ux*s0+nx,ax+ux*s1+nx};
                    const float y[4]={ay+uy*s0-ny,ay+uy*s1-ny,ay+uy*s0+ny,ay+uy*s1+ny};
                    m.addCorners(m.ui->mk7[unsigned(Mk7Marker::StartLine)],x,y,0xffffffffu);
                }
            }
        }
        // Items on the course: Mario Kart 7's map markers (the Wii fake box).
        for(unsigned i=0;i<(sb<.5f?std::min<unsigned>(view.items,BottomView::MaxItems):0u);++i){
            const auto& it=view.item[i];
            float x,y;screenAt(it.x,it.z,x,y);
            if(it.marker==unsigned(BottomMarker::BlastRed)||it.marker==unsigned(BottomMarker::BlastBlue)){
                // A quarter ring texture, mirrored into the four quadrants.
                const float r=std::clamp(it.radius/upp,8.f,36.f);
                if(!onMap(x,y,r))continue;
                const auto& art=m.ui->mk7[it.marker];
                m.add(art,x-r,y-r,x,y,0xffffffffu,false,true);
                m.add(art,x,y-r,x+r,y,0xffffffffu,true,true);
                m.add(art,x-r,y,x,y+r,0xffffffffu,false,false);
                m.add(art,x,y,x+r,y+r,0xffffffffu,true,false);
                continue;
            }
            if(!onMap(x,y,MarkerSize))continue;
            const auto& art=it.marker==unsigned(BottomMarker::FakeBox)?m.ui->fakeBox:m.ui->mk7[it.marker];
            const float h=MarkerSize*.5f;
            m.add(art,x-h,y-h,x+h,y+h,0xffffffffu,false,it.marker!=unsigned(BottomMarker::FakeBox));
        }
        // Racers: the worst position first, the local racer last (retail z).
        std::array<const BottomRacer*,12> order{};unsigned n=0;
        for(unsigned i=0;i<std::min(view.racers,12u);++i)if(!view.racer[i].local)order[n++]=&view.racer[i];
        std::sort(order.begin(),order.begin()+n,[](const BottomRacer* a,const BottomRacer* b){return a->rank>b->rank;});
        if(local)order[n++]=local;
        // The course preview shows the map at the grid without the racers (user, Nov 25).
        if(!view.racerIconsVisible)n=0;
        for(unsigned i=0;i<n;++i){
            const auto& r=*order[i];
            float x,y;if(sb>=.5f)standAt(r.x,r.z,x,y);else screenAt(r.x,r.z,x,y);
            const float scale=r.local?LocalScale:OtherScale;
            const float face=FaceUnits*scale*Unit*(sb>=.5f?(r.local?DsLocalFace:DsOtherFace):1.f);
            if(!onMap(x,y,face))continue;
            if(r.local&&sb>=.5f){
                // Standings: the heading pointer in place of the searchlight (added after the overlay).
                const auto& c=*m.dsCourse;float hx=c.a*view.headingX+c.b*view.headingZ,hy=c.c*view.headingX+c.d*view.headingZ;
                if(view.mirror)hx=-hx;
                const float l=std::sqrt(hx*hx+hy*hy);if(l>1e-6f){pointer={x,y,hx/l,hy/l};hasPointer=true;}
            }else if(r.local){
                // The searchlight points along the kart, its apex on the face.
                const float mx=s*view.headingX,mz=view.headingZ;
                float hx=rc*mx-rs*mz,hy=rs*mx+rc*mz;const float l=std::sqrt(hx*hx+hy*hy);
                if(l>1e-4f){
                    hx/=l;hy/=l;const float px=-hy,py=hx;
                    const float tx=x+hx*LightH,ty=y+hy*LightH,w=LightW*.5f;
                    const float cxs[4]={tx-px*w,tx+px*w,x-px*w,x+px*w},cys[4]={ty-py*w,ty+py*w,y-py*w,y+py*w};
                    m.addCorners(m.ui->searchlight,cxs,cys,LightColor);
                }
            }
            const auto& art=m.ui->faces[std::min<unsigned>(r.character,23)];
            const uint32_t alpha=255;
            const float hf=face*.5f;
            // common_w039_map_chara_icon_{red,blue}: recolor only the
            // two shadow copies. White RGB + original alpha avoids tinting faces.
            const uint32_t outline=(r.team==RaceTeam::Red?0xff000000u:r.team==RaceTeam::Blue?0x0000ff00u:0u)|alpha;
            auto portrait=[&](const RaceMapRect& rect,float half,uint32_t color){
                const float angle=view.mirror?-r.hitAngle:r.hitAngle; // spins turn the other way in Mirror
                const float c=std::cos(angle),sn=std::sin(angle);float xs[4],ys[4];
                for(unsigned j=0;j<4;++j){const float dx=(j&1)?half:-half,dy=(j&2)?half:-half;
                    xs[j]=x+c*dx-sn*dy;ys[j]=y+sn*dx+c*dy;}
                m.addCorners(rect,xs,ys,color);
            };
            const bool native=r.character>=24&&m.mii.ready;
            if(r.local&&!native){
                // The outline: white with the face's alpha, tinted. The 256x256
                // atlas draws it from the face with colour0 white; in black (no
                // team) the face's colour times black is the same black.
                const uint32_t tint=(outline&0xffffff00u)|80u;
                portrait(art,face*.53f,tint);if(tint&0xffffff00u)m.outline=m.count-1;
            }
            portrait(art,hf,0xffffff00u|alpha);
            if(native){auto& q=m.miiQuads[m.half][0];q=m.base[--m.count];for(unsigned k=0;k<4;++k){q.u[k]=(k&1)?1.f:0.f;q.v[k]=(k&2)?1.f:0.f;}m.miiMap=true;}
        }
    }
    unsigned overlayCount=m.count-overlayFirst;
    // The heading pointer: the atlas's arrow (pointing up) turned to the kart's heading on the screen.
    const unsigned pointerFirst=m.count;
    if(hasPointer&&m.dsAtlas.ready&&m.count<QuadCapacity){
        const auto& r=StandingsPointer;const float A=float(StandingsAtlasSize),h=float(r.w)*.5f;
        auto& q=m.base[m.count++];
        for(unsigned k=0;k<4;++k){
            const float dx=(k&1)?h:-h,dy=(k&2)?h:-h;
            q.x[k]=pointer.x-pointer.hy*dx-pointer.hx*dy;q.y[k]=pointer.y+pointer.hx*dx-pointer.hy*dy;
            q.u[k]=(float(r.x)+((k&1)?float(r.w):0.f))/A;q.v[k]=(float(r.y)+((k&2)?float(r.h):0.f))/A;q.color[k]=0xffffffffu;
        }
    }
    const unsigned pointerCount=m.count-pointerFirst;

    // 4. The standings rows, then the time (one atlas batch). The Standings view: its fixed number
    // column first (its own atlas), the rows in the race atlas, then their names (the names texture).
    const unsigned numberFirst=m.count;
    struct NameAt{unsigned racer;float x,y,w;};std::array<NameAt,12> names{};unsigned nameRows=0;
    const float dsSlide=(1-smooth(m.standings))*-DsRowSlide;
    if(m.ds&&atlas&&m.standings>0&&m.dsAtlas.ready){
        const float A=float(StandingsAtlasSize);
        for(unsigned k=0;k<std::min(view.racers,12u);++k){const auto& r=StandingsNumbers[k];
            m.addTexels(r.x,r.y,r.x+r.w,r.y+r.h,A,A,dsSlide,float(k)*DsSlot,dsSlide+r.w,float(k)*DsSlot+r.h,0xffffffffu);}
    }
    const unsigned numberCount=m.count-numberFirst;
    const unsigned topFirst=m.count;
    if(atlas&&m.standings>0&&m.ds){
        const auto& R=*m.ui;const float aw=R.size[0],ah=R.size[1];
        if(!m.rowsSet){for(unsigned i=0;i<view.racers;++i)m.row[i]=float(std::clamp<unsigned>(view.racer[i].rank,1,12)-1);m.rowsSet=true;}
        for(unsigned i=0;i<std::min(view.racers,12u);++i){
            const float target=float(std::clamp<unsigned>(view.racer[i].rank,1,12)-1);
            for(unsigned t=0;t<elapsed;++t)m.row[i]+=(target-m.row[i])*.22f;
            if(std::fabs(target-m.row[i])<.01f)m.row[i]=target;
        }
        auto drawRow=[&](unsigned i){
            const auto& r=view.racer[i];
            // A tab in the close-up (face and item), widened into a row with the switch.
            const float x=DsRowX+dsSlide,y=std::floor(m.row[i]*DsSlot+.5f);
            const float tab=r.local?DsTabLocal:DsTab,full=r.local?DsRowLocal:DsRow,w=std::round(tab+(full-tab)*sb);
            const auto& f=r.local?R.rowPlayer:R.rowOther;const float cap=5.f,fx=f.x,fy=f.y,fw=f.w,fh=f.h;
            m.addTexels(fx,fy,fx+cap,fy+fh,aw,ah,x,y,x+cap,y+fh,0xffffffffu);
            m.addTexels(fx+cap,fy,fx+fw-cap,fy+fh,aw,ah,x+cap,y,x+w-cap,y+fh,0xffffffffu);
            m.addTexels(fx+fw-cap,fy,fx+fw,fy+fh,aw,ah,x+w-cap,y,x+w,y+fh,0xffffffffu);
            m.add(R.faces[std::min<unsigned>(r.character,23)],x+DsFaceX,y+DsIconY,x+DsFaceX+DsFace,y+DsIconY+DsFace);
            if(r.character>=24&&m.mii.ready){auto& q=m.miiQuads[m.half][1];q=m.base[--m.count];for(unsigned k=0;k<4;++k){q.u[k]=(k&1)?1.f:0.f;q.v[k]=(k&2)?1.f:0.f;}m.miiRow=true;}
            if(r.item<R.items.size()&&R.items[r.item].w)m.add(R.items[r.item],x+w-DsItemRight,y+DsIconY,x+w-DsItemRight+DsFace,y+DsIconY+DsFace);
            if(nameRows<names.size())names[nameRows++]={i,x,y,w};
        };
        for(unsigned i=0;i<std::min(view.racers,12u);++i)if(!view.racer[i].local)drawRow(i);
        for(unsigned i=0;i<std::min(view.racers,12u);++i)if(view.racer[i].local)drawRow(i);
    }else if(atlas&&m.standings>0){
        const auto& R=*m.ui;
        // Rows slide in from the left once the race starts and move between
        // places when positions change (Mario Kart 7).
        const float slide=(1-smooth(m.standings))*-RowSlide;
        if(!m.rowsSet){for(unsigned i=0;i<view.racers;++i)m.row[i]=float(std::clamp<unsigned>(view.racer[i].rank,1,12)-1);m.rowsSet=true;}
        for(unsigned i=0;i<std::min(view.racers,12u);++i){
            const float target=float(std::clamp<unsigned>(view.racer[i].rank,1,12)-1);
            for(unsigned t=0;t<elapsed;++t)m.row[i]+=(target-m.row[i])*.22f;
            if(std::fabs(target-m.row[i])<.01f)m.row[i]=target;
        }
        auto drawRow=[&](unsigned i){
            const auto& r=view.racer[i];
            // Faces and items sit 1 px above the box's centre: its border is heavier below.
            const float x=RowX+slide,y=rowY(m.row[i]),icon=R.icon,iy=y+std::floor((R.rowH-icon)*.5f)-1;
            m.add(r.local?R.rowPlayer:R.rowOther,x,y,x+R.rowW,y+R.rowH);
            if(!view.battle){
                const auto& ord=R.ordinals[std::clamp<unsigned>(r.rank,1,12)-1];
                const float oy=y+std::floor((R.rowH-ord.h)*.5f);
                m.add(ord,x+R.ordinalX,oy,x+R.ordinalX+ord.w,oy+ord.h);
            }else{
                // Battle: the score where the ordinal goes, in the team's colour.
                const unsigned v=std::min(unsigned(r.score),999u);char text[4];unsigned n=0;
                if(v>=100)text[n++]=char('0'+v/100);
                if(v>=10)text[n++]=char('0'+v/10%10);
                text[n++]=char('0'+v%10);
                const uint32_t color=r.team==RaceTeam::Red?0xff3030ffu:r.team==RaceTeam::Blue?0x4080ffffu:0xffffffffu;
                float dx=x+R.ordinalX;
                for(unsigned j=0;j<n;++j){const auto& d=R.scoreDigits[text[j]-'0'];const float dy=y+std::floor((R.rowH-d.h)*.5f);
                    m.add(d,dx,dy,dx+d.w,dy+d.h,color);dx+=d.w;}
            }
            m.add(R.rowFaces[std::min<unsigned>(r.character,23)],x+R.faceX,iy,x+R.faceX+icon,iy+icon);
            if(r.character>=24&&m.mii.ready){auto& q=m.miiQuads[m.half][1];q=m.base[--m.count];for(unsigned k=0;k<4;++k){q.u[k]=(k&1)?1.f:0.f;q.v[k]=(k&2)?1.f:0.f;}m.miiRow=true;}
            if(r.item<R.items.size()&&R.items[r.item].w)m.add(R.items[r.item],x+R.itemX,iy,x+R.itemX+icon,iy+icon);
        };
        for(unsigned i=0;i<std::min(view.racers,12u);++i)if(!view.racer[i].local)drawRow(i);
        for(unsigned i=0;i<std::min(view.racers,12u);++i)if(view.racer[i].local)drawRow(i);
    }
    if(atlas&&view.timerVisible){
        // CtrlRaceTime_refresh: minutes clamp at 99'59"999.
        unsigned ms=view.milliseconds,minutes=ms/60000,seconds=ms/1000%60,milli=ms%1000;
        if(minutes>99){minutes=99;seconds=59;milli=999;}
        const unsigned digit[7]={minutes/10,minutes%10,seconds/10,seconds%10,milli/100,milli/10%10,milli%10};
        // RaceTimerPanes: race_null, time_01, coron01, time_02, time_03, coron00, time_04..06,
        // then the TIME label (set_p) with Timer: Full. Right-aligned at TimerRight.
        const float right=RaceTimerPanes[8].x+RaceTimerPanes[8].w*.5f;
        const float sq=m.timerSlide*m.timerSlide;
        const float lift=400.f*1.0005f*(sq>=.1f?sq-.1f:-sq)*TimerScale*Unit;
        const float ox=TimerRight-right*TimerScale,oy=TimerTop+RaceTimerPanes[0].h*.5f*TimerScale-lift;
        const RaceMapRect* art[10]={&m.ui->digits[digit[0]],&m.ui->digits[digit[1]],&m.ui->colon,&m.ui->digits[digit[2]],&m.ui->digits[digit[3]],
                                    &m.ui->dot,&m.ui->digits[digit[4]],&m.ui->digits[digit[5]],&m.ui->digits[digit[6]],&m.ui->timeLabel};
        for(unsigned i=0;i<(view.timerLabel?10u:9u)&&m.count<QuadCapacity;++i){
            const auto& p=RaceTimerPanes[i];
            // Each piece at its cooked pixel size, on whole pixels, centred on its pane:
            // no resampling (the TIME pane is 17.5 px tall at this scale, its art 18).
            const float x=ox+p.x*TimerScale,y=oy-p.y*TimerScale;
            const float x0=std::floor(x-art[i]->w*.5f+.5f),y0=std::floor(y-art[i]->h*.5f+.5f);
            if(!m.add(*art[i],x0,y0,x0+art[i]->w,y0+art[i]->h))break;
            auto& q=m.base[m.count-1];
            for(unsigned k=0;k<4;++k)q.color[k]=TimerGold[k];
        }
    }
    const unsigned topCount=m.count-topFirst;
    // The names fade in over the switch's second half (as the rows reach their length), cut to the row.
    const unsigned nameFirst=m.count;
    if(m.ds&&m.dsNames.ready&&m.blend>.5f){
        const uint32_t alpha=uint32_t(std::lround(std::min(1.f,(m.blend-.5f)/.5f)*255.f));
        const float ny=std::floor(DsCentre-float(StandingsCapTop+StandingsCapBottom)*.5f+.5f);
        for(unsigned k=0;k<nameRows;++k){
            const auto& n=names[k];const float crop=std::min(m.nameW[n.racer],std::max(0.f,n.w-40.f));if(crop<1.f)continue;
            const float row=float(n.racer*DsNameRow),x=n.x+DsNameX-m.nameInk[n.racer];
            m.addTexels(0,row,crop,row+14.f,float(DsNamesW),float(DsNamesH),x,n.y+ny,x+crop,n.y+ny+14.f,0xffffff00u|alpha);
        }
    }
    const unsigned nameCount=m.count-nameFirst;
    if(m.ds)m.dsQuadPeak=std::max(m.dsQuadPeak,m.count);
    // 5. The course intro's info box over the map: the frame art (its dark inside, the long edges'
    // strips, the short edges with the corners at this phase), the medallion (shadow, the ring turned
    // with the gold, the cup emblem or the flag), then its text. The gold runs round as on the menus'
    // selected buttons (common_w004_menu's Loop: the pattern moves left one period in 40 frames), here
    // once every InfoLoopTicks; the short edges step through FlyPhases baked phases and the long edges
    // scroll with them (anchored to the box at rest while it unrolls).
    m.infoArtCount=m.infoTextCount=0;
    if(m.infoQuads&&m.infoFont.ready&&m.infoArt.ready){
        for(unsigned i=0;i<elapsed;++i){
            m.infoIn=m.infoMk7?(view.infoShown?InfoInTicks:0u):view.infoShown?std::min(m.infoIn+1,InfoInTicks):m.infoIn?m.infoIn-1:0u;
            if(m.infoIn)++m.infoTicks;}
        if(m.infoIn&&view.infoCourse!=~0u){
            if(view.infoCourse!=m.infoLaidCourse)m.layoutInfo(view.infoCourse);
            auto* q0=m.infoQuads.get()+m.half*InfoCapacity;unsigned na=0,nt=0;
            const float aw=float(FlyArtSize[0]),ah=float(FlyArtSize[1]);
            // An art quad: texels u0..u1 x v0..v1 on corners (x, y) top left, top right, bottom left, bottom right.
            auto quad=[&](float u0,float v0,float u1,float v1,const float (&x)[4],const float (&y)[4],uint32_t colour){
                if(na>=InfoArtCapacity)return;auto& q=q0[na++];
                for(unsigned c=0;c<4;++c){q.x[c]=x[c];q.y[c]=y[c];q.color[c]=colour;}
                q.u[0]=u0/aw;q.v[0]=v0/ah;q.u[1]=u1/aw;q.v[1]=v0/ah;q.u[2]=u0/aw;q.v[2]=v1/ah;q.u[3]=u1/aw;q.v[3]=v1/ah;
            };
            auto art=[&](float u0,float v0,float u1,float v1,float x0,float y0,float x1,float y1,uint32_t colour=0xffffffffu){
                quad(u0,v0,u1,v1,{x0,x1,x0,x1},{y0,y0,y1,y1},colour);
            };
            const float k=float(m.infoIn),W=float(FlyBoxW),H=m.infoH,E=float(FlyEdge),ox=InfoX,oy=InfoY;
            const float cx=ox+W-InfoRingRight,cy=oy+InfoRingY,ringR=float(FlyRing.w)*.5f;
            const unsigned phase=(m.infoTicks*FlyPhases/InfoLoopTicks)%FlyPhases;
            // The entrance at k (MK7-style: settled at once).
            const float pop=m.infoMk7?1.f:backOut(k/InfoPopTicks),textAlpha=m.infoMk7?1.f:std::clamp((k-InfoTextAt)/(float(InfoInTicks)-InfoTextAt),0.f,1.f);
            if(m.infoMk7||k>=InfoUnrollAt){
                // The box's left end: from behind the medallion to ox, on whole pixels.
                const float bl=m.infoMk7?ox:std::floor(ox+(1.f-cubicOut((k-InfoUnrollAt)/InfoUnrollTicks))*(cx-12.f-ox)+.5f);
                const float inU=float(FlyInside.x)+float(FlyInside.w)*.5f,inV=float(FlyInside.y)+float(FlyInside.h)*.5f;
                // The middle: the inside, then the bottom gold (the stretched middle's last rows in the
                // rows' nine-slice; the drop shadow below it is the bottom edge).
                const float lineY=oy+E+(H-2*E)*FlyLineTop;
                art(inU,inV,inU,inV,bl+E,oy+E,ox+W-E,lineY);
                // A long edge: its periodic strip (one wrap texel each end) from the phase's offset at
                // the box's left end (box px from the rested box's corner), split where the period wraps.
                auto edge=[&](const FlyRect& r,float y0,float y1){
                    const float texels=float(r.w-2),perPx=texels/FlyPeriod;
                    float u=std::fmod(FlyStripStart+float(phase)*FlyPeriod/float(FlyPhases)+(bl-ox),FlyPeriod)*perPx,x=bl+E,left=ox+W-E-x;
                    for(unsigned pieces=0;left>.01f&&pieces<3;++pieces){
                        if(texels-u<1e-3f)u=0;
                        const float run=std::min(left,(texels-u)/perPx);
                        art(float(r.x)+1+u,float(r.y),float(r.x)+1+u+run*perPx,float(r.y+r.h),x,y0,x+run,y1);
                        x+=run;left-=run;u=0;
                    }
                };
                edge(FlyTop,oy,oy+E);edge(FlyLine,lineY,oy+H-E);edge(FlyBottom,oy+H-E,oy+H);
                const float sx=float(FlySide.x+(phase%15)*10),sy=float(FlySide.y+(phase/15)*(FlySide.h+2)),sh=float(FlySide.h);
                for(unsigned side=0;side<2;++side){
                    const float u=sx+float(side*5),x=side?ox+W-E:bl;
                    art(u,sy,u+E,sy+E,x,oy,x+E,oy+E);
                    art(u,sy+E,u+E,sy+sh-E,x,oy+E,x+E,oy+H-E);
                    art(u,sy+sh-E,u+E,sy+sh,x,oy+H-E,x+E,oy+H);
                }
            }
            if(pop>.02f){
                // The medallion over the box: its shadow, the ring (its bright arc one turn a gold loop),
                // the emblem (its 32 px cell) or the flag, all 1:1 at rest.
                const float rr=ringR*pop,turn=float(phase)*6.2831853f/float(FlyPhases),c=std::cos(turn),sn=std::sin(turn);
                const float u0=float(FlyRing.x),v0=float(FlyRing.y),u1=u0+float(FlyRing.w),v1=v0+float(FlyRing.h);
                art(u0,v0,u1,v1,cx+InfoShadowX-rr,cy+InfoShadowY-rr,cx+InfoShadowX+rr,cy+InfoShadowY+rr,InfoShadowColour);
                float x[4],y[4];
                for(unsigned i=0;i<4;++i){const float px=i&1?rr:-rr,py=i&2?rr:-rr;x[i]=cx+px*c-py*sn;y[i]=cy+px*sn+py*c;}
                quad(u0,v0,u1,v1,x,y,0xffffffffu);
                const FlyRect& e=m.infoEmblem>=0?FlyEmblem:FlyFlag;const float h=float(e.w)*.5f*pop;
                art(float(e.x),float(e.y),float(e.x+e.w),float(e.y+e.h),cx-h,cy-h,cx+h,cy+h);
            }
            const float fw=float(FlyFontSize[0]),fh=float(FlyFontSize[1]);
            for(unsigned i=0;textAlpha>0&&i<m.infoGlyphs&&nt<InfoCapacity-InfoArtCapacity;++i){
                const auto& lg=m.infoGlyph[i];const auto& g=FlyGlyphs[lg.glyph];
                const float x0=ox+lg.x+float(g.dx)*lg.sx,y0=oy+lg.y+float(g.dy),x1=x0+float(g.w)*lg.sx,y1=y0+float(g.h);
                auto& q=q0[InfoArtCapacity+nt++];
                q.x[0]=x0;q.y[0]=y0;q.x[1]=x1;q.y[1]=y0;q.x[2]=x0;q.y[2]=y1;q.x[3]=x1;q.y[3]=y1;
                const float u0=float(g.x)/fw,v0=float(g.y)/fh,u1=float(g.x+g.w)/fw,v1=float(g.y+g.h)/fh;
                q.u[0]=u0;q.v[0]=v0;q.u[1]=u1;q.v[1]=v0;q.u[2]=u0;q.v[2]=v1;q.u[3]=u1;q.v[3]=v1;
                const uint32_t colour=(lg.colour&~0xffu)|uint32_t(std::lround(float(lg.colour&0xffu)*textAlpha));
                for(auto& c:q.color)c=colour;
            }
            m.infoArtCount=na;m.infoTextCount=nt;++m.infoFrames;m.infoQuadPeak=std::max(m.infoQuadPeak,na+nt);
        }
    }
    const unsigned fadeFirst=m.count;
    if(view.fade>0)m.add(m.ui->white,0,0,ScreenW,ScreenH,uint32_t(std::lround(view.fade*255.f)));
    // NW4R default material: lerp(C0 = transparent black, C1 = white, texture) x vertex colour.
    // The full-screen map hides the fill and the clear, and needs no blend:
    // race maps and overviews are ETC1 (no alpha), drawn in white. Battle
    // maps are RGBA4 and keep all three.
    // The Standings tile covers the screen opaquely once the switch is done.
    const bool covered=skipCoveredFill&&(mapCount||bgOpaque)&&m.course<32;
    m.b={fills,mapFirst,mapCount,objectFirst,objectCount,overlayFirst,overlayCount,topFirst,topCount,fadeFirst,m.half,m.outline,mapTexture,covered,atlas};
    m.b.bgFirst=bgFirst;m.b.bgCount=bgCount;m.b.shadeFirst=shadeFirst;m.b.shadeCount=shadeCount;m.b.silFirst=silFirst;m.b.silCount=silCount;
    m.b.pointerFirst=pointerFirst;m.b.pointerCount=pointerCount;m.b.numberFirst=numberFirst;m.b.numberCount=numberCount;
    m.b.nameFirst=nameFirst;m.b.nameCount=nameCount;m.b.bg0=bg0;m.b.bg1=bg1;m.b.bgOpaque=bgOpaque;
    return true;
}

void RaceBottomScreen::submit(Renderer& renderer){
    auto& m=*impl_;const auto& b=m.b;
    if(b.atlas&&b.fills&&!b.covered)renderer.drawScreenMaterialQuadsInPlace(&m.atlas,m.base,b.fills,0x00000000u,0xffffffffu);
    if(!b.atlas&&!b.covered)renderer.drawScreenMaterialQuadsInPlace(nullptr,m.base,b.fills,0,0xffffffffu);
    // That fill is opaque over the whole screen (battle arenas, whose maps
    // keep it): the frame's clear is hidden too (A/B: no-bottom-fill-cover.txt).
    if(!b.covered&&b.fills&&fillCover)renderer.bottomCovered();
    if(b.mapCount)renderer.drawScreenMaterialQuadsInPlace(b.mapTexture,m.base+b.mapFirst,b.mapCount,0x00000000u,0xffffffffu,b.covered);
    if(b.covered)renderer.bottomCovered();
    if(b.objectCount)renderer.drawScreenMaterialQuadsInPlace(&m.objects,m.base+b.objectFirst,b.objectCount,0x00000000u,0xffffffffu);
    // The Standings picture: the tile in the track's colours, its shade, the silhouette.
    if(b.bgCount)renderer.drawScreenMaterialQuadsInPlace(&m.dsTile,m.base+b.bgFirst,b.bgCount,b.bg0,b.bg1,b.bgOpaque);
    if(b.shadeCount)renderer.drawScreenMaterialQuadsInPlace(&m.atlas,m.base+b.shadeFirst,b.shadeCount,0x00000000u,0xffffffffu);
    if(b.silCount)renderer.drawScreenMaterialQuadsInPlace(&m.dsSil,m.base+b.silFirst,b.silCount,0x00000000u,0xffffffffu);
    if(b.outline>=b.overlayFirst&&b.outline<b.overlayFirst+b.overlayCount){
        // lerp(white, white, face) = white RGB with the face's alpha, as its silhouette.
        const unsigned before=b.outline-b.overlayFirst,after=b.overlayCount-before-1;
        if(before)renderer.drawScreenMaterialQuadsInPlace(&m.atlas,m.base+b.overlayFirst,before,0x00000000u,0xffffffffu);
        renderer.drawScreenMaterialQuadsInPlace(&m.atlas,m.base+b.outline,1,0xffffff00u,0xffffffffu);
        if(after)renderer.drawScreenMaterialQuadsInPlace(&m.atlas,m.base+b.outline+1,after,0x00000000u,0xffffffffu);
    }else if(b.overlayCount)renderer.drawScreenMaterialQuadsInPlace(&m.atlas,m.base+b.overlayFirst,b.overlayCount,0x00000000u,0xffffffffu);
    if(m.miiMap)renderer.drawScreenMaterialQuadsInPlace(&m.mii,&m.miiQuads[b.mii][0],1,0,0xffffffffu);
    if(b.pointerCount)renderer.drawScreenMaterialQuadsInPlace(&m.dsAtlas,m.base+b.pointerFirst,b.pointerCount,0x00000000u,0xffffffffu);
    if(b.numberCount)renderer.drawScreenMaterialQuadsInPlace(&m.dsAtlas,m.base+b.numberFirst,b.numberCount,0x00000000u,0xffffffffu);
    if(b.topCount){
        if(b.atlas)renderer.drawScreenMaterialQuadsInPlace(&m.atlas,m.base+b.topFirst,b.topCount,0x00000000u,0xffffffffu);
        else renderer.drawScreenMaterialQuadsInPlace(nullptr,m.base+b.topFirst,b.topCount,0,0xffffffffu);
    }
    if(m.miiRow)renderer.drawScreenMaterialQuadsInPlace(&m.mii,&m.miiQuads[b.mii][1],1,0,0xffffffffu);
    if(b.nameCount)renderer.drawScreenMaterialQuadsInPlace(&m.dsNames,m.base+b.nameFirst,b.nameCount,0x00000000u,0xffffffffu);
    // The info box: lerp(transparent black, white, texture) x vertex colour, art then glyphs.
    if(m.infoArtCount)renderer.drawScreenMaterialQuadsInPlace(&m.infoArt,m.infoQuads.get()+b.mii*InfoCapacity,m.infoArtCount,0x00000000u,0xffffffffu);
    if(m.infoTextCount)renderer.drawScreenMaterialQuadsInPlace(&m.infoFont,m.infoQuads.get()+b.mii*InfoCapacity+InfoArtCapacity,m.infoTextCount,0x00000000u,0xffffffffu);
    if(m.count>b.fadeFirst)renderer.drawScreenMaterialQuadsInPlace(b.atlas?&m.atlas:nullptr,m.base+b.fadeFirst,m.count-b.fadeFirst,0x00000000u,0xffffffffu);
}

void RaceBottomScreen::draw(Renderer& renderer,const BottomView& view){
    // Pause pages and the like: what follows on the lower LCD is not ours.
    impl_->shownValid=false;
    if(compose(renderer,view))submit(renderer);
}

// Everything the composed frame draws: its quads, batches and textures.
uint64_t RaceBottomScreen::frameHash() const{
    const auto& m=*impl_;const auto& b=m.b;
    uint32_t a=2166136261u,c=0x9e3779b9u;
    auto word=[&](uint32_t w){a=(a^w)*16777619u;c=(c^w)*0x27d4eb2du;c^=c>>15;};
    auto bytes=[&](const void* p,size_t n){const auto* q=static_cast<const uint8_t*>(p);
        for(size_t i=0;i+4<=n;i+=4){uint32_t w;std::memcpy(&w,q+i,4);word(w);}};
    for(uint32_t w:std::initializer_list<uint32_t>{b.fills,b.mapFirst,b.mapCount,b.objectFirst,b.objectCount,b.overlayFirst,b.overlayCount,b.topFirst,b.topCount,
                    b.fadeFirst,b.outline,m.count,unsigned(b.covered),unsigned(b.atlas),unsigned(m.miiMap),unsigned(m.miiRow),m.miiGeneration})word(w);
    word(uint32_t(reinterpret_cast<uintptr_t>(b.mapTexture)));
    for(uint32_t w:std::initializer_list<uint32_t>{b.bgFirst,b.bgCount,b.shadeFirst,b.shadeCount,b.silFirst,b.silCount,b.pointerFirst,b.pointerCount,
                    b.numberFirst,b.numberCount,b.nameFirst,b.nameCount,b.bg0,b.bg1,unsigned(b.bgOpaque),m.dsNamesGeneration})word(w);
    static_assert(sizeof(Renderer::MaterialQuad)%4==0);
    bytes(m.base,m.count*sizeof(Renderer::MaterialQuad));
    if(m.miiMap)bytes(&m.miiQuads[b.mii][0],sizeof(Renderer::MaterialQuad));
    if(m.miiRow)bytes(&m.miiQuads[b.mii][1],sizeof(Renderer::MaterialQuad));
    word(m.infoArtCount);word(m.infoTextCount);
    if(m.infoArtCount)bytes(m.infoQuads.get()+b.mii*InfoCapacity,m.infoArtCount*sizeof(Renderer::MaterialQuad));
    if(m.infoTextCount)bytes(m.infoQuads.get()+b.mii*InfoCapacity+InfoArtCapacity,m.infoTextCount*sizeof(Renderer::MaterialQuad));
    return uint64_t(a)<<32|c;
}

void RaceBottomScreen::drawRace(Renderer& renderer,const BottomView& view){
    auto& m=*impl_;
    if(!compose(renderer,view))return;
    const uint64_t hash=frameHash();
    const uint32_t serial=renderer.frameSerial();
    const unsigned apt=aptEvents.load(std::memory_order_relaxed),scopes=loadingScopes();
    // A DIAG leave-out frame draws less than it composed, and a loading frame
    // ends black: neither leaves this picture on the lower LCD.
    const bool leaveOut=diag::passState.skip==diag::PassBottom||diag::passState.skip==diag::PassBottomQuads;
    const bool shown=!leaveOut&&!renderer.loadingFrame;
    // The previous frame showed exactly this picture and nothing has touched
    // the lower LCD or its target since: draw nothing, and skip the clear.
    if(m.keep&&shown&&m.shownValid&&serial==m.shownSerial+1&&apt==m.shownApt&&scopes==m.shownScopes&&hash==m.shownHash&&renderer.bottomCovered()){
        m.shownSerial=serial;++m.keptFrames;return;
    }
    renderer.beginBottom();
    if(m.depthOff)renderer.bottomNoDepth();
    submit(renderer);
    m.shownValid=shown;m.shownHash=hash;m.shownSerial=serial;m.shownApt=apt;m.shownScopes=scopes;++m.drawnFrames;
}

bool RaceBottomScreen::skipRace(Renderer& renderer){
    auto& m=*impl_;
    // A loading frame ends black; a gap or an APT event may have changed the LCD.
    if(!m.shownValid||renderer.loadingFrame||renderer.frameSerial()!=m.shownSerial+1||
       aptEvents.load(std::memory_order_relaxed)!=m.shownApt||loadingScopes()!=m.shownScopes)return false;
    if(!renderer.bottomCovered())return false;
    m.shownSerial=renderer.frameSerial();++m.skippedFrames;
    return true;
}
void RaceBottomScreen::endRace(){
    auto& m=*impl_;
#ifndef MKW_RELEASE
    if(m.keptFrames+m.drawnFrames)if(FILE* f=std::fopen("sdmc:/3ds/MKWiiDS/runtime.log","a")){
        std::fprintf(f,"bottom keep: course=%d kept=%u drawn=%u skipped=%u keep=%d info frames=%u peak quads=%u standings=%d frames=%u peak quads=%u\n",m.course==~0u?-1:int(m.course),m.keptFrames,m.drawnFrames,m.skippedFrames,int(m.keep),m.infoFrames,m.infoQuadPeak,int(m.ds),m.dsFrames,m.dsQuadPeak);std::fclose(f);}
#endif
    m.keptFrames=m.drawnFrames=m.skippedFrames=0;m.shownValid=false;m.infoFrames=m.infoQuadPeak=0;m.dsFrames=m.dsQuadPeak=0;
}
}
