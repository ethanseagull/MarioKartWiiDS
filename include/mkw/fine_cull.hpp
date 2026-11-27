#pragma once
// Finer course culling (Nov 21): each 128-triangle culling block of a course
// part or static @world object also keeps the bounds of its 16-triangle
// children, in the order the block is drawn. A block partly in the view tests
// its children and submits only those that reach it, up to FineRanges draws
// per submesh (the smallest gaps merge first, as before). The same triangles
// in the same order reach the screen: only wholly off-screen triangles are
// left out, so the picture is unchanged. Off-file: no-fine-cull.txt (the
// 128-triangle blocks and 4 draws per submesh, as before). DIAG:
// fine-cull-alternate.txt turns it on for the first race and off for the
// next, and so on; perf-cull.csv has one row per race (state and counts).
#include <cstdint>
#include <cstdio>
#include <mkw/course_weld.hpp>
namespace mkw::fineCull {
inline constexpr unsigned ChildTriangles=16,FineRanges=16;
// Outward slack of a child test, in units of its summed plane terms (the
// block test's is 1e-5): a child is left out only when clearly outside, well
// beyond the GPU's f24 rounding of its vertices.
inline constexpr float ChildSlack=1e-4f;
inline bool on=true;      // drawNow uses the children
inline bool build=true;   // loads build them (off only with no-fine-cull.txt and no alternate)
inline bool alternate=false;
inline unsigned raceStarts=0;
// Per race (render thread writes, the main thread reads at race end; DIAG counts).
struct Counts {uint64_t frames=0,draws=0,submeshTests=0,blockTests=0,childTests=0,ranges=0,triangles=0,vertexRuns=0,verifyDropped=0,verifyFailed=0,setupOnly=0;uint32_t lastFrame=~0u;};
inline Counts counts;
inline bool countVertices=false; // DIAG cull-vertex-count.txt: FIFO-16 vertex runs of the culled draws (slow)
// DIAG fine-cull-verify.txt: every triangle the blocks would submit and the
// children leave out is checked, from the drawn index list, to lie wholly
// outside one clip plane (perf-cull.csv verify_dropped / verify_failed).
inline bool verify=false;
// Racing starts (RuntimeProfile::autoFrame, DIAG).
inline void raceStart(){
    counts=Counts{};
    if(alternate)on=(raceStarts%2)==0;
    ++raceStarts;
}
// Race end (RuntimeProfile::write, DIAG): sdmc:/3ds/MKWiiDS/perf-cull.csv.
inline void write(int race,unsigned course){
    if(race<0)return;
    const Counts c=counts;const double n=c.frames?double(c.frames):1.0;
    bool header=true;if(FILE* f=std::fopen("sdmc:/3ds/MKWiiDS/perf-cull.csv","r")){header=false;std::fclose(f);}
    if(FILE* f=std::fopen("sdmc:/3ds/MKWiiDS/perf-cull.csv","a")){
        if(header)std::fputs("race,course,fine_cull,alternate,frames,culled_model_draws,submesh_tests,block_tests,child_tests,draw_calls,triangles,vertex_runs,setup_only,verify,verify_dropped,verify_failed,course_weld\n",f);
        std::fprintf(f,"%d,%u,%u,%u,%llu,%.2f,%.1f,%.1f,%.1f,%.2f,%.1f,%.1f,%.2f,%u,%llu,%llu,%u\n",race,course,unsigned(on),unsigned(alternate),(unsigned long long)c.frames,
            c.draws/n,c.submeshTests/n,c.blockTests/n,c.childTests/n,c.ranges/n,c.triangles/n,countVertices?c.vertexRuns/n:-1.0,c.setupOnly/n,
            unsigned(verify),(unsigned long long)c.verifyDropped,(unsigned long long)c.verifyFailed,unsigned(courseWeld::active));
        std::fclose(f);
    }
}
}
