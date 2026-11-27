#include "ObjectCasinoRoulette.hh"
#include "game/system/RaceManager.hh"
#include "game/field/CollisionDirector.hh"
namespace Kinoko::Field {
namespace {
constexpr f32 SPEED=0.002f; // 808adcbc
// +0x114: one frame of rotation (ctor 0x80877148, makeR). Only its 3x3 is read.
EGG::Matrix34f stepMatrix(){EGG::Matrix34f m;m.makeR(EGG::Vector3f(0.0f,SPEED,0.0f));return m;}
}
/// @addr{USA 0x808770c0}
ObjectCasinoRoulette::ObjectCasinoRoulette(const System::MapdataGeoObj &p):ObjectKCL(p),m_speed(SPEED),
    m_step(0.0f),m_invStep(0.0f),m_extent(0.0f),m_minY(0.0f),m_maxY(0.0f),m_nrmY{},m_nrmXZ{}{}
ObjectCasinoRoulette::~ObjectCasinoRoulette()=default;
/// @addr{USA 0x80877188} Samples the deck along +z every (12000+extent)/256 by
/// sweeping a sphere down from above it: floor normal y and normal z / distance.
void ObjectCasinoRoulette::init(){
    const EGG::Vector3f low=m_objColMgr->kclLowWorld(),high=m_objColMgr->kclHighWorld();
    f32 z=std::abs(high.z),x=std::abs(high.x);
    const f32 lz=std::abs(low.z),lx=std::abs(low.x);
    if(lz>=z)z=lz;
    if(lx>=x)x=lx;
    if(x>=z)z=x;
    m_extent=z;m_minY=low.y;m_maxY=high.y;
    m_step=(12000.0f+m_extent)*0.00390625f;
    m_invStep=1.0f/m_step;
    for(u32 i=0;i<256;++i){
        EGG::Vector3f pos(0.0f,m_maxY+m_step,static_cast<f32>(i)*m_step);
        CollisionInfo info;info.bbox.setZero();info.floorObject=nullptr;
        sweep(pos,-EGG::Vector3f::ey,0x20e80fff,&info,m_step);
        if(info.floorDist>-std::numeric_limits<f32>::min()){
            m_nrmY[i]=info.floorNrm.y;
            m_nrmXZ[i]=(m_invStep*info.floorNrm.z)/static_cast<f32>(i);
        }else{
            m_nrmY[i]=i?m_nrmY[i-1]:1.0f;
            m_nrmXZ[i]=i?m_nrmXZ[i-1]:0.0f;
        }
    }
}
/// @addr{USA 0x808773d0} Steps against `dir` while inside geometry, then along
/// `dir` until a hit, halving the step from the first hit; returns the contact
/// pushed out (or the last position after 1000 steps or below y 0).
EGG::Vector3f ObjectCasinoRoulette::sweep(EGG::Vector3f &pos,EGG::Vector3f dir,KCLTypeMask mask,
        CollisionInfo *info,f32 radius){
    dir.normalise2(); // DOL 80242cb0
    f32 step=299.0f;
    const f32 want=1.9f*radius;
    if(want<=step)step=want;
    const EGG::Vector3f back=-dir;
    const EGG::Vector3f out=back*step;
    auto *cd=CollisionDirector::Instance();
    u32 n=0;
    while(n<1000&&0.0f<=pos.y){
        if(!cd->checkSphereFull(radius,pos,EGG::Vector3f::inf,mask,info,nullptr,0)){ // static at USA 0x8051243c = (inf,inf,inf)
            EGG::Vector3f s=dir*step;
            bool hit=false;
            while(n<1000&&0.0f<=pos.y){
                const EGG::Vector3f probe=pos+s;
                if(cd->checkSphereFull(radius,probe,pos,mask,info,nullptr,0)){
                    hit=true;
                    if(EGG::Mathf::sqrt(s.x*s.x+s.y*s.y+s.z*s.z)<10.0f)return probe+info->tangentOff;
                }else{
                    pos+=s;
                }
                if(hit)s=s*0.5f;
                ++n;
            }
            return pos;
        }
        ++n;pos+=out;
    }
    return pos;
}
/// @addr{USA 0x80877714}
void ObjectCasinoRoulette::calc(){setTransform(getUpdatedMatrix(0));}
/// @addr{USA 0x80877fd4}
void ObjectCasinoRoulette::narrScLocal(f32 radius,const EGG::Vector3f &pos,KCLTypeMask mask,u32){
    calcCollisionTransform();
    m_objColMgr->narrScLocal(radius,pos,mask);
}
/// @addr{USA 0x80877694}
const EGG::Matrix34f &ObjectCasinoRoulette::getUpdatedMatrix(u32 offset){
    const u32 t=System::RaceManager::Instance()->timer()-offset;
    m_rotation.makeR(EGG::Vector3f(0.0f,static_cast<f32>(t)*m_speed,0.0f));
    return m_rotation;
}
EGG::Vector3f ObjectCasinoRoulette::surfaceMotion(const EGG::Vector3f &p){
    return surfaceMotion(p,EGG::Mathf::sqrt(p.x*p.x+p.z*p.z));
}
/// @addr{USA 0x80877bc0} One frame of rotation of the contact, scaled back to
/// its XZ radius, minus the contact: a displacement, y 0.
EGG::Vector3f ObjectCasinoRoulette::surfaceMotion(const EGG::Vector3f &p,f32 radius){
    static const EGG::Matrix34f m=stepMatrix();
    EGG::Vector3f v(m[0,0]*p.x+m[0,2]*p.z,0.0f,m[2,0]*p.x+m[2,2]*p.z);
    const f32 len=EGG::Mathf::sqrt(v.x*v.x+v.z*v.z);
    if(0.0f<len){const f32 k=radius/len;v.x*=k;v.y*=k;v.z*=k;}
    return EGG::Vector3f(v.x-p.x,0.0f,v.z-p.z);
}
EGG::Vector3f ObjectCasinoRoulette::deckFloor(const EGG::Vector3f &pos,f32 dist) const{
    s32 i=static_cast<s32>(dist*m_invStep);
    if(i<0)i=0;
    if(i>255)i=255;
    const f32 k=m_nrmXZ[i];
    return EGG::Vector3f(pos.x*k,m_nrmY[i],pos.z*k);
}
bool ObjectCasinoRoulette::insideBounds(f32 r,const EGG::Vector3f &p) const{
    const f32 e=m_extent+r,top=m_maxY+r,bottom=m_minY-r;
    return -e<p.x&&p.x<e&&bottom<p.y&&p.y<top&&-e<p.z&&p.z<e;
}
/// USA 80877984..808779e8 / 80877d58..80877db8: the contacts gathered so far
/// (course KCL and objects before this one) that ride the deck: types 7, 8, 15,
/// 29, or a boost pad of variant 1.
bool ObjectCasinoRoulette::carriesCourseContact(KCLTypeMask *maskOut){
    if(!maskOut)return false;
    KCLTypeMask mask=*maskOut;
    if(mask&0x20008180u)return true;
    if(!(mask&KCL_TYPE_BIT(COL_TYPE_BOOST_PAD)))return false;
    auto *cd=CollisionDirector::Instance();
    return cd->findClosestCollisionEntry(maskOut,KCL_TYPE_BIT(COL_TYPE_BOOST_PAD))&&
            cd->closestCollisionEntry()->variant()==1;
}
/// USA 80877a88..80877b40: support priority 100, road velocity = deck motion.
void ObjectCasinoRoulette::carry(CollisionInfo *info,const EGG::Vector3f &pos){
    const f32 dist=EGG::Mathf::sqrt(pos.x*pos.x+pos.z*pos.z);
    const EGG::Vector3f motion=surfaceMotion(pos,dist);
    if(100.0f>info->movingFloorDist){
        info->movingFloorDist=100.0f;info->roadVelocity=motion;info->floorObject=this;
    }
}
/// @addr{USA 0x808777dc} Inside the deck's box the deck KCL decides alone;
/// outside it the contacts gathered so far do.
bool ObjectCasinoRoulette::checkCollision(f32 radius,const EGG::Vector3f &pos,const EGG::Vector3f &prevPos,
        KCLTypeMask mask,CollisionInfo *info,KCLTypeMask *maskOut,u32 timeOffset){
    bool hit=carriesCourseContact(maskOut);
    if(insideBounds(radius,pos)){
        update(timeOffset);
        hit=m_objColMgr->checkSphereFullPush(radius,pos,prevPos,mask,info,maskOut);
    }
    if(!hit)return false;
    if(info)carry(info,pos);
    if(maskOut&&*maskOut)CollisionDirector::Instance()->findClosestCollisionEntry(maskOut,KCL_ANY);
    return true;
}
/// @addr{USA 0x80877d18}
bool ObjectCasinoRoulette::checkCollisionCached(f32 radius,const EGG::Vector3f &pos,const EGG::Vector3f &prevPos,
        KCLTypeMask mask,CollisionInfo *info,KCLTypeMask *maskOut,u32 timeOffset){
    bool hit=carriesCourseContact(maskOut);
    if(insideBounds(radius,pos)){
        update(timeOffset);
        hit=m_objColMgr->checkSphereCachedFullPush(radius,pos,prevPos,mask,info,maskOut)||hit;
    }
    if(hit&&info)carry(info,pos);
    return hit;
}
}
