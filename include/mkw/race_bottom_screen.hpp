#pragma once
#include <mkw/race_teams.hpp>
#include <mkw/license_identity.hpp>
#include <array>
#include <cstdint>
#include <memory>
namespace mkw {
class Renderer;
class Texture;
class RaceGrid;
class RaceItems;
// The race bottom screen (docs/BOTTOM_SCREEN.md), laid out as Mario Kart 7's:
// the standings down the left (Wii result-board rows: position, face, held
// item), a top-down map of the course on the right that follows the player at
// a fixed orientation (tap the touch screen for the whole course; Battle
// always shows the whole arena and ignores the toggle), every
// racer's face at its live position, items on the course, and the race time
// (Wii CtrlRaceTime) at the top right, as Mario Kart DS shows it.
//
// Presentation only: the view is filled read-only from the race each
// presented frame. Memory is fixed at load: VRAM for the bottom target, one
// course map, its overview, UI atlas and optional object atlas; 2 x QuadCapacity quads of heap.
struct BottomRacer {
    float x=0,z=0;            // world position
    float hitAngle=0;         // native map portrait damage animation
    uint8_t character=0;      // retail character id (0..23)
    uint8_t rank=12;          // current position, 1..12
    uint8_t item=20;          // held item icon (ItemKind; 20 none)
    uint16_t score=0;
    bool local=false;RaceTeam team=RaceTeam::None;
};
struct BottomItem {
    float x=0,z=0;
    uint8_t marker=0;         // BottomMarker
    float radius=0;           // blast radius (world units) for blasts
};
enum class BottomMarker : uint8_t {Banana,Bobomb,GreenShell,RedShell,SpinyShell,BlastRed,BlastBlue,FakeBox};
struct BottomMapObject {
    float x[4]{},z[4]{},height=0; // projected model-local top silhouette
    uint16_t art=0;
};
struct BottomView {
    static constexpr unsigned MaxItems=24,MaxMapObjects=32;
    unsigned mapObjects=0,mapCourse=~0u;
    std::array<BottomMapObject,MaxMapObjects> mapObject{};
    unsigned racers=0;
    std::array<BottomRacer,12> racer{};
    unsigned items=0;
    std::array<BottomItem,MaxItems> item{};
    float headingX=0,headingZ=-1; // the local kart's forward direction (world XZ)
    bool mirror=false;
    bool battle=false;
    bool timeTrial=false;         // no standings; full-width map and centred timer
    bool timerVisible=false;      // CtrlRaceTime: shown once the countdown runs
    bool timerLabel=false;        // with its TIME label (Timer: Full)
    uint32_t milliseconds=0;      // official race time (the player's own once finished)
    bool standingsVisible=false;  // Mario Kart 7: from the start of the race
    bool raceStarted=false;       // the race has started (the Standings view's Time Trial column)
    bool racerIconsVisible=true;  // racer faces on the map; off during the course preview
    uint32_t tick=0;              // simulation step (animations advance at 60 Hz)
    float fade=0;                 // black over the screen while leaving the race (1 opaque)
    // The course intro's info box (Mario Kart 7's flyover box in the standings rows' style, with a cup
    // medallion): the catalog course it names, whether it is in (its entrance plays forwards, its exit
    // backwards).
    unsigned infoCourse=~0u;bool infoShown=false;
};
// Fills view from the running race, read-only: racer positions, ranks and
// characters, each racer's item window picture, the local kart's heading,
// items on the course, the official time (the player's own once finished).
// leaveFade is the top screen's fade when leaving after the race (RaceResults::leaveFade).
void fillBottomView(BottomView& view,const RaceGrid& grid,const RaceItems& items,bool introActive,uint32_t tick,float leaveFade);
class RaceBottomScreen {
public:
    // On race courses the map is an opaque (ETC1) quad over the whole screen:
    // the background fill under it is never seen and is not drawn, the
    // target's per-frame clear is skipped, and the map draws without blending.
    // A/B: no-bottom-cover.txt.
    static inline bool skipCoveredFill=true;
    // The opaque full-screen fill (battle arenas) also skips the clear. A/B: no-bottom-fill-cover.txt.
    static inline bool fillCover=true;
    // Worst case by construction: fill 1, map 1, start line 24, 24 items as
    // blast rings (4 each) 96, racers 12 x 3 + searchlight 1, column 2,
    // standings 12 x 6 (up to three score digits in battle; the Standings view's
    // rows: three frame pieces, face, item, name), timer 10, leave fade 1, plus at
    // most 32 map objects, plus the Standings view's background, its shade,
    // silhouette, heading pointer and 12 number boxes (16) = 292.
    static constexpr unsigned QuadCapacity=BottomView::MaxMapObjects+1+1+24+BottomView::MaxItems*4+12*3+1+2+12*6+10+1+16;
    // The info box's own quads: inside 1, long edges and the bottom gold up to 9, short edges 6, the medallion
    // 3 (shadow, ring, emblem), then its glyphs (a cup line and the longest course name).
    static constexpr unsigned InfoArtCapacity=20,InfoCapacity=InfoArtCapacity+176;
    RaceBottomScreen();
    ~RaceBottomScreen();
    // Race load, render thread idle, after Renderer::reserveBloom() for a race
    // with bloom: creates the bottom target, then loads the course's map,
    // overview and the UI atlas into VRAM. A missing texture only drops what
    // it draws; false when the bottom screen cannot exist (it is then not
    // drawn). Reloading the same course is free.
    bool load(Renderer& renderer,unsigned course);
    // Frees everything (waits for the GPU) and blanks the lower LCD.
    void unload(Renderer& renderer);
    bool active() const;
    bool setMiiPortrait(Renderer&,const NativeMii&);
    const Texture* miiPortrait() const; // resident through the result screen
    // The info box's textures are resident (not off: no-flyinfo.txt; its emblem is the course's cup's,
    // copied in by load()). The top screen then leaves the course name to the box while the intro plays.
    bool infoReady() const;
    // How many ticks before the intro's end the box must start leaving to be gone by the countdown
    // (its exit, the entrance backwards; 0 with the DIAG MK7-style switch flyinfo-mk7-entrance.txt).
    unsigned infoLeadTicks() const;
    // Mario Kart 7's touch: detailed <-> whole course. Battle ignores it.
    // With the Standings view: close-up <-> Standings.
    void toggleView();
    // The Standings view (as Mario Kart DS's lower screen; tools/cook_race_standings.py): it replaces
    // the whole-course map on race courses (not Battle; off: no-ds-standings.txt). The view a race
    // starts in (settings.dat byte 15, the last one shown): set before load().
    static inline bool standingsRemembered=false;
    // True when this race has the Standings view; view: whether Standings is the chosen view now.
    bool standingsChoice(bool& view) const;
    // Race load, after load(): the racers' names for the Standings rows (UTF-8: a Mii's name, else
    // the character's), composed into one texture; also seeds the background's drift direction.
    void setRacerNames(Renderer&,const std::array<uint8_t,12>& characters,
                       const std::array<std::array<char,32>,12>& miiNames,unsigned racers);
    // Records the frame's bottom screen (after Renderer::beginBottom()).
    void draw(Renderer& renderer,const BottomView& view);
    // A race frame's bottom screen, with its own beginBottom(): when its
    // quads equal those of the previous frame, which was shown, the lower
    // LCD keeps that picture (no beginBottom, no clear, no draw, no transfer).
    // A/B: no-race-bottom-keep.txt.
    void drawRace(Renderer& renderer,const BottomView& view);
    // A race frame not due to redraw the lower LCD (RenderQuality::bottomInterval):
    // when the LCD shows this race's last shown picture, marks the frame as
    // covering it (no clear, no transfer) and returns true; the next drawRace may
    // still keep that picture. Otherwise returns false: draw this frame.
    bool skipRace(Renderer& renderer);
    // runtime.log line: what is resident.
    void describe(char* text,unsigned size) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool compose(Renderer&,const BottomView&);
    void submit(Renderer&);
    uint64_t frameHash() const;
    // DIAG: runtime.log "bottom keep:" with the race's kept and drawn frames.
    void endRace();
};
}
