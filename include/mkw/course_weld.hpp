#pragma once
// Course weld: course and sky parts store a separate vertex for many
// triangle corners, split by normals the draw never reads. After the
// load-time bake, a submesh lit by its baked vertex colours (unlit GX channel,
// or StaticLighting) and without any environment mapping feeds the vertex
// shader's normal to nothing (mkw.v.pica reads it only under diffuseLighting,
// environmentMapped or skinning). Its vertices that then agree in everything
// the draw reads (position, both UV pairs, colour) are the same vertex, so
// every index list of the part (authored, contact, cache order, castle
// regions) is remapped to one of them: the same triangles in the same order
// with the same vertex values, and the post-transform vertex cache shades
// each shared corner once. The picture is unchanged.
// Off-file: no-course-weld.txt. DIAG: course-weld-alternate.txt (on for the
// first course load, off for the next, ...).
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <new>
namespace mkw::courseWeld {
inline bool on=true;
inline bool alternate=false;
inline unsigned loads=0;
inline bool active=false; // this course load welded (DIAG perf-cull.csv)
struct Stats {uint32_t weldable=0,merged=0,remapped=0;bool done=false;};
// remap[v]: the vertex v's corners use. weldable(v): v may be merged (every
// submesh drawing it qualifies); group(v): v merges only within its group
// (the one submesh using it); hash(v) / equal(a,b): of everything the draw
// reads. Representatives are the first vertex of each set of equal ones.
// One pass through an open-addressing table (2 bytes a slot, at most 256 KB).
template<class Weldable,class Group,class Hash,class Equal>
bool weldRemap(uint32_t vertexCount,uint16_t* remap,Weldable weldable,Group group,Hash hash,Equal equal,Stats& stats){
    for(uint32_t v=0;v<vertexCount;++v)remap[v]=uint16_t(v);
    uint32_t n=0;for(uint32_t v=0;v<vertexCount;++v)n+=weldable(v)?1u:0u;
    stats.weldable=n;if(n<2)return true;
    uint32_t size=1024;while(size<2*n)size*=2;
    auto* table=new(std::nothrow) uint16_t[size];if(!table)return false;
    auto* used=new(std::nothrow) uint8_t[size/8];if(!used){delete[] table;return false;}
    std::memset(used,0,size/8);
    for(uint32_t v=0;v<vertexCount;++v){
        if(!weldable(v))continue;
        const uint32_t g=group(v);
        uint32_t at=(uint32_t(hash(v))^(g*0x9e3779b9u))&(size-1);
        for(;;at=(at+1)&(size-1)){
            if(!(used[at>>3]&(1u<<(at&7)))){used[at>>3]|=uint8_t(1u<<(at&7));table[at]=uint16_t(v);break;}
            const uint16_t r=table[at];
            if(group(r)==g&&equal(v,r)){remap[v]=r;++stats.merged;break;}
        }
    }
    delete[] used;delete[] table;
    return true;
}
}
