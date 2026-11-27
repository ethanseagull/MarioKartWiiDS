#pragma once
#include <cstdint>
namespace mkw {
class Renderer;
// Only instantiate around blocking main-thread work, outside a GPU frame.
// The spinner owns no filesystem or Citro3D state on its timer thread.
void initializeLoadingScreen(Renderer&);
void shutdownLoadingScreen();
// GPU wait screens pass their own start time; normal scene frames never call this.
void drawLoadingWheel(Renderer&,std::uint64_t waitStarted);
// LoadingScopes opened so far: each may have drawn on the lower LCD.
unsigned loadingScopes();
// The race loading screen draws the lower LCD's framebuffers itself: nothing else may write or swap them.
bool loadingScreenOwnsBottom();
class LoadingScope {
public:
 LoadingScope();~LoadingScope();
 LoadingScope(const LoadingScope&)=delete;LoadingScope& operator=(const LoadingScope&)=delete;
private:bool owner_=false;
};
// What the race loading screen shows about the coming race (main.cpp fills it
// once the race is chosen; nothing here is read from the SD card).
struct RaceLoadingInfo {
 unsigned course=0;      // catalog index (0-31 races, 32-41 arenas)
 unsigned mode=0;        // 0 Grand Prix, 1 VS, 2 Time Trials, 3 Battle
 unsigned engine=2;      // 0 50cc, 1 100cc, 2 150cc, 3 Mirror
 unsigned race=1,races=4;// Grand Prix and VS: this race of races; Battle: this battle
 bool coinRunners=false;
 // Time Trials: the course's records, fastest first (the license's best time,
 // the staff ghosts it can race); none without a time of the license's own.
 struct Record {std::uint32_t ms=0;unsigned character=~0u;bool mine=false;};
 unsigned records=0;Record record[4]{};
};
// The top screen while a race loads, drawn into the framebuffers by a timer
// thread like the lower screen's wheel, so open it around blocking main-thread
// work only, with no GPU frames in between; the destructor fades it out to
// black. With `info` (New 3DS): the course reveal (tools/cook_loading_reveal.py)
// on both screens: the course, its cup and its Standings map drawn by a pen
// along the route; the cup's courses or the course's records below. Otherwise,
// or without the reveal's files: the loading picture (tools/cook_loading_picture.py),
// whose periods count up. Without either it does nothing (black, as before).
class RaceLoadingPicture {
public:
 explicit RaceLoadingPicture(const RaceLoadingInfo* info=nullptr);~RaceLoadingPicture();
 RaceLoadingPicture(const RaceLoadingPicture&)=delete;RaceLoadingPicture& operator=(const RaceLoadingPicture&)=delete;
private:bool active_=false,reveal_=false;
};
}
