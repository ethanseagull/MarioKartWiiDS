#pragma once
#include <cctype>
#include <cstddef>
#include <cstring>
// Race texture placement order (main.cpp moveRaceTexturesToVram): VRAM, then
// the New 3DS extra memory, go to the textures that cover the most screen
// pixels per byte. Scores are per texture slot, the same on every course:
//  - vehicle parts by slot, "<character>_<vehicle>_kart_<part>_NNN" ->
//    "kart_<part>_NNN" (the same slot is drawn, or never drawn, for every
//    character: kart_tire_fl_000 was drawn for none of 161 racers);
//  - other race textures by name without the trailing number (hud_rank_2 ->
//    hud_rank).
// Measured in Azahar (DIAG texture-use.txt + model-pixels.txt, all 32
// courses, captures/2026-11-14-extra-memory/); skinned draws count no pixels
// there, so their per-draw pixels are the part's median from counted draws.
// Placement never changes a pixel: it only picks the memory a texture is
// sampled from. Generated table: texture_rank_table.inc
// (tools/diag/texture_rank_table.py).
namespace mkw::textureRank {
struct Slot {const char* key;float score;};
#include "mkw/texture_rank_table.inc"
// The slot key of a texture name; false for effects (fx_, drift_: their
// draws are not all counted) and names without a slot.
inline bool slotKey(const char* name,char* out,std::size_t size){
    if(!name||!*name||!size)return false;
    if(!std::strncmp(name,"fx_",3)||!std::strncmp(name,"drift_",6))return false;
    const std::size_t len=std::strlen(name);
    const bool threeDigits=len>=4&&name[len-4]=='_'&&std::isdigit(static_cast<unsigned char>(name[len-3]))&&
        std::isdigit(static_cast<unsigned char>(name[len-2]))&&std::isdigit(static_cast<unsigned char>(name[len-1]));
    const char* vehicle=nullptr;
    for(const char* p=name;*p;++p)if(!std::strncmp(p,"_kart_",6)||!std::strncmp(p,"_bike_",6))vehicle=p;
    const char* from=threeDigits&&vehicle?vehicle+1:name;
    std::size_t n=std::strlen(from);
    if(!(threeDigits&&vehicle)){
        while(n&&std::isdigit(static_cast<unsigned char>(from[n-1])))--n;
        if(n<std::strlen(from)&&n&&from[n-1]=='_')--n;
    }
    if(!n||n>=size)return false;
    std::memcpy(out,from,n);out[n]=0;
    return true;
}
// Pixels per byte for the texture's slot (Fallback when unknown).
inline float score(const char* name){
    char key[64];
    if(!slotKey(name,key,sizeof key))return Fallback;
    std::size_t lo=0,hi=sizeof(Slots)/sizeof(Slots[0]);
    while(lo<hi){const std::size_t mid=(lo+hi)/2;const int c=std::strcmp(Slots[mid].key,key);
        if(!c)return Slots[mid].score;if(c<0)lo=mid+1;else hi=mid;}
    return Fallback;
}
// This race's order (perf-races.csv tex_rank): set by main.cpp per race load.
inline bool raceRanked=false;
}
