#include "ObjectQuicksand.hh"
#include "game/field/CollisionDirector.hh"
#include "game/field/ObjectDirector.hh"
#include "game/kart/KartObject.hh"
#include <bit>
#include <cmath>
#include "game/system/RaceManager.hh"
#include <algorithm>
#include <type_traits>
namespace Kinoko::Field {
// RMCU807fceb0, globals808d05f0: wait1200, wave800, reset600;
// amplitude200, spatial frequency1.5, depth360, period40, radius9500.
ObjectQuicksand::ObjectQuicksand(const System::MapdataGeoObj& p):ObjectDrivable(p),m_origin(pos()){
    if(p.setting(0)!=1){m_thwomp=EGG::egg_new<ObjectQuicksandThwomp>(p);m_thwomp->load();}
}
// USA 807fe308..807fe398: signed (divw) on the frame, so a frame before 0 stays negative.
unsigned ObjectQuicksand::state(u32 frame){const s32 p=static_cast<s32>(frame)%2600;return p<1200?1:p<2000?2:0;}
s32 ObjectQuicksand::stateFrame(u32 frame){const s32 p=static_cast<s32>(frame)%2600;return p<1200?p:p<2000?p-1200:p-2000;}
f32 ObjectQuicksand::waveAmplitude(u32 frame)const{
    // USA 807fe39c..807fe49c, operation for operation: the crest angle (global,
    // 15 degrees) in table units of 128/180, then 200 x SinFIdx.
    if(state(frame)!=2)return 0.f;
    constexpr f32 DEG=15.f,DEG2FIDX=std::bit_cast<f32>(0x3f360b61u);
    const s32 age=stateFrame(frame);const f32 a=static_cast<f32>(age);
    const f32 rise=180.f-DEG,fall=20.f*DEG;f32 fidx;
    if(a<rise)fidx=DEG2FIDX*((rise*a)/rise);
    else if(a<800.f-fall)fidx=DEG2FIDX*DEG;
    else fidx=DEG2FIDX*((DEG*static_cast<f32>(800-age))/fall);
    return 200.f*EGG::Mathf::SinFIdx(fidx);
}
f32 ObjectQuicksand::surfaceOffset(f32 radius,u32 frame)const{
    // 807fdd8c..e028; this same radial function drives all20 mesh joints
    // and the original analytic floor collision807fe21c (vertical normal).
    const f32 phase=F_TAU-F_PI*(2.f*radius)/9500.f;
    const f32 wave=waveAmplitude(frame)*phase*EGG::Mathf::sin(phase*1.5f+F_PI*static_cast<f32>(frame)/40.f);
    return wave-360.f*phase;
}
EGG::Vector3f ObjectQuicksand::flowVelocity(const EGG::Vector3f& p,u32 frame)const{
    // USA 807fee24, operation for operation. Retail passes the query's world
    // position (not relative to the object).
    const f32 r=EGG::Mathf::sqrt(p.z*p.z+p.x*p.x);
    const unsigned s=state(frame);const s32 age=stateFrame(frame);
    const f32 step=-10.f/1200.f,a=static_cast<f32>(age);f32 strength=-1.f;
    if(s==0){f32 v=step*a;if(v<-1.f)v=-1.f;else if(v>0.f)v=0.f;strength=v;}
    else if(s==2)strength=10.f+step*a;
    EGG::Vector3f direction(p.x,0.f,p.z);direction.normalise2(); // DOL 80242cb0
    if(s==2&&a<10.f)direction=EGG::Vector3f::ey;
    f32 rc=r;if(rc<3000.f)rc=3000.f;else if(rc>9500.f)rc=9500.f;
    return direction*strength*13.f*2000.f*(1.f/rc);
}
void ObjectQuicksand::calc(){
    const auto t=System::RaceManager::Instance()->timer();
    const unsigned phase=state(t);const s32 age=stateFrame(t);
    //807fda34..dcf8: reverse the sand while sinking; ease through the
    // last60 wave ticks and the first240 reset ticks, retaining SRT phase.
    f32 rate=-3.f;
    if(phase==2)rate=age<=740?3.f:3.f*(800.f-age)/60.f;
    else if(phase==0)rate=-3.f*std::min(1.f,age/240.f);
    m_scrollFrame+=rate;
    if(m_scrollFrame<0.f)m_scrollFrame+=240.f;
    else if(m_scrollFrame>=240.f)m_scrollFrame-=240.f;
    cueSound(phase==2?0x311:0x310,SoundCall::Hold);
}
template<class T> bool ObjectQuicksand::collide(f32 radius,const EGG::Vector3f& p,KCLTypeMask mask,T* info,KCLTypeMask* out,u32 offset,bool push){
    if(!(mask&(KCL_TYPE_FLOOR|KCL_TYPE_BIT(COL_TYPE_FALL_BOUNDARY))))return false;
    const auto local=p-m_origin;const f32 r=EGG::Mathf::sqrt(local.x*local.x+local.z*local.z);
    if(r>9500.f)return false;
    const u32 t=System::RaceManager::Instance()->timer()-offset;
    const f32 dist=radius-(local.y-surfaceOffset(r,t));if(dist<=0.f)return false;
    const bool centre=r<.4f*900.f&&state(t)!=2&&t!=0;
    const bool sink=push&&centre;
    const auto tangent=EGG::Vector3f::ey*dist;
    // The centre reports a non-solid fall trigger, not a supporting floor.
    // Kinoko applies every returned bbox displacement directly to the body
    // and tyres, so it must not carry sand support with this trigger.
    if(info&&!sink){info->bbox.min=info->bbox.min.minimize(tangent);info->bbox.max=info->bbox.max.maximize(tangent);
        if constexpr(std::is_same_v<T,CollisionInfo>){
            info->updateFloor(dist,EGG::Vector3f::ey);
            if(dist>info->movingFloorDist){info->movingFloorDist=dist;info->roadVelocity=flowVelocity(p,t);info->floorObject=this;}
        }
    }
    if(out){
        // Retail USA80800864..87c selects slippery inner sand / weak
        // off-road outer sand, not ordinary road variant3. Only the wave
        // phase is trickable (80800b44..b54).
        const auto type=r>3000.f?COL_TYPE_SLIPPERY_ROAD:COL_TYPE_WEAK_OFF_ROAD;
        if(push){
            auto* cd=CollisionDirector::Instance();
            // USA80800ae4..b28: the still centre is a sink, except frame0.
            if(sink){
                cd->pushCollisionEntry(dist,out,KCL_TYPE_BIT(COL_TYPE_FALL_BOUNDARY),CollisionDirector::CollisionAttribute(COL_TYPE_FALL_BOUNDARY));
                cd->setCurrentCollisionVariant(6);
            }else{
                cd->pushCollisionEntry(dist,out,KCL_TYPE_BIT(type),CollisionDirector::CollisionAttribute(type));
                cd->setCurrentCollisionTrickable(state(t)==2);
            }
        }else *out|=KCL_TYPE_BIT(centre?COL_TYPE_FALL_BOUNDARY:type); // USA 808003e8..80800418
    }
    return true;
}
ObjectQuicksandThwomp::ObjectQuicksandThwomp(const System::MapdataGeoObj& p):ObjectDossunNormal(p){
    // Original807fcf94..fc8. Use dossunc's existing sphere/reaction entry
    // for this original dossun child, retaining its owning KMP placement.
    m_groundDuration=800;m_id=ObjectId::Dossunc;setPos(EGG::Vector3f(pos().x,11000.f,pos().z));setScale(EGG::Vector3f(3.f,3.f,3.f));
}
void ObjectQuicksandThwomp::init(){ObjectDossunNormal::init();m_stillTimer=10000;}
void ObjectQuicksandThwomp::calc(){
    const auto* race=System::RaceManager::Instance();
    const u32 t=race->isStageReached(System::RaceManager::Stage::Race)?race->timer():0;
    const unsigned s=ObjectQuicksand::state(t),age=ObjectQuicksand::stateFrame(t);
    if(s==1)m_stillTimer=std::max<s32>(1,1200-static_cast<s32>(age+7+m_fallDuration+10));
    else m_stillTimer=800000;
    if(s==2&&age==0)cueSound(0x312,SoundCall::Start);
    ObjectDossunNormal::calc();
}
Kart::Reaction ObjectQuicksandThwomp::onCollision(Kart::KartObject* kart,Kart::Reaction onKart,Kart::Reaction,EGG::Vector3f& depth){
    //807fcd54: top contact within29 degrees, excluding the rising phase.
    // Retail 807fcd94..807fce20: against -ey (the Thwomp lands on the kart), EGG sqrt
    // and atan2 (DOL 0x8022f488, 0x8022f560).
    const EGG::Vector3f down=-EGG::Vector3f::ey;
    const f32 dot=depth.x*down.x+depth.y*down.y+depth.z*down.z;
    const EGG::Vector3f c=depth.cross(down);
    f32 angle=EGG::Mathf::atan2(EGG::Mathf::sqrt(c.x*c.x+c.y*c.y+c.z*c.z),dot);
    if(!(angle>0.f))angle=-angle;
    angle*=std::bit_cast<f32>(0x42652ee0u); // 0x808a56a8, not RAD2DEG
    if(angle<29.f&&m_anmState!=AnmState::Rising){
        cueSound(0x245,SoundCall::Voice);
        const auto& table=ObjectDirector::Instance()->hitTableKart();
        return table.reaction(table.slot(ObjectId::DossuncSoko),ObjectHitTable::KartColumn(kart));
    }
    return onKart;
}
}
