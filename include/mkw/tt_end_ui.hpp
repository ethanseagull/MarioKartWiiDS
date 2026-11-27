#pragma once
#include <mkw/race_flow_pose.hpp>
#include <array>
#include <cstdint>
#include <memory>

namespace mkw {
class Renderer;
class RaceFlowUi;
class RaceResultsFlow;
// The Time Trial end pages (RaceResultsFlow::timeTrialEnd), drawn over the race on the top
// screen with the cooked retail art (tools/cook_race_flow.py cook_tt_end, FlowTtTextures):
//  TTSplits (0x2D)  the total (TIME) and each lap's time (time_number's Lap 1..3), the best lap
//                   flashing (game_image_number_best_lap.brlan) and, for a new best time, the
//                   total too, with the NEW RECORD! control (newrecord.brctr);
//  TTEnd (0x21)     Retry, Change Course, Change Character, View Replay, Quit Game.
// Its own textures are loaded one per frame after the finish; the button art is RaceFlowUi's
// (RaceFlowUi::drawBatch).
struct TtEndData {
 uint32_t totalMs=0;
 std::array<uint32_t,5> lapMs{};unsigned laps=0;
 unsigned bestLap=0;          // 1-based lap that flashes
 bool newRecord=false;        // a new best time: NEW RECORD!, its sound, and the total flashes
};
class TtEndUi {
public:
 TtEndUi();~TtEndUi();
 void setData(const TtEndData& data){data_=data;}
 const TtEndData& data()const{return data_;}
 // One texture per call (FlowTtTextures); true once all are resident, false on a failed load.
 bool loadStep();
 bool loaded()const;
 bool failed()const;
 unsigned bytes()const;
 void unload();
 // One 60 Hz tick of the pages' animations (flash, NEW RECORD, buttons).
 void step(const RaceResultsFlow& flow);
 // The frame's TT pages, after RaceFlowUi::drawResults (the HUD layer).
 void draw(Renderer& renderer,RaceFlowUi& flowUi,const RaceResultsFlow& flow);
 // DIAG counters (runtime.log): quads drawn by page.
 unsigned splitsQuads=0,menuQuads=0;
private:
 struct Impl;std::unique_ptr<Impl> impl_;
 TtEndData data_;
};
}
