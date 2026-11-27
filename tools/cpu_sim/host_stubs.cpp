// PC CPU simulator: Coin Runners (battle only, never reached in a race; it lives in
// src/coin_battle_runtime.cpp with its rendering). Abort if a race ever reaches it.
#include <mkw/coin_battle_runtime.hpp>
#include <cstdio>
#include <cstdlib>
namespace mkw {
[[noreturn]] static void notInRaces(const char* what){std::fprintf(stderr,"cpu_sim: %s is battle-only\n",what);std::abort();}
bool CoinBattleRuntime::targetPosition(unsigned,Vec3&) const{notInRaces("CoinBattleRuntime::targetPosition");}
void CoinBattleRuntime::failedRegistration(unsigned) const{notInRaces("CoinBattleRuntime::failedRegistration");}
unsigned CoinBattleRuntime::targetCount() const{notInRaces("CoinBattleRuntime::targetCount");}
}
