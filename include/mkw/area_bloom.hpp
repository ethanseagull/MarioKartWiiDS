#pragma once
#include <mkw/course_bloom.hpp>
#include <array>
#include <cstdint>
namespace mkw {
// KMP AREA type 6 ("BBLM swap"): the camera inside such an area switches the
// course's bloom to posteffect.bblm<setting 1> (0 = posteffect.bblm outside
// any), blending over 20 frames. Retail (USA StaticR): the race's per-frame
// update (0x805a6cb8) takes the area at the camera's position (getCurrentAreaID
// type 6) and stores its first setting as the screen's target (0x805a6d08);
// 0x805b4b68 then, while target != current, adds 0.05 to the progress each
// frame up to 1 (done), and on the frame after done makes the target current
// with progress 0. 0x805b49f8 applies the blend while it runs, only when both
// files exist (a missing file leaves the last applied parameters); the EGG
// blend (DOL 0x8021e1c0) takes the current file's fields and lerps the
// threshold, the threshold colour and tint (per byte, truncated), the blur
// strengths and intensities and the composite weights by progress.
// Retail courses with posteffect.bblm1: Mushroom Gorge, Koopa Cape and Grumble
// Volcano (tools/cook_course_bloom.py); off-file no-retail-areabloom.txt (main.cpp).
struct AreaBloom {
    uint8_t current=0,target=0;
    float progress=0;
    bool blending=false,done=false;
    // The parameters shown: a cooked table entry, or a blend in `blends`
    // (a ring: the render thread may still read the last frames' requests).
    const CourseBloom* shown=nullptr;
    std::array<CourseBloom,8> blends{};
    unsigned next=0;
    void reset(const char* archive){current=target=0;progress=0;blending=done=false;shown=courseBloom(archive,0);next=0;}
    static CourseBloom blend(const CourseBloom& from,const CourseBloom& to,float t){
        if(t<0.f)return from;
        if(t>1.f)return to;
        CourseBloom out=from;
        const float s=1.f-t;
        auto lerp=[&](float a,float b){return a*s+b*t;};
        auto color=[&](uint32_t a,uint32_t b){
            uint32_t c=0;
            for(unsigned shift:{24u,16u,8u,0u})c|=uint32_t(int(float((a>>shift)&255)*s+float((b>>shift)&255)*t))<<shift;
            return c;
        };
        out.threshold=lerp(from.threshold,to.threshold);
        out.thresholdColor=color(from.thresholdColor,to.thresholdColor);
        out.tint=color(from.tint,to.tint);
        for(unsigned i=0;i<2;++i){
            out.strength[i]=lerp(from.strength[i],to.strength[i]);
            out.intensity[i]=lerp(from.intensity[i],to.intensity[i]);
            out.weight[i]=lerp(from.weight[i],to.weight[i]);
        }
        return out;
    }
    // One 60 Hz frame; area: the KMP AREA type 6 setting 1 at the camera (-1: none).
    void step(const char* archive,int area){
        target=uint8_t(area>0&&area<256?area:0);
        if(target==current)return;
        if(done){blending=done=false;current=target;progress=0;return;}
        blending=true;progress+=.05f;
        if(progress>=1.f){progress=1.f;done=true;}
        const CourseBloom* from=courseBloom(archive,current);const CourseBloom* to=courseBloom(archive,target);
        if(!from||!to)return;
        blends[next]=blend(*from,*to,progress);shown=&blends[next];next=(next+1)%blends.size();
    }
};
}
