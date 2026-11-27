#include <3ds.h>
#include <atomic>
#include <cstring>
#include "mkw/console_model.hpp"
#include "mkw/extra_memory.hpp"
namespace mkw::extraMemory {
namespace {
// Sorted by address; free and used blocks tile the whole region.
struct Block {uint32_t at,size;bool used;uint32_t hash;bool hashed;};
constexpr unsigned MaxBlocks=1024;
Block table[MaxBlocks];unsigned count=0;bool ready=false,checked=false,mapped=false;
std::atomic<bool> on{true},resumed{false},broken{false};
RecursiveLock lock; // zero-initialized: valid before static constructors
struct Guard {Guard(){RecursiveLock_Lock(&lock);}~Guard(){RecursiveLock_Unlock(&lock);}};
aptHookCookie cookie;
void hook(APT_HookType type,void*){if(type==APTHOOK_ONRESTORE||type==APTHOOK_ONWAKEUP)resumed.store(true,std::memory_order_relaxed);}
void setup(){
 if(ready)return;
 table[0]={uint32_t(Base),uint32_t(Size),false,0,false};count=1;ready=true;
}
int find(uint32_t at){
 for(unsigned i=0;i<count;++i)if(table[i].at==at)return int(i);
 return -1;
}
uint32_t fnv(const uint8_t* p,std::size_t n){uint32_t h=2166136261u;for(std::size_t i=0;i<n;++i)h=(h^p[i])*16777619u;return h;}
}
bool available(){
 Guard g;
 if(checked)return mapped;
 checked=true;
 if(!isNew3ds())return mapped=false;
 MemInfo info{};PageInfo page{};
 if(R_FAILED(svcQueryMemory(&info,&page,Base)))return mapped=false;
 const bool rw=(info.perm&MEMPERM_READWRITE)==MEMPERM_READWRITE;
 mapped=info.state!=MEMSTATE_FREE&&rw&&info.base_addr<=Base&&info.base_addr+info.size>=Base+Size;
 if(mapped)setup();
 return mapped;
}
void setEnabled(bool value){on.store(value,std::memory_order_relaxed);}
void markBroken(){broken.store(true,std::memory_order_relaxed);}
bool enabled(){return on.load(std::memory_order_relaxed)&&!broken.load(std::memory_order_relaxed)&&available();}
void* alloc(std::size_t bytes){
 if(!bytes||!enabled())return nullptr;
 Guard g;
 const uint32_t need=uint32_t((bytes+127)&~std::size_t(127));
 for(unsigned i=0;i<count;++i){
  Block& b=table[i];
  if(b.used||b.size<need)continue;
  if(b.size>need){
   if(count==MaxBlocks)return nullptr;
   std::memmove(&table[i+2],&table[i+1],(count-i-1)*sizeof(Block));++count;
   table[i+1]={b.at+need,b.size-need,false,0,false};
   b.size=need;
  }
  b.used=true;b.hashed=false;
  return reinterpret_cast<void*>(uintptr_t(b.at));
 }
 return nullptr;
}
void free(void* p){
 if(!p)return;
 Guard g;
 const int i=find(uint32_t(uintptr_t(p)));
 if(i<0||!table[i].used)return;
 table[i].used=false;table[i].hashed=false;
 // Merge with free neighbours.
 unsigned at=unsigned(i);
 if(at+1<count&&!table[at+1].used){table[at].size+=table[at+1].size;std::memmove(&table[at+1],&table[at+2],(count-at-2)*sizeof(Block));--count;}
 if(at>0&&!table[at-1].used){table[at-1].size+=table[at].size;std::memmove(&table[at],&table[at+1],(count-at-1)*sizeof(Block));--count;}
}
std::size_t sizeOf(const void* p){
 Guard g;
 const int i=find(uint32_t(uintptr_t(p)));
 return i>=0&&table[i].used?table[i].size:0;
}
std::size_t used(){Guard g;std::size_t n=0;for(unsigned i=0;i<count;++i)if(table[i].used)n+=table[i].size;return n;}
std::size_t largestFree(){Guard g;std::size_t n=0;for(unsigned i=0;i<count;++i)if(!table[i].used&&table[i].size>n)n=table[i].size;return n;}
unsigned blocks(){Guard g;unsigned n=0;for(unsigned i=0;i<count;++i)n+=table[i].used;return n;}
void remember(const void* p){
 Guard g;
 const int i=find(uint32_t(uintptr_t(p)));
 if(i<0||!table[i].used)return;
 auto* bytes=reinterpret_cast<uint8_t*>(uintptr_t(table[i].at));
 GSPGPU_InvalidateDataCache(bytes,table[i].size);
 table[i].hash=fnv(bytes,table[i].size);table[i].hashed=true;
}
unsigned verify(unsigned* checkedBlocks){
 Guard g;
 unsigned changed=0,n=0;
 for(unsigned i=0;i<count;++i){
  Block& b=table[i];
  if(!b.used||!b.hashed)continue;
  auto* bytes=reinterpret_cast<uint8_t*>(uintptr_t(b.at));
  GSPGPU_InvalidateDataCache(bytes,b.size);
  ++n;changed+=fnv(bytes,b.size)!=b.hash;
 }
 if(checkedBlocks)*checkedBlocks=n;
 return changed;
}
bool takeResumed(){return resumed.exchange(false,std::memory_order_relaxed);}
void installHook(){static bool done=false;if(!done){done=true;aptHook(&cookie,hook,nullptr);}}
}
