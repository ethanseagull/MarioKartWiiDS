#pragma once
#include <cstddef>
#include <cstdint>
// The New 3DS's extra memory (libctru OS_QTMRAM: virtual 0x1E800000,
// physical 0x1F000000). Its first 2 MiB is free for the title when the
// exheader maps it (RSF MemoryMapping 1e800000-1e9fffff); QTM and the kernel
// use offset 0x200000 on, and only the first 2 MiB is GPU-reachable.
// Console test Nov 8: the GPU samples textures there 2.6x faster than LINEAR
// (2.2x slower than VRAM); GX texture copies work both ways; GX memory fill
// does not write there. Azahar's GPU can't see it.
//
// Used as a texture tier between VRAM and LINEAR (Renderer::moveToExtra).
// Blocks are freed through linearFree (linear_lock.cpp routes pointers in this
// range here), so citro3d's C3D_TexDelete frees them like any LINEAR texture.
namespace mkw::extraMemory {
inline constexpr uintptr_t Base=0x1E800000u,Size=0x200000u;
inline bool owns(const void* p){const uintptr_t a=uintptr_t(p);return a>=Base&&a<Base+Size;}
// New 3DS, mapped read/write, not turned off. Checked once (main thread).
bool available();
// Turns the tier off for later allocations (off-file, A/B alternation).
void setEnabled(bool on);
// A failed copy check: off for the rest of the session, whatever setEnabled says.
void markBroken();
bool enabled();
// 128-byte aligned first fit; null when full or not available.
void* alloc(std::size_t bytes);
void free(void* p);
std::size_t sizeOf(const void* p);
std::size_t used();
std::size_t largestFree();
unsigned blocks();
// Integrity after HOME/sleep: remember() hashes a block's bytes when it is
// filled; verify() rehashes every remembered live block (CPU cache
// invalidated first) and returns how many changed. Main thread, GPU idle.
void remember(const void* p);
unsigned verify(unsigned* checked=nullptr);
// Set by the APT hook on restore/wake; the main loop calls verify() once.
bool takeResumed();
// Bytes of race textures placed here for the current race (perf-races.csv extra_kb).
inline std::size_t raceBytes=0;
void installHook();
}
