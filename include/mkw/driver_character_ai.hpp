#pragma once
#include <cmath>
#include <cstdint>
#include <span>
#include <mkw/math.hpp>

namespace mkw {
// Retail AI::Character, the per-racer driver "gesture" AI (Retro Rewind's GameSource AI/CharacterAI.hpp;
// PAL 0x80735948..0x80738660, USA = PAL + 0xca1c here). Presentation only: it writes the
// DriverController head-look request (+0x2a8 target, +0x2b8 response, +0x2bc state: 1 = this AI,
// 2 = an item's, which it never overrides) and the overtake voice (CharacterActor 0x80865580,
// PlayCharacterSound type 4 behind +0x6ff and the +0x6dc cooldown). No physics or input change.
// Actions (Character +0x38 + 0x2c each; Base::UpdateActions 0x8074af38 runs OnEnd, OnStart, Update):
//   0 start wait (555 frames), 1 scan, 2 CPU item target (not ported: CPUDriving's target),
//   3 racer close ahead, 4 overtaken it (voice + look back), 5 a racer drafting behind,
//   6 bump (the old DriverCollisionLook), 7 inked (head shake), lookAhead (after the goal),
//   lookAtCamera (a Mii in a replay / after its goal: not ported, treated as lookAhead), default (TT).
// The look target's height offset (bss 0x805114c0 USA) has no writer in StaticR: 0.
struct CharacterAiRacer {
    Vec3 pos,up,dir;             // Kart::Link position, Movement +0x38 smoothedUp, +0x74 vel1Dir
    bool driftManual=false,driftAuto=false,ink=false,hidden=false; // status 3, 28, 92, 83
    bool slipCharging=false,slipBoost=false;  // status 64, 65
    int slipTimer=0;             // Movement +0xec
    unsigned rank=0;             // RaceInfo player +0x20
};

class DriverCharacterAi {
public:
    enum class Mode : uint8_t {Race,Battle,Idle};
    enum Action : uint8_t {Default,Start,Scan,Ahead,Overtake,Drafted,Bump,Ink,LookAhead,Count};
    // Character::Init (0x807362dc); CharacterBattle::Init (0x807385ec) goes straight to Scan.
    void init(Mode mode){
        *this={};battle_=mode==Mode::Battle;
        next_=mode==Mode::Idle?Default:battle_?Scan:Start;pending_=true;cur_=Default;
    }
    // Kart respawn (USA 0x8057dda0 -> Player vf_0x20 -> Character::vf_0x20 0x80736400).
    void respawn(){if(cur_!=Default&&cur_!=Start&&cur_!=LookAhead){next_=Scan;pending_=true;}}
    // Player::OnStartPostRaceDriving (USA 0x80728bfc) -> StartDriving (0x80736480).
    void finish(){next_=LookAhead;pending_=true;}
    // The accepted bump (0x807364d8: impulse length over 8), the frame's flag +0x20c.
    // It writes +0x204 (the racer looked at) at once, whatever the action.
    void bump(unsigned other,Vec3 impulse){if(length(impulse)>8.f){bump_=true;bumpImpulse_=impulse;other_=other;}}
    // Character::Update (0x807363c0): the actions, then the bump flag clears.
    void step(unsigned self,std::span<const CharacterAiRacer> racers){
        if(self>=racers.size())return;
        if(pending_){onEnd();cur_=next_;pending_=false;onStart(self,racers);}
        update(self,racers);
        bump_=false;
    }
    bool look()const{return look_;}
    Vec3 target()const{return target_;}
    Action action()const{return pending_?next_:cur_;}
    bool takeOvertake(){const bool v=overtake_;overtake_=false;return v;}
    unsigned other()const{return other_;}
private:
    static constexpr float FarDraft=500.f,NearAhead=1000.f,Drafter=2500.f,DraftCone=.7f,PassMargin=50.f;
    static int noise(float v){return int(std::fabs(v)*.1f);}
    void lookOn(){look_=true;}
    void lookOff(){look_=false;}
    void setNext(Action a){next_=a;pending_=true;}
    // ArePlayersClose (0x80736764): within d and ahead of the travel direction.
    static bool close(const CharacterAiRacer& self,const CharacterAiRacer& other,float d){
        const Vec3 v=other.pos-self.pos;return length(v)<d&&dot(v,self.dir)>0.f;
    }
    // 0x8073684c: the other racer drafts behind (status 64, +0xec over 20) in the 0.7 cone.
    static bool drafting(const CharacterAiRacer& self,const CharacterAiRacer& other){
        if(!other.slipCharging||other.slipTimer<=20)return false;
        const Vec3 v=self.pos-other.pos;const float l=length(v);
        return l<Drafter&&dot(v,self.dir)>l*DraftCone;
    }
    // vf_0x8c (0x807369bc): two candidates a frame from the +0x204 cursor.
    bool find(unsigned self,std::span<const CharacterAiRacer> racers,unsigned& found,bool& draft){
        const unsigned n=unsigned(racers.size());bool hit=false;unsigned at=cursor_%n;
        for(unsigned k=0;k<2&&!hit;++k,at=(at+1)%n){
            if(at==self||racers[at].hidden)continue;
            if(close(racers[self],racers[at],NearAhead)){found=at;draft=false;hit=true;}
            else if(drafting(racers[self],racers[at])){found=at;draft=true;hit=true;}
        }
        cursor_=(cursor_+2)%n;return hit;
    }
    // The +0x208 helper (ctor 0x80737c30): 0x80737ecc glances over a shoulder, 0x807381a4 shakes the head.
    void glance(const CharacterAiRacer& me){
        --hTimer_;const Vec3& p=me.pos;const Vec3& f=me.dir;
        switch(hPhase_){
        case 0:hCount_=noise(p.x+p.z)%3+1;hSide_=noise(p.x+p.z+f.x+f.z)%2==0?1.f:-1.f;hPhase_=1;[[fallthrough]];
        case 1:hSide_=-hSide_;hTimer_=noise(p.x+p.z)%30+25;hPhase_=2;[[fallthrough]];
        case 2:{ // 0x80737c9c: 200 to the side, 500 behind
            const Vec3 s=normalized(cross(me.up,me.dir),{1,0,0});lookOn();target_=p+s*(200.f*hSide_)-me.dir*500.f;
            if(hTimer_>=0)return;
            if(--hCount_>0){hTimer_=10;hPhase_=3;}else{hPhase_=4;hTimer_=noise(p.x+p.z)%80+80;}
            return;}
        case 3:lookOff();if(hTimer_<0)hPhase_=1;return;
        case 4:lookOff();if(hTimer_<0)hPhase_=0;return;
        }
    }
    void shake(const CharacterAiRacer& me){
        --hTimer_;const Vec3& p=me.pos;const Vec3& f=me.dir;
        switch(hPhase_){
        case 0:hSide_=noise(f.x+f.z+p.x+p.z)%2==0?1.f:-1.f;hPhase_=1;[[fallthrough]];
        case 1:hCount_=noise(p.x+p.z)%2+2;hTimer_=noise(p.x+p.z)%10+10;hPhase_=2;[[fallthrough]];
        case 2:{
            const Vec3 s=normalized(cross(me.up,me.dir),{1,0,0});lookOn();target_=p+s*(200.f*hSide_);
            if(hTimer_>=0)return;
            const int c=--hCount_;
            if(c<0){hPhase_=1;hSide_=-hSide_;}
            else if(c){hSide_=-hSide_;hTimer_=noise(p.x+p.z)%10+10;}
            else hTimer_=noise(p.x+p.z)%20+30;
            return;}
        }
    }
    void onStart(unsigned self,std::span<const CharacterAiRacer> racers){
        const auto& me=racers[self];
        switch(cur_){
        case Start:case Ahead:timer_=0;break;
        case Overtake:{ // 0x807371a8
            const int r=noise(me.pos.x+me.pos.z)%5;
            if(r>=4)lookOff();else{overtake_=true;lookOn();}
            timer_=25+r*10;break;}
        case Drafted:case Ink:timer_=0;hPhase_=0;break;
        case Bump:{ // 0x8073763c
            const int r=noise(me.pos.x+me.pos.z);
            timer_=length(bumpImpulse_)<15.f?15+r%10:30+r%20;break;}
        case LookAhead:lookOff();break;
        default:break;
        }
    }
    void onEnd(){if(cur_==Overtake||cur_==Drafted||cur_==Bump||cur_==Ink)lookOff();}
    void update(unsigned self,std::span<const CharacterAiRacer> racers){
        const auto& me=racers[self];
        auto follow=[&]{if(other_<racers.size()){lookOn();target_=racers[other_].pos;}};
        switch(cur_){
        case Start:if(++timer_>555)setNext(Scan);break;
        case Scan:{
            if(battle_){if(me.ink)setNext(Ink);else if(bump_)setNext(Bump);return;} // 0x80738628
            const bool drifting=me.driftManual||me.driftAuto;
            unsigned found=0;bool draft=false;
            if(me.ink){setNext(Ink);return;}
            if(find(self,racers,found,draft)&&!drifting){other_=found;setNext(draft?Drafted:Ahead);return;}
            if(bump_)setNext(Bump);
            return;}
        case Ahead: // 0x80737054: passed it when ahead in rank and 50 behind along the travel direction
            if(++timer_>=60)setNext(Scan);
            else if(other_<racers.size()&&me.rank<racers[other_].rank&&dot(racers[other_].pos-me.pos,me.dir)<-PassMargin)setNext(Overtake);
            break;
        case Overtake: // 0x807372e4
            if(look_)follow();
            if(--timer_<0)setNext(Scan);
            break;
        case Drafted:{ // 0x807373f8
            if(other_>=racers.size()){setNext(Scan);break;}
            const auto& o=racers[other_];
            lookOn();
            if(o.slipBoost||length(o.pos-me.pos)<=FarDraft)target_=o.pos;else glance(me);
            if(++timer_>60&&((!o.slipCharging&&!o.slipBoost)||timer_>180))setNext(Scan);
            break;}
        case Bump: // 0x80737798: follows the latest bumper
            follow();if(--timer_<0)setNext(Scan);
            return; // no bump check here
        case Ink:shake(me);if(!me.ink)setNext(Scan);return;
        default:return;
        }
        if(bump_)setNext(Bump); // Start, Ahead, Overtake, Drafted
    }
    Action cur_=Default,next_=Default;bool pending_=false,battle_=false;
    bool look_=false,overtake_=false,bump_=false;
    Vec3 target_{},bumpImpulse_{};
    unsigned other_=~0u,cursor_=0;
    int timer_=0,hPhase_=0,hTimer_=0,hCount_=0;float hSide_=1.f;
};
}
