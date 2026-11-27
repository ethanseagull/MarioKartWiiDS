#pragma once
// The Old 3DS profile (user, Nov 23-24): a lighter game on consoles that are not a New 3DS, on top of
// what Old 3DS already locks (the Performance settings preset, 30 fps races,
// RenderQuality::setReduced). New 3DS keeps every value below at its default.
//   race sound: wave cache at most 384 KB (640), the player's voice group plays
//     through the cache instead of staying resident
//   course objects: hidden at 0.7x retail's clip distance, their -low models
//     from 0.7x the distance
//   at most 64 live effect particles (pool 1024); kart effect LOD as retail's
//     3-4 screen rule (full effects for the nearest other kart only)
//   16 sound voices (24)
//   course meshes in LINEAR (Model::packCourse, after the course weld):
//     a) parts no draw reads a normal of store vertices without it (44 -> 32
//        bytes); b) vertices no index list uses (the weld's merged-away ones)
//        are freed. a per submesh: a part with some lit submeshes keeps the
//        full record only for the vertices those draw (a vertex shared with a
//        normal-free submesh is stored twice) and the rest without the
//        normal. All exact.
//     c) such parts store s16 positions on one grid per course (track, sky),
//        28 bytes, near: within half a grid step. Off: it moves vertices by up
//        to half a step and changes pixels (DIAG o3ds-short-positions.txt).
//     DIAG off-files: o3ds-keep-normals.txt (a), o3ds-no-mesh-compact.txt (b),
//     o3ds-part-normals.txt (a per part only).
// Racer, item and Lakitu shadow volumes stay (user, Nov 24). 3D is already off
// on Old 3DS (stereoScreen).
// DIAG: o3ds-profile-off.txt turns it off on an Old 3DS (A/B), and
// o3ds-profile-on.txt turns it on on a New 3DS (picture comparison).
#include <mkw/console_model.hpp>
#include <cstdio>

namespace mkw {
struct Old3dsProfile {
    bool active=false;
    bool switched=false;          // a DIAG file changed the console's default
    unsigned raceSoundCache=0;    // race wave cache cap in bytes (0: the default)
    bool voicesThroughCache=false;// local voice group not resident
    float drawDistance=1.f;       // retail clip distance and -low switch scale
    unsigned particleLimit=0;     // live particles (0: the pool's capacity)
    unsigned kartEffectScreens=0; // KartEffectLod screens (0: unchanged)
    int voices=0;                 // hardware voices (0: VoiceMax)
    bool meshNoNormals=false;     // course meshes a
    bool meshCompact=false;       // course meshes b
    bool meshShortPositions=false;// course meshes c
    bool meshSubmeshNormals=false;// course meshes a per submesh
};
inline const Old3dsProfile& old3dsProfile(){
    static const Old3dsProfile profile=[]{
        Old3dsProfile p;
        bool on=!isNew3ds();
#ifndef MKW_RELEASE
        auto exists=[](const char* path){FILE* f=std::fopen(path,"rb");if(f)std::fclose(f);return f!=nullptr;};
        if(on&&exists("sdmc:/3ds/MKWiiDS/o3ds-profile-off.txt")){on=false;p.switched=true;}
        else if(!on&&exists("sdmc:/3ds/MKWiiDS/o3ds-profile-on.txt")){on=true;p.switched=true;}
#endif
        if(!on)return p;
        p.active=true;p.raceSoundCache=384u*1024;p.voicesThroughCache=true;
        p.drawDistance=.7f;p.particleLimit=64;p.kartEffectScreens=3;p.voices=16;
        p.meshNoNormals=p.meshCompact=p.meshSubmeshNormals=true;
#ifndef MKW_RELEASE
        if(exists("sdmc:/3ds/MKWiiDS/o3ds-keep-normals.txt"))p.meshNoNormals=false;
        if(exists("sdmc:/3ds/MKWiiDS/o3ds-no-mesh-compact.txt"))p.meshCompact=false;
        if(exists("sdmc:/3ds/MKWiiDS/o3ds-short-positions.txt"))p.meshShortPositions=true;
        if(exists("sdmc:/3ds/MKWiiDS/o3ds-part-normals.txt"))p.meshSubmeshNormals=false;
#endif
        return p;
    }();
    return profile;
}
}
