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
        return p;
    }();
    return profile;
}
}
