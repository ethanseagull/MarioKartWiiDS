#include <mkw/flow_name_font.hpp>
#include <mkw/flow_text.hpp>
#include <mkw/menu_name_glyph_atlas.hpp>
#include <3ds.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace mkw {
namespace {
// The menus' name glyph sprites (tools/cook_menu_names.py): the atlas pages first,
// then one row per glyph in code order, beside UiGlyphPlaces.
struct GlyphRow {const char* key;const char* path;float w,h,u,v;const char* text;unsigned font,style,gpuBytes;};
constexpr GlyphRow Rows[]={
#include <mkw/menu_name_glyphs.hpp>
};
constexpr unsigned Pages=std::size(UiGlyphPageKeys),Glyphs=std::size(UiGlyphPlaces);
static_assert(std::size(Rows)==Pages+Glyphs,"menu_name_glyphs.hpp: pages then one row per glyph");
unsigned rowCode(unsigned i){return unsigned(std::strtoul(Rows[Pages+i].key+11,nullptr,16));} // "name_glyph_XXXX"
// The glyph's index for a character code, or Glyphs.
unsigned glyphIndex(char16_t code){
 unsigned lo=0,hi=Glyphs;
 while(lo<hi){const unsigned mid=(lo+hi)/2;if(rowCode(mid)<code)lo=mid+1;else hi=mid;}
 return lo<Glyphs&&rowCode(lo)==code?lo:Glyphs;
}
// An 8-bit texel (LA4) in a tiled texture: 8x8 tiles, Morton order inside (tex3ds rows top first).
std::size_t texel(unsigned x,unsigned y,unsigned width){
 unsigned m=0;for(unsigned b=0;b<3;++b)m|=((x>>b)&1u)<<(2*b)|((y>>b)&1u)<<(2*b+1);
 return std::size_t(((y>>3)*(width>>3)+(x>>3))*64+m);
}
constexpr unsigned AtlasW=512,AtlasH=64;
}

void FlowNameFont::reset(){texture.reset();font_={};text_[0]=0;}

bool FlowNameFont::build(const char* utf8,const FlowFont& like,int slot){
 reset();
 if(!utf8||!*utf8)return false;
 // The name's distinct characters that the font has, in order.
 struct Want {char16_t code;unsigned index;};std::array<Want,32> want{};unsigned count=0;
 for(const char* p=utf8;*p&&count<want.size();){
  const char16_t code=nextFlowGlyph(p);const unsigned index=glyphIndex(code);
  if(index==Glyphs||std::any_of(want.begin(),want.begin()+count,[&](const Want& w){return w.code==code;}))continue;
  want[count++]={code,index};
 }
 if(!count)return false;
 if(!C3D_TexInit(&texture.gpu,AtlasW,AtlasH,GPU_LA4))return false;
 texture.ready=true; // owns the allocation from here (reset frees it)
 auto* out=static_cast<uint8_t*>(texture.gpu.data);std::memset(out,0,AtlasW*AtlasH);
 // Shelf of glyphs one transparent texel apart, row 1 (texel 0 stays clear for the filter).
 unsigned x=1,placed=0;
 for(unsigned page=0;page<Pages;++page){
  bool needed=false;for(unsigned i=0;i<count;++i)needed|=UiGlyphPlaces[want[i].index].page==page;
  if(!needed)continue;
  Texture source;
  if(!source.load(Rows[page].path,0)||source.gpu.fmt!=GPU_LA4){std::fprintf(stderr,"name font page %u unavailable\n",page);continue;}
  const auto* in=static_cast<const uint8_t*>(source.gpu.data);const unsigned sw=source.gpu.width,sh=source.gpu.height;
  for(unsigned i=0;i<count;++i){
   const auto& place=UiGlyphPlaces[want[i].index];if(place.page!=page)continue;
   const auto& row=Rows[Pages+want[i].index];const unsigned w=unsigned(row.w),h=std::min(unsigned(row.h),AtlasH-2);
   if(x+w+1>AtlasW||place.x+w>sw||place.y+h>sh)continue;
   for(unsigned yy=0;yy<h;++yy)for(unsigned xx=0;xx<w;++xx)out[texel(x+xx,1+yy,AtlasW)]=in[texel(place.x+xx,place.y+yy,sw)];
   // Advances and bearings: the boards' font's own where it has the glyph; else the
   // menus' glyph (outline ink on both sides) a texel in, three narrower.
   const FlowGlyph* known=nullptr;for(unsigned k=0;k<like.glyphCount;++k)if(like.glyphs[k].code==want[i].code){known=&like.glyphs[k];break;}
   const int left=known?std::min<int>(0,known->left):-1;const unsigned advance=known?known->advance:unsigned(std::max(1,int(w)-3));
   glyphs_[placed++]={want[i].code,uint16_t(x),uint16_t(1),uint16_t(w),int8_t(left),uint8_t(advance)};
   x+=w+1;
  }
 }
 if(!placed){reset();return false;}
 C3D_TexFlush(&texture.gpu);C3D_TexSetFilter(&texture.gpu,GPU_LINEAR,GPU_LINEAR);C3D_TexSetWrap(&texture.gpu,GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE);
 font_=like;font_.texture=int8_t(slot);font_.atlasW=AtlasW;font_.atlasH=AtlasH;font_.glyphs=glyphs_.data();font_.glyphCount=uint16_t(placed);
 std::snprintf(text_,sizeof text_,"%s",utf8);
 return true;
}
}
