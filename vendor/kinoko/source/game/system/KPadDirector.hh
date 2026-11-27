#pragma once

#include "game/system/KPadController.hh"

namespace Kinoko {

namespace Host {

class Context;

} // namespace Host

namespace System {

/// @brief The highest level abstraction for controller processing.
/// @addr{0x809BD70C}
class KPadDirector : EGG::Disposer {
    friend class Host::Context;

public:
    void calc();
    void calcPads();

    /// @addr{0x80523724}
    void clear() {}

    void reset();
    void startGhostProxies();
    void endGhostProxies();

    [[nodiscard]] const KPadPlayer &playerInput(size_t index = 0) const {
        ASSERT(index < m_playerCount);
        return m_playerInputs[index];
    }

    [[nodiscard]] KPadHostController *hostController(size_t index = 0) {
        ASSERT(index < m_playerCount);
        return m_hostControllers[index];
    }

    /// `slot`: the ghost's stream (0-3). One ghost uses slot 0; multi-ghost Time Trials
    /// up to four.
    void setGhostPad(const u8 *inputs, bool driftIsAuto, size_t index = 0, size_t slot = 0);
    /// Before a race's controllers are set: only ghost stream 0 runs until set again.
    void resetGhostSlots() {
        m_ghostSlots = 1;
    }
    void setHostPad(bool driftIsAuto, size_t index = 0);
    void setPlayerCount(size_t count) {
        ASSERT(count >= 1 && count <= m_playerInputs.size());
        m_playerCount = count;
    }

    static KPadDirector *CreateInstance();
    static void DestroyInstance();

    [[nodiscard]] static KPadDirector *Instance() {
        return s_instance;
    }

private:
    EGG_NEW_DELETE_FRIEND

    KPadDirector();
    ~KPadDirector() override;

    std::array<KPadPlayer, 12> m_playerInputs;
    size_t m_playerCount = 1;
    std::array<KPadGhostController *, 4> m_ghostControllers;
    size_t m_ghostSlots = 1; ///< Ghost streams in use (stream 0 always calculated, as retail's one).
    std::array<KPadHostController *, 12> m_hostControllers;

    static KPadDirector *s_instance; ///< @addr{0x809BD70C}
};

} // namespace System

} // namespace Kinoko
