#include <mkw/heap_guard.hpp>
#include <mkw/asset_pack.hpp>
#include <mkw/sound_system.hpp>
#include <3ds.h>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <malloc.h>
extern "C" {extern u32 __ctru_linear_heap,__ctru_linear_heap_size;}

namespace mkw::heapGuard {
namespace {
std::new_handler next=nullptr;bool on=true;
std::atomic<unsigned> releaseCount{0},roomDrops{0},linearCount{0};
std::atomic<std::size_t> releasedBytes{0},roomBytes{0},linearBytes{0};
void handler(){
    if(releaseCaches())return;
    // Nothing left to free: this allocation fails as it did before the handler
    // (DIAG: `next` logs it; otherwise operator new aborts as bad_alloc did, and a
    // nothrow new returns null). arm() reinstalls the handler.
    std::set_new_handler(next);
}
}
Snapshot snapshot(bool withLargest){
    const struct mallinfo m=mallinfo();
    Snapshot s;
    const std::size_t heap=envGetHeapSize(),arena=std::size_t(m.arena);
    s.used=std::size_t(m.uordblks);s.unclaimed=heap>arena?heap-arena:0;s.free=std::size_t(m.fordblks)+s.unclaimed;
    s.top=std::size_t(m.keepcost);s.freeChunks=unsigned(m.ordblks);
    if(withLargest)s.largest=largestBlock();
    return s;
}
std::size_t largestBlock(){
    const struct mallinfo m=mallinfo();
    std::size_t lo=0,hi=std::size_t(m.fordblks)+(envGetHeapSize()>std::size_t(m.arena)?envGetHeapSize()-std::size_t(m.arena):0)+4096;
    // Through volatile pointers: GCC drops a malloc whose block is only freed
    // and takes it as a success, so the search read the whole free total.
    void* (*volatile allocate)(std::size_t)=std::malloc;void (*volatile release)(void*)=std::free;
    while(hi-lo>4096){const std::size_t mid=(lo+hi)/2;if(void* p=allocate(mid)){release(p);lo=mid;}else hi=mid;}
    return lo;
}
void setEnabled(bool enabled){on=enabled;}
bool enabled(){return on;}
std::size_t releaseCaches(){
    if(!on)return 0;
    const std::size_t freed=pack::releaseCache()+SoundSystem::releaseRamCache();
    if(freed){releaseCount.fetch_add(1);releasedBytes.fetch_add(freed);}
    return freed;
}
void install(std::new_handler then){next=then;std::set_new_handler(handler);}
void arm(){std::set_new_handler(handler);}
void loadDone(){SoundSystem::relaxRamCache();}
std::size_t roomFree=16u<<20,roomBlock=4u<<20,roomAssetsBlock=2u<<20;
bool room(const Snapshot& s){return s.free>=roomFree&&s.largest>=roomBlock;}
bool roomWithoutAssets(const Snapshot& s){return s.free>=roomFree&&s.largest>=roomAssetsBlock;}
std::size_t dropCaches(){
    // Whole caches, not entries: what a cache keeps is spread through the heap,
    // and only emptying it gives the load its contiguous room back.
    // As after a failed allocation: nothing is kept again until this load ends.
    const std::size_t freed=pack::releaseCache()+SoundSystem::releaseRamCache();
    if(freed){roomDrops.fetch_add(1);roomBytes.fetch_add(freed);}
    return freed;
}
Counters counters(){Counters c;c.releases=releaseCount.load();c.roomDrops=roomDrops.load();c.releasedBytes=releasedBytes.load();c.roomBytes=roomBytes.load();c.linearBlocks=linearCount.load();c.linearBytes=linearBytes.load();return c;}
void* linearFallback(std::size_t n){
    if(!on||n<LinearFallbackBytes)return nullptr;
    const std::size_t padded=(n+0x7Fu)&~std::size_t(0x7F);
    if(linearSpaceFree()<padded+LinearFallbackReserve)return nullptr;
    void* p=linearAlloc(n);
    if(p){linearCount.fetch_add(1);linearBytes.fetch_add(n);}
    return p;
}
bool linearOwned(const void* p){
    const auto a=reinterpret_cast<std::uintptr_t>(p);
    return a>=__ctru_linear_heap&&a<std::uintptr_t(__ctru_linear_heap)+__ctru_linear_heap_size;
}
namespace {
void release(void* p){if(!p)return;if(linearOwned(p))linearFree(p);else std::free(p);}
#ifdef MKW_RELEASE
// libstdc++'s loop (malloc, else the new handler, else bad_alloc, which -fno-exceptions
// code cannot catch: abort) with the caches and LINEAR in between. A nothrow new keeps
// its fallbacks: null once nothing else helps.
void* allocate(std::size_t n,bool nothrow){
    if(!n)n=1;
    for(;;){
        if(void* p=std::malloc(n))return p;
        if(releaseCaches())continue;
        if(void* p=linearFallback(n))return p;
        if(nothrow)return nullptr;
        std::abort();
    }
}
#endif
}
}
#ifdef MKW_RELEASE
void* operator new(std::size_t n){return mkw::heapGuard::allocate(n,false);}
void* operator new[](std::size_t n){return mkw::heapGuard::allocate(n,false);}
void* operator new(std::size_t n,const std::nothrow_t&)noexcept{return mkw::heapGuard::allocate(n,true);}
void* operator new[](std::size_t n,const std::nothrow_t&)noexcept{return mkw::heapGuard::allocate(n,true);}
#endif
// Both builds: a block operator new took from LINEAR goes back there.
void operator delete(void* p)noexcept{mkw::heapGuard::release(p);}
void operator delete[](void* p)noexcept{mkw::heapGuard::release(p);}
void operator delete(void* p,std::size_t)noexcept{mkw::heapGuard::release(p);}
void operator delete[](void* p,std::size_t)noexcept{mkw::heapGuard::release(p);}
void operator delete(void* p,const std::nothrow_t&)noexcept{mkw::heapGuard::release(p);}
void operator delete[](void* p,const std::nothrow_t&)noexcept{mkw::heapGuard::release(p);}
