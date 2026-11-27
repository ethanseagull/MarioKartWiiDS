#pragma once
#include <mkw/driver_controller.hpp>
#include <span>

namespace mkw {
// Original collision observer 80742ef4 and look state 80744058..807442a0.
// This observes the accepted physical bump before audio clears its cue.
class DriverCollisionLook {
public:
    void step(unsigned other, Vec3 impulse, Vec3 position, std::span<const Vec3> racers) {
        const float strength = length(impulse);
        if (other < racers.size() && strength > 8.f) {
            target_ = other;
            // The active native state follows the latest opponent, while its
            // original timer continues. Repeated contact cannot restart it.
            if (remaining_ < 0) {
                const int phase = int(.1f * std::fabs(position.x + position.z));
                remaining_ = strength < 15.f ? 15 + phase % 10 : 30 + phase % 20;
            }
        }
        if (target_ >= racers.size() || --remaining_ < 0) {
            target_ = ~0u;
            remaining_ = -1;
            return;
        }
        targetPosition_ = racers[target_];
    }

    // AI::Character (mkw/driver_character_ai.hpp) owns the type1 request when it runs:
    // its look replaces this observer's (which is then its bump action).
    void setCharacterLook(bool on, Vec3 target) { character_ = true; characterOn_ = on; characterTarget_ = target; }
    void clearCharacterLook() { character_ = false; }

    void apply(DriverSignals& signals, unsigned character) const {
        // Native type2 item requests outrank the type1 collision request.
        if (signals.hasLookTarget) return;
        if (character_) {
            if (!characterOn_) return;
            signals.hasLookTarget = true;
            signals.lookTarget = characterTarget_;
            signals.lookResponse = response(character);
            return;
        }
        if (target_ == ~0u) return;
        signals.hasLookTarget = true;
        signals.lookTarget = targetPosition_;
        signals.lookResponse = response(character);
    }

    // Character +0x200 (0x80736588): .96 / .2 / .6 by the character's weight class.
    static float response(unsigned character) {
        constexpr float response[] = {
            .6f, .2f, .6f, .96f, .2f, .2f, .2f, .6f, .6f, .96f, .6f, .96f,
            .2f, .2f, .2f, .6f, .6f, .6f, .2f, .96f, .2f, .96f, .6f, .6f,
            .2f, .2f, .2f, .2f, .2f, .2f, .6f, .6f, .6f, .6f, .6f, .6f,
            .96f, .96f, .96f, .96f, .96f, .96f
        };
        return character < std::size(response) ? response[character] : .6f;
    }

    bool active() const { return target_ != ~0u; }
    unsigned remaining() const { return remaining_ >= 0 ? unsigned(remaining_) : 0; }

private:
    unsigned target_ = ~0u;
    int remaining_ = -1;
    Vec3 targetPosition_{};
    bool character_ = false, characterOn_ = false;
    Vec3 characterTarget_{};
};
}
