// Headless PC race of the game's CPU drivers: the game's own Kinoko
// (vendor/kinoko), route and CPU AI (race_grid.cpp, cpu_ai.cpp), item system
// (race_items.cpp, item_native.cpp, item_world.cpp, shell_guidance.cpp) and
// course preview timing (race_intro.cpp), stepped in src/main.cpp's per-tick
// order. Rendering, sound, UI and files are not part of it.
//
//   --course N [--seed S] ...    a VS race on course N (the Courses[] index: 0-31
//                                race courses), opponents drawn as the menus do
//                                from S; player 0 is driven by the CPU route as the
//                                DIAG autopilot's auto:N does.
//   --course N --battle ...      an offline Balloon Battle round on arena N (32-41),
//                                main.cpp's battle wiring without its presentation.
// Race options: --cc 0|1|2 (50/100/150cc), --cc 3 (150cc mirror, as the menus' Mirror),
// --gp (Grand Prix: GP CPU rows, CPU difficulty from the class), --cpu N, --vehicles N,
// --items N, --roster "11 garage ids" (fixed opponents, as DIAG bench-cpus.txt),
// --no-retail-start (the CPU AI as before the retail start/respawn rules, as DIAG
// no-retail-cpu-start.txt: CpuAi::retailStart off).
// Output: a JSON report (--out FILE, default stdout): falls, respawns, stuck
// spells, wall contact per route point, laps, finish order; p0Finish is the tick
// player 0 finished (the race the player sees ends there; a battle: its end).
// Data: --root DIR holds Race/Common.szs and Race/Course/*.szs as race.pak serves
// them. Build and sweep: tools/cpu_sim/gen_ninja.py, tools/cpu_sim/sweep.py.
#include <host/SceneCreatorDynamic.hh>
#include <egg/core/ExpHeap.hh>
#include <egg/core/SceneManager.hh>
#include <game/field/ObjectDirector.hh>
#include <game/field/ObjectDrivableDirector.hh>
#include <game/field/CollisionDirector.hh>
#include <game/kart/KartObjectManager.hh>
#include <game/kart/KartMove.hh>
#include <game/kart/KartCollide.hh>
#include <game/kart/KartAction.hh>
#include <game/kart/KartParam.hh>
#include <game/kart/BattleEvents.hh>
#include <game/system/CourseMap.hh>
#include <game/system/KPadDirector.hh>
#include <game/system/RaceConfig.hh>
#include <game/system/RaceManager.hh>
#include <game/system/ResourceManager.hh>
#include <mkw/balloon_battle.hpp>
#include <mkw/battle_rules.hpp>
#include <mkw/course_catalog.hpp>
#include <mkw/cpu_ai.hpp>
#include <mkw/cpu_roster.hpp>
#include <mkw/garage.hpp>
#include <mkw/race_grid.hpp>
#include <mkw/race_intro.hpp>
#include <mkw/race_items.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace K = Kinoko;
using mkw::Courses;

namespace {
constexpr unsigned CourseTotal=unsigned(std::size(Courses));
constexpr unsigned RaceLaps=3; // retail courses: three laps (main.cpp jugem.lapCount)
// ---- the race to run (what main.cpp's menus and configure() decide) ----
struct Choice {
    unsigned courseIndex=0;           // Courses[] index
    unsigned engineClass=2,cpu=1,vehicles=2,items=0,mode=1; // VS 150cc normal CPUs, karts and bikes, basic items
    bool mirror=false,automaticDrift=false;
    unsigned localGarage=0;           // Mario, Standard Kart M
    bool battle=false;                // offline Balloon Battle (mode 3)
    std::array<unsigned,12> roster{};bool rosterReady=false;
    std::array<uint8_t,12> startPositions{};
} g;
mkw::BalloonBattle gBalloons;

// main.cpp configure() for a full (non Time Trials) race.
void configure(K::System::RaceConfig* config,void* context){
    auto& s=config->raceScenario();s.course=Courses[g.courseIndex].id;
    s.engineClass=uint8_t(g.engineClass);s.mirror=g.mirror;
    s.cpuDifficulty=uint8_t(g.cpu);s.cpuVehicles=uint8_t(g.vehicles);s.itemMode=uint8_t(g.items);
    s.gameMode=static_cast<K::System::RaceConfig::Scenario::GameMode>(g.mode);
    // VS: no teams. Battle: red/blue teams (the menus' Battle Mode page), the human on red.
    s.teams=g.battle;s.humanTeam=0;s.battleType=0;
    static_cast<mkw::RaceGrid*>(context)->configure(*config,g.localGarage,12,g.rosterReady?&g.roster:nullptr);
    for(unsigned i=0;i<s.playerCount;++i)s.players[i].startPosition=g.startPositions[i];
    s.players[0].driftIsAuto=g.automaticDrift;
}

// ---- analysis ----
struct FallEvent { unsigned tick,racer,lap,point;float x,y,z,speed;bool wall,ground;unsigned hitAgo,bumpAgo,action;bool after,ai,empty; };
struct StuckEvent { unsigned tick,racer,lap,point,frames;float x,y,z; };
struct AiEvent { unsigned tick,racer,state,point;float x,y,z;bool empty; }; // the AI stuck handler entering recover (1) or respawn (2)
struct RacerStats {
    unsigned falls=0,respawns=0,aiStuck=0,aiRespawns=0;
    unsigned wallFrames=0,racingFrames=0,slowFrames=0;
    std::map<unsigned,unsigned> wallByPoint;   // ENPT -> frames touching a wall while racing
    bool falling=false,inRespawn=false;int lastStuck=0;
    unsigned slowRun=0;float slowX=0,slowY=0,slowZ=0;unsigned slowPoint=0,slowLap=0,slowStart=0;
    float bestCompletion=0;unsigned bestAt=0;
    uint32_t actionStarts=0;unsigned lastHit=~0u/2,lastBump=~0u/2;unsigned lastAction=0;
};

const char* argValue(int& i,int argc,char** argv){if(i+1>=argc){std::fprintf(stderr,"missing value for %s\n",argv[i]);std::exit(2);}return argv[++i];}
}

int main(int argc,char** argv){
    std::setvbuf(stdout,nullptr,_IONBF,0);
#if defined(__x86_64__) || defined(__i386__)
    // The game runs the ARM11 VFP with flush-to-zero (main.cpp sets FPSCR.FZ).
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);_MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
#endif
    // CPUSIM_RETAIL_ORDER=1: the port's exact Kinoko reorderings off (retail's per-kart
    // pass order), to check that a result does not depend on them.
    if(const char* e=std::getenv("CPUSIM_RETAIL_ORDER");e&&*e=='1'){
        K::Kart::KartObjectManager::s_pass1Grouped=false;K::Kart::KartObjectManager::s_pass0Split=false;
        K::Kart::KartObjectManager::s_phaseMajor=false;
    }
    int courseId=-1;unsigned seed=1;unsigned maxTicks=60*60*8;const char* outPath=nullptr;const char* root=nullptr;
    bool bootScene=true;unsigned qaRollPhase=300,skipAt=0;bool qaRolls=true,printRoster=false,cpuGiven=false;
    unsigned afterFinish=0;   // ticks to keep stepping once every racer finished
    bool fixedRoster=false;std::array<unsigned,12> fixedCpus{};
    for(int i=1;i<argc;++i){const std::string a=argv[i];
        if(a=="--course")courseId=std::atoi(argValue(i,argc,argv));
        else if(a=="--seed")seed=unsigned(std::strtoul(argValue(i,argc,argv),nullptr,0));
        else if(a=="--ticks")maxTicks=unsigned(std::atoi(argValue(i,argc,argv)));
        else if(a=="--out")outPath=argValue(i,argc,argv);
        else if(a=="--root")root=argValue(i,argc,argv);
        // The menus' classes (main.cpp: cc 3 = Mirror, 150cc mirrored).
        else if(a=="--cc"){const unsigned cc=unsigned(std::atoi(argValue(i,argc,argv)));g.mirror=cc==3;g.engineClass=std::min(cc,2u);}
        else if(a=="--cpu"){g.cpu=unsigned(std::atoi(argValue(i,argc,argv)));cpuGiven=true;}
        else if(a=="--vehicles")g.vehicles=unsigned(std::atoi(argValue(i,argc,argv)));
        else if(a=="--items")g.items=unsigned(std::atoi(argValue(i,argc,argv)));
        else if(a=="--no-boot")bootScene=false;
        else if(a=="--battle"){g.battle=true;g.mode=3;}
        else if(a=="--gp")g.mode=0;
        else if(a=="--no-retail-start")mkw::CpuAi::retailStart=false;
        // --roster "C1 ... C11": the eleven opponents' garage indices (main.cpp bench-cpus.txt), instead of the seed's draw.
        else if(a=="--roster"){const char* t=argValue(i,argc,argv);fixedRoster=true;
            for(unsigned k=1;k<12;++k){char* e=nullptr;const unsigned long v=std::strtoul(t,&e,10);if(e==t){std::fprintf(stderr,"--roster needs 11 values\n");return 2;}fixedCpus[k]=unsigned(v);t=e;}}
        else if(a=="--no-qa-rolls")qaRolls=false;
        else if(a=="--print-roster")printRoster=true;
        else if(a=="--skip-preview")skipAt=unsigned(std::atoi(argValue(i,argc,argv))); // an OK press from this tick skips the course preview
        else if(a=="--qa-phase")qaRollPhase=unsigned(std::atoi(argValue(i,argc,argv)));
        else if(a=="--after-finish")afterFinish=unsigned(std::atoi(argValue(i,argc,argv)));
        else{std::fprintf(stderr,"unknown argument %s\n",a.c_str());return 2;}
    }
    // Grand Prix: the CPUs' difficulty follows the class (main.cpp selectedCpu).
    if(g.mode==0&&!cpuGiven)g.cpu=std::min(g.engineClass,2u);
    if(root)setenv("KINOKO_FILESYSTEM_ROOT",root,1);
    if(courseId<0||unsigned(courseId)>=CourseTotal){std::fprintf(stderr,"course %d not in the catalog (0-%u)\n",courseId,CourseTotal-1);return 2;}
    g.courseIndex=unsigned(courseId);
    if(g.battle!=(g.courseIndex>=32)){std::fprintf(stderr,"courses 0-31 race, 32-41 are battle arenas (--battle)\n");return 2;}
    {
        // The menus' opponents (menu_state.cpp: CpuRoster characters then vehicles from cpuRandom).
        mkw::CpuRosterRandom random;random.state=seed;
        mkw::CpuRoster roster;mkw::LicenseUnlockFlags all;all.fill(~0u);
        roster.characters(0,all,random);roster.vehicles(0,g.vehicles,g.battle,all,random);
        g.roster=roster.choices;g.rosterReady=true;
        if(fixedRoster)for(unsigned k=1;k<12;++k)if(fixedCpus[k]<mkw::GarageChoices.size())g.roster[k]=fixedCpus[k];
        if(printRoster){for(unsigned i=1;i<12;++i)std::printf("%u%s",g.roster[i],i<11?" ":"\n");return 0;}
    }

    // Kinoko's arena as the New 3DS game sizes it.
    const size_t arenaSize=size_t(K::MEMORY_SPACE_SIZE);
    void* memory=std::aligned_alloc(32,arenaSize);
    auto* heap=K::EGG::ExpHeap::create(memory,arenaSize,K::DEFAULT_OPT);
    heap->becomeCurrentHeap();K::EGG::SceneManager::SetRootHeap(heap);

    static mkw::RaceGrid raceGrid;
    auto raceItems=std::make_unique<mkw::RaceItems>();
    K::Host::SceneCreatorDynamic creator;
    K::EGG::SceneManager scenes(&creator);
    K::System::RaceConfig::RegisterInitCallback(configure,&raceGrid);
    // The game builds a race scene at boot (course 0, menu defaults) and replaces it for each race.
    const unsigned target=g.courseIndex;const auto saved=g;
    if(bootScene){g=Choice{};g.courseIndex=0;scenes.changeScene(0);g=saved;}
    g.courseIndex=target;
    if(!bootScene)scenes.changeScene(0);
    else{scenes.currentScene()->heap()->enableAllocation();
        if(scenes.currentSceneId()==0)scenes.createChildScene(2,scenes.currentScene());else scenes.changeSiblingScene(2);}

    // resetRaceSystems(true), the parts that change the race.
    if(g.battle){
        size_t ruleSize=0;const void* ruleData=K::System::ResourceManager::Instance()->getFile("minigame.kmg",&ruleSize,K::System::ArchiveId::Core);
        mkw::BattleRules rules;
        if(!rules.load(ruleData,ruleSize,unsigned(Courses[g.courseIndex].id),raceGrid.count(),mkw::BattleMode::Balloon)){std::printf("{\"error\":\"FAIL battle rules\"}\n");return 3;}
        std::array<mkw::RaceTeam,12> teams;teams.fill(mkw::RaceTeam::None);
        {const auto& sc=K::System::RaceConfig::Instance()->raceScenario();
         if(sc.teams)for(unsigned i=0;i<raceGrid.count();++i)teams[i]=mkw::RaceTeam(sc.players[i].team);}
        gBalloons.configure(rules,teams);gBalloons.reset(raceGrid.count());
        K::Kart::BattleEvents::SetHook([](void*,const K::Kart::BattleEvents::Event& event){
            auto* race=K::System::RaceManager::Instance();
            if(!race||!race->isStageReached(K::System::RaceManager::Stage::Race)||race->isStageReached(K::System::RaceManager::Stage::FinishGlobal))return false;
            const unsigned victim=unsigned(event.victim),attacker=unsigned(event.attacker);
            const bool changed=event.kind==K::Kart::BattleEvents::Kind::Steal?gBalloons.stealBalloon(attacker,victim):gBalloons.loseBalloon(victim,attacker);
            if(changed&&!gBalloons.balloons(victim))K::Kart::KartObjectManager::Instance()->object(victim)->move()->loseAllBalloons();
            return changed;
        },nullptr);
        K::Kart::KartMove::SetBattleRefillHook([](void*,size_t racer){
            auto* race=K::System::RaceManager::Instance();
            if(race&&race->isStageReached(K::System::RaceManager::Stage::Race)&&!race->isStageReached(K::System::RaceManager::Stage::FinishGlobal))
                gBalloons.replenish(unsigned(racer));
        },nullptr);
    }
    if(!raceGrid.reset()){std::printf("{\"error\":\"FAIL course CPU route\"}\n");return 3;}
    raceItems->setTimeTrial(false);mkw::RaceItems::ghostOwnsItems=false;
    raceItems->resetFromCourse(raceGrid.itemCount());
    if(g.battle&&!raceGrid.ai().setBattleSources(*raceItems,nullptr)){std::printf("{\"error\":\"FAIL battle search graph\"}\n");return 3;}
    if(!raceItems->oddsReady()){std::printf("{\"error\":\"FAIL item odds\"}\n");return 3;}
    unsigned battleTicks=0,battleEnd=0;
    // startIntro(): the course preview holds the intro stage.
    mkw::RaceIntro intro;mkw::DemoSection demo;
    {size_t size=0;const void* kmp=K::System::ResourceManager::Instance()->getFile("course.kmp",&size,K::System::ArchiveId::Course);
     if(intro.load(kmp,size))intro.start();}
    demo.enter(intro.active());
    auto* race=K::System::RaceManager::Instance();auto* karts=K::Kart::KartObjectManager::Instance();
    race->setIntroHeld(intro.active());
    unsigned warmup=2;race->setIntroHeld(true);

    const unsigned count=raceGrid.count();
    std::array<RacerStats,12> stats{};std::vector<FallEvent> falls;std::vector<StuckEvent> stucks;
    struct RespawnEvent{unsigned tick,racer,lap,jgpt,point;float x,y,z;};std::vector<RespawnEvent> respawns;std::array<unsigned,12> respawnCheck{};
    unsigned allFinishedAt=0;unsigned introEnd=0;
    unsigned p0Finish=0;std::array<float,12> completionAtP0{};std::vector<AiEvent> aiEvents;
    auto* host0=K::System::KPadDirector::Instance()->hostController(0);
    unsigned tick=0;
    for(;tick<maxTicks;++tick){
        // The human's (autopilot's) input for this tick: held A in the race, nothing in the preview.
        {const bool held=!warmup&&!intro.active();host0->setInputsRawStick(held?1:0,7,7,K::System::Trick::None);}
        const bool afterGoal=g.battle?battleEnd!=0:raceGrid.finished();
        if(g.battle){std::array<uint16_t,12> scores{};for(unsigned i=0;i<count;++i)scores[i]=uint16_t(gBalloons.score(i));raceItems->setBattleScores(scores);}
        raceGrid.driveLocal=true;
        std::array<mkw::ItemInput,12> inputs{};
        if(afterGoal)inputs[0]={};
        else{
            inputs[0]=raceItems->autoItemInput(0);
            if(qaRolls&&tick%600==qaRollPhase)raceItems->startRoulette(0);
        }
        for(unsigned i=1;i<count;++i){
            if(!raceGrid.ai().ready()){inputs[i]=raceItems->autoItemInput(i);continue;}
            const auto& c=raceGrid.ai().itemCommand(i);inputs[i]={c.use,c.backwards,c.forwards};
        }
        raceItems->beforeStepNative(inputs.data(),raceGrid.itemCount(),race->isStageReached(K::System::RaceManager::Stage::Race),raceGrid.bulletNoEndPlayers());
        for(unsigned i=0;i<count;++i){
            const auto& h=raceItems->hud(i);const bool held=h.deployed&&h.heldCharges>0;
            raceGrid.ai().setItemView(i,{uint8_t(h.item),uint8_t(held?h.heldCharges:0),h.roulette!=0,held,
                held&&(h.heldKind==mkw::ItemKind::RedShell||h.heldKind==mkw::ItemKind::TripleRedShell),h.ink&&!h.inkDelay,uint8_t(h.heldKind)});
        }
        {
            std::array<mkw::Vec3,mkw::CpuAi::MaxHazards> hazards{};std::array<float,mkw::CpuAi::MaxHazards> radii{};unsigned n=0;
            const auto& pool=raceItems->actors();
            raceItems->forEachActorSlot([&](unsigned slot){
                const auto& a=pool[slot];
                if(n>=hazards.size())return false;
                if(!a.active||a.explosion||!a.settled)return true;
                if(a.kind!=mkw::ItemKind::Banana&&a.kind!=mkw::ItemKind::FakeBox&&a.kind!=mkw::ItemKind::Bobomb)return true;
                hazards[n]=a.position;radii[n]=a.radius;++n;return true;
            });
            raceGrid.ai().setHazards(hazards.data(),radii.data(),n);
        }
        raceGrid.beforeStep(raceItems->bulletPlayers());
        scenes.calc();
        if(!warmup){
            const auto event=demo.step(intro.finished(),skipAt&&tick>=skipAt);
            if(event==mkw::DemoSection::Event::EnterRace){
                intro.stop();race->setIntroHeld(false);
                K::Field::ObjectDirector::Instance()->reinitForRace();introEnd=tick;
            }
        }
        if(intro.active()&&!warmup)intro.step();
        raceGrid.afterStep();raceItems->afterStepNative(raceGrid.itemCount());
        if(warmup&&!--warmup)race->setIntroHeld(intro.active());
        if(g.battle){
            gBalloons.updateRanks();std::array<unsigned,12> ranks{};for(unsigned i=0;i<count;++i)ranks[i]=gBalloons.rank(i);
            raceGrid.setBattleRanks(ranks);
            if(unsigned(race->stage())>=2&&!battleEnd&&battleTicks<gBalloons.durationTicks())++battleTicks;
            if(!battleEnd&&battleTicks>=gBalloons.durationTicks()){
                std::array<K::u8,12> order{};for(unsigned i=0;i<count;++i)order[i]=K::u8(i);
                std::stable_sort(order.begin(),order.begin()+count,[](unsigned a,unsigned b){return gBalloons.rank(a)<gBalloons.rank(b);});
                race->endBattle(order);battleEnd=tick;
            }
        }

        // Analysis (reads only).
        const bool racing=race->isStageReached(K::System::RaceManager::Stage::Race);
        // CPUSIM_TRACE="RACER FROM TO": that racer's kart and AI state per tick (stderr), analysis only.
        if(static const char* te=std::getenv("CPUSIM_TRACE");te){unsigned tr=0,t0=0,t1=0;
            if(std::sscanf(te,"%u %u %u",&tr,&t0,&t1)==3&&tick>=t0&&tick<=t1&&tr<count){
                auto* k=karts->object(tr);const auto& p=k->pos();const auto& body=k->collisionData();const auto t=raceGrid.ai().target(tr);
                const auto f=k->bodyFront();using St=K::Kart::eStatus;const auto& st=k->status();
                std::fprintf(stderr,"t %u pos %.0f %.0f %.0f front %.2f %.2f spd %.1f wall %d%d gnd %d act %d pt %u tgt %.0f %.0f %.0f stuck %d avoid %d cmd %04x %.2f\n",tick,p.x,p.y,p.z,f.x,f.z,k->move()->speed(),
                    int(body.bWall),int(body.bWall3),int(st.onBit(St::TouchingGround)),int(st.onBit(St::InAction)),raceGrid.ai().routePoint(tr),t.x,t.y,t.z,
                    raceGrid.ai().stuckState(tr),raceGrid.ai().avoidState(tr),unsigned(raceGrid.ai().command(tr).buttons),raceGrid.ai().command(tr).stick);}}
        bool all=true;
        for(unsigned i=0;i<count;++i){
            auto& s=stats[i];auto* k=karts->object(i);const auto& st=k->status();using St=K::Kart::eStatus;
            const auto& pl=race->player(i);const unsigned lap=raceGrid.lap(i),point=raceGrid.ai().routePoint(i);
            const auto& p=k->pos();
            if(!pl.finished())all=false;
            // Item hits and other actions (spins, tumbles: KartAction starts) and kart bumps, for the falls' cause.
            if(k->action()->startCount()!=s.actionStarts){s.actionStarts=k->action()->startCount();s.lastHit=tick;s.lastAction=unsigned(k->action()->lastStarted());}
            if(k->collide()->kartBumped())s.lastBump=tick;
            const bool fall=st.onBit(St::TriggerRespawn,St::BeforeRespawn);
            // Balloon Battle: a racer without balloons vanishes and is respawned (not a fall).
            const bool emptyBattle=st.onBit(St::BattleEmpty,St::BattleHidden,St::BattleVanish);
            // Every fall is listed (after: the racer had finished); the counts are the race's.
            if(fall&&!s.falling){if(!pl.finished())++s.falls;const auto& body=k->collisionData();
                falls.push_back({tick,i,lap,point,p.x,p.y,p.z,k->move()->speed(),body.bWall||body.bWall3,st.onBit(St::TouchingGround),tick-s.lastHit,tick-s.lastBump,s.lastAction,pl.finished(),
                                 raceGrid.ai().stuckState(i)==2,emptyBattle});}
            s.falling=fall;
            const bool respawn=st.onBit(St::InRespawn);if(respawn&&!s.inRespawn)++s.respawns;
            // When the respawn ends (Lakitu lets go): its JGPT and the ENPT the AI drives on from.
            if(!respawn&&s.inRespawn)respawnCheck[i]=tick+1;
            s.inRespawn=respawn;
            if(respawnCheck[i]&&tick>=respawnCheck[i]){respawnCheck[i]=0;respawns.push_back({tick,i,lap,unsigned(std::max<int>(g.battle?race->battleRespawnJugem(i):pl.jugemId(),0)),raceGrid.ai().routePoint(i),p.x,p.y,p.z});}
            const int stuck=raceGrid.ai().stuckState(i);
            if(stuck!=s.lastStuck){if(stuck==1)++s.aiStuck;if(stuck==2)++s.aiRespawns;s.lastStuck=stuck;
                if(stuck&&racing&&!pl.finished())aiEvents.push_back({tick,i,unsigned(stuck),point,p.x,p.y,p.z,emptyBattle});}
            if(racing&&!pl.finished()){
                ++s.racingFrames;const auto& body=k->collisionData();
                if((body.bWall||body.bWall3)&&!body.bInvisibleWallOnly){++s.wallFrames;++s.wallByPoint[point];}
                const bool slow=std::fabs(k->move()->speed())<5.f&&!fall&&!respawn&&tick>introEnd+600;
                if(slow){if(!s.slowRun){s.slowStart=tick;s.slowX=p.x;s.slowY=p.y;s.slowZ=p.z;s.slowPoint=point;s.slowLap=lap;}++s.slowRun;++s.slowFrames;}
                else{if(s.slowRun>=120)stucks.push_back({s.slowStart,i,s.slowLap,s.slowPoint,s.slowRun,s.slowX,s.slowY,s.slowZ});s.slowRun=0;}
                if(pl.raceCompletion()>s.bestCompletion+0.002f){s.bestCompletion=pl.raceCompletion();s.bestAt=tick;}
            }
        }
        // Player 0's finish (a battle: its end): where the race the player sees ends.
        if(!p0Finish&&(g.battle?battleEnd!=0:race->player(0).finished())){p0Finish=tick;for(unsigned i=0;i<count;++i)completionAtP0[i]=race->player(i).raceCompletion();}
        if(g.battle&&battleEnd)break;
        if(all&&count){if(!allFinishedAt)allFinishedAt=tick;if(tick-allFinishedAt>=afterFinish)break;}
        if(race->stage()==K::System::RaceManager::Stage::FinishGlobal&&!allFinishedAt)allFinishedAt=tick;
    }
    for(unsigned i=0;i<count;++i)if(stats[i].slowRun>=120)stucks.push_back({stats[i].slowStart,i,stats[i].slowLap,stats[i].slowPoint,stats[i].slowRun,stats[i].slowX,stats[i].slowY,stats[i].slowZ});

    // Report.
    std::FILE* out=outPath?std::fopen(outPath,"w"):stdout;if(!out)return 4;
    const auto& sc=K::System::RaceConfig::Instance()->raceScenario();
    std::fprintf(out,"{\"course\":%u,\"name\":\"%s\",\"slot\":%u,\"seed\":%u,\"ticks\":%u,\"introEnd\":%u,\"laps\":%u,\"engineClass\":%u,\"mirror\":%d,\"finishedAt\":%u,\"p0Finish\":%u,\"battle\":%d,\"gameMode\":%u,\"cpu\":%u,\"retailStart\":%d,\n",
        g.courseIndex,Courses[g.courseIndex].name,unsigned(sc.course),seed,tick,introEnd,RaceLaps,unsigned(sc.engineClass),int(sc.mirror),
        allFinishedAt,p0Finish,int(g.battle),unsigned(sc.gameMode),unsigned(sc.cpuDifficulty),int(mkw::CpuAi::retailStart));
    std::fprintf(out,"\"racers\":[\n");
    for(unsigned i=0;i<count;++i){const auto& s=stats[i];const auto& pl=race->player(i);const auto& p=sc.players[i];
        std::fprintf(out," {\"i\":%u,\"character\":%u,\"vehicle\":%u,\"finished\":%d,\"order\":%u,\"rank\":%u,\"completion\":%.4f,\"time\":%u,\"laps\":[",
            i,unsigned(p.character),unsigned(p.vehicle),int(pl.finished()),unsigned(pl.finishOrder()),raceGrid.rank(i),pl.raceCompletion(),
            pl.finished()?unsigned(pl.raceTimer().min)*60000u+unsigned(pl.raceTimer().sec)*1000u+unsigned(pl.raceTimer().mil):0u);
        for(unsigned l=1;l<=RaceLaps;++l){const auto t=pl.getLapSplit(l);std::fprintf(out,"%s%u",l>1?",":"",unsigned(t.min)*60000u+unsigned(t.sec)*1000u+unsigned(t.mil));}
        std::fprintf(out,"],\"falls\":%u,\"respawns\":%u,\"aiStuck\":%u,\"aiRespawns\":%u,\"wallFrames\":%u,\"racingFrames\":%u,\"slowFrames\":%u,\"completionAtP0\":%.4f,\"score\":%u,\"wallPoints\":{",
            s.falls,s.respawns,s.aiStuck,s.aiRespawns,s.wallFrames,s.racingFrames,s.slowFrames,completionAtP0[i],g.battle?gBalloons.score(i):0u);
        bool first=true;for(const auto& [pt,n]:s.wallByPoint)if(n>=30){std::fprintf(out,"%s\"%u\":%u",first?"":",",pt,n);first=false;}
        std::fprintf(out,"}}%s\n",i+1<count?",":"");}
    std::fprintf(out,"],\"falls\":[\n");
    for(size_t k=0;k<falls.size();++k){const auto& f=falls[k];
        std::fprintf(out," {\"tick\":%u,\"racer\":%u,\"lap\":%u,\"point\":%u,\"pos\":[%.0f,%.0f,%.0f],\"speed\":%.1f,\"wall\":%d,\"ground\":%d,\"hitAgo\":%u,\"bumpAgo\":%u,\"action\":%u,\"after\":%d,\"ai\":%d,\"empty\":%d}%s\n",
            f.tick,f.racer,f.lap,f.point,f.x,f.y,f.z,f.speed,int(f.wall),int(f.ground),std::min(f.hitAgo,99999u),std::min(f.bumpAgo,99999u),f.action,int(f.after),int(f.ai),int(f.empty),k+1<falls.size()?",":"");}
    std::fprintf(out,"],\"stuck\":[\n");
    for(size_t k=0;k<stucks.size();++k){const auto& f=stucks[k];
        std::fprintf(out," {\"tick\":%u,\"racer\":%u,\"lap\":%u,\"point\":%u,\"frames\":%u,\"pos\":[%.0f,%.0f,%.0f]}%s\n",
            f.tick,f.racer,f.lap,f.point,f.frames,f.x,f.y,f.z,k+1<stucks.size()?",":"");}
    std::fprintf(out,"],\"respawns\":[\n");
    for(size_t k=0;k<respawns.size();++k){const auto& f=respawns[k];
        std::fprintf(out," {\"tick\":%u,\"racer\":%u,\"lap\":%u,\"jgpt\":%u,\"point\":%u,\"pos\":[%.0f,%.0f,%.0f]}%s\n",f.tick,f.racer,f.lap,f.jgpt,f.point,f.x,f.y,f.z,k+1<respawns.size()?",":"");}
    std::fprintf(out,"],\"aiEvents\":[\n");
    for(size_t k=0;k<aiEvents.size();++k){const auto& f=aiEvents[k];
        std::fprintf(out," {\"tick\":%u,\"racer\":%u,\"state\":%u,\"point\":%u,\"pos\":[%.0f,%.0f,%.0f],\"empty\":%d}%s\n",f.tick,f.racer,f.state,f.point,f.x,f.y,f.z,int(f.empty),k+1<aiEvents.size()?",":"");}
    std::fprintf(out,"]}\n");
    if(out!=stdout)std::fclose(out);
    return 0;
}
