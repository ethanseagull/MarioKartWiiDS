#pragma once
#include <egg/math/Matrix.hh>
namespace mkw {
// Cheep cheep (pukupuku, ObjFlow 0x172; retail ctor PAL 80889744, init 808897f0, calc 808899a8,
// tables USA 0x808d8a58 / 0x808ae130). Kinoko has no class for it (no collision): presentation
// only. It swims around its placement (`center`): init puts it 500 out along ez turned by Euler
// (i, i+1, i+2) radians, i = its construction index (static counter PAL 805177f4). Each frame:
// pitch eases (x0.992 + target*0.008) toward -0.3 while rising until it is 400 above the
// placement, then +0.3 until 400 below; beyond 500 from the placement it picks a turn side once
// (ey x toCenter against its forward) and the yaw rate eases (x0.95 + target*0.05) toward -/+0.01,
// inside 500 toward 0; yaw += rate; rotation = EGG makeR(pitch, yaw, 0); speed eases
// (x0.995 + 3.5*0.005); velocity = forward*speed + (0,-2,0) + (0,2.01,0) (static vectors PAL
// 80517804/8051781c); position += velocity, held within 1000 of the placement; the swim clip
// plays at 0.6*|velocity|/3.5. After 800 frames (not while turning) it coasts 120 frames (yaw
// rate easing to 0, pitch held), then back.
struct PukupukuMotion {
    using V=Kinoko::EGG::Vector3f;using M=Kinoko::EGG::Matrix34f;
    V center{},pos{},vel{};M rot{};
    float pitch=0,yaw=0,yawRate=0,frame=0;
    bool turnSide=true,turning=false,rising=false;
    unsigned state=0,timer=0,tick=0;bool started=false;
    void reset(const V& placement,const M& placementRot,unsigned index){
        center=placement;pitch=yaw=yawRate=frame=0;vel=V(0,0,0);turnSide=true;turning=false;rising=!(index&1);
        state=0;timer=0;tick=0;started=true;rot=placementRot;
        M r;r.makeR(V(float(index),float(index+1),float(index+2)));
        pos=center+r.multVector33(V(0,0,1)*500.f);
    }
    void step(){
        if(state==0){
            float target;
            if(rising){if(center.y+400.f<pos.y){target=.3f;rising=false;}else target=-.3f;}
            else if(pos.y<center.y-400.f){target=-.3f;rising=true;}else target=.3f;
            const V toCenter=center-pos;
            if(500.f<=Kinoko::EGG::Mathf::sqrt(toCenter.dot(toCenter))){
                if(!turning){turnSide=V(0,1,0).cross(toCenter).dot(rot.base(2))>=0.f;turning=true;}
            }else turning=false;
            yawRate=yawRate*(1.f-.05f)+(turning?(turnSide?-.01f:.01f):0.f)*.05f;
            pitch=pitch*(1.f-.008f)+target*.008f;
        }else yawRate=yawRate*(1.f-.05f);
        yaw+=yawRate;
        rot.makeR(V(pitch,yaw,0));
        const float speed=Kinoko::EGG::Mathf::sqrt(vel.dot(vel))*(1.f-.005f)+3.5f*.005f;
        vel=rot.base(2)*speed+V(0,-2.f,0)+V(0,2.01f,0);
        pos=pos+vel;
        const V d=center-pos;const float len=Kinoko::EGG::Mathf::sqrt(d.dot(d));
        if(1000.f<len&&d.dot(d)>1.1920929e-07f){V n=d;n.normalise();pos=center-n*1000.f;}
        frame+=.6f*Kinoko::EGG::Mathf::sqrt(vel.dot(vel))/3.5f;
        ++timer;
        if(state==0&&timer>=800&&!turning){state=1;timer=0;}
        else if(state==1&&timer>=120){state=0;timer=0;}
    }
    // Advance simulation ticks rather than render calls, including time hidden.
    void advance(unsigned now){while(tick<now){step();++tick;}}
};
}
