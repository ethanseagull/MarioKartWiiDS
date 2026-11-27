#include "ObjectBGTeresa.hh"
#include "game/field/ObjectDirector.hh"
#include "game/system/RaceManager.hh"
#include <egg/math/Math.hh>
namespace Kinoko::Field {
namespace {
// Retail parameter block (USA 0x808cac98) and constants (USA 0x8089f8c8).
constexpr u32 LIFETIME_MIN = 180;
constexpr u32 LIFETIME_MAX = 420;
constexpr f32 OFFSET_MIN = -1000.0f, OFFSET_MAX = 1000.0f;
constexpr f32 MOTION_STEP = 1.0f;
constexpr u32 ALPHA_STEP = 5;
constexpr u32 FADE_OUT_STEP = 5;
constexpr f32 ALPHA_SPEED_MIN = 1.0f, ALPHA_SPEED_MAX = 3.0f;
constexpr u32 DELAY_RANGE = 180;
constexpr f32 DEG_TO_RAD = 0.01745329238474369f; // 0x3c8efa35
} // namespace

void ObjectBGTeresa::load() { ObjectDirector::Instance()->addObject(this); }

/// @addr{USA 0x8076e354}
void ObjectBGTeresa::init() {
    auto &random = System::RaceManager::Instance()->random();
    m_state = State::Hidden;
    m_alpha = 0;
    m_origin = pos();
    m_motionAngle = 0.0f;
    m_delay = m_countdown = static_cast<s32>(random.getU32(DELAY_RANGE));
    m_mirrorU = random.getU32(2) == 0; // texture mirroring (presentation)
}

/// @addr{USA 0x8076ede0}
void ObjectBGTeresa::appear() {
    auto &random = System::RaceManager::Instance()->random();
    m_state = State::FadeIn;
    const f32 ox = OFFSET_MIN + random.getF32(OFFSET_MAX - OFFSET_MIN);
    const f32 oy = OFFSET_MIN + random.getF32(OFFSET_MAX - OFFSET_MIN);
    const f32 oz = OFFSET_MIN + random.getF32(OFFSET_MAX - OFFSET_MIN);
    m_offset = EGG::Vector3f(ox, oy, oz);
    const f32 ax = OFFSET_MIN + random.getF32(OFFSET_MAX - OFFSET_MIN);
    const f32 ay = OFFSET_MIN + random.getF32(OFFSET_MAX - OFFSET_MIN);
    const f32 az = OFFSET_MIN + random.getF32(OFFSET_MAX - OFFSET_MIN);
    m_amplitude = EGG::Vector3f(ax, ay, az);
    m_lifetime = static_cast<s32>(LIFETIME_MIN + random.getU32(LIFETIME_MAX - LIFETIME_MIN));
}

/// Retail's motion block, repeated at the end of the three visible states.
void ObjectBGTeresa::calcMotion() {
    m_motionAngle += MOTION_STEP;
    if (m_motionAngle > 360.0f) {
        m_motionAngle = 0.0f;
    }
    // USA 0x8022f4e8 is EGG::Mathf::cos (PAL 0x8022f86c).
    const EGG::Vector3f wave = m_amplitude * EGG::Mathf::cos(m_motionAngle * DEG_TO_RAD);
    setPos((m_origin + m_offset) + wave);
    if (--m_lifetime == 0) {
        m_state = State::FadeOut;
    }
}

/// @addr{USA 0x8076e4d8}
void ObjectBGTeresa::calc() {
    switch (m_state) {
    case State::Hidden:
        if (--m_countdown == 0) {
            appear();
        }
        break;
    case State::FadeIn:
        m_alpha = static_cast<u8>(m_alpha + ALPHA_STEP);
        if (m_alpha > 255 - ALPHA_STEP) {
            m_alpha = 255;
            m_alphaAngle = 0.0f;
            m_state = State::Visible;
            m_alphaSpeed = ALPHA_SPEED_MIN +
                    System::RaceManager::Instance()->random().getF32(ALPHA_SPEED_MAX - ALPHA_SPEED_MIN);
        }
        calcMotion();
        break;
    case State::Visible:
        m_alphaAngle += m_alphaSpeed;
        if (m_alphaAngle > 360.0f) {
            m_alphaAngle = 0.0f;
        }
        m_alpha = static_cast<u8>(static_cast<s32>(
                191.0f + 64.0f * EGG::Mathf::cos(m_alphaAngle * DEG_TO_RAD)));
        calcMotion();
        break;
    case State::FadeOut:
        m_alpha = static_cast<u8>(m_alpha - FADE_OUT_STEP);
        if (m_alpha < ALPHA_STEP) {
            m_alpha = 0;
            m_state = State::Hidden;
            m_countdown = m_delay;
        }
        calcMotion();
        break;
    }
}
}
