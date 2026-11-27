// Copies the race state the audio thread needs out of Kinoko into a
// RaceAudioFrame, once per simulation tick on the main thread. Only the
// audio-only sound cues are taken (cleared); the simulation state is read.
#include <mkw/race_audio.hpp>
#include <mkw/race_item_internal.hpp>
#include <mkw/race_items.hpp>
#include <game/kart/CollisionGroup.hh>
#include <game/kart/KartDynamics.hh>
#include <game/kart/KartMove.hh>
#include <game/kart/KartObjectManager.hh>
#include <game/kart/KartState.hh>
#include <game/kart/KartSuspensionPhysics.hh>
#include <game/kart/Status.hh>
#include <game/kart/KartAction.hh>
#include <game/kart/KartCollide.hh>
#include <game/field/ObjectDirector.hh>
#include <game/field/ObjectDrivableDirector.hh>
#include <game/field/obj/ObjectBase.hh>
#include <game/system/RaceConfig.hh>
#include <game/system/RaceManager.hh>
#include <game/system/CourseMap.hh>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace mkw {
namespace K = Kinoko;
// Kinoko numbers its status flags exactly as retail's KartStatus bitfields;
// the flag set is a plain array of those 32-bit words.
static_assert(sizeof(K::Kart::Status) == 5 * sizeof(std::uint32_t));
using Cue = K::Kart::KartMove::SoundCue;
static_assert(std::uint32_t(Cue::Boost) == cue::Boost && std::uint32_t(Cue::BoostRamp) == cue::BoostRamp &&
              std::uint32_t(Cue::JumpPad) == cue::JumpPad && std::uint32_t(Cue::JumpPadMushroom) == cue::JumpPadMushroom &&
              std::uint32_t(Cue::Hop) == cue::Hop && std::uint32_t(Cue::Cannon) == cue::Cannon &&
              std::uint32_t(Cue::Landing) == cue::Landing && std::uint32_t(Cue::Horn) == cue::Horn &&
              std::uint32_t(Cue::Bumped) == cue::Bumped && std::uint32_t(Cue::BushWall) == cue::BushWall &&
              std::uint32_t(Cue::BoostVoice) == cue::BoostVoice && std::uint32_t(Cue::Trick) == cue::Trick &&
              std::uint32_t(Cue::Respawn) == cue::Respawn && std::uint32_t(Cue::StartFail) == cue::StartFail &&
              std::uint32_t(Cue::Wheelie) == cue::Wheelie && std::uint32_t(Cue::PowerHit) == cue::PowerHit &&
              std::uint32_t(Cue::StarHit) == cue::StarHit && std::uint32_t(Cue::Bump) == cue::Bump &&
              std::uint32_t(Cue::Fall) == cue::Fall && std::uint32_t(Cue::MegaUse) == cue::MegaUse &&
              std::uint32_t(Cue::MegaEnd) == cue::MegaEnd && std::uint32_t(Cue::Grow) == cue::Grow &&
              std::uint32_t(Cue::Press) == cue::Press && std::uint32_t(Cue::PressEnd) == cue::PressEnd &&
              std::uint32_t(Cue::Spin) == cue::Spin && std::uint32_t(Cue::BlowSide) == cue::BlowSide &&
              std::uint32_t(Cue::SoundTrigger) == cue::SoundTrigger);
static_assert(std::uint32_t(K::Kart::eStatus::ZipperTrick) == 47);
// The presentation cues must not collide with Kinoko's.
static_assert(cue::JumpAction > std::uint32_t(Cue::SoundTrigger) && cue::ZipperTrick > cue::JumpAction);

void fillRaceAudioFrame(RaceAudioFrame& f, snd::Vec3f eye, snd::Vec3f target) {
 f.eye = eye; f.target = target;
 const auto* manager = K::Kart::KartObjectManager::Instance();
 const auto& scenario = K::System::RaceConfig::Instance()->raceScenario();
 f.mirror=scenario.mirror;
 f.karts = std::uint8_t(std::min<unsigned>(scenario.playerCount, 12));
 for (unsigned i = 0; i < f.karts; ++i) {
  RaceAudioKart& k = f.kart[i];
  auto* kart = manager->object(i);
  const auto& player = scenario.players[i];
  k.present = kart != nullptr;
  if (!kart) continue;
  const auto p = kart->pos();
  k.position = {p.x, p.y, p.z};
  const auto& v = kart->dynamics()->velocity();
  k.velocity = {v.x, v.y, v.z};
  auto* move = kart->move();
  k.speed = move->speed(); k.speedRatioCapped = move->speedRatioCapped(); k.baseSpeed = move->baseSpeed();
  k.startBoostCharge = kart->state()->startBoostCharge();
  k.canWheelie = move->canWheelie();
  // A half-pipe trick start (retail fn_1_64AAC, Kinoko KartHalfPipe::activateTrick
  // setting ZipperTrick): Kinoko cues no sound there, so take the rising edge.
  const bool zipperTrick = kart->status().onBit(K::Kart::eStatus::ZipperTrick);
  if (zipperTrick && !(k.status[47 / 32] & (1u << (47 % 32)))) k.cues |= cue::ZipperTrick;
  // The same entry edge as KartItemEffects::WaterFall. Static pocha
  // surfaces use the captured collision slot; Psea uses Diving. A plain
  // cliff fall must not gain a water sound merely from its fall voice.
  const bool beforeRespawn = kart->status().onBit(K::Kart::eStatus::BeforeRespawn);
  if (beforeRespawn && !(k.status[0] & (1u << 4))) {
   const auto* objects = K::Field::ObjectDirector::Instance();
   bool water = objects && objects->psea() && kart->status().onBit(K::Kart::eStatus::Diving);
   if (kart->collide()->oobWaterHeightSlot() >= 0)
    if (const auto* map = K::System::CourseMap::Instance())
     for (std::uint16_t j = 0; j < map->getGeoObjCount(); ++j) {
      const auto* object = map->getGeoObj(j);
      if (object->id() == 7 && (object->presenceFlag() & 1)) { water = true; break; }
     }
   if (water) k.cues |= cue::WaterSplash;
   if (kart->collide()->oobLavaHeightSlot() >= 0)
    if (const auto* map = K::System::CourseMap::Instance())
     for (std::uint16_t j = 0; j < map->getGeoObjCount(); ++j) {
      const auto* object = map->getGeoObj(j);
      if (object->id() == 17 && (object->presenceFlag() & 1)) { k.cues |= cue::LavaSplash; break; }
     }
   // EffectsMgr +0x16: Rainbow's fall effect needs the course's "entry" object.
   if (kart->collide()->rainbowFall())
    if (const auto* map = K::System::CourseMap::Instance())
     for (std::uint16_t j = 0; j < map->getGeoObjCount(); ++j) {
      const auto* object = map->getGeoObj(j);
      if (object->id() == 0x12 && (object->presenceFlag() & 1)) { k.rainbowFall = true; break; }
     }
  }
  std::memcpy(k.status, &kart->status(), sizeof k.status);
  k.moveFlags = move->ssmtCharged() ? 4 : 0;
  k.driftState = std::uint8_t(move->driftState());
  k.wheels = std::uint8_t(std::min<unsigned>(kart->tireCount(), 4));
  for (unsigned w = 0; w < k.wheels; ++w) {
   const auto& c = kart->tirePhysics(std::uint16_t(w))->hitboxGroup()->collisionData();
   const std::uint32_t mask = std::uint32_t(c.closestFloorFlags);
   std::uint8_t type = 0;
   for (int b = 0; b < 32; ++b) if (mask & (1u << b)) { type = std::uint8_t(b); break; }
   k.floorType[w] = type; k.floorVariant[w] = std::uint16_t(c.closestFloorSettings);
  }
  // The body's wall contact as Effects::Player::UpdateFromKartCollision sees it.
  const auto& body = kart->collisionData();
  k.wallType = std::uint8_t(body.closestWallFlags); k.wallVariant = std::uint8_t(body.closestWallSettings);
  k.wallContact = (body.bWall || body.bWall3) && !body.bInvisibleWallOnly;
  const auto taken = move->takeSoundCues();
  k.cues |= taken.cues;
  k.bumpedBy |= taken.bumpedBy;
  k.powerObjectContact |= taken.powerObjectContact;
  if (taken.cues & cue::Bump) k.bump = taken.bump;
  if (taken.cues & cue::Fall) k.fallVoice = taken.fallVoice;
  if (taken.cues & cue::SoundTrigger) k.soundZone = taken.soundZone;
  k.airtime = std::uint16_t(std::min<std::uint32_t>(kart->state()->airtime(), 0xffff));
  k.slipCharging = move->slipstreamCharging(); k.slipBoost = move->slipstreamBoost();
  k.actionCount = kart->action()->startCount(); k.action = std::int8_t(kart->action()->lastStarted());
  if (const auto* race = K::System::RaceManager::Instance()) {   // one RaceManager player per scenario racer
   const auto& p = race->player(i);
   k.finished = p.finished(); k.rank = std::uint8_t(p.finishOrder());
  }
  k.vehicle = std::uint8_t(player.vehicle); k.character = std::uint8_t(player.character);
  k.local = player.type == K::System::RaceConfig::Player::Type::Local;
 }
}

void fillRaceAudioItems(RaceAudioFrame& f, const RaceItems& items) {
 f.items = 0;
 const auto& actors = items.actors();
 items.forEachActorSlot([&](unsigned i) {
  if (f.items >= f.item.size()) return false;
  const ItemActor& a = actors[i];
  if (!a.active) return true;
  RaceAudioItem& o = f.item[f.items++];
  const Vec3 p = a.visualPosition();
  o.position = {p.x, p.y, p.z}; o.index = std::uint8_t(i); o.kind = std::uint8_t(a.kind);
  o.object = std::uint8_t(items::retailObjectId(a.kind));
  o.phase = a.guidance.phase; o.target = a.target;
  o.explosion = a.explosion; o.scattered = a.scattered; o.settled = a.settled;
  return true;
 });
 for (const ItemSound& s : items.sounds()) {
  if (f.itemSounds >= f.itemSound.size()) break;
  RaceAudioItemSound& o = f.itemSound[f.itemSounds++];
  o.position = {s.position.x, s.position.y, s.position.z}; o.cue = std::uint8_t(s.cue);
  o.player = s.player; o.actor = s.actor; o.kind = std::uint8_t(s.kind); o.flag = s.flag; o.value = s.value;
  o.object = std::uint8_t(items::retailObjectId(s.kind));
 }
 f.roulette = 0;
 for (unsigned i = 0; i < 12 && i < f.karts; ++i) {
  const ItemHud& h = items.hud(i);
  if (h.roulette) f.roulette |= std::uint16_t(1u << i);
  const bool spin = h.deployed && (h.heldKind == ItemKind::TripleGreenShell || h.heldKind == ItemKind::TripleRedShell);
  f.orbit[i] = spin ? h.heldCharges : 0;
  f.cloud[i] = h.cloud;
 }
}

namespace {
// Kinoko objects whose ObjFlow entry gives them a sound actor; a slot is the
// object's index here for the rest of the race.
struct ObjectRegistry {
 std::size_t scanned = 0, scannedDrivable = 0;
 std::uint8_t count = 0;
 std::array<const K::Field::ObjectBase*, 128> object{};
 std::array<std::uint8_t, 128> mode{}, index{};
 int find(const K::Field::ObjectBase* o) const { for (int i = 0; i < count; ++i) if (object[i] == o) return i; return -1; }
 int add(const K::Field::ObjectBase* o, int m) {
  if (count >= object.size()) return -1;
  std::uint8_t same = 0;   // the object's index among those of its kind (bats alternate by it)
  for (int i = 0; i < count; ++i) same += object[i]->id() == o->id();
  object[count] = o; mode[count] = std::uint8_t(m); index[count] = same;
  return count++;
 }
 // Placed sound objects: their registry slots and sounds, found once as the objects are scanned.
 std::uint8_t placedCount = 0;
 std::array<std::uint8_t, 16> placedSlot{};
 std::array<std::uint16_t, 16> placedSoundId{};
 std::array<bool, 128> placed{};
};
ObjectRegistry registry;

// Placed sound objects (sound_river, sound_sea, sound_sand_fall...): retail class USA
// 0x806ed4f4 owns a route sound actor (0x80715e88). Its init (0x807160f4) picks the
// sound by object id (table 0x808c49d8; sound_sea on courses 16 and 23 plays 866 and
// 897), and every frame (0x80716208) it moves to the point of the object's route
// nearest to the listener and holds the sound there (not 866 in Time Trial).
std::uint16_t placedSound(std::uint16_t id, unsigned course) {
 static constexpr std::uint16_t table[][2] = {{0x5, 0x21d}, {0x6, 0x21f}, {0x8, 0x221}, {0x9, 0x220},
  {0xa, 0x222}, {0xb, 0x223}, {0xc, 0x226}, {0xd, 0x224}, {0xe, 0x21e}, {0xf, 0x2cd}, {0x10, 0x2b4},
  {0x15, 0x225}};
 for (const auto& e : table) {
  if (e[0] != id) continue;
  if (e[1] == 546 && course == 16) return 866;
  if (e[1] == 546 && course == 23) return 897;
  return e[1];
 }
 return 0;
}
// Port: the nearest point of the route's polyline (retail 0x806b02d4 measures along
// the route's own interpolation); a placement without a route stays where it is.
K::EGG::Vector3f routePoint(const K::System::MapdataGeoObj& geo, const K::EGG::Vector3f& at, const K::EGG::Vector3f& fallback) {
 const auto* map = K::System::CourseMap::Instance();
 const int path = geo.pathId();
 if (!map || path < 0 || path >= int(map->getPointInfoCount())) return fallback;
 const auto* info = map->getPointInfo(path);
 if (!info || info->pointCount() == 0) return fallback;
 const auto& pts = info->points();
 K::EGG::Vector3f best = pts[0].pos;
 float bestD = (best - at).squaredLength();
 for (std::size_t i = 0; i + 1 < info->pointCount(); ++i) {
  const auto a = pts[i].pos, d = pts[i + 1].pos - a;
  const float n = d.dot(d);
  float t = n > 0.0f ? (at - a).dot(d) / n : 0.0f;
  t = std::clamp(t, 0.0f, 1.0f);
  const auto p = a + d * t;
  const float dist = (p - at).squaredLength();
  if (dist < bestD) { bestD = dist; best = p; }
 }
 return best;
}
}

void resetRaceAudioObjects() {
 registry = ObjectRegistry{};
 K::Field::ObjectBase::SoundCue drop[96];
 while (K::Field::ObjectBase::TakeSoundCues(drop, 96) == 96) {}
}

void fillRaceAudioObjects(RaceAudioFrame& f) {
 const auto* dir = K::Field::ObjectDirector::Instance();
 if (!dir) { f.objects = 0; return; }
 // Both directors: drivable objects (escalators, belts, bridges) live apart.
 const auto& objects = dir->objects();
 const auto* drivables = K::Field::ObjectDrivableDirector::Instance();
 const std::size_t drivableCount = drivables ? drivables->objects().size() : 0;
 if (objects.size() < registry.scanned || drivableCount < registry.scannedDrivable) registry = ObjectRegistry{};
 auto scan = [&](const K::Field::ObjectBase* o) {
  const int m = dir->flowTable().soundMode(o->id());
  if ((m == 1 || m == 3) && registry.find(o) < 0) registry.add(o, m);
 };
 const std::size_t scannedBefore = registry.scanned;
 for (std::size_t i = registry.scanned; i < objects.size(); ++i) scan(objects[i]);
 for (std::size_t i = registry.scannedDrivable; i < drivableCount; ++i) scan(drivables->objects()[i]);
 registry.scanned = objects.size();
 registry.scannedDrivable = drivableCount;
 // Placed sound objects: registered after the course objects' scan, one held cue a frame.
 const auto& scenario = K::System::RaceConfig::Instance()->raceScenario();
 const unsigned course = unsigned(scenario.course);
 if (placedObjectSounds && scannedBefore < objects.size()) {
  const bool timeTrial = scenario.gameMode == K::System::RaceConfig::Scenario::GameMode::Time_Trial;
  const unsigned placedBefore = registry.placedCount;
  for (std::size_t i = scannedBefore; i < objects.size(); ++i) {
   const auto* o = objects[i];
   const std::uint16_t sound = o->mapObj() ? placedSound(std::uint16_t(o->id()), course) : 0;
   if (!sound || (timeTrial && sound == 866) || registry.placedCount >= registry.placedSlot.size()) continue;
   int slot = registry.find(o);
   if (slot < 0) slot = registry.add(o, 1);
   if (slot < 0) continue;
   registry.placed[unsigned(slot)] = true;
   registry.placedSlot[registry.placedCount] = std::uint8_t(slot); registry.placedSoundId[registry.placedCount++] = sound;
  }
#ifndef MKW_RELEASE
  // DIAG: the placed sounds this race holds (once, as they are found).
  if (registry.placedCount != placedBefore) if (std::FILE* log = std::fopen("sdmc:/3ds/MKWiiDS/placed-sounds.txt", "a")) {
   std::fprintf(log, "course=%u mode=%u placed=%u:", course, unsigned(scenario.gameMode), unsigned(registry.placedCount));
   for (unsigned k = placedBefore; k < registry.placedCount; ++k) {
    const auto* o = registry.object[registry.placedSlot[k]];
    std::fprintf(log, " id=0x%x sound=%u path=%d", unsigned(o->id()), unsigned(registry.placedSoundId[k]), int(o->mapObj()->pathId()));
   }
   std::fputc('\n', log); std::fclose(log);
  }
#endif
 }
 for (unsigned k = 0; k < registry.placedCount && f.objectSounds < f.objectSound.size(); ++k)
  f.objectSound[f.objectSounds++] = {std::uint16_t(registry.placedSlot[k]), registry.placedSoundId[k], std::uint8_t(RaceAudioObjectSound::Hold), 0.0f, 1.0f};
 K::Field::ObjectBase::SoundCue cues[96];
 const std::size_t n = K::Field::ObjectBase::TakeSoundCues(cues, 96);
 for (std::size_t i = 0; i < n; ++i) {
  const auto& c = cues[i];
  int slot = registry.find(c.object);
  if (slot < 0) { const int m = dir->flowTable().soundMode(c.object->id()); slot = registry.add(c.object, m == 3 ? 3 : 1); }
  if (slot < 0 || f.objectSounds >= f.objectSound.size()) continue;
  f.objectSound[f.objectSounds++] = {std::uint16_t(slot), c.sound, std::uint8_t(c.call), c.value, c.animationPitch};
 }
 f.objects = registry.count;
 for (int i = 0; i < registry.count; ++i) {
  const auto* o = registry.object[i];
  auto p = o->getPosition();
  if (registry.placed[unsigned(i)]) {
   const K::EGG::Vector3f eye(f.eye.x, f.eye.y, f.eye.z), target(f.target.x, f.target.y, f.target.z);
   p = routePoint(*o->mapObj(), eye + (target - eye) * 0.99f, p);
  }
  f.object[i] = {{p.x, p.y, p.z}, o->scale().x, std::uint16_t(o->id()), registry.mode[i], registry.index[i]};
 }
}
}
