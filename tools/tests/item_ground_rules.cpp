#include "mkw/race_items.hpp"
#include <cassert>
#include <cstdio>

// Items are placed on plain road first: a trailing (held) item is still destroyed by
// fall-boundary and solid-fall contact, which these tests leave out.
// Respawn, fall-boundary, solid-fall, Fake Item Box and trap wall rules against retail
// StaticR (USA): respawn clear 0x8078f83c, base calc 0x80796558..0x807966d8, trap
// update 0x807a8e70..0x807a8eec. Build with -DMKW_ITEMS_HOST_TEST (see item_shell_expiry).
using namespace mkw;

namespace {
constexpr uint32_t Road=1u<<0x00,SolidFall=1u<<0x0a,FallBoundary=1u<<0x10;
uint32_t floorType=Road;bool fallBoundaryEverywhere=false;Vec3 wallNormal{};
// Flat floor at y 500 of the current type; optionally a fall-boundary volume everywhere and
// a wall touched by any item below y 700.
ItemSurface ground(Vec3,Vec3 p,float radius){
    ItemSurface s{};
    if(fallBoundaryEverywhere){s.floorTypes|=FallBoundary;s.kill=true;s.collided=true;}
    if(p.y<500+radius){s.correction.y=500+radius-p.y;s.floorNormal={0,1,0};s.floorTypes|=floorType;s.collided=true;
        if(floorType&SolidFall)s.kill=true;}
    if(length(wallNormal)>0&&p.y<700){s.wallNormal=wallNormal;s.collided=true;}
    return s;
}
struct Race {
    RaceItems items;ItemRacer racers[2]{};
    Race(){
        items.reset({},2);
        for(unsigned i=0;i<2;++i){racers[i].grounded=true;racers[i].forward={0,0,1};racers[i].up={0,1,0};
            racers[i].position={float(i)*4000,500,0};racers[i].progress=float(2-i);}
    }
    void step(bool use=false,bool backwards=false){
        ItemInput inputs[2]{};inputs[0].use=use;inputs[0].backwards=backwards;
        items.beforeStep(racers,inputs,2,true);items.afterStep(racers,2,ground);
    }
    unsigned live(ItemKind kind)const{unsigned n=0;for(const auto& a:items.actors())n+=a.active&&a.kind==kind;return n;}
    const ItemActor* find(ItemKind kind)const{for(const auto& a:items.actors())if(a.active&&a.kind==kind)return &a;return nullptr;}
    const ItemActor* drop(ItemKind kind){
        assert(items.give(0,kind));step(true,true);step(false,true);
        for(unsigned wait=0;wait<12;++wait){if(auto* a=find(kind))return a;step(false,true);}
        return nullptr;
    }
};
void reset(){floorType=Road;fallBoundaryEverywhere=false;wallNormal={};}
}

int main(){
    setvbuf(stdout,nullptr,_IONBF,0);
    // Respawning deletes the inventory and the trailing items: nothing reaches the road.
    {
        reset();Race race;
        assert(race.items.give(0,ItemKind::TripleGreenShell));race.step(true);race.step();
        assert(race.items.hud(0).deployed);
        assert(race.items.give(0,ItemKind::Banana));
        race.racers[0].unavailable=true;
        for(unsigned f=0;f<20;++f)race.step();
        assert(race.live(ItemKind::GreenShell)==0&&race.live(ItemKind::Banana)==0);
        assert(!race.items.hud(0).deployed&&race.items.hud(0).item==ItemKind::None);
        std::printf("respawn: inventory and trailing items deleted, none dropped: PASS\n");
    }
    // A dropped Banana inside a fall-boundary volume stays.
    {
        reset();Race race;
        const ItemActor* banana=race.drop(ItemKind::Banana);assert(banana);
        fallBoundaryEverywhere=true;
        for(unsigned f=0;f<600;++f){race.step();assert(banana->active);}
        std::printf("fall boundary: Banana kept for 600 frames: PASS\n");
    }
    // A Banana resting on solid fall goes after the 180-frame landed countdown, not at once.
    {
        reset();Race race;
        const ItemActor* banana=race.drop(ItemKind::Banana);assert(banana);
        floorType=SolidFall;
        unsigned frames=0;while(banana->active&&frames<400){race.step();++frames;}
        assert(!banana->active&&frames>=150&&frames<=200);
        std::printf("solid fall: Banana removed after %u frames: PASS\n",frames);
    }
    // A Bob-omb never starts the solid-fall countdown (it still explodes on its fuse).
    {
        reset();Race race;race.racers[1].position={0,500,90000};
        const ItemActor* bomb=race.drop(ItemKind::Bobomb);assert(bomb);
        floorType=SolidFall;
        for(unsigned f=0;f<60;++f){race.step();assert(bomb->active&&!bomb->explosion);}
        assert(bomb->solidFallTicks==0);
        std::printf("solid fall: Bob-omb exempt: PASS\n");
    }
    // A Fake Item Box has no timer.
    {
        reset();Race race;
        const ItemActor* box=race.drop(ItemKind::FakeBox);assert(box&&box->lifetime==0);
        for(unsigned f=0;f<2400;++f){race.step();assert(box->active);}
        std::printf("Fake Item Box: no 30 s expiry: PASS\n");
    }
    // A trap touching an up-facing wall (normal y >= .98) is removed; a steep one bounces it.
    {
        reset();wallNormal={0,1,0};Race race;
        assert(race.items.give(0,ItemKind::Banana));race.step(true,true);race.step(false,true);
        for(unsigned f=0;f<12;++f)race.step(false,true);
        assert(race.live(ItemKind::Banana)==0);
        reset();wallNormal={0,.5f,-.8660254f};Race steep;
        const ItemActor* banana=steep.drop(ItemKind::Banana);assert(banana);
        for(unsigned f=0;f<30;++f)steep.step();
        assert(banana->active);
        std::printf("trap wall: up-facing removes, steep keeps: PASS\n");
    }
    return 0;
}
