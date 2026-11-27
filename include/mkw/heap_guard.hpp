#pragma once
#include <cstddef>
#include <new>

// Heap pressure in long sessions (src/heap_guard.cpp). The race load RAM caches
// (pack entries, asset_pack.cpp; sound waves, sound_system.cpp) only speed up
// loads: when an allocation fails they give their memory back and the
// allocation tries again, and before each race load they shrink so the load
// has its room. Short sessions never reach either path.
namespace mkw::heapGuard {
// The heap: used (malloc), free (free chunks plus the never claimed end),
// the top chunk, the largest block malloc can hand out (only when asked: a
// binary search of trial allocations) and the number of free chunks.
struct Snapshot {std::size_t used=0,free=0,unclaimed=0,top=0,largest=0;unsigned freeChunks=0;};
Snapshot snapshot(bool largest);
std::size_t largestBlock();
// DIAG A/B (no-heap-guard.txt): off, failures go on as before and a race load makes no room.
void setEnabled(bool enabled);
bool enabled();
// Frees the load caches from any thread, even one inside a pack or sound
// cache read; returns the bytes freed. They keep nothing more until loadDone.
std::size_t releaseCaches();
// The new handler: a failed allocation frees the caches and tries again.
// With nothing left to free it steps aside for `then` (DIAG: the out-of-heap
// log), or the allocation fails as before. arm() puts it back (race loads).
void install(std::new_handler then);
void arm();
// A race load ended: the caches may keep again.
void loadDone();
// A race load once the last race is released (main.cpp, after "race cleanup"):
// it wants roomFree of free heap (16 MB) and a roomBlock-sized block (4 MB).
// Short of room the caches go first (a set's first race leaves several MB that
// later races never read again), then, still short or under a roomAssetsBlock
// (2 MB) block, the set's kept assets (they load again). Bigger single blocks
// than the heap holds go to LINEAR (linearFallback).
extern std::size_t roomFree,roomBlock,roomAssetsBlock; // DIAG load-room.txt: "<free KB> <block KB> <assets block KB>"
// Enough room for the load as it is; enough once the caches are gone (else the set's kept assets go too).
bool room(const Snapshot& s);
bool roomWithoutAssets(const Snapshot& s);
std::size_t dropCaches();
struct Counters {unsigned releases=0,roomDrops=0,linearBlocks=0;std::size_t releasedBytes=0,roomBytes=0,linearBytes=0;};
// Last resort for operator new (both builds: here in release, main.cpp's DIAG new): a
// request of LinearFallbackBytes or more that the heap cannot serve even with the caches
// freed takes LINEAR while LinearFallbackReserve stays free there. Long sessions break the
// heap into thousands of free chunks; such a request then quit to HOME.
// operator delete (heap_guard.cpp) gives LINEAR blocks back to LINEAR.
constexpr std::size_t LinearFallbackBytes=256u<<10,LinearFallbackReserve=8u<<20;
void* linearFallback(std::size_t bytes);
bool linearOwned(const void* p);
Counters counters();
}
