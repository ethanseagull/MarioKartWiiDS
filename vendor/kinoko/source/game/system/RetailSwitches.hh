#pragma once

namespace Kinoko::System {

/// @brief Port switches for retail behaviour added after 1.2.2, for same-binary A/B checks.
/// @details All true: retail. The frontend's DIAG off-files clear one before a race is
/// built (sdmc:/3ds/MKWiiDS/no-*.txt, named at each field).
struct RetailSwitches {
    bool miiStats = true;         ///< no-retail-mii-stats.txt: Mii stat entry 23 + (id - 23) / 6
    bool classRules = true;       ///< no-retail-class-rules.txt: course rules by class, flip boundary
    bool random2 = true;          ///< no-race-random2.txt: RaceManager's second random stream
    bool checkpointSearch = true; ///< no-retail-checkpoint-search.txt: retail sector search
    bool battleSlots = true;      ///< no-retail-battle-slots.txt: slots 34/35 start and deck turn
    bool areas = true;            ///< no-retail-areas.txt: AREA cylinder test and priority sort
    bool kartDepth = true;        ///< no-retail-kart-depth.txt: kart-kart depth as retail's sqrt
};

inline RetailSwitches g_retailSwitches; // a plain global: no guard on per-frame reads

} // namespace Kinoko::System
