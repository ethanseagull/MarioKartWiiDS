#pragma once

#include "host/KSystem.hh"

#include <egg/core/SceneManager.hh>

#include <game/system/RaceConfig.hh>

#include <array>
#include <string>

namespace Kinoko {

/// @brief Kinoko system designed to execute replays.
class KReplaySystem : public KSystem {
public:
    void init() override;
    void calc() override;
    bool run() override;
    void parseOptions(int argc, char **argv) override;

    static KReplaySystem *CreateInstance();
    static void DestroyInstance();

    static KReplaySystem *Instance() {
        return static_cast<KReplaySystem *>(s_instance);
    }

    KReplaySystem();
    ~KReplaySystem() override;

private:
    typedef std::pair<const System::Timer &, const System::Timer &> DesyncingTimerPair;

    KReplaySystem(const KReplaySystem &) = delete;
    KReplaySystem(KReplaySystem &&) = delete;

    bool calcEnd() const;
    bool runReinitTest(int previewFrames);
    void reportFail(const std::string &msg) const;

    bool success() const;
    s32 getDesyncingTimerIdx() const;
    DesyncingTimerPair getDesyncingTimer(s32 i) const;

    static void OnInit(System::RaceConfig *config, void *arg);

    EGG::SceneManager *m_sceneMgr;

    const char *m_currentGhostFileName;
    const System::GhostFile *m_currentGhost;
    const u8 *m_currentRawGhost;
    size_t m_currentRawGhostSize;

    // Multi-ghost replays (KINOKO_EXTRA_GHOSTS="a.rkg,b.rkg,c.rkg"): up to three more
    // ghosts on ghost streams 1-3 as players 1-3, each checked against its own file.
    std::array<u8 *, 3> m_extraRaw{};
    std::array<System::GhostFile *, 3> m_extraGhost{};
    std::array<std::string, 3> m_extraName{};
    size_t m_extraCount = 0;
};

} // namespace Kinoko
