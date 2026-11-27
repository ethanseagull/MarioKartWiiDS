#pragma once
#include "game/field/obj/ObjectCollidable.hh"
namespace Kinoko::Field {
/// Chain Chomp Roulette's rolling giant (Twanwan, flow 0xeb), distinct from Hwanwan.
/// Line port of retail (USA): ctor 0x80885f80, init 0x8088608c, calc 0x8088744c,
/// target 0x80886474/0x80886548, motion 0x808866d4, onCollision 0x8088779c.
/// Collision transform: the ObjectCollidable default (retail does not override it).
class ObjectTwanwan final : public ObjectCollidable {
public:
    explicit ObjectTwanwan(const System::MapdataGeoObj& params);
    void init() override;
    void calc() override;
    u32 loadFlags() const override {return 1;}
    Kart::Reaction onCollision(Kart::KartObject*,Kart::Reaction,Kart::Reaction,EGG::Vector3f&) override;
    s16 onItemCollision(const EGG::Vector3f&,const EGG::Vector3f&,s16,s16,EGG::Vector3f&) override;
    void onGlobalItemCollision(s16,s16) override;
    bool fullSphere() const {return m_fullSphere;}
    bool grounded() const {return m_grounded;}
    f32 hopHeight() const {return m_hopHeight;}
private:
    void step();
    void calcTarget();
    void move();
    void hit(s16 reaction);
    f32 m_radius;                 ///< +0xb4: 296 x scale.x
    f32 m_speed=0.f;              ///< +0xb8: setting 0
    s32 m_startAtPos=0;           ///< +0xbc: setting 1
    f32 m_jumpWidth=0.f;          ///< +0xc0: setting 2
    s32 m_jumpPeriod=0;           ///< +0xc4: setting 3
    f32 m_invPeriod=0.f;          ///< +0xc8
    u32 m_stepTimer=0;            ///< +0xcc
    u32 m_fallStart=0;            ///< +0xd0: setting 4
    u32 m_moveStart=0;            ///< +0xd4: setting 5
    EGG::Vector3f m_work;         ///< +0xd8
    EGG::Vector3f m_velocity;     ///< +0xe4
    EGG::Vector3f m_forward;      ///< +0xf0 (initial transform column 1)
    EGG::Vector3f m_right;        ///< +0xfc (initial transform column 0)
    f32 m_pitch=0.f;              ///< +0x108
    EGG::Vector3f m_turnAxis;     ///< +0x10c
    f32 m_yaw=0.f;                ///< +0x118
    EGG::Vector3f m_target;       ///< +0x178
    bool m_grounded=false;        ///< +0x184
    f32 m_hopVelocity=0.f;        ///< +0x188
    f32 m_hopHeight=0.f;          ///< +0x18c
    u32 m_hitAge=65;              ///< +0x190
    bool m_fullSphere=true;       ///< +0x194
    bool m_started=false;         ///< +0x19c
    u32 m_age=0;                  ///< +0x1a0
};
}
