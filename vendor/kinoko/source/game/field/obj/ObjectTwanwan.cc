#include "ObjectTwanwan.hh"
#include "game/field/CollisionDirector.hh"
#include "game/field/ObjectDirector.hh"
#include "game/kart/KartObject.hh"
#include "game/system/RaceConfig.hh"
#include "game/system/RaceManager.hh"
#include <limits>
namespace Kinoko::Field {
namespace {
using V=EGG::Vector3f;
constexpr f32 EPS=1.1920928955078125e-7f;      // 0x808ae180
constexpr f32 CHOMP_GRAVITY_Y=-103.0f;          // static vector at 0x805133c8 (0,-103,0)
constexpr KCLTypeMask MASK=0x64ebdfff;
/// 0x8086868c: a + (b - a) * t.
V lerp(f32 t,const V& a,const V& b){return a+(b-a)*t;}
/// 0x8053365c: a - b * (b . a).
V reject(const V& a,const V& b){
    const f32 d=b.x*a.x+b.y*a.y+b.z*a.z;
    return a-b*d;
}
f32 sq(const V& v){return v.x*v.x+v.y*v.y+v.z*v.z;}
/// 0x805336d8: zero for a null vector; otherwise the inward part reflected
/// with restitution (d + d*r along n).
V reflect(const V& v,const V& n,f32 restitution){
    if(sq(v)<=EPS)return V::zero;
    const f32 d=-(n.x*v.x+n.y*v.y+n.z*v.z);
    if(0.0f>=d)return v;
    return v+n*(d+d*restitution);
}
}
/// @addr{USA 0x80885f80}
ObjectTwanwan::ObjectTwanwan(const System::MapdataGeoObj& p):ObjectCollidable(p),m_radius(296.0f*scale().x),
    m_work(V::zero),m_velocity(V::zero),m_forward(V::zero),m_right(V::zero),m_turnAxis(V::zero),m_target(V::zero){}
/// @addr{USA 0x8088608c}
void ObjectTwanwan::init(){
    m_speed=static_cast<f32>(static_cast<s16>(m_mapObj->setting(0)));
    m_startAtPos=static_cast<s16>(m_mapObj->setting(1));
    m_jumpWidth=static_cast<f32>(static_cast<s16>(m_mapObj->setting(2)));
    m_jumpPeriod=static_cast<s16>(m_mapObj->setting(3));
    m_fallStart=static_cast<u32>(static_cast<s32>(static_cast<s16>(m_mapObj->setting(4))));
    m_moveStart=static_cast<u32>(static_cast<s32>(static_cast<s16>(m_mapObj->setting(5))));
    m_invPeriod=m_jumpPeriod==0?0.0f:1.0f/static_cast<f32>(static_cast<u32>(m_jumpPeriod));
    ASSERT(m_railInterpolator);
    m_railInterpolator->init(0.0f,0);
    calcTransform();m_forward=transform().base(1);
    calcTransform();m_right=transform().base(0);
    m_turnAxis=V::zero;m_velocity=V::zero;
    m_hitAge=65;m_hopVelocity=0.0f;m_hopHeight=0.0f;
    if(m_fullSphere){
        m_target=m_railInterpolator->curPos()+V::ey*m_radius;
        if(m_startAtPos==0)m_work=m_target;
        else{const V up=V::ey*m_radius;calcTransform();m_work=transform().base(3)+up;}
        setPos(V(m_work.x,m_work.y+m_hopHeight,m_work.z));
    }else{
        m_target=m_railInterpolator->curPos()+V::ey*200.0f;
        if(m_startAtPos==0)m_work=m_target;
        else{const V f=m_forward*200.0f;calcTransform();m_work=transform().base(3)+f;}
        setPos(m_work+m_forward*(m_radius-200.0f)+V::ey*m_hopHeight);
    }
    m_grounded=false;
    m_stepTimer=System::RaceManager::Instance()->timer();
    m_started=false;m_age=0;
}
/// @addr{USA 0x8088744c}: one step per race frame up to the current timer.
void ObjectTwanwan::calc(){
    while(m_stepTimer<=System::RaceManager::Instance()->timer()){
        step();
        ++m_stepTimer;
    }
}
void ObjectTwanwan::step(){
    if(!m_started){m_railInterpolator->setCurrVel(0.0f);m_started=true;m_age=0;}
    // 0x808874d0: the wheel-only collision shrink is limited to Chain Chomp Roulette.
    if(m_age==m_fallStart+300&&static_cast<s32>(System::RaceConfig::Instance()->raceScenario().course)==34){
        m_fullSphere=false;
        m_target=m_railInterpolator->curPos()+V::ey*200.0f;
        m_work=pos()-(m_forward*(m_radius-200.0f)+V::ey*m_hopHeight);
    }
    if(m_moveStart<=m_age){
        if(m_age==m_moveStart){
            m_railInterpolator->setCurrVel(m_speed);
            m_target=m_railInterpolator->curPos()+V::ey*(m_fullSphere?m_radius:200.0f);
        }
        calcTarget();
    }
    if(m_fallStart<=m_age){
        if(m_age==m_fallStart)cueSound(0x23a,SoundCall::Start);
        move();
        cueSound(0x237,SoundCall::Hold);
    }
    ++m_age;
}
/// @addr{USA 0x80886548}
void ObjectTwanwan::calcTarget(){
    m_railInterpolator->calc();
    f32 frame=0.0f;
    if(m_stepTimer>=m_fallStart&&m_jumpPeriod!=0){
        frame=static_cast<f32>((m_stepTimer-m_fallStart)%static_cast<u32>(m_jumpPeriod));
    }
    const f32 phase=m_jumpPeriod==0?0.0f:frame*m_invPeriod;
    const f32 width=m_jumpWidth*EGG::Mathf::sin(F_TAU*phase);
    V side=V::ey.cross(m_railInterpolator->curTangentDir());
    side.normalise2();
    const V goal=m_railInterpolator->curPos()+side*width;
    m_target=lerp(0.05f,m_target,goal);
}
/// @addr{USA 0x808866d4}
void ObjectTwanwan::move(){
    V velocity=lerp(0.08f,m_velocity,V(m_velocity.x,m_velocity.y+CHOMP_GRAVITY_Y,m_velocity.z));
    V proposed=m_work+velocity;
    proposed=lerp(0.7f,m_work,proposed);
    f32 hopVelocity=m_hopVelocity+0.99f*((m_hopVelocity-5.0f)-m_hopVelocity);
    f32 hopHeight=m_hopHeight+0.99f*((m_hopHeight+hopVelocity)-m_hopHeight);
    if(hopHeight<0.0f){
        hopHeight=0.0f;
        if(hopVelocity<0.0f)hopVelocity=0.3f*(-hopVelocity);
    }
    const f32 hopAcceleration=hopVelocity-m_hopVelocity;
    V forward=m_forward,right=m_right,axis=m_turnAxis;
    f32 pitch=m_pitch,yaw=m_yaw;
    CollisionInfo info;info.bbox.setZero();
    KCLTypeMask maskOut=KCL_NONE;
    auto* cd=CollisionDirector::Instance();
    if(m_fullSphere){
        cd->checkCourseColNarrScLocal(0.1f+m_radius,proposed,MASK,1);
        m_grounded=cd->checkSphereCachedFullPush(m_radius,proposed,V::inf,MASK,&info,&maskOut,0);
    }else{
        m_grounded=cd->checkSphereFullPush(200.0f,proposed,V::inf,MASK,&info,&maskOut,0);
    }
    if(m_grounded){
        const V tangent=info.tangentOff;
        const f32 tangentSq=sq(tangent);
        proposed=V(proposed.x+tangent.x,proposed.y+tangent.y,proposed.z+tangent.z);
        if(!(tangentSq<=EPS)){
            V n=tangent;n.normalise2();
            velocity=reflect(velocity,n,0.6f);
            forward=lerp(0.05f,m_forward,n);
            forward.normalise2();
        }
        const V displacement=proposed-m_work;
        if(!(sq(displacement)<=EPS)){
            right=m_forward.cross(displacement);
            if(sq(right)<=EPS)right=m_right;
            else right.normalise2();
            right=lerp(0.35f,m_right,right);
            right.normalise2();
        }
        if(info.floorDist>-std::numeric_limits<f32>::min()){
            const V aim=reject(m_target-proposed,forward);
            if(!(sq(aim)<=EPS)){
                V aimDir=aim;aimDir.normalise2();
                const V travel=reject(velocity,forward);
                V travelDir=sq(travel)<=EPS?aimDir:travel;
                travelDir.normalise2();
                const f32 cosine=aimDir.x*travelDir.x+aimDir.y*travelDir.y+aimDir.z*travelDir.z;
                const f32 sine=EGG::Mathf::sqrt(sq(aimDir.cross(travelDir)));
                f32 angle=EGG::Mathf::atan2(sine,cosine); // DOL 0x8022f560
                if(!(angle>0.0f))angle=-angle;
                const f32 turn=0.04f*(-angle);
                V turnAxis=aimDir.cross(travelDir);
                if(sq(turnAxis)<=EPS)turnAxis=forward;
                else turnAxis.normalise2();
                EGG::Matrix34f rotation;rotation.setAxisRotation(turn,turnAxis);rotation.setBase(3,V::zero);
                f32 targetSpeed=0.3f*EGG::Mathf::sqrt(sq(aim));
                if(targetSpeed>70.0f)targetSpeed=70.0f;
                const f32 travelSpeed=EGG::Mathf::sqrt(sq(travel));
                const f32 speed=travelSpeed+0.1f*(targetSpeed-travelSpeed);
                const V change=rotation.ps_multVector(travelDir)*speed-travel;
                velocity=V(velocity.x+change.x,velocity.y+change.y,velocity.z+change.z);
            }
            const f32 sideways=EGG::Mathf::sqrt(sq(reject(displacement,forward)));
            pitch=m_pitch+0.9f*(0.0007f*sideways-m_pitch);
            // 0x80886f78: the floor's road velocity (CollisionInfo +0x50/+0x3c), not the wall.
            if(info.movingFloorDist>-std::numeric_limits<f32>::min()){
                const V push=info.roadVelocity*0.01f;
                velocity=V(velocity.x+push.x,velocity.y+push.y,velocity.z+push.z);
                if(!(sq(info.roadVelocity)<=EPS)){
                    V n=info.roadVelocity;n.normalise2();
                    axis=lerp(0.04f,m_turnAxis,forward.cross(n));
                }
                yaw=m_yaw+0.04f*(-0.0012f*EGG::Mathf::sqrt(sq(info.roadVelocity))-m_yaw);
            }
            const f32 change=EGG::Mathf::sqrt(sq(velocity-m_velocity));
            if(30.0f<=change||30.0f<=EGG::Mathf::abs(hopAcceleration))cueSound(0x239,SoundCall::Start);
            else if(hopHeight<5.0f)cueSound(0x238,SoundCall::Hold);
        }
    }
    m_forward=forward;m_right=right;m_pitch=pitch;m_turnAxis=axis;m_yaw=yaw;
    m_velocity=velocity;m_work=proposed;m_hopVelocity=hopVelocity;m_hopHeight=hopHeight;
    if(m_hitAge<65)++m_hitAge;
    V centre;
    if(m_fullSphere)centre=V(m_work.x,m_work.y+m_hopHeight,m_work.z);
    else centre=(m_work+m_forward*(m_radius-200.0f))+V::ey*m_hopHeight;
    setPos(centre);
    // 0x80887318..0x80887400: pitch about the right axis, then yaw about the turn axis.
    EGG::Matrix34f pitchRot;pitchRot.setAxisRotation(m_pitch,m_right);pitchRot.setBase(3,V::zero);
    calcTransform();
    EGG::Matrix34f m=pitchRot.multiplyTo(transform());
    EGG::Matrix34f yawRot;yawRot.setAxisRotation(m_yaw,m_turnAxis);yawRot.setBase(3,V::zero);
    m=yawRot.multiplyTo(m);
    m.setBase(3,pos());
    setTransform(m);
}
void ObjectTwanwan::hit(s16 reaction){
    if((reaction==3||reaction==5)&&m_grounded&&m_hitAge>=65){m_hitAge=0;m_hopVelocity=170.0f;}
}
/// @addr{USA 0x8088779c}
Kart::Reaction ObjectTwanwan::onCollision(Kart::KartObject* kart,Kart::Reaction onKart,Kart::Reaction onObj,V&){
    hit(static_cast<s16>(onObj));
    // 0x80887800..0x808878b0: upper or airborne hits use the Twanwan_ue row.
    bool upper=true;
    if(m_grounded){
        const V d=kart->pos()-pos();
        const f32 along=m_forward.x*d.x+m_forward.y*d.y+m_forward.z*d.z;
        if(!(200.0f-m_radius<along))upper=false;
    }
    if(upper){
        const auto& table=ObjectDirector::Instance()->hitTableKart();
        return table.reaction(table.slot(static_cast<ObjectId>(0xef)),ObjectHitTable::KartColumn(kart));
    }
    return onKart;
}
s16 ObjectTwanwan::onItemCollision(const V&,const V&,s16 onItem,s16 onObj,V&){hit(onObj);return onItem;}
void ObjectTwanwan::onGlobalItemCollision(s16,s16 onObj){hit(onObj);}
}
