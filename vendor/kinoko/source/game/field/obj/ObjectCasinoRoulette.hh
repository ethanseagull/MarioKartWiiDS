#pragma once
#include "game/field/obj/ObjectKCL.hh"
namespace Kinoko::Field {
/// casino_roulette (USA ctor 0x808770c0, vtable 0x808d7d50): the rotating deck.
/// Overrides (USA): init 80877188 (deck table), calc 80877714 (vtable +0x14),
/// narrScLocal 80877fd4, getUpdatedMatrix 80877694, colRadiusAdditionalLength
/// 80877fc8 (12000), checkCollision 808777dc, checkCollisionCached 80877d18,
/// surface motion 80877bc0. Slots 63/65/69 (80878054/64/5c) and the link-record
/// branch of checkCollision serve CollisionInfo+0x58 records, which only items
/// and riding objects carry (a kart's CollisionInfo has none, USA 805ab8c0), so
/// Kinoko has no record and that branch is kept as deckFloor() only.
class ObjectCasinoRoulette final : public ObjectKCL {
public:
    explicit ObjectCasinoRoulette(const System::MapdataGeoObj &p);
    ~ObjectCasinoRoulette() override;
    void init() override;
    void calc() override;
    u32 loadFlags() const override {return 1;}
    void narrScLocal(f32 radius,const EGG::Vector3f &pos,KCLTypeMask mask,u32 timeOffset) override;
    const EGG::Matrix34f &getUpdatedMatrix(u32 offset) override;
    f32 colRadiusAdditionalLength() const override {return 12000.0f;}
    bool checkCollision(f32,const EGG::Vector3f&,const EGG::Vector3f&,KCLTypeMask,CollisionInfo*,KCLTypeMask*,u32) override;
    bool checkCollisionCached(f32,const EGG::Vector3f&,const EGG::Vector3f&,KCLTypeMask,CollisionInfo*,KCLTypeMask*,u32) override;
    /// Displacement of a point on the deck over one frame (radius kept).
    static EGG::Vector3f surfaceMotion(const EGG::Vector3f &point);
    /// The link-record answer (USA 80877870..80877978): floor normal of the deck
    /// at XZ distance `dist` from the axis, from the 256-sample table.
    EGG::Vector3f deckFloor(const EGG::Vector3f &pos,f32 dist) const;
private:
    static EGG::Vector3f surfaceMotion(const EGG::Vector3f &point,f32 radius);
    bool insideBounds(f32 radius,const EGG::Vector3f &pos) const;
    static bool carriesCourseContact(KCLTypeMask *maskOut);
    void carry(CollisionInfo *info,const EGG::Vector3f &pos);
    EGG::Vector3f sweep(EGG::Vector3f &pos,EGG::Vector3f dir,KCLTypeMask mask,CollisionInfo *info,f32 radius);
    f32 m_speed;                 ///< +0x110 rad/frame
    EGG::Matrix34f m_rotation;   ///< +0x174
    f32 m_step;                  ///< +0x1a4 table spacing
    f32 m_invStep;               ///< +0x1a8
    f32 m_extent;                ///< +0x1ac max |x|,|z| of the KCL
    f32 m_minY,m_maxY;           ///< +0x1b0, +0x1b4
    f32 m_nrmY[256];             ///< +0x1b8 floor normal y per sample
    f32 m_nrmXZ[256];            ///< +0x5b8 floor normal z / sample distance
};
}
