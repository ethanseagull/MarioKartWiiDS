#pragma once
// Course effect archives spawned by placed objects and the KartEffect environment roots.
// tools/cook_race_effects.py ENV_ARCHIVES puts a root in the course bank only when retail's
// EffectManager would load its archive (an object of that name placed), so a missing root
// (find < 0) means the Wii has no such effect on this course. Each case below cites its
// retail update (USA addresses).
// Presentation only: reads GOBJ entries and Kinoko object state, never changes them.
#include <mkw/race_effects.hpp>
#include <mkw/kart_effect_lod.hpp>
#include <game/field/ObjectDirector.hh>
#include <game/field/ObjectDrivableDirector.hh>
#include <game/field/obj/ObjectBoble.hh>
#include <game/field/obj/ObjectPress.hh>
#include <game/field/obj/ObjectShip64.hh>
#include <game/field/obj/ObjectCarTGE.hh>
#include <game/field/obj/ObjectHeyhoBall.hh>
#include <game/field/obj/ObjectWanwan.hh>
#include <game/field/obj/ObjectHwanwan.hh>
#include <game/field/obj/ObjectTruckWagon.hh>
#include <game/field/obj/ObjectKoopaBall.hh>
#include <game/system/CourseMap.hh>
#include <game/field/RailManager.hh>
#include <game/field/RailInterpolator.hh>
#include <game/system/RaceManager.hh>
#include <game/system/RaceConfig.hh>
#include <game/kart/KartObjectManager.hh>
#include <game/kart/KartObject.hh>
#include <game/kart/KartMove.hh>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>

namespace mkw {
class CourseEnvEffects {
public:
    static constexpr unsigned MaxUnits=128;
    // Bounded share of the effect pools: an object effect is not started while fewer than these
    // stay free, so course scenery never takes an item hit's or a kart's effect (kart_effect_lod).
    static constexpr unsigned ReserveEffects=32,ReserveEmitters=48,ReserveParticles=192;
    struct Input {Vec3 eye,target;int soundZone=-1;};
    bool bind(RaceEffects& effects){
        auto f=[&](const char* n){return effects.find(n);};
        envFire_=f("rk_EnvFire");envKareha_=f("rk_EnvKareha");karehaUp_=f("rk_EnvKarehaUp");snow_=f("rk_snow");clouds_=f("rk_kinokoClouds");moya_=f("rk_moyaW2D");housi_=f("rk_housi2D");housiDemo_=f("rk_housi2Ddemo");
        torch_=f("rk_taimatsu");volPre_=f("rk_volsmkPre");volBurst_=f("rk_volsmk1T");
        const char* falls[]={"rk_fallbasinA","rk_fallbasinB","rk_fallbasinC","rk_ridgeFall","rk_yoshiFallsA","rk_yoshiFallsB","rk_yoshiFallsC","rk_64DKfalls"};
        for(unsigned i=0;i<8;++i)falls_[i]=f(falls[i]);
        const char* flash[]={"rk_flashBeginner","rk_flashBCross","rk_flash64mario","rk_flashSkate_A","rk_flashSkate_B","rk_flashWaluigi_A","rk_flashWaluigi_B","rk_flashWaluigi_C"};
        for(unsigned i=0;i<8;++i)flash_[i]=f(flash[i]);
        heyho_=f("rk_HeyhoBallGBA");boble_=f("rk_boble");press_=f("rk_press");ship_=f("rk_DKshipSplash");pillar_=f("rk_dc_pillar");
        sandcone_=f("rk_dc_sandcone");bombCar_=f("rk_K_bomb_car");cruiser_=f("rk_cruiser");cruiserSail_=f("rk_cruiserSail");
        venice_=f("rk_venice_hasi");wanSmoke_=f("rk_wanSmk");truck_=f("rk_truck");bombHei_=f("rk_bombHei");
        reset();
        return true;
    }
    // Off-file (main.cpp): no-course-effects.txt stops every spawn here (object and environment
    // effects). Banks stay cooked.
    void setEnabled(bool course){courseOn_=course;}
    bool courseEnabled() const {return courseOn_;}
    void reset(){units_={};count_=0;linked_=false;generation_=~0u;last_={};lastSet_=false;stats={};}
    struct Stats {unsigned units=0,active=0,peakActive=0,peakParticles=0,peakEffects=0,peakEmitters=0,bursts=0,refused=0,envFrames=0,snowFrames=0,karehaFrames=0,karehaUpFrames=0,cloudFrames=0,mistFrames=0,sporeFrames=0;} stats;

    void step(RaceEffects& effects,const Input& in){
        namespace K=Kinoko;
        if(!effects.ready)return;
        if(!linked_||generation_!=effects.generation())link(effects);
        auto* kartManager=K::Kart::KartObjectManager::Instance();
        const auto* map=K::System::CourseMap::Instance();
        const auto* race=K::System::RaceManager::Instance();
        if(!kartManager||!map||!race)return;
        const auto* kart=kartManager->object(0);
        const auto& scenario=K::System::RaceConfig::Instance()->raceScenario();
        const unsigned course=unsigned(scenario.course);
        const int t=int(race->timer());
        const auto kp=kart->pos();const Vec3 kartPos{kp.x,kp.y,kp.z};
        const s16 area=map->getCurrentAreaID(-1,kp,K::System::MapdataAreaBase::Type(1));
        bool karehaUp=false;
        auto& sys=effects.system();
        auto room=[&](){
            // A live-particle limit below the pool (the Old 3DS profile's 64) keeps the same reserve
            // under the limit; main.cpp turns these effects off with that profile anyway.
            const bool underLimit=sys.particleLimit>=sys.ParticleCapacity||sys.liveParticles+ReserveParticles<=sys.particleLimit;
            if(underLimit&&sys.effects.freeCount>=ReserveEffects&&sys.emitters.freeCount>=ReserveEmitters&&sys.particles.freeCount>=ReserveParticles)return true;
            ++stats.refused;return false;
        };
        // create (vtable +0xc) only when the slot is dead, within the reserve.
        auto start=[&](unsigned slot,int spec,const EffectFrame& at){
            if(spec<0||effects.alive(slot))return;
            if(room())effects.create(slot,spec,at,0);
        };
        auto burst=[&](int spec,const EffectFrame& at){if(spec>=0&&room()){effects.burst(spec,at);++stats.bursts;}};
        unsigned active=0;
        for(unsigned i=0;i<count_;++i){
            Unit& u=units_[i];const auto& g=*u.gobj;
            const Vec3 pos=vec(g.pos());
            switch(u.kind){
            case Kind::Torch:{
                // 0x806ee260: torch 0 owns the shared rk_taimatsu and is never faded; every other
                // torch emits copies at its own position, faded while clipped (no rot/scale).
                EffectFrame at;at.origin=pos;
                if(u.first||!u.suppressed)start(u.slot,torch_,at);else effects.fade(u.slot);
                effects.move(u.slot,at);break;
            }
            case Kind::Fall:case Kind::Cruiser:{
                // 0x8077e3d8 (falls), 0x806e21e4 (static cruiser): create at makeRT(rot, pos) when not
                // clipped, re-created when it ends; fade when clipped. Scale ignored.
                const EffectFrame at=rtFrame(g);
                if(!u.suppressed)start(u.slot,u.spec,at);else effects.fade(u.slot);
                effects.move(u.slot,at);break;
            }
            case Kind::CruiserSail:{
                // 0x80882fa0: rk_cruiserSail with the GOBJ scale on the boat. The boat follows the
                // port's yacht approximation (course_object_movers.cpp drawCruiser): its route at
                // setting 0 speed, heading eased 10% a frame, bobbing 50 sin(2 (90 n + t)) degrees.
                EffectFrame at=rtFrame(g);const auto s=g.scale();
                if(u.rail){
                    u.rail->calc();const auto tg=u.rail->curTangentDir();
                    u.heading=normalized(u.heading+(Vec3{tg.x,tg.y,tg.z}-u.heading)*.1f,{0,0,1});
                    Vec3 p=vec(u.rail->curPos());p.y+=50.f*std::sin(2.f*float(u.a*90+t)*3.14159265f/180.f);
                    const Vec3 up{0,1,0},z=normalized(u.heading-up*dot(u.heading,up),{0,0,1});
                    at.origin=p;at.x=cross(up,z);at.y=up;at.z=z;
                }
                at.x=at.x*s.x;at.y=at.y*s.y;at.z=at.z*s.z;
                if(!u.suppressed)start(u.slot,cruiserSail_,at);else effects.fade(u.slot);
                effects.move(u.slot,at);break;
            }
            case Kind::Flash:{
                // 0x806e6924: mode 1 (Flash_L/B/M) follows a local racer's sound-trigger zone on
                // Luigi Circuit (8) and DK Summit (6): 2 or 3, Mario Raceway (0x1a): 1 or 2; other
                // slots never. Mode 0 (Flash_S/W) while the local kart tricks (ZipperTrick 0x8000,
                // TrickRot 0x400000). Root = GOBJ matrix, scale ignored.
                bool on=false;
                if(u.mode==1){
                    const int z=in.soundZone;
                    on=(course==8||course==6)?(z==2||z==3):course==0x1a?(z==1||z==2):false;
                }else{
                    using St=K::Kart::eStatus;on=kart->status().onBit(St::ZipperTrick,St::TrickRot);
                }
                const EffectFrame at=rtFrame(g);
                if(on&&!u.suppressed)start(u.slot,u.spec,at);else effects.fade(u.slot);
                effects.move(u.slot,at);break;
            }
            case Kind::Volsmk:{
                // 0x806eea04: rk_volsmkPre between start and end race frames (setting pair by engine
                // class), the one-shot rk_volsmk1T at end; both at the GOBJ position.
                EffectFrame at;at.origin=pos;
                if(t>=u.a&&t<u.b&&!u.suppressed){start(u.slot,volPre_,at);effects.move(u.slot,at);}else effects.fade(u.slot);
                if(t==u.b&&u.fired!=t&&!u.suppressed){u.fired=t;burst(volBurst_,at);}
                break;
            }
            case Kind::KarehaUp:{
                // 0x806ec8c0: on at the object's position while the local kart is in a type-1 AREA
                // whose first setting is 1; it then holds off the normal EnvKareha (+0xa8).
                const auto* a=area>=0?map->getArea(u16(area)):nullptr;
                EffectFrame at;at.origin=pos;
                if(a&&a->param(0)==1){start(u.slot,karehaUp_,at);effects.move(u.slot,at);karehaUp=true;++stats.karehaUpFrames;}
                else effects.fade(u.slot);
                break;
            }
            case Kind::Boble:{
                // 0x807510c4: rk_boble on the bubble's matrix every frame unless clipped (a clipped
                // frame neither creates nor moves it).
                if(!u.suppressed){const EffectFrame at=objFrame(*u.obj);start(u.slot,boble_,at);effects.move(u.slot,at);}
                break;
            }
            case Kind::Press:{
                // 0x80763c64: rk_press when Lowering meets the floor, at the press position with
                // the GOBJ scale.
                const int s=static_cast<const K::Field::ObjectPress*>(u.obj)->state();
                if(s==3&&u.prev==2&&!u.suppressed){
                    EffectFrame at;at.origin=vec(u.obj->pos());const auto sc=g.scale();at.x={sc.x,0,0};at.y={0,sc.y,0};at.z={0,0,sc.z};
                    burst(press_,at);
                }
                u.prev=s;break;
            }
            case Kind::Ship:{
                // 0x80775e78: rk_DKshipSplash on the ship's matrix, faded while clipped.
                const EffectFrame at=objFrame(*u.obj);
                if(!u.suppressed)start(u.slot,ship_,at);else effects.fade(u.slot);
                effects.move(u.slot,at);break;
            }
            case Kind::BombCar:{
                // 0x806c808c: rk_K_bomb_car on the car while it is intact; killed at the explosion,
                // created again once the regrown car is no longer exploded. Uniform scale.x.
                const auto& car=*static_cast<const K::Field::ObjectCarTGE*>(u.obj);
                EffectFrame at=objFrame(*u.obj);at.scale=u.obj->scale().x;
                if(!car.exploded())start(u.slot,bombCar_,at);
                if(!car.bombShown())effects.kill(u.slot);
                effects.move(u.slot,at);
                // 0x806caf5c (from the explosion, 0x806c938c): a one-shot rk_bombHei on the car's
                // matrix at uniform scale 1250 / 900, clip-gated (the blast model draws apart).
                if(car.exploded()&&u.prev==0&&!u.suppressed){EffectFrame b=objFrame(*u.obj);b.scale=1250.f/900.f;burst(bombHei_,b);}
                u.prev=car.exploded()?1:0;break;
            }
            case Kind::HeyhoBall:{
                // 0x806eb488: rk_HeyhoBallGBA at the GOBJ position 5 frames before the falling ball
                // lands (state 1, frame == settings[1] - 5). The blast is the model (course_object_kinds).
                const auto& ball=*static_cast<const K::Field::ObjectHeyhoBall*>(u.obj);
                if(ball.stateId()==1&&int(ball.stateFrame())==int(g.setting(1))-5&&u.fired!=t&&!u.suppressed){
                    u.fired=t;EffectFrame at;at.origin=pos;burst(heyho_,at);
                }
                // The explosion's rk_bombHei (+0xd8): created at state 3 frame 0 at the GOBJ position,
                // uniform scale 1500 / 900; its drawing is switched off while the ball is clipped
                // and on again when it is not (edges, +0x130); updated every frame.
                if(ball.stateId()==3&&ball.stateFrame()==0&&u.state!=t+1){
                    u.state=t+1;EffectFrame at;at.origin=pos;at.scale=1500.f/900.f;
                    effects.kill(u.slot);start(u.slot,bombHei_,at);
                }
                if(u.suppressed!=(u.mode!=0)){effects.setDisabled(u.slot,false,u.suppressed);u.mode=u.suppressed;}
                break;
            }
            case Kind::Pillar:{
                // 0x8084548c: the pillar falls from its start frame (setting by engine class) with
                // rot.x + 1e-7 * dt^3; the frame it reaches settings[1] degrees, rk_dc_pillar fires
                // once at makeRT((last falling angle, rot.y, rot.z), pos).
                const int dt=t-u.a;
                if(u.state==0){if(t==u.a)u.state=1;else if(t>u.a)u.state=2;}
                if(u.state==1&&dt>0){
                    const float rot=u.base+1e-7f*float(dt)*float(dt)*float(dt);
                    if(rot>=u.target){
                        u.state=2;
                        if(!u.suppressed){
                            K::EGG::Matrix34f m;const auto r=g.rot();
                            m.makeRT({u.last,r.y*DegToRad,r.z*DegToRad},g.pos());burst(pillar_,matrixFrame(m));
                        }
                    }else u.last=rot;
                }
                break;
            }
            case Kind::Sandcone:{
                // 0x80682f34: one shared rk_dc_sandcone: the first cone's is always on, the others
                // fade while clipped. Root = the rising cone (ObjectSandcone::getUpdatedMatrix) +
                // (0, 600 * scale.y, 0).
                const float flow=float(g.setting(0))/100.f;const int startFrame=int(g.setting(2));
                const int duration=flow>0?int(u16(float(g.setting(1))/flow)):0;
                const float rise=t>startFrame+duration?float(duration)*flow:t>startFrame?float(t-startFrame)*flow:0.f;
                EffectFrame at;at.origin=pos+Vec3{0,rise+600.f*g.scale().y,0};
                if(u.first||!u.suppressed)start(u.slot,sandcone_,at);else effects.fade(u.slot);
                effects.move(u.slot,at);break;
            }
            case Kind::Wanwan:case Kind::Hwanwan:{
                // 0x806d7928 / 0x806efe30: a one-shot rk_wanSmk with its colour at each spawn the
                // Kinoko object counts (floor contacts on the course's smoke surface), clip-gated.
                const bool big=u.kind==Kind::Hwanwan;
                const auto* chomp=big?static_cast<const K::Field::ObjectHwanwan*>(u.obj):nullptr;
                const unsigned serial=big?chomp->smokeSerial():static_cast<const K::Field::ObjectWanwan*>(u.obj)->smokeSerial();
                if(serial!=unsigned(u.prev)){
                    u.prev=int(serial);
                    if(!u.suppressed&&wanSmoke_>=0&&room()){
                        EffectFrame at;at.origin=vec(big?chomp->smokePos():static_cast<const K::Field::ObjectWanwan*>(u.obj)->smokePos());
                        static constexpr uint8_t small[4]{231,220,143,255},large[4]{255,255,208,255};
                        effects.burst(wanSmoke_,at,big?large:small);++stats.bursts;
                    }
                }
                break;
            }
            case Kind::Truck:{
                // 0x806d0140: rk_truck when a cart lands from the drop (Kinoko landSerial), then it
                // follows the cart's matrix while it lives.
                const auto& cart=*static_cast<const K::Field::ObjectTruckWagonCart*>(u.obj);
                const EffectFrame at=objFrame(cart);
                if(cart.landSerial()!=unsigned(u.prev)){u.prev=int(cart.landSerial());if(!u.suppressed)start(u.slot,truck_,at);}
                effects.move(u.slot,at);break;
            }
            case Kind::KoopaBall:{
                // 0x8075ec00 (Bowser's Castle fireball explodes, Kinoko state 2): a one-shot
                // rk_bombHei on the ball's matrix at scale 2, clip-gated (the blast is the model).
                const int s=static_cast<const K::Field::ObjectKoopaBall*>(u.obj)->state();
                if(s==2&&u.prev!=2&&!u.suppressed){EffectFrame b=objFrame(*u.obj);b.scale=2.f;burst(bombHei_,b);}
                u.prev=s;break;
            }
            case Kind::Venice:{
                // 0x80803d38: the bridge moves for 30 * frameTime frames from settings[0] (900 when
                // 0); rk_venice_hasi fires once at its position when that move has ended.
                if(t>=u.a&&t<u.b)u.state=1;
                if(u.state==1&&t>=u.b){u.state=2;if(!u.suppressed){EffectFrame at;at.origin=pos;burst(venice_,at);}}
                break;
            }
            }
            if(u.slot&&effects.alive(u.slot))++active;
        }
        stepEnvironment(effects,in,kart,course,area,karehaUp);
        stats.active=active;stats.peakActive=std::max(stats.peakActive,active);
        stats.peakParticles=std::max<unsigned>(stats.peakParticles,effects.liveParticles());
        stats.peakEffects=std::max<unsigned>(stats.peakEffects,unsigned(sys.effects.used()));
        stats.peakEmitters=std::max<unsigned>(stats.peakEmitters,unsigned(sys.emitters.used()));
    }
    // ClipInfoMgr flags read on the next step, as CreatureEffects::updateClip.
    void updateClip(const EffectClipCamera& camera,bool forwards){
        for(unsigned i=0;i<count_;++i){
            Unit& u=units_[i];
            const Vec3 p=u.obj?vec(u.obj->pos()):u.rail?vec(u.rail->curPos()):vec(u.gobj->pos());
            u.suppressed=forwards&&u.clip.radius>=0&&camera.objectCulled(p,u.clip.radius,u.clip.far,u.clip.horizontal);
        }
    }
    unsigned units() const {return count_;}
private:
    static constexpr float DegToRad=0.017453292f; // 0x8080bd94
    enum class Kind:u8 {Torch,Fall,Cruiser,CruiserSail,Flash,Volsmk,KarehaUp,Boble,Press,Ship,BombCar,HeyhoBall,Pillar,Sandcone,Venice,Wanwan,Hwanwan,Truck,KoopaBall};
    struct ClipSettings {float radius=-1,far=0;bool horizontal=false;};
    struct Unit {
        Kind kind{};const Kinoko::System::MapdataGeoObj* gobj=nullptr;const Kinoko::Field::ObjectBase* obj=nullptr;
        unsigned slot=0;int spec=-1,mode=0,prev=-1,state=0,fired=-1,a=0,b=0;float base=0,target=0,last=0;
        bool first=false,suppressed=false;ClipSettings clip;
        std::shared_ptr<Kinoko::Field::RailInterpolator> rail;Vec3 heading{0,0,1};
    };
    template<class V> static Vec3 vec(const V& v){return {v.x,v.y,v.z};}
    static EffectFrame matrixFrame(const Kinoko::EGG::Matrix34f& m){
        EffectFrame at;at.x=vec(m.base(0));at.y=vec(m.base(1));at.z=vec(m.base(2));at.origin=vec(m.base(3));return at;
    }
    // UpdateMatrix (0x8080db2c): makeRT(GOBJ rotation in radians, position).
    static EffectFrame rtFrame(const Kinoko::System::MapdataGeoObj& g){
        Kinoko::EGG::Matrix34f m;const auto r=g.rot();m.makeRT({r.x*DegToRad,r.y*DegToRad,r.z*DegToRad},g.pos());return matrixFrame(m);
    }
    static EffectFrame objFrame(const Kinoko::Field::ObjectBase& o){return matrixFrame(o.transform());}
    static ClipSettings clipSettings(unsigned id){
        // ObjFlow +0x64 mode, +0x66 radius, +0x68 far / 10 (CreatureEffects::clipSettings). Mode 0
        // registers no ClipInfo: never clipped (EnvKarehaUp).
        const auto* director=Kinoko::Field::ObjectDirector::Instance();
        const auto& flow=director->flowTable();const auto* entry=flow.set(flow.slot(Kinoko::Field::ObjectId(id)));
        if(!entry)return {};
        auto field=[&](unsigned n){return float(static_cast<int16_t>((unsigned(entry->_62[n])<<8)|entry->_62[n+1]));};
        if(field(2)==0)return {};
        return {field(4),10.f*field(6),field(2)==2};
    }
    void link(RaceEffects& effects){
        namespace K=Kinoko;
        units_={};count_=0;linked_=true;generation_=effects.generation();lastSet_=false;
        const auto* map=K::System::CourseMap::Instance();auto* director=K::Field::ObjectDirector::Instance();
        if(!map||!director)return;
        const auto& scenario=K::System::RaceConfig::Instance()->raceScenario();
        const unsigned engine=scenario.engineClass,course=unsigned(scenario.course);
        bool firstTorch=true,firstCone=true;unsigned slots=0,cruisers=0;
        // Settings shared by all bridges: retail keeps them in statics, the last one built wins.
        int veniceStart=900;float veniceTime=1.f;
        for(u16 i=0;i<map->getGeoObjCount();++i){
            const auto* g=map->getGeoObj(i);
            if(g&&g->id()==0x20a&&(g->presenceFlag()&1)){veniceStart=g->setting(0)?int(g->setting(0)):900;veniceTime=g->setting(1)?float(g->setting(1)):1.f;}
        }
        // The object built from this GOBJ entry; children built from the same entry share its id
        // (the chomp's stake "pile", Hwanwan's chomp, the truck carts), so `kcl` / `flags` pick one.
        // KCL objects live in ObjectDrivableDirector.
        auto objectFor=[&](const K::System::MapdataGeoObj* g,unsigned id,const char* kcl=nullptr,bool notKcl=false,unsigned flags=0)->const K::Field::ObjectBase*{
            auto match=[&](const K::Field::ObjectBase* o){
                return o->mapObj()==g&&unsigned(o->id())==id&&(!kcl||(std::strcmp(o->getKclName(),kcl)==0)!=notKcl)&&(!flags||o->loadFlags()==flags);
            };
            for(auto* o:director->objects())if(match(o))return o;
            if(auto* drivable=K::Field::ObjectDrivableDirector::Instance())
                for(auto* o:drivable->objects())if(match(o))return o;
            return nullptr;
        };
        for(u16 i=0;i<map->getGeoObjCount()&&count_<MaxUnits;++i){
            const auto* g=map->getGeoObj(i);
            if(!g||!(g->presenceFlag()&1))continue;
            const unsigned id=g->id();
            if(!courseOn_)continue;
            Unit u;u.gobj=g;bool slot=true;
            switch(id){
            case 0x2bf:if(torch_<0)continue;u.kind=Kind::Torch;u.first=firstTorch;firstTorch=false;break;
            case 0x2cb:case 0x2cc:case 0x2cd:u.kind=Kind::Fall;u.spec=falls_[id-0x2cb];break;
            case 0x2d9:u.kind=Kind::Fall;u.spec=falls_[3];break;
            case 0x2da:u.kind=Kind::Fall;u.spec=g->setting(0)<3?falls_[4+g->setting(0)]:-1;break;
            case 0x2f3:u.kind=Kind::Fall;u.spec=falls_[7];break;
            case 0x2d0:u.kind=Kind::Flash;u.mode=1;u.spec=flash_[0];break;
            case 0x2d1:u.kind=Kind::Flash;u.mode=1;u.spec=flash_[1];break;
            case 0x2d3:u.kind=Kind::Flash;u.mode=1;u.spec=flash_[2];break;
            case 0x2d4:u.kind=Kind::Flash;u.mode=0;u.spec=g->setting(0)<2?flash_[3+g->setting(0)]:-1;break;
            case 0x2d2:u.kind=Kind::Flash;u.mode=0;u.spec=g->setting(0)<3?flash_[5+g->setting(0)]:-1;break;
            case 0x2ce:{
                if(volPre_<0&&volBurst_<0)continue;
                u.kind=Kind::Volsmk;
                // 0x806ee8c8: settings (1,2) at 150cc, (3,4) at 100cc, (5,6) at 50cc; Block Plaza (0x21) (1,2).
                unsigned a=engine==0?5:engine==1?3:1;if(course==0x21)a=1;
                u.a=int(g->setting(a))*60;u.b=u.a+int(g->setting(a+1))*60;break;
            }
            case 0x2f1:if(karehaUp_<0)continue;u.kind=Kind::KarehaUp;break;
            case 0x147:u.kind=Kind::Cruiser;u.spec=cruiser_;break;
            case 0xec:{
                u.kind=Kind::CruiserSail;u.spec=cruiserSail_;u.a=int(cruisers++);
                auto* rails=K::Field::RailManager::Instance();const int path=g->pathId();
                if(rails&&path>=0&&unsigned(path)<map->getPointInfoCount()){
                    const float speed=float(g->setting(0));
                    if(map->getPointInfo(u16(path))->setting(0)==0)u.rail=std::make_shared<K::Field::RailLinearInterpolator>(speed,unsigned(path));
                    else u.rail=std::make_shared<K::Field::RailSmoothInterpolator>(speed,unsigned(path));
                    u.rail->init(0.f,0);const auto tg=u.rail->curTangentDir();u.heading={tg.x,tg.y,tg.z};
                }
                break;
            }
            case 0xdd:u.kind=Kind::Boble;u.spec=boble_;u.obj=objectFor(g,id);if(!u.obj)continue;break;
            case 0x19d:u.kind=Kind::Press;u.spec=press_;u.obj=objectFor(g,id);slot=false;if(!u.obj)continue;break;
            case 0x200:u.kind=Kind::Ship;u.spec=ship_;u.obj=objectFor(g,id);if(!u.obj)continue;break;
            case 0xde:{
                u.kind=Kind::BombCar;u.spec=bombCar_;u.obj=objectFor(g,id);u.prev=0;if(!u.obj)continue;
                if(static_cast<const K::Field::ObjectCarTGE*>(u.obj)->carType()!=K::Field::ObjectCarTGE::CarType::BombCar)continue;
                break;
            }
            case 0xea:u.kind=Kind::HeyhoBall;u.spec=heyho_;u.obj=objectFor(g,id);if(!u.obj)continue;break;
            case 0x208:{
                u.kind=Kind::Pillar;u.spec=pillar_;slot=false;
                // 0x80845090: fall start settings[0] at 150cc, [2] at 100cc, [3] at 50cc.
                u.a=int(g->setting(engine==1?2:engine==0?3:0));u.target=3.14159265f*float(g->setting(1))/180.f;
                u.base=g->rot().x*DegToRad;u.last=u.base;break;
            }
            case 0x209:if(sandcone_<0)continue;u.kind=Kind::Sandcone;u.first=firstCone;firstCone=false;break;
            case 0x196:{
                u.kind=Kind::Wanwan;u.spec=wanSmoke_;u.obj=objectFor(g,id,"pile",true);slot=false;if(!u.obj)continue;
                u.prev=int(static_cast<const K::Field::ObjectWanwan*>(u.obj)->smokeSerial());break;
            }
            case 0xe9:{
                u.kind=Kind::Hwanwan;u.spec=wanSmoke_;u.obj=objectFor(g,id,"wanwan");slot=false;if(!u.obj)continue;
                u.prev=int(static_cast<const K::Field::ObjectHwanwan*>(u.obj)->smokeSerial());break;
            }
            case 0x19b:{
                // One unit per cart of the spawner (each its own handle).
                if(truck_<0)continue;
                const auto* spawner=static_cast<const K::Field::ObjectTruckWagon*>(objectFor(g,id,nullptr,false,3));if(!spawner)continue;
                const ClipSettings clip=clipSettings(id);
                for(size_t c=0;c<spawner->cartCount()&&count_<MaxUnits;++c){
                    Unit cu;cu.gobj=g;cu.kind=Kind::Truck;cu.spec=truck_;cu.obj=spawner->cart(c);cu.clip=clip;cu.slot=1;
                    cu.prev=int(spawner->cart(c)->landSerial());units_[count_++]=cu;++slots;
                }
                continue;
            }
            case 0x20a:u.kind=Kind::Venice;u.spec=venice_;slot=false;u.a=veniceStart;u.b=veniceStart+int(30.f*veniceTime);break;
            default:continue;
            }
            if(u.spec<0&&u.kind!=Kind::Torch&&u.kind!=Kind::Volsmk&&u.kind!=Kind::KarehaUp&&u.kind!=Kind::Sandcone)continue;
            u.clip=clipSettings(id);u.slot=slot?1:0;
            units_[count_++]=u;slots+=slot;
        }
        // Bowser's Castle fireballs: Kinoko builds them from their GOBJ entries (one unit each).
        for(auto* o:director->objects()){
            if(!courseOn_||bombHei_<0||count_>=MaxUnits||unsigned(o->id())!=0xcf||!o->mapObj())continue;
            Unit u;u.gobj=o->mapObj();u.obj=o;u.kind=Kind::KoopaBall;u.spec=bombHei_;u.clip=clipSettings(0xcf);
            u.prev=static_cast<const K::Field::ObjectKoopaBall*>(o)->state();units_[count_++]=u;
        }
        // Three KartEffect environment handles, then one per persistent object effect.
        const unsigned first=effects.allocateObjectSlots(7+slots);envSlot_=first;
        unsigned next=first+7;
        for(unsigned i=0;i<count_;++i)units_[i].slot=units_[i].slot?next++:0;
        stats.units=count_;
    }
    // KartEffect (single screen, the local player's camera): EnvFire 0x8069af94 -> 0x806a8c00 (on
    // outside every type-1 AREA), EnvKareha 0x8069b208 -> 0x806a915c (on inside one, unless
    // EnvKarehaUp claimed the frame), rk_snow 0x806a8694 on course slot 6 (always). Each: point P =
    // eye - horizontal view axis * 1400, root at P + 20 * (P - last), emit ratio 0.5 + 1.5 s/max,
    // life 150 - 125 s/max (s = |velocity| clamped to [0, max], max = max(soft speed limit, 50));
    // then last = P. The fade (0x806a9090) sets last = eye.
    void stepEnvironment(RaceEffects& effects,const Input& in,const Kinoko::Kart::KartObject* kart,unsigned course,s16 area,bool karehaUp){
        if(!courseOn_)return;
        Vec3 back=in.eye-in.target;back.y=0;
        const float len=std::sqrt(dot(back,back));back=len>0?back*(1.f/len):Vec3{0,0,1};
        const Vec3 p=in.eye-back*1400.f;
        const auto v=kart->velocity();const float speed=std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z);
        const float limit=std::max(kart->move()->softSpeedLimit(),50.f);
        const float s=std::min(std::max(speed,0.f),limit);
        const float ratio=0.5f+s*(2.f-0.5f)/limit;
        const uint16_t life=uint16_t(150.f-s*(150.f-25.f)/limit);
        const auto kp=kart->pos();const Vec3 kartPos{kp.x,kp.y,kp.z};
        using St=Kinoko::Kart::eStatus;const bool overZipper=kart->status().onBit(St::OverZipper);
        auto run=[&](unsigned slot,int spec,bool on){
            if(spec<0)return;
            if(!on){effects.fade(slot);last_=in.eye;lastSet_=true;return;}
            if(!lastSet_){last_=p;lastSet_=true;}
            EffectFrame at;at.origin=p+(p-last_)*20.f;
            // 0x806a8f54..0x806a8fa8: while the kart flies off a half-pipe zipper (KartState
            // bitfield1 0x400, Kinoko OverZipper) the root goes to the kart's position instead.
            if(overZipper)at.origin=kartPos;
            effects.create(slot,spec,at,0);effects.move(slot,at);
            effects.setEmissionRatio(slot,ratio);effects.setParticleLife(slot,life);
            last_=p;
        };
        if(snow_>=0&&course==6){run(envSlot_+2,snow_,true);++stats.snowFrames;}
        // EffectManager cloud handle (calc 0x80676f40): rk_kinokoClouds at the world origin on
        // course slot 2, created again whenever it ends, every frame.
        if(clouds_>=0&&course==2){effects.create(envSlot_+3,clouds_,EffectFrame{},0);++stats.cloudFrames;}
        // KartEffect 2D screen effects (update 0x8069b490, slot 2, 1 screen, game type 0): outside
        // every type-1 AREA the mist rk_moyaW2D (emit ratio 0.02, life 125, radiation 1); inside one
        // the spores rk_housi2D (ratio 0.2 + 0.8 s/max, life 125 - 100 s/max, radiation 5 s/max), or
        // rk_housi2Ddemo once the local racer has finished; the others fade. Their translate is the
        // origin: drawn in camera space like the Blooper ink (group 1, blooper_screen_effects.hpp).
        if(course==2&&(moya_>=0||housi_>=0||housiDemo_>=0)){
            effects.system().cameraSpaceGroups|=1u<<1;
            const unsigned moya=envSlot_+4,spores=envSlot_+5,demo=envSlot_+6;
            const auto* race=Kinoko::System::RaceManager::Instance();
            const bool finished=race&&race->player(0).finished();
            if(area>=0){
                effects.fade(moya);
                if(finished&&housiDemo_>=0){effects.create(demo,housiDemo_,EffectFrame{},1);++stats.sporeFrames;}
                else if(housi_>=0){
                    effects.create(spores,housi_,EffectFrame{},1);
                    effects.setEmissionRatio(spores,0.2f+s*(1.f-0.2f)/limit);
                    effects.setParticleLife(spores,uint16_t(125.f-s*(125.f-25.f)/limit));
                    effects.setRadiationVelocity(spores,s*5.f/limit);
                    ++stats.sporeFrames;
                }
            }else{
                if(moya_>=0){
                    effects.create(moya,moya_,EffectFrame{},1);
                    effects.setEmissionRatio(moya,0.02f);effects.setParticleLife(moya,125);effects.setRadiationVelocity(moya,1.f);
                    ++stats.mistFrames;
                }
                effects.fade(spores);effects.fade(demo);
            }
        }
        if(envFire_>=0){run(envSlot_,envFire_,area<0);if(area<0)++stats.envFrames;}
        if(envKareha_>=0){const bool on=area>=0&&!karehaUp;run(envSlot_+1,envKareha_,on);if(on)++stats.karehaFrames;}
    }
    std::array<Unit,MaxUnits> units_{};unsigned count_=0,generation_=~0u,envSlot_=0;bool linked_=false,lastSet_=false,courseOn_=true;Vec3 last_{};
    int envFire_=-1,envKareha_=-1,karehaUp_=-1,snow_=-1,clouds_=-1,moya_=-1,housi_=-1,housiDemo_=-1,torch_=-1,volPre_=-1,volBurst_=-1,falls_[8]{},flash_[8]{},heyho_=-1,boble_=-1,
        press_=-1,ship_=-1,pillar_=-1,sandcone_=-1,bombCar_=-1,cruiser_=-1,cruiserSail_=-1,venice_=-1,wanSmoke_=-1,truck_=-1,bombHei_=-1;
};
}
