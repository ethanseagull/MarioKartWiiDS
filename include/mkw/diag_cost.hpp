#pragma once
// DIAG track-cost.txt: per-race counts for the course cost sweep (Azahar runs:
// counts only, never timings). Included at the end of mkw/diag.hpp.
//
// Every CostPeriod-th profiled frame that leaves nothing out is a cost frame.
// On it, each model draw of the left eye (render thread) has its triangles
// binned by view depth (the corners' mean clip w, in course units) and by
// clipped screen area (as trianglePixels; GPU-skinned draws: "not computed"),
// its vertex-shader runs estimated with a 16-entry FIFO post-vertex cache, and
// its (pass, model) row counts its triangles beyond CostFar, under one pixel
// and wholly off screen, and those its frustum test culled. Other draws
// (effects, sprites, HUD, shadow volumes) count draws, triangles and three
// vertex runs a triangle. Written at race end (costWrite):
//   perf-race-NNN-cost-bins.csv    pass, depth bin, area bin: triangles per cost frame
//   perf-race-NNN-cost-models.csv  per (pass, model), per cost frame
//   perf-race-NNN-cost-frames.csv  per group: draws, triangles, vertex runs (median, p90, max, mean)
//   perf-cost.csv                  one row per race (appended): CPU counts, load failures, pack reads
// Load failures (texture files that failed to load or import, LINEAR/VRAM
// allocation failures) and pack reads count from the previous costWrite, so a
// race's row includes its own load.
#ifndef MKW_RELEASE
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
namespace mkw {
extern std::atomic<unsigned> packReads;extern std::atomic<unsigned long long> packReadBytes;
}
namespace mkw::diag {
inline bool trackCost=false;
inline constexpr unsigned CostPeriod=4,CostDepthBins=8,CostAreaBins=9,CostGroups=7,MaxCostFrames=4000,MaxCostModels=1024;
// Course units (a kart is ~150 long; the race projection's far plane is 400000).
inline constexpr float CostFar=8000.f;
inline constexpr float CostDepthEdges[CostDepthBins-1]={500,1000,2000,4000,8000,16000,32000};
// Area bins: 0 wholly off screen (clipped away; still vertex-shaded), 1 under
// 1/4 pixel, 2 1/4-1, 3 1-2, 4 2-8, 5 8-32, 6 32-128, 7 128 and more, 8 not
// computed (GPU-skinned draws, triangles of paths without positions).
inline constexpr float CostAreaEdges[6]={.25f,1.f,2.f,8.f,32.f,128.f};
inline constexpr const char* CostGroupNames[CostGroups]={"sky","course","objects","racers","items","effects","other"};
constexpr unsigned costGroup(uint8_t pass){
    switch(pass){
    case PassSky:return 0;
    case PassCourse:case PassTranslucent:return 1;
    case PassScenery:return 2;
    case PassOpponents:case PassPlayer:case PassPlayerShadow:return 3;
    case PassItems:return 4;
    case PassEffects:case PassFlare:return 5;
    default:return 6;
    }
}
inline unsigned costDepthBin(float w){unsigned b=0;while(b<CostDepthBins-1&&w>=CostDepthEdges[b])++b;return b;}
inline unsigned costAreaBin(double px){if(px<=0)return 1;unsigned b=1;while(b<7&&px>=CostAreaEdges[b-1])++b;return b;}
// One model draw being counted (render thread only).
struct CostDraw {bool on=false;uint8_t pass=0;const void* model=nullptr;float origin=0;
    uint32_t bins[CostDepthBins][CostAreaBins]{};uint32_t tris=0,far=0,tiny=0,offscreen=0,vs=0;};
struct CostModelRow {const void* model=nullptr;uint8_t pass=0;uint32_t instances=0,farInstances=0,draws=0;
    uint64_t triangles=0,far=0,tiny=0,offscreen=0,culled=0,vs=0;double originSum=0;};
struct CostFrameRow {uint16_t draws[CostGroups];uint32_t tris[CostGroups],vs[CostGroups];};
struct CostTally {
    LightLock lock;
    // Render thread.
    CostDraw draw;bool frame=false;unsigned counter=0;uint32_t frameVs[PassCount]{};
    // Shared (lock): sums over cost frames.
    uint64_t bins[PassCount][CostDepthBins][CostAreaBins]{};
    std::vector<CostModelRow> models;std::vector<uint16_t> index; // open addressing, 2048 slots (0 = empty, else row+1)
    std::vector<CostFrameRow> frameRows;unsigned frames=0,droppedModels=0;
    CostTally(){LightLock_Init(&lock);}
};
inline CostTally costTally;
// CPU-side counters (main.cpp sets it): 0 Kinoko prism tests (all threads,
// running total), 1 objects that calc() each frame, 2 objects in the scene.
inline uint64_t (*costCpu)(unsigned which)=nullptr;
// Since the previous costWrite (any thread).
struct CostFailures {LightLock lock;unsigned textures=0,imports=0;std::string list;CostFailures(){LightLock_Init(&lock);}};
inline CostFailures costFailures;
// src/linear_lock.cpp: allocations that returned null (any caller; some callers
// fall back on purpose), and the largest such LINEAR request.
extern std::atomic<unsigned> linearAllocFails,vramAllocFails,linearAllocFailMax;
// Render thread, at each executed frame list: tally says the frame counts
// (profiled, leaves nothing out).
inline void costFrameBegin(bool tally){
    auto& t=costTally;t.draw.on=false;
    t.frame=trackCost&&tally&&(t.counter++%CostPeriod)==0;
    if(t.frame)std::memset(t.frameVs,0,sizeof t.frameVs);
}
inline void costDrawBegin(uint8_t pass,const void* model){
    auto& d=costTally.draw;if(!costTally.frame){d.on=false;return;}
    d=CostDraw{};d.on=true;d.pass=pass<PassCount?pass:PassNone;d.model=model;
}
// After drawNow: the draw's draw calls, triangles and frustum-culled triangles (stats deltas).
inline void costDrawEnd(uint32_t draws,uint32_t triangles,uint32_t culled){
    auto& t=costTally;auto& d=t.draw;if(!d.on)return;d.on=false;
    // Triangles drawn by a path that did not bin them: depth from the model origin, area not computed.
    if(triangles>d.tris){const uint32_t rest=triangles-d.tris;d.bins[costDepthBin(d.origin)][CostAreaBins-1]+=rest;if(d.origin>CostFar)d.far+=rest;d.vs+=3*rest;d.tris=triangles;}
    t.frameVs[d.pass]+=d.vs;
    LightLock_Lock(&t.lock);
    for(unsigned a=0;a<CostDepthBins;++a)for(unsigned b=0;b<CostAreaBins;++b)t.bins[d.pass][a][b]+=d.bins[a][b];
    if(t.index.size()!=2048)t.index.assign(2048,0);
    unsigned slot=unsigned((uintptr_t(d.model)>>3)*2654435761u+d.pass*40503u)&2047u;
    CostModelRow* row=nullptr;
    for(unsigned probe=0;probe<2048;++probe,slot=(slot+1)&2047u){
        const uint16_t at=t.index[slot];
        if(!at){if(t.models.size()<MaxCostModels){t.models.push_back({d.model,d.pass});t.index[slot]=uint16_t(t.models.size());row=&t.models.back();}else ++t.droppedModels;break;}
        auto& r=t.models[at-1u];if(r.model==d.model&&r.pass==d.pass){row=&r;break;}
    }
    if(row){
        if(draws){++row->instances;row->farInstances+=d.origin>CostFar;row->originSum+=d.origin;}
        row->draws+=draws;row->triangles+=triangles;row->far+=d.far;row->tiny+=d.tiny;row->offscreen+=d.offscreen;row->culled+=culled;row->vs+=d.vs;
    }
    LightLock_Unlock(&t.lock);
}
// Non-model commands (pass as diag::drawCount): three vertex runs a triangle.
inline void costOther(uint8_t pass,uint32_t triangles){auto& t=costTally;if(t.frame&&pass<PassCount)t.frameVs[pass]+=3*triangles;}
inline void costFrameEnd(const uint32_t (&draws)[PassCount],const uint32_t (&triangles)[PassCount]){
    auto& t=costTally;if(!t.frame)return;t.frame=false;
    CostFrameRow r{};
    for(unsigned p=0;p<PassCount;++p){const unsigned g=costGroup(uint8_t(p));r.draws[g]=uint16_t(std::min<uint32_t>(65535u,r.draws[g]+draws[p]));r.tris[g]+=triangles[p];r.vs[g]+=t.frameVs[p];}
    LightLock_Lock(&t.lock);
    ++t.frames;if(t.frameRows.size()<MaxCostFrames){if(t.frameRows.capacity()<MaxCostFrames)t.frameRows.reserve(MaxCostFrames);t.frameRows.push_back(r);}
    LightLock_Unlock(&t.lock);
}
// Any thread: a texture file that did not load (import: the file was read
// but its texture could not be created, i.e. no memory for it).
inline void costTextureFailed(const char* path,bool import){
    if(!trackCost)return;auto& f=costFailures;LightLock_Lock(&f.lock);
    ++f.textures;f.imports+=import;
    if(f.list.size()<700){f.list+=path?path:"?";f.list+=import?"(import) ":" ";}
    LightLock_Unlock(&f.lock);
}
struct CostMarks {uint64_t kcl=0;unsigned linear=0,vram=0,reads=0;unsigned long long readBytes=0;bool set=false;};
inline CostMarks costRaceMarks,costLoadMarks;
// Racing starts (RuntimeProfile::autoFrame).
inline void costReset(){
    auto& t=costTally;LightLock_Lock(&t.lock);
    std::memset(t.bins,0,sizeof t.bins);t.models.clear();if(t.index.size()==2048)std::fill(t.index.begin(),t.index.end(),0);
    t.frameRows.clear();t.frames=0;t.droppedModels=0;
    LightLock_Unlock(&t.lock);
    costRaceMarks.kcl=costCpu?costCpu(0):0;costRaceMarks.set=true;
}
// Race end (RuntimeProfile::write): ticks = physics steps profiled.
inline void costWrite(int race,unsigned course,unsigned long ticks){
    if(!trackCost||race<0)return;
    auto& t=costTally;
    uint64_t bins[PassCount][CostDepthBins][CostAreaBins];std::vector<CostModelRow> models;std::vector<CostFrameRow> rows;unsigned frames,dropped;
    LightLock_Lock(&t.lock);std::memcpy(bins,t.bins,sizeof bins);models=t.models;rows=t.frameRows;frames=t.frames;dropped=t.droppedModels;LightLock_Unlock(&t.lock);
    const double n=frames?double(frames):1.0;
    char path[80];
    std::snprintf(path,sizeof path,"sdmc:/3ds/MKWiiDS/perf-race-%03d-cost-bins.csv",race);
    if(FILE* f=std::fopen(path,"w")){
        std::fputs("pass,depth_bin,depth_from,area_bin,triangles_per_frame\n",f);
        for(unsigned p=0;p<PassCount;++p)for(unsigned a=0;a<CostDepthBins;++a)for(unsigned b=0;b<CostAreaBins;++b)if(bins[p][a][b])
            std::fprintf(f,"%s,%u,%.0f,%u,%.2f\n",PassNames[p],a,a?double(CostDepthEdges[a-1]):0.0,b,double(bins[p][a][b])/n);
        std::fclose(f);
    }
    std::snprintf(path,sizeof path,"sdmc:/3ds/MKWiiDS/perf-race-%03d-cost-models.csv",race);
    if(FILE* f=std::fopen(path,"w")){
        std::sort(models.begin(),models.end(),[](const CostModelRow& a,const CostModelRow& b){return a.pass!=b.pass?a.pass<b.pass:a.triangles>b.triangles;});
        std::fputs("pass,model,instances,far_instances,draws,triangles,far_triangles,tiny_triangles,offscreen_triangles,culled_triangles,vertex_runs,mean_origin_depth\n",f);
        for(const auto& m:models){
            const char* name=nullptr;for(const auto& k:modelNames)if(k.model==m.model){name=k.name.c_str();break;}
            char fallback[24];if(!name){std::snprintf(fallback,sizeof fallback,"model@%08lx",(unsigned long)uintptr_t(m.model));name=fallback;}
            std::fprintf(f,"%s,%s,%.3f,%.3f,%.2f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.0f\n",PassNames[m.pass],name,m.instances/n,m.farInstances/n,m.draws/n,double(m.triangles)/n,
                double(m.far)/n,double(m.tiny)/n,double(m.offscreen)/n,double(m.culled)/n,double(m.vs)/n,m.instances?m.originSum/m.instances:0.0);
        }
        std::fclose(f);
    }
    std::snprintf(path,sizeof path,"sdmc:/3ds/MKWiiDS/perf-race-%03d-cost-frames.csv",race);
    if(FILE* f=std::fopen(path,"w")){
        std::fputs("group,frames,draws_median,draws_p90,draws_max,draws_mean,triangles_median,triangles_p90,triangles_max,triangles_mean,vertex_runs_median,vertex_runs_p90,vertex_runs_max,vertex_runs_mean\n",f);
        std::vector<uint32_t> v;v.reserve(rows.size());
        auto stats=[&](auto get){v.clear();double sum=0;for(const auto& r:rows){const uint32_t x=get(r);v.push_back(x);sum+=x;}
            if(v.empty()){std::fputs(",0,0,0,0",f);return;}std::sort(v.begin(),v.end());
            std::fprintf(f,",%lu,%lu,%lu,%.1f",(unsigned long)v[v.size()/2],(unsigned long)v[std::min(v.size()-1,v.size()*9/10)],(unsigned long)v.back(),sum/double(v.size()));};
        for(unsigned g=0;g<=CostGroups;++g){
            std::fprintf(f,"%s,%u",g<CostGroups?CostGroupNames[g]:"total",unsigned(rows.size()));
            stats([&](const CostFrameRow& r){uint32_t s=0;for(unsigned k=0;k<CostGroups;++k)if(g==CostGroups||k==g)s+=r.draws[k];return s;});
            stats([&](const CostFrameRow& r){uint32_t s=0;for(unsigned k=0;k<CostGroups;++k)if(g==CostGroups||k==g)s+=r.tris[k];return s;});
            stats([&](const CostFrameRow& r){uint32_t s=0;for(unsigned k=0;k<CostGroups;++k)if(g==CostGroups||k==g)s+=r.vs[k];return s;});
            std::fputc('\n',f);
        }
        std::fclose(f);
    }
    // Load failures and pack reads since the previous write.
    CostMarks now;now.linear=linearAllocFails.load();now.vram=vramAllocFails.load();now.reads=packReads.load();now.readBytes=packReadBytes.load();now.kcl=costCpu?costCpu(0):0;
    unsigned textures,imports;std::string list;
    {auto& fl=costFailures;LightLock_Lock(&fl.lock);textures=fl.textures;imports=fl.imports;list.swap(fl.list);fl.textures=fl.imports=0;LightLock_Unlock(&fl.lock);}
    for(auto& c:list)if(c==','||c=='"')c=';';
    bool header=true;if(FILE* f=std::fopen("sdmc:/3ds/MKWiiDS/perf-cost.csv","r")){header=false;std::fclose(f);}
    if(FILE* f=std::fopen("sdmc:/3ds/MKWiiDS/perf-cost.csv","a")){
        if(header)std::fputs("race,course,cost_frames,physics_ticks,kcl_prism_tests_per_tick,calc_objects,objects,models_counted,models_dropped,texture_failures,texture_import_failures,linear_alloc_fails,linear_fail_max,vram_alloc_fails,pack_reads,pack_kb,failed_textures\n",f);
        std::fprintf(f,"%d,%u,%u,%lu,%.1f,%lu,%lu,%u,%u,%u,%u,%u,%u,%u,%u,%llu,\"%s\"\n",race,course,frames,ticks,
            ticks&&costRaceMarks.set?double(uint32_t(now.kcl-costRaceMarks.kcl))/double(ticks):0.0,
            (unsigned long)(costCpu?costCpu(1):0),(unsigned long)(costCpu?costCpu(2):0),unsigned(models.size()),dropped,textures,imports,
            now.linear-costLoadMarks.linear,linearAllocFailMax.load(),now.vram-costLoadMarks.vram,now.reads-costLoadMarks.reads,(now.readBytes-costLoadMarks.readBytes)/1024,list.c_str());
        std::fclose(f);
    }
    linearAllocFailMax.store(0);costLoadMarks=now;costRaceMarks.set=false;
}
}
#endif
