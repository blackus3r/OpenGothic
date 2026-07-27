#include "runtimetest.h"

#include <Tempest/Log>
#include <Tempest/SystemApi>

#include <zenkit/DaedalusScript.hh>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

#include "camera.h"
#include "game/gamescript.h"
#include "gothic.h"
#include "graphics/mesh/skeleton.h"
#include "marvin.h"
#include "resources.h"
#include "world/objects/interactive.h"
#include "world/objects/npc.h"
#include "world/waypoint.h"
#include "world/world.h"

using namespace Tempest;

namespace {

constexpr uint64_t IntroDuration      = 1800;
constexpr uint64_t WarmupTimeout      = 7000;
constexpr uint64_t HealSampleInterval = 900;
constexpr uint32_t HealSampleCount    = 8;
constexpr uint64_t OrcWarmupDuration  = 3000;
constexpr uint64_t OrcObserveDuration = 7000;
constexpr uint64_t OrcNaturalObservationDuration = 1500;
constexpr uint64_t SleepObserveDuration = 45000;
constexpr uint64_t SleepSampleInterval  = 100;
constexpr uint64_t ResultDuration     = 1800;
constexpr uint64_t StatusRefresh      = 1100;
constexpr float    MeleeDistance      = 150.f;
constexpr float    OrcDistance        = 271.f;
constexpr float    FixtureIsolationRadius = 3000.f;
constexpr float    NpcDetectionRadius     = 3000.f;
constexpr float    NpcQuarantineRadius    = 6000.f;
constexpr float    NpcQuarantineDistance  = 9000.f;

const std::vector<std::vector<std::string_view>> healCandidates = {
  {"WOLF"},
  {"BLOODFLY"},
  {"GOBBO_GREEN", "GOBBO_BLACK", "GOBBO"},
  };

const std::vector<std::string_view> orcCandidates = {
  "ORCWARRIOR_ROAM",
  "ORCWARRIOR",
  "ORC_WARRIOR",
  "ORCELITE_ROAM",
  "ORCELITE",
  };

std::string boolText(bool value) {
  return value ? "true" : "false";
  }

std::string passText(bool value) {
  return value ? "PASS" : "FAIL";
  }

float length(const Vec3& value) {
  return std::sqrt(value.x*value.x + value.y*value.y + value.z*value.z);
  }

}

bool RuntimeTest::HealResult::passed() const {
  return spawned && combatReached && assessDamageCallback=="B_MM_ASSESSDAMAGE" &&
         attackAnimationSeen && samples==HealSampleCount &&
         fightSamples==HealSampleCount && callbackInvocations==HealSampleCount &&
         healEvents==0 && frontFovFalse==0 && freeLosFalse==0;
  }

bool RuntimeTest::OrcResult::passed() const {
  const bool coneIsolation = !seeOnly || initialSenseCone==int32_t(SensesBit::SENSE_NONE);
  const bool expectsRegularPerception =
      (!assessPlayerCallback.empty() && initialSenseCone!=int32_t(SensesBit::SENSE_NONE)) ||
      (!assessEnemyCallback.empty()  && initialSenseFree!=int32_t(SensesBit::SENSE_NONE));
  const bool enginePerceptionObserved =
      fightStateReachedBeforeProbe || initialRegularPerceptionInvoked;
  return spawned && initiallyBehind && !initialFov && initialFreeLos &&
         coneIsolation && nearbyOtherNpcs==0 &&
         initialRegularPerceptionInvoked==expectsRegularPerception &&
         enginePerceptionObserved && selectorAcquiredPlayer && turned;
  }

bool RuntimeTest::SleepPlacementResult::passed() const {
  return found && gotoBedStateSeen && sleepStateSeen && bedFixtureSeen &&
         bedAttached && liePoseSeen && locomotionSeen && doorClassSeen &&
         !doorSemanticsSeen &&
         maxAttachmentGroundDelta>50.f && samples>0 && attachedSamples>=10 &&
         settledSamples>=10 &&
         baseFloatingSamples==0 && rootFloatingSamples==0 &&
         rootBelowSupportSamples==0 && misplacedSamples==0 &&
         horizontalLocomotionSamples==0;
  }

RuntimeTest::RuntimeTest(Gothic& owner, std::string_view name, std::string_view output)
  :owner(owner), marvin(std::make_unique<Marvin>()), testName(name), outputPath(output) {
  if(name=="enemy-heal-combat")
    mode = Mode::EnemyHeal;
  else if(name=="orc-behind-detection")
    mode = Mode::OrcBehind;
  else if(name=="npc-sleep-placement")
    mode = Mode::NpcSleepPlacement;

  if(outputPath.empty())
    outputPath = std::string(name) + ".json";

  Log::e("[RUNTIME_TEST] requested=",testName," output=",outputPath);
  }

RuntimeTest::~RuntimeTest() = default;

void RuntimeTest::tick(uint64_t dt) {
  auto* activeWorld  = owner.world();
  auto* activePlayer = owner.player();
  if(activeWorld==nullptr || activePlayer==nullptr)
    return;

  if(!initialized || world!=activeWorld || player!=activePlayer)
    initialize(*activeWorld,*activePlayer);

  phaseTime += dt;
  if(nextMessage<=phaseTime) {
    showStatus();
    nextMessage = phaseTime+StatusRefresh;
    }

  switch(mode) {
    case Mode::EnemyHeal:
      tickEnemyHeal(dt);
      break;
    case Mode::OrcBehind:
      tickOrcBehind(dt);
      break;
    case Mode::NpcSleepPlacement:
      tickNpcSleepPlacement(dt);
      break;
    case Mode::Invalid:
      if(phase!=Phase::Done) {
        Log::e("[RUNTIME_TEST] unknown test: ",testName);
        finish();
        }
      break;
    }
  }

void RuntimeTest::initialize(World& activeWorld, Npc& activePlayer) {
  removeSubject();
  if(mode==Mode::NpcSleepPlacement && player!=nullptr)
    player->physic.setEnable(true);
  world       = &activeWorld;
  player      = &activePlayer;
  initialized = true;
  caseIndex   = 0;
  healResults.clear();
  orcResults.clear();
  sleepSubject = nullptr;
  sleepResult = SleepPlacementResult();
  sleepLastSignature.clear();
  resultPass  = false;
  fixtureQuarantinedNpcCount = 0;

  initialAnchor = player->position();
  anchor = initialAnchor;
  if(!owner.isGodMode()) {
    const bool enabled = marvin->exec("cheat god");
    Log::e("[RUNTIME_TEST] marvin command `cheat god`: ",enabled ? "accepted" : "rejected");
    }

  if(mode==Mode::NpcSleepPlacement) {
    // The detached test camera does not need a physical player body. Keeping it disabled
    // prevents the save-specific player position from blocking Brian's route to the bed.
    player->physic.setEnable(false);
    sleepSubject = findNpc("VLK_457_BRIAN","Brian");
    sleepResult.found = sleepSubject!=nullptr;
    if(sleepSubject!=nullptr) {
      if(const auto* symbol = world->script().findSymbol(sleepSubject->handle().symbol_index()))
        sleepResult.instance = symbol->name();
      frameSleepCamera();
      }
    fixtureClear = sleepResult.found;
    fixtureScore = sleepResult.found ? 1 : 0;
    fixtureScoreMax = 1;
    fixtureNpcCount = 0;
    fixtureNpcClearance = sleepResult.found ?
                          length(sleepSubject->position()-player->position()) : 0.f;
    enter(Phase::Intro);
    Log::e("[RUNTIME_TEST] world-ready name=",activeWorld.name(),
           " player=(",anchor.x,",",anchor.y,",",anchor.z,")",
           " sleepSubject=",sleepResult.instance,
           " god=",boolText(owner.isGodMode()));
    return;
    }

  const float angle = player->rotationRad();
  fixtureClear = chooseClearFixture(initialAnchor,angle);
  if(fixtureClear)
    quarantineNearbyNpcs();

  player->setPosition(anchor);
  player->setDirection(forward);
  player->setTarget(nullptr);
  player->updateTransform();
  if(auto* camera = owner.camera())
    camera->reset(player);

  enter(Phase::Intro);
  Log::e("[RUNTIME_TEST] world-ready name=",activeWorld.name(),
         " player=(",anchor.x,",",anchor.y,",",anchor.z,")",
         " clearDirection=(",forward.x,",",forward.z,")",
         " god=",boolText(owner.isGodMode()));
  }

void RuntimeTest::tickEnemyHeal(uint64_t) {
  switch(phase) {
    case Phase::Intro:
      if(phaseTime>=IntroDuration)
        beginHealCase();
      break;
    case Phase::Warmup: {
      if(subject==nullptr) {
        enter(Phase::Result);
        break;
        }

      currentHeal.attackAnimationSeen |= subject->isAttackAnim();
      currentHeal.combatReached       |= world->script().isAttack(*subject);

      if(nextAction<=phaseTime) {
        subject->setTarget(player);
        subject->perceptionProcess(*player);
        nextAction = phaseTime+250;
        }

      if(!currentHeal.combatReached && !currentHeal.fightStateForced && phaseTime>=2000) {
        const ScriptFn fightState(world->script().findSymbolIndex("ZS_MM_ATTACK"));
        subject->setOther(player);
        subject->setTarget(player);
        currentHeal.fightStateForced = subject->startState(fightState,"");
        Log::e("[RUNTIME_TEST] deterministic FightAI fixture instance=",currentHeal.instance,
               " state=ZS_MM_ATTACK accepted=",boolText(currentHeal.fightStateForced));
        }

      if(currentHeal.combatReached) {
        const int32_t hpMax   = subject->attribute(ATR_HITPOINTSMAX);
        const int32_t desired = std::max<int32_t>(1,hpMax/2);
        subject->changeAttribute(ATR_HITPOINTS,desired-subject->attribute(ATR_HITPOINTS),false);
        enter(Phase::Observe);
        }
      else if(phaseTime>=WarmupTimeout) {
        Log::e("[RUNTIME_TEST] heal case ",currentHeal.requested,
               " failed to enter FightAI within ",WarmupTimeout,"ms");
        enter(Phase::Result);
        }
      break;
      }
    case Phase::Observe:
      currentHeal.attackAnimationSeen |= subject!=nullptr && subject->isAttackAnim();
      currentHeal.combatReached       |= subject!=nullptr && world->script().isAttack(*subject);
      if(nextAction<=phaseTime)
        sampleHealCase();
      if(currentHeal.samples>=HealSampleCount)
        enter(Phase::Result);
      break;
    case Phase::Result:
      if(phaseTime>=ResultDuration)
        finishHealCase();
      break;
    case Phase::Done:
      if(phaseTime>=ResultDuration)
        SystemApi::exit();
      break;
    }
  }

void RuntimeTest::beginHealCase() {
  currentHeal = HealResult();
  currentHeal.requested = std::string(healCandidates[caseIndex][0]);
  subject = insertNpc(healCandidates[caseIndex],currentHeal.instance);
  currentHeal.spawned = subject!=nullptr;

  if(subject==nullptr) {
    Log::e("[RUNTIME_TEST] unable to insert heal-test NPC requested=",currentHeal.requested);
    enter(Phase::Result);
    return;
    }

  currentHeal.displayName = std::string(subject->displayName());
  player->setPosition(anchor);
  player->setDirection(forward);
  player->setTarget(subject);
  player->updateTransform();

  subject->setPosition(anchor + forward*MeleeDistance);
  subject->setDirection(forward*-1.f);
  subject->setAttitude(ATT_HOSTILE);
  subject->setTempAttitude(ATT_HOSTILE);
  subject->setTarget(player);
  subject->clearNearestEnemy();
  subject->updateTransform();

  frameSubjectCamera();

  Log::e("[RUNTIME_TEST] heal case start requested=",currentHeal.requested,
         " instance=",currentHeal.instance," display=",currentHeal.displayName);
  enter(Phase::Warmup);
  }

void RuntimeTest::sampleHealCase() {
  nextAction = phaseTime+HealSampleInterval;
  if(subject==nullptr)
    return;

  quarantineNearbyNpcs();

  // The controlled geometry keeps the logical heading fixed on the player while the model and
  // head bone continue their real combat animations. This reproduces the wolf and bloodfly bugs
  // without accepting a legitimate turn-away as a false FOV result.
  subject->setPosition(player->position() + forward*MeleeDistance);
  subject->setDirection(forward*-1.f);
  subject->setTarget(player);
  subject->updateTransform();

  const bool fov     = subject->canSeeNpc(*player,false);
  const bool freeLos = subject->canSeeNpc(*player,true);
  const bool legacyFov = legacyCanSeeNpc(*subject,*player);
  const bool inFight = world->script().isAttack(*subject);
  if(inFight)
    ++currentHeal.fightSamples;
  if(!fov)
    ++currentHeal.frontFovFalse;
  if(!freeLos)
    ++currentHeal.freeLosFalse;
  if(!legacyFov)
    ++currentHeal.legacyFovFalse;

  const float headOffset = length(subject->mapHeadBone()-subject->position());
  const float distance   = length(subject->position()-player->position());
  currentHeal.maxHeadOffset = std::max(currentHeal.maxHeadOffset,headOffset);
  currentHeal.maxDistance   = std::max(currentHeal.maxDistance,distance);

  const ScriptFn callback = subject->perception[PERC_ASSESSDAMAGE].func;
  if(callback.isValid()) {
    if(auto* symbol = world->script().findSymbol(callback.ptr))
      currentHeal.assessDamageCallback = symbol->name();
    }

  const int32_t hpMax   = subject->attribute(ATR_HITPOINTSMAX);
  const int32_t desired = std::max<int32_t>(1,hpMax/2);
  subject->changeAttribute(ATR_HITPOINTS,desired-subject->attribute(ATR_HITPOINTS),false);
  const int32_t before = subject->attribute(ATR_HITPOINTS);
  const bool invoked = subject->perceptionProcess(*player,subject,0.f,PERC_ASSESSDAMAGE);
  if(invoked)
    ++currentHeal.callbackInvocations;
  const int32_t after = subject->attribute(ATR_HITPOINTS);
  const int32_t delta = after-before;

  ++currentHeal.samples;
  currentHeal.hpDeltas.push_back(delta);
  if(delta>0)
    ++currentHeal.healEvents;

  Log::e("[RUNTIME_TEST] heal sample instance=",currentHeal.instance,
         " n=",currentHeal.samples,
         " fight=",boolText(inFight),
         " callback=",currentHeal.assessDamageCallback,
         " invoked=",boolText(invoked),
         " fov=",boolText(fov),
         " legacyFov=",boolText(legacyFov),
         " freeLos=",boolText(freeLos),
         " hp=",before,"->",after,
         " headOffset=",headOffset,
         " distance=",distance);
  }

void RuntimeTest::finishHealCase() {
  Log::e("[RUNTIME_TEST] heal case result instance=",currentHeal.instance,
         " result=",passText(currentHeal.passed()),
         " combat=",boolText(currentHeal.combatReached),
         " stateForced=",boolText(currentHeal.fightStateForced),
         " callback=",currentHeal.assessDamageCallback,
         " samples=",currentHeal.samples,
         " fightSamples=",currentHeal.fightSamples,
         " callbackInvocations=",currentHeal.callbackInvocations,
         " heals=",currentHeal.healEvents,
         " frontFovFalse=",currentHeal.frontFovFalse,
         " legacyFovFalse=",currentHeal.legacyFovFalse);
  healResults.push_back(currentHeal);
  removeSubject();
  ++caseIndex;
  if(caseIndex<healCandidates.size()) {
    beginHealCase();
    return;
    }
  finish();
  }

void RuntimeTest::tickOrcBehind(uint64_t) {
  switch(phase) {
    case Phase::Intro:
      if(phaseTime>=IntroDuration)
        beginOrcCase(false);
      break;
    case Phase::Warmup:
      if(subject==nullptr) {
        enter(Phase::Result);
        break;
        }
      // Far AI may restore routine positions. Reapply isolation every frame so no actor can
      // become a one-frame perception target during the pre-probe observation window.
      quarantineNearbyNpcs();
      currentOrc.nearbyOtherNpcs =
          std::max(currentOrc.nearbyOtherNpcs,
                   countOtherNpcs(player->position(),NpcDetectionRadius,subject));
      if(phaseTime>=OrcWarmupDuration) {
        prepareOrcCase();
        enter(Phase::Observe);
        }
      break;
    case Phase::Observe:
      if(nextAction<=phaseTime) {
        sampleOrcCase();
        nextAction = phaseTime+250;
        }
      if(phaseTime>=OrcObserveDuration)
        enter(Phase::Result);
      break;
    case Phase::Result:
      if(phaseTime>=ResultDuration)
        finishOrcCase();
      break;
    case Phase::Done:
      if(phaseTime>=ResultDuration)
        SystemApi::exit();
      break;
    }
  }

void RuntimeTest::beginOrcCase(bool seeOnly) {
  currentOrc = OrcResult();
  currentOrc.variant = seeOnly ? "controlled-see-only" : "stock-senses";
  currentOrc.seeOnly = seeOnly;
  subject = insertNpc(orcCandidates,currentOrc.instance);
  currentOrc.spawned = subject!=nullptr;

  if(subject==nullptr) {
    Log::e("[RUNTIME_TEST] unable to insert Orc Warrior");
    enter(Phase::Result);
    return;
    }

  player->setPosition(anchor);
  player->setDirection(forward);
  player->setTarget(nullptr);
  player->updateTransform();

  subject->setPosition(anchor + forward*OrcDistance);
  subject->setDirection(forward);
  subject->setAttitude(ATT_HOSTILE);
  subject->setTempAttitude(ATT_HOSTILE);
  subject->setTarget(nullptr);
  subject->clearNearestEnemy();
  if(seeOnly)
    subject->handle().senses = int32_t(SensesBit::SENSE_SEE);
  subject->updateTransform();

  frameSubjectCamera();
  quarantineNearbyNpcs();
  Log::e("[RUNTIME_TEST] orc warmup variant=",currentOrc.variant,
         " instance=",currentOrc.instance);
  enter(Phase::Warmup);
  }

void RuntimeTest::prepareOrcCase() {
  const float warmupTurn = std::abs(subject->rotation()-player->rotation());
  const float wrappedWarmupTurn = std::min(warmupTurn,360.f-warmupTurn);
  currentOrc.targetAcquiredBeforeProbe = subject->target()==player;
  currentOrc.fightStateReachedBeforeProbe = world->script().isAttack(*subject);
  currentOrc.turnObservedBeforeProbe = wrappedWarmupTurn>=30.f;
  currentOrc.fightStateReached = currentOrc.fightStateReachedBeforeProbe;

  player->setPosition(anchor);
  player->setDirection(forward);
  player->setTarget(nullptr);
  player->updateTransform();

  subject->setPosition(anchor + forward*OrcDistance);
  subject->setDirection(forward);
  subject->setAttitude(ATT_HOSTILE);
  subject->setTempAttitude(ATT_HOSTILE);
  if(currentOrc.seeOnly)
    subject->handle().senses = int32_t(SensesBit::SENSE_SEE);
  subject->updateTransform();

  frameSubjectCamera();
  quarantineNearbyNpcs();
  currentOrc.sensesMask       = subject->handle().senses;
  const ScriptFn playerCallback = subject->perception[PERC_ASSESSPLAYER].func;
  if(playerCallback.isValid()) {
    if(auto* symbol = world->script().findSymbol(playerCallback.ptr))
      currentOrc.assessPlayerCallback = symbol->name();
    }
  const ScriptFn enemyCallback = subject->perception[PERC_ASSESSENEMY].func;
  if(enemyCallback.isValid()) {
    if(auto* symbol = world->script().findSymbol(enemyCallback.ptr))
      currentOrc.assessEnemyCallback = symbol->name();
    }
  currentOrc.initialAngle     = relativeAngle(*subject,*player);
  currentOrc.initiallyBehind  = currentOrc.initialAngle>=170.f;
  currentOrc.initialFov       = subject->canSeeNpc(*player,false);
  currentOrc.initialFreeLos   = subject->canSeeNpc(*player,true);
  currentOrc.initialSenseCone = int32_t(subject->canSenseNpc(*player,false));
  currentOrc.initialSenseFree = int32_t(subject->canSenseNpc(*player,true));
  currentOrc.nearbyOtherNpcs =
      std::max(currentOrc.nearbyOtherNpcs,
               countOtherNpcs(player->position(),NpcDetectionRadius,subject));
  currentOrc.nearestOtherNpc =
      nearestOtherNpcDistance(player->position(),subject);
  if(currentOrc.nearbyOtherNpcs!=0) {
    world->detectNpc(player->position(),NpcDetectionRadius,[&](Npc& npc) {
      if(&npc==player || &npc==subject)
        return;
      const auto* symbol = world->script().findSymbol(npc.handle().symbol_index());
      const auto position = npc.position();
      Log::e("[RUNTIME_TEST] unrelated NPC in Orc fixture instance=",
             symbol!=nullptr ? symbol->name() : std::string_view("?"),
             " display=",npc.displayName(),
             " position=(",position.x,",",position.y,",",position.z,")");
      });
    }

  // Run the real top-level perception entry point exactly once while the measured 180-degree
  // geometry is still intact. This distinguishes an installed PERC_ASSESSPLAYER/SMELL path from
  // an installed PERC_ASSESSENEMY/free-LOS path without later turning contaminating the result.
  currentOrc.initialRegularPerceptionInvoked = subject->perceptionProcess(*player);
  currentOrc.fightStateReached |= world->script().isAttack(*subject);

  Log::e("[RUNTIME_TEST] orc case start variant=",currentOrc.variant,
         " instance=",currentOrc.instance,
         " angle=",currentOrc.initialAngle,
         " fov=",boolText(currentOrc.initialFov),
         " freeLos=",boolText(currentOrc.initialFreeLos),
         " senseCone=",currentOrc.initialSenseCone,
         " senseFree=",currentOrc.initialSenseFree,
         " sensesMask=",currentOrc.sensesMask,
         " assessPlayer=",currentOrc.assessPlayerCallback,
         " assessEnemy=",currentOrc.assessEnemyCallback,
         " regularPerception=",boolText(currentOrc.initialRegularPerceptionInvoked),
         " targetBeforeProbe=",boolText(currentOrc.targetAcquiredBeforeProbe),
         " fightBeforeProbe=",boolText(currentOrc.fightStateReachedBeforeProbe),
         " turnBeforeProbe=",boolText(currentOrc.turnObservedBeforeProbe),
         " nearbyOtherNpcs=",currentOrc.nearbyOtherNpcs,
         " nearestOtherNpc=",currentOrc.nearestOtherNpc);
  }

void RuntimeTest::sampleOrcCase() {
  if(subject==nullptr)
    return;

  quarantineNearbyNpcs();

  const size_t nearbyOtherNpcs =
      countOtherNpcs(player->position(),NpcDetectionRadius,subject);
  if(nearbyOtherNpcs>currentOrc.nearbyOtherNpcs) {
    world->detectNpc(player->position(),NpcDetectionRadius,[&](Npc& npc) {
      if(&npc==player || &npc==subject)
        return;
      const auto* symbol = world->script().findSymbol(npc.handle().symbol_index());
      const auto position = npc.position();
      Log::e("[RUNTIME_TEST] NPC entered active Orc fixture instance=",
             symbol!=nullptr ? symbol->name() : std::string_view("?"),
             " display=",npc.displayName(),
             " position=(",position.x,",",position.y,",",position.z,")");
      });
    currentOrc.nearbyOtherNpcs = nearbyOtherNpcs;
    }

  // Probe the exact direction-independent selector behind the maintainer's observation as an
  // explicit assertion too. It evaluates canSenseNpc(..., true), bypassing the front cone.
  Npc* detected = subject->updateNearestEnemy();
  if(!currentOrc.selectorAcquiredPlayer && detected==player) {
    currentOrc.selectorAcquiredPlayer = true;
    currentOrc.detectionTimeMs = phaseTime;
    Log::e("[RUNTIME_TEST] updateNearestEnemy selected player variant=",currentOrc.variant,
           " at=",phaseTime,"ms",
           " fov-now=",boolText(subject->canSeeNpc(*player,false)),
           " freeLos-now=",boolText(subject->canSeeNpc(*player,true)));
    }

  if(detected==player) {
    subject->setOther(player);
    subject->setTarget(player);
    if(!world->script().isAttack(*subject) && !currentOrc.fightStateForced &&
       phaseTime>=OrcNaturalObservationDuration) {
      const ScriptFn fightState(world->script().findSymbolIndex("ZS_MM_ATTACK"));
      currentOrc.fightStateForced = subject->startState(fightState,"");
      Log::e("[RUNTIME_TEST] deterministic Orc FightAI fixture variant=",currentOrc.variant,
             " accepted=",boolText(currentOrc.fightStateForced));
      }
    }

  // Also exercise the regular top-level perception entry point used by Npc::tick.
  subject->perceptionProcess(*player);

  const float turn = std::abs(subject->rotation()-player->rotation());
  const float wrappedTurn = std::min(turn,360.f-turn);
  currentOrc.maximumTurnDegrees = std::max(currentOrc.maximumTurnDegrees,wrappedTurn);
  currentOrc.turned |= wrappedTurn>=30.f;

  if(!currentOrc.fightStateReached && world->script().isAttack(*subject)) {
    currentOrc.fightStateReached = true;
    currentOrc.fightStateTimeMs  = phaseTime;
    }
  }

void RuntimeTest::finishOrcCase() {
  Log::e("[RUNTIME_TEST] orc case result variant=",currentOrc.variant,
         " result=",passText(currentOrc.passed()),
         " initialFov=",boolText(currentOrc.initialFov),
         " initialFreeLos=",boolText(currentOrc.initialFreeLos),
         " nearbyOtherNpcs=",currentOrc.nearbyOtherNpcs,
         " regularPerception=",boolText(currentOrc.initialRegularPerceptionInvoked),
         " targetBeforeProbe=",boolText(currentOrc.targetAcquiredBeforeProbe),
         " selectorAcquired=",boolText(currentOrc.selectorAcquiredPlayer),
         " turned=",boolText(currentOrc.turned),
         " fight=",boolText(currentOrc.fightStateReached),
         " fightBeforeProbe=",boolText(currentOrc.fightStateReachedBeforeProbe),
         " turnBeforeProbe=",boolText(currentOrc.turnObservedBeforeProbe),
         " stateForced=",boolText(currentOrc.fightStateForced));
  orcResults.push_back(currentOrc);
  removeSubject();
  ++caseIndex;
  if(caseIndex==1) {
    beginOrcCase(true);
    return;
    }
  finish();
  }

void RuntimeTest::tickNpcSleepPlacement(uint64_t) {
  switch(phase) {
    case Phase::Intro:
      if(phaseTime>=IntroDuration)
        enter(Phase::Observe);
      break;
    case Phase::Warmup:
      break;
    case Phase::Observe:
      if(nextAction<=phaseTime) {
        sampleSleepPlacement();
        nextAction = phaseTime+SleepSampleInterval;
        }
      if(sleepResult.attachedSamples>=50 || phaseTime>=SleepObserveDuration)
        enter(Phase::Result);
      break;
    case Phase::Result:
      if(phaseTime>=ResultDuration)
        finish();
      break;
    case Phase::Done:
      if(phaseTime>=ResultDuration)
        SystemApi::exit();
      break;
    }
  }

void RuntimeTest::sampleSleepPlacement() {
  if(sleepSubject==nullptr)
    return;

  ++sleepResult.samples;
  frameSleepCamera();

  const auto* stateSymbol = world->script().findSymbol(sleepSubject->aiState.funcIni.ptr);
  const std::string_view state = stateSymbol!=nullptr ? stateSymbol->name() : std::string_view();
  sleepResult.gotoBedStateSeen |= state=="ZS_GOTOBED";
  sleepResult.sleepStateSeen   |= state=="ZS_SLEEP";

  std::string animations;
  bool locomotion = false;
  bool transitionAnim = false;
  for(const auto& layer:sleepSubject->visual.pose().lay) {
    if(layer.seq==nullptr)
      continue;
    if(!animations.empty())
      animations += "+";
    animations += layer.seq->name;
    const auto name = std::string_view(layer.seq->name);
    transitionAnim |= name.rfind("T_",0)==0;
    locomotion |= layer.seq->animCls==Animation::Loop &&
                  (name.find("WALK")!=std::string_view::npos ||
                   name.find("RUN") !=std::string_view::npos);
    }

  Vec3 root = {};
  sleepSubject->visual.pose().rootBone().project(root);
  const Vec3 head = sleepSubject->mapHeadBone();
  const Vec3 rootToHead = head-root;
  const float rootHeadLength = length(rootToHead);
  const float upright = rootHeadLength>0.001f ? rootToHead.y/rootHeadLength : 1.f;
  if(locomotion) {
    sleepResult.locomotionSeen = true;
    sleepResult.minLocomotionUpright =
        std::min(sleepResult.minLocomotionUpright,upright);
    if(upright<0.45f)
      ++sleepResult.horizontalLocomotionSamples;
    }

  auto* interaction = sleepSubject->interactive();
  auto* bed = interaction;
  if(bed==nullptr || bed->schemeName()!="BEDHIGH")
    bed = world->availableMob(*sleepSubject,"BEDHIGH");
  if(bed!=nullptr && bed->schemeName()=="BEDHIGH") {
    sleepResult.bedFixtureSeen = true;
    sleepResult.doorClassSeen |= bed->isDoor();
    sleepResult.doorSemanticsSeen |= bed->isDoorInteraction();
    if(interaction!=bed) {
      const Vec3 attachment = bed->nearestPoint(*sleepSubject);
      const Vec3 grounded = bed->groundedPosition(attachment);
      sleepResult.maxAttachmentGroundDelta =
          std::max(sleepResult.maxAttachmentGroundDelta,
                   std::abs(attachment.y-grounded.y));
      }
    }

  float baseGroundOffset = 0.f;
  float baseAboveSupport = 0.f;
  float rootAboveSupport = 0.f;
  float rootBelowSupport = 0.f;
  float rootHorizontalOffset = 0.f;
  bool attachedBed = false;
  if(interaction!=nullptr && interaction->schemeName()=="BEDHIGH") {
    attachedBed = true;
    sleepResult.bedAttached = true;
    sleepResult.liePoseSeen |= sleepSubject->bodyStateMasked()==BS_LIE;
    ++sleepResult.attachedSamples;

    // The engine grounds the base once in Interactive::setPos and then MoveAlgo::tick returns
    // early for MOBSI, so y stays fixed while the entry animation still moves x/z. Measuring the
    // base against the floor below its final spot would therefore assert behavior the engine
    // never implements. The bug this test guards is the NPC hovering over its support, so both
    // base and root are measured against the bed volume instead.
    const auto* bounds = interaction->bBox();
    const Vec3 grounded = interaction->groundedPosition(sleepSubject->position());
    baseGroundOffset = std::abs(sleepSubject->position().y-grounded.y);
    baseAboveSupport = std::max(sleepSubject->position().y-bounds[1].y,0.f);
    rootAboveSupport = std::max(root.y-bounds[1].y,0.f);
    rootBelowSupport = std::max(bounds[0].y-root.y,0.f);
    rootHorizontalOffset =
        std::max({bounds[0].x-root.x,root.x-bounds[1].x,
                  bounds[0].z-root.z,root.z-bounds[1].z,0.f});
    sleepResult.maxBaseGroundOffset =
        std::max(sleepResult.maxBaseGroundOffset,baseGroundOffset);
    sleepResult.maxRootHorizontalOffset =
        std::max(sleepResult.maxRootHorizontalOffset,rootHorizontalOffset);
    // Entry animations legitimately swing the body over the mattress before it settles, so the
    // hover figures only describe the looping sleep pose. The transition peak is kept separate.
    if(transitionAnim) {
      sleepResult.maxTransitionRootAbove =
          std::max(sleepResult.maxTransitionRootAbove,rootAboveSupport);
      } else {
      ++sleepResult.settledSamples;
      sleepResult.maxBaseAboveSupport =
          std::max(sleepResult.maxBaseAboveSupport,baseAboveSupport);
      sleepResult.maxRootAboveSupport =
          std::max(sleepResult.maxRootAboveSupport,rootAboveSupport);
      sleepResult.maxRootBelowSupport =
          std::max(sleepResult.maxRootBelowSupport,rootBelowSupport);
      if(baseAboveSupport>0.f)
        ++sleepResult.baseFloatingSamples;
      if(rootAboveSupport>0.f)
        ++sleepResult.rootFloatingSamples;
      if(rootBelowSupport>0.f)
        ++sleepResult.rootBelowSupportSamples;
      }
    if(rootHorizontalOffset>20.f)
      ++sleepResult.misplacedSamples;
    }

  std::ostringstream signature;
  signature << state << "|" << animations << "|"
            << (interaction!=nullptr ? interaction->schemeName() : std::string_view());
  if(signature.str()!=sleepLastSignature || sleepResult.samples%10==0 ||
     (locomotion && upright<0.45f) ||
     (attachedBed && (baseAboveSupport>0.f || rootAboveSupport>0.f ||
                      rootBelowSupport>0.f || rootHorizontalOffset>20.f))) {
    // Tempest::Log truncates at 256 characters, so the geometry is reported on its own line.
    Log::e("[RUNTIME_TEST] sleep sample n=",sleepResult.samples,
           " state=",state,
           " animation=",animations,
           " bodyState=",int32_t(sleepSubject->bodyStateMasked()),
           " interaction=",interaction!=nullptr ? interaction->schemeName() : std::string_view(),
           " position=(",sleepSubject->position().x,",",sleepSubject->position().y,",",
           sleepSubject->position().z,")");
    Log::e("[RUNTIME_TEST] sleep geom n=",sleepResult.samples,
           " baseAboveSupport=",baseAboveSupport,
           " rootAboveSupport=",rootAboveSupport,
           " rootBelowSupport=",rootBelowSupport,
           " rootHorizontal=",rootHorizontalOffset,
           " baseGroundOffset=",baseGroundOffset,
           " upright=",upright);
    sleepLastSignature = signature.str();
    }
  }

void RuntimeTest::frameSleepCamera() {
  auto* camera = owner.camera();
  if(camera==nullptr || sleepSubject==nullptr)
    return;

  const float angle = sleepSubject->rotationRad();
  const Vec3 direction = {std::cos(angle),0.f,std::sin(angle)};
  const Vec3 side = {-direction.z,0.f,direction.x};
  const Vec3 target = sleepSubject->position() + Vec3(0.f,100.f,0.f);

  // A bedroom is smaller than the framing offset, so the unclamped viewpoint ends up outside
  // the building and records a wall. Pull in until the subject is actually visible.
  Vec3 origin = target;
  for(float scale:{1.f, 0.7f, 0.5f, 0.35f, 0.25f}) {
    const Vec3 candidate = sleepSubject->position() - direction*(220.f*scale) +
                           side*(300.f*scale) + Vec3(0.f,60.f+130.f*scale,0.f);
    if(!world->physic()->ray(target,candidate).hasCol) {
      origin = candidate;
      break;
      }
    }
  const Vec3 delta = target-origin;
  const float horizontal = Vec2(delta.x,delta.z).length();
  const float pitch = -std::atan2(delta.y,horizontal)*180.f/float(M_PI);
  const float yaw = std::atan2(delta.z,delta.x)*180.f/float(M_PI);

  camera->setFirstPerson(true);
  camera->setMarvinMode(Camera::M_Freeze);
  camera->setPosition(origin);
  camera->setAngles({pitch,yaw});
  }

Npc* RuntimeTest::findNpc(std::string_view instance, std::string_view displayName) const {
  Npc* displayMatch = nullptr;
  for(uint32_t i=0;i<world->npcCount();++i) {
    auto* npc = world->npcById(i);
    if(npc==nullptr)
      continue;
    const auto* symbol = world->script().findSymbol(npc->handle().symbol_index());
    if(symbol!=nullptr && symbol->name()==instance)
      return npc;
    if(displayMatch==nullptr && npc->displayName()==displayName)
      displayMatch = npc;
    }
  return displayMatch;
  }

Npc* RuntimeTest::insertNpc(const std::vector<std::string_view>& candidates, std::string& instance) {
  for(auto name:candidates) {
    if(!isNpcInstance(name))
      continue;

    const uint32_t before = world->npcCount();
    const std::string cmd = std::string("insert ") + std::string(name);
    const bool accepted = marvin->exec(cmd);
    const uint32_t after = world->npcCount();
    Log::e("[RUNTIME_TEST] marvin command `",cmd,"`: ",
           accepted ? "accepted" : "rejected"," npcCount=",before,"->",after);
    if(accepted && after==before+1) {
      instance = std::string(name);
      return world->npcById(after-1);
      }
    }
  return nullptr;
  }

bool RuntimeTest::isNpcInstance(std::string_view name) const {
  auto& script = world->script();
  auto* symbol = script.findSymbol(name);
  if(symbol==nullptr || symbol->type()!=zenkit::DaedalusDataType::INSTANCE ||
     symbol->address()==0 || symbol->parent()==uint32_t(-1))
    return false;

  auto* cls = symbol;
  while(cls!=nullptr && cls->parent()!=uint32_t(-1))
    cls = script.findSymbol(cls->parent());
  return cls!=nullptr && cls->name()=="C_NPC";
  }

void RuntimeTest::removeSubject() {
  if(subject==nullptr || world==nullptr)
    return;
  if(player!=nullptr)
    player->setTarget(nullptr);
  subject->setTarget(nullptr);
  world->removeNpc(*subject);
  subject = nullptr;
  }

void RuntimeTest::frameSubjectCamera() {
  auto* camera = owner.camera();
  if(camera==nullptr || player==nullptr || subject==nullptr)
    return;

  // Keep recorded evidence independent of third-person collision. An elevated view along the
  // validated corridor also looks over non-colliding crops and shrubs that ray tests cannot see.
  const Vec3 origin = player->position() - forward*250.f + Vec3(0.f,320.f,0.f);
  const Vec3 target = subject->position() + Vec3(0.f,110.f,0.f);
  const Vec3 delta  = target-origin;
  const float horizontal = Vec2(delta.x,delta.z).length();
  const float pitch = -std::atan2(delta.y,horizontal)*180.f/float(M_PI);
  const float yaw   =  std::atan2(delta.z,delta.x)*180.f/float(M_PI);

  camera->setFirstPerson(true);
  camera->setMarvinMode(Camera::M_Freeze);
  camera->setPosition(origin);
  camera->setAngles({pitch,yaw});
  }

void RuntimeTest::quarantineNearbyNpcs() {
  world->detectNpc(anchor,NpcQuarantineRadius,[&](Npc& npc) {
    if(&npc==player || &npc==subject)
      return;

    const Vec3 original = npc.position();
    Vec3 radial = original-anchor;
    radial.y = 0.f;
    float radialLength = Vec2(radial.x,radial.z).length();
    if(radialLength<1.f) {
      radial = forward*-1.f;
      radialLength = 1.f;
      }

    const auto existing =
        std::find_if(quarantinedNpcs.begin(),quarantinedNpcs.end(),
                     [&](const QuarantinedNpc& value){ return value.npc==&npc; });
    const bool firstQuarantine = existing==quarantinedNpcs.end();
    if(firstQuarantine)
      quarantinedNpcs.push_back({&npc,original,npc.rotation()});
    Vec3 destination = anchor + radial*(NpcQuarantineDistance/radialLength);
    destination.y = original.y;
    npc.setPosition(destination);
    npc.updateTransform();

    const auto* symbol = world->script().findSymbol(npc.handle().symbol_index());
    Log::e("[RUNTIME_TEST] ",
           firstQuarantine ? "quarantined" : "re-quarantined",
           " fixture NPC instance=",
           symbol!=nullptr ? symbol->name() : std::string_view("?"),
           " display=",npc.displayName(),
           " distance=",length(original-anchor),
           " -> ",length(destination-anchor));
    });
  fixtureQuarantinedNpcCount = quarantinedNpcs.size();
  }

void RuntimeTest::restoreQuarantinedNpcs() {
  for(auto& value:quarantinedNpcs) {
    value.npc->setPosition(value.position);
    value.npc->setDirection(value.rotation);
    value.npc->updateTransform();
    }
  if(!quarantinedNpcs.empty())
    Log::e("[RUNTIME_TEST] restored fixture NPCs count=",quarantinedNpcs.size());
  quarantinedNpcs.clear();
  }

void RuntimeTest::enter(Phase next) {
  phase       = next;
  phaseTime   = 0;
  nextAction  = 0;
  // PrintScreen entries live for at least one second. Delay the first message of a new phase
  // until the previous entry has expired, so changing values never render on top of each other.
  nextMessage = StatusRefresh;
  }

void RuntimeTest::showStatus() {
  std::ostringstream title;
  std::ostringstream details;
  if(mode==Mode::EnemyHeal) {
    title << "AUTOTEST: enemy heal | L'Hiver Uriziel";
    if(!currentHeal.requested.empty()) {
      title << " | " << currentHeal.requested;
      details << "FightAI: " << (currentHeal.combatReached ? "YES" : "WAIT")
              << " | heals: " << currentHeal.healEvents << "/" << currentHeal.samples;
      }
    }
  else if(mode==Mode::OrcBehind) {
    title << "AUTOTEST: Orc detects player behind";
    if(!currentOrc.variant.empty()) {
      title << " | " << currentOrc.variant;
      details << "initial FOV: " << (currentOrc.initialFov ? "TRUE" : "FALSE")
              << " | other NPCs: " << currentOrc.nearbyOtherNpcs
              << " | target: " << (currentOrc.selectorAcquiredPlayer ? "YES" : "WAIT")
              << " | FightAI: "
              << (currentOrc.fightStateForced ? "FIXTURE" :
                  currentOrc.fightStateReached ? "SCRIPT" : "WAIT");
      }
    }
  else if(mode==Mode::NpcSleepPlacement) {
    title << "AUTOTEST: Brian sleep placement | L'Hiver Uriziel";
    details << "bed: " << (sleepResult.bedAttached ? "ATTACHED" : "WAIT")
            << " | lie: " << (sleepResult.liePoseSeen ? "YES" : "WAIT")
            << " | raw door: " << (sleepResult.doorClassSeen ? "YES" : "WAIT")
            << " | door behavior: " << (sleepResult.doorSemanticsSeen ? "WRONG" : "NO")
            << " | base float: " << sleepResult.baseFloatingSamples
            << " | root float: " << sleepResult.rootFloatingSamples
            << " | misplaced: " << sleepResult.misplacedSamples
            << " | horizontal walk: " << sleepResult.horizontalLocomotionSamples;
    }
  else {
    title << "AUTOTEST: unknown test " << testName;
    }

  const auto color = (phase==Phase::Done && !resultPass) ?
                     Resources::FontType::Red : Resources::FontType::Yellow;
  const auto& font = Resources::font(color,1.f);
  owner.onPrintScreen(title.str(),2,7,0,font);
  if(!details.str().empty())
    owner.onPrintScreen(details.str(),2,11,0,font);
  }

void RuntimeTest::finish() {
  removeSubject();
  if(mode==Mode::EnemyHeal) {
    resultPass = fixtureClear && !healResults.empty() &&
                 std::all_of(healResults.begin(),healResults.end(),
                             [](const HealResult& value){ return value.passed(); });
    }
  else if(mode==Mode::OrcBehind) {
    resultPass = fixtureClear && orcResults.size()==2 &&
                 std::all_of(orcResults.begin(),orcResults.end(),
                             [](const OrcResult& value){ return value.passed(); });
    }
  else if(mode==Mode::NpcSleepPlacement) {
    resultPass = sleepResult.passed();
    Log::e("[RUNTIME_TEST] sleep result instance=",sleepResult.instance,
           " result=",passText(resultPass),
           " gotoBed=",boolText(sleepResult.gotoBedStateSeen),
           " sleep=",boolText(sleepResult.sleepStateSeen),
           " bedFixture=",boolText(sleepResult.bedFixtureSeen),
           " bedAttached=",boolText(sleepResult.bedAttached),
           " liePose=",boolText(sleepResult.liePoseSeen),
           " doorClass=",boolText(sleepResult.doorClassSeen),
           " doorSemantics=",boolText(sleepResult.doorSemanticsSeen),
           " samples=",sleepResult.samples,
           " attachedSamples=",sleepResult.attachedSamples,
           " settledSamples=",sleepResult.settledSamples,
           " maxTransitionRootAbove=",sleepResult.maxTransitionRootAbove,
           " baseFloatingSamples=",sleepResult.baseFloatingSamples,
           " rootFloatingSamples=",sleepResult.rootFloatingSamples,
           " rootBelowSupportSamples=",sleepResult.rootBelowSupportSamples,
           " misplacedSamples=",sleepResult.misplacedSamples,
           " horizontalLocomotionSamples=",sleepResult.horizontalLocomotionSamples,
           " maxBaseGroundOffset=",sleepResult.maxBaseGroundOffset,
           " maxBaseAboveSupport=",sleepResult.maxBaseAboveSupport,
           " maxRootAboveSupport=",sleepResult.maxRootAboveSupport,
           " maxRootBelowSupport=",sleepResult.maxRootBelowSupport,
           " maxRootHorizontalOffset=",sleepResult.maxRootHorizontalOffset,
           " maxAttachmentGroundDelta=",sleepResult.maxAttachmentGroundDelta,
           " minLocomotionUpright=",sleepResult.minLocomotionUpright);
    }
  else {
    resultPass = false;
    }

  if(!writeResult(resultPass))
    resultPass = false;
  Log::e("[RUNTIME_TEST] FINAL test=",testName," result=",passText(resultPass),
         " output=",outputPath);
  restoreQuarantinedNpcs();
  if(mode==Mode::NpcSleepPlacement && player!=nullptr)
    player->physic.setEnable(true);
  enter(Phase::Done);
  }

std::string RuntimeTest::jsonEscape(std::string_view text) {
  std::string ret;
  ret.reserve(text.size()+8);
  for(char c:text) {
    switch(c) {
      case '\\': ret += "\\\\"; break;
      case '"':  ret += "\\\""; break;
      case '\n': ret += "\\n";  break;
      case '\r': ret += "\\r";  break;
      case '\t': ret += "\\t";  break;
      default:   ret += c;      break;
      }
    }
  return ret;
  }

bool RuntimeTest::chooseClearFixture(const Vec3& preferredAnchor, float preferredAngle) {
  constexpr float searchRadii[]     = {
    0.f,1200.f,2400.f,3600.f,5000.f,7000.f,9000.f,12000.f,16000.f,20000.f,
    };
  constexpr float corridorHeights[] = {35.f,70.f,105.f,150.f,210.f};
  constexpr float corridorSides[]   = {-180.f,-120.f,-60.f,0.f,60.f,120.f,180.f};
  constexpr float groundSides[]     = {-160.f,0.f,160.f};
  constexpr float groundAlong[]     = {0.f,150.f};
  constexpr size_t groundSamples    = std::size(groundSides)*std::size(groundAlong);

  auto* physics = world->physic();
  Vec3 bestAnchor = preferredAnchor;
  Vec3 bestForward = {std::cos(preferredAngle),0.f,std::sin(preferredAngle)};
  size_t bestScore = 0;
  size_t bestNpcCount = countOtherNpcs(preferredAnchor,FixtureIsolationRadius);
  float bestNpcClearance = nearestOtherNpcDistance(preferredAnchor);
  fixtureScoreMax = groundSamples +
                    std::size(corridorHeights)*std::size(corridorSides);

  for(float radius:searchRadii) {
    const int32_t positionCount = radius==0.f ? 1 : 24;
    for(int32_t positionIndex=0;positionIndex<positionCount;++positionIndex) {
      const float positionAngle = float(positionIndex)*float(2.0*M_PI/positionCount);
      Vec3 candidate = preferredAnchor +
                       Vec3(std::cos(positionAngle)*radius,0.f,
                            std::sin(positionAngle)*radius);
      const auto ground = physics->landRay(candidate+Vec3(0.f,500.f,0.f),1400.f);
      if(!ground.hasCol || ground.n.y<0.65f ||
         std::abs(ground.v.y-preferredAnchor.y)>600.f)
        continue;
      candidate.y = ground.v.y;
      const float npcClearance = nearestOtherNpcDistance(candidate);
      if(npcClearance<FixtureIsolationRadius)
        continue;

      for(int32_t directionIndex=0;directionIndex<24;++directionIndex) {
        const float directionAngle =
            preferredAngle + float(directionIndex)*float(M_PI/12.0);
        const Vec3 direction = {std::cos(directionAngle),0.f,std::sin(directionAngle)};
        const Vec3 side = {-direction.z,0.f,direction.x};
        size_t score = 0;

        for(float along:groundAlong) {
          for(float sideOffset:groundSides) {
            const Vec3 probe = candidate + direction*along + side*sideOffset;
            const auto land = physics->landRay(probe+Vec3(0.f,100.f,0.f),220.f);
            if(land.hasCol && land.n.y>=0.65f &&
               std::abs(land.v.y-candidate.y)<=15.f)
              ++score;
            }
          }

        for(float sideOffset:corridorSides) {
          for(float height:corridorHeights) {
            const Vec3 from = candidate - direction*120.f +
                              side*sideOffset + Vec3(0.f,height,0.f);
            const Vec3 to = candidate + direction*360.f +
                            side*sideOffset + Vec3(0.f,height,0.f);
            if(!physics->ray(from,to).hasCol)
              ++score;
            }
          }

        if(score>bestScore || (score==bestScore && npcClearance>bestNpcClearance)) {
          bestScore   = score;
          bestAnchor  = candidate;
          bestForward = direction;
          bestNpcCount = 0;
          bestNpcClearance = npcClearance;
          }
        if(score==fixtureScoreMax)
          break;
        }
      }
    }

  anchor       = bestAnchor;
  forward      = bestForward;
  fixtureScore = bestScore;
  fixtureNpcCount = bestNpcCount;
  fixtureNpcClearance = bestNpcClearance;
  const Vec3 offset = anchor-preferredAnchor;
  const bool clear = fixtureScore==fixtureScoreMax && fixtureNpcCount==0 &&
                     fixtureNpcClearance>=FixtureIsolationRadius;
  Log::e("[RUNTIME_TEST] clear-fixture scan score=",fixtureScore,"/",fixtureScoreMax,
         " nearbyOtherNpcs=",fixtureNpcCount,
         " nearestOtherNpc=",fixtureNpcClearance,
         " isolationRadius=",FixtureIsolationRadius,
         " clear=",boolText(clear),
         " offset=(",offset.x,",",offset.y,",",offset.z,")");
  return clear;
  }

size_t RuntimeTest::countOtherNpcs(const Vec3& center, float radius,
                                   const Npc* fixtureSubject) const {
  size_t count = 0;
  world->detectNpc(center,radius,[&](Npc& npc) {
    if(&npc!=player && &npc!=fixtureSubject)
      ++count;
    });
  return count;
  }

float RuntimeTest::nearestOtherNpcDistance(const Vec3& center,
                                           const Npc* fixtureSubject) const {
  float nearestSquared = std::numeric_limits<float>::max();
  for(uint32_t i=0;i<world->npcCount();++i) {
    const Npc* npc = world->npcById(i);
    if(npc==nullptr || npc==player || npc==fixtureSubject)
      continue;
    nearestSquared = std::min(nearestSquared,(npc->position()-center).quadLength());
    }
  return std::sqrt(nearestSquared);
  }

bool RuntimeTest::legacyCanSeeNpc(const Npc& from, const Npc& to) const {
  const Vec3 self = from.visual.mapHeadBone();
  static const double ref = std::cos(100.0*M_PI/180.0);

  auto canSeePoint = [&](const Vec3& point) {
    const float dir = Npc::angleDir(self.x-point.x,self.z-point.z);
    const float view = from.visual.viewDirection();
    const float delta = float(M_PI)*(view-dir)/180.f;
    return double(std::cos(delta))<=ref &&
           !world->physic()->ray(self,point).hasCol;
    };

  if(canSeePoint(to.physic.center()))
    return true;
  const auto* skeleton = to.visual.visualSkeleton();
  return skeleton!=nullptr && skeleton->BIP01_HEAD!=size_t(-1) &&
         canSeePoint(to.visual.mapHeadBone());
  }

bool RuntimeTest::writeResult(bool passed) const {
  std::ofstream out(outputPath,std::ios::binary|std::ios::trunc);
  if(!out) {
    Log::e("[RUNTIME_TEST] unable to write result file: ",outputPath);
    return false;
    }

  out << std::boolalpha << std::fixed << std::setprecision(2);
  out << "{\n"
      << "  \"test\": \"" << jsonEscape(testName) << "\",\n"
      << "  \"passed\": " << passed << ",\n"
      << "  \"fixture\": {\n"
      << "    \"clear\": " << fixtureClear << ",\n"
      << "    \"clearance_score\": " << fixtureScore << ",\n"
      << "    \"clearance_score_max\": " << fixtureScoreMax << ",\n"
      << "    \"npc_isolation_radius_cm\": " << FixtureIsolationRadius << ",\n"
      << "    \"nearby_other_npcs\": " << fixtureNpcCount << ",\n"
      << "    \"nearest_other_npc_cm\": " << fixtureNpcClearance << ",\n"
      << "    \"npc_quarantine_radius_cm\": " << NpcQuarantineRadius << ",\n"
      << "    \"quarantined_npcs\": " << fixtureQuarantinedNpcCount << ",\n"
      << "    \"offset_cm\": ["
      << anchor.x-initialAnchor.x << ", "
      << anchor.y-initialAnchor.y << ", "
      << anchor.z-initialAnchor.z << "]\n"
      << "  },\n";

  if(mode==Mode::EnemyHeal) {
    out << "  \"cases\": [\n";
    for(size_t i=0;i<healResults.size();++i) {
      const auto& value = healResults[i];
      out << "    {\n"
          << "      \"requested\": \"" << jsonEscape(value.requested) << "\",\n"
          << "      \"instance\": \"" << jsonEscape(value.instance) << "\",\n"
          << "      \"display_name\": \"" << jsonEscape(value.displayName) << "\",\n"
          << "      \"assess_damage_callback\": \"" << jsonEscape(value.assessDamageCallback) << "\",\n"
          << "      \"spawned\": " << value.spawned << ",\n"
          << "      \"combat_reached\": " << value.combatReached << ",\n"
          << "      \"fight_state_forced\": " << value.fightStateForced << ",\n"
          << "      \"attack_animation_seen\": " << value.attackAnimationSeen << ",\n"
          << "      \"samples\": " << value.samples << ",\n"
          << "      \"fight_samples\": " << value.fightSamples << ",\n"
          << "      \"callback_invocations\": " << value.callbackInvocations << ",\n"
          << "      \"heal_events\": " << value.healEvents << ",\n"
          << "      \"front_fov_false_samples\": " << value.frontFovFalse << ",\n"
          << "      \"free_los_false_samples\": " << value.freeLosFalse << ",\n"
          << "      \"legacy_head_model_fov_false_samples\": " << value.legacyFovFalse << ",\n"
          << "      \"max_head_offset_cm\": " << value.maxHeadOffset << ",\n"
          << "      \"max_distance_cm\": " << value.maxDistance << ",\n"
          << "      \"hp_deltas\": [";
      for(size_t n=0;n<value.hpDeltas.size();++n) {
        if(n>0)
          out << ", ";
        out << value.hpDeltas[n];
        }
      out << "],\n"
          << "      \"passed\": " << value.passed() << "\n"
          << "    }" << (i+1<healResults.size() ? "," : "") << "\n";
      }
    out << "  ]\n";
    }
  else if(mode==Mode::NpcSleepPlacement) {
    out << "  \"case\": {\n"
        << "    \"instance\": \"" << jsonEscape(sleepResult.instance) << "\",\n"
        << "    \"found\": " << sleepResult.found << ",\n"
        << "    \"goto_bed_state_seen\": " << sleepResult.gotoBedStateSeen << ",\n"
        << "    \"sleep_state_seen\": " << sleepResult.sleepStateSeen << ",\n"
        << "    \"bed_fixture_seen\": " << sleepResult.bedFixtureSeen << ",\n"
        << "    \"bed_attached\": " << sleepResult.bedAttached << ",\n"
        << "    \"lie_pose_seen\": " << sleepResult.liePoseSeen << ",\n"
        << "    \"locomotion_seen\": " << sleepResult.locomotionSeen << ",\n"
        << "    \"door_class_seen\": " << sleepResult.doorClassSeen << ",\n"
        << "    \"door_semantics_seen\": " << sleepResult.doorSemanticsSeen << ",\n"
        << "    \"samples\": " << sleepResult.samples << ",\n"
        << "    \"attached_samples\": " << sleepResult.attachedSamples << ",\n"
        << "    \"settled_samples\": " << sleepResult.settledSamples << ",\n"
        << "    \"max_transition_root_above_cm\": "
        << sleepResult.maxTransitionRootAbove << ",\n"
        << "    \"base_floating_samples\": " << sleepResult.baseFloatingSamples << ",\n"
        << "    \"root_floating_samples\": " << sleepResult.rootFloatingSamples << ",\n"
        << "    \"root_below_support_samples\": " << sleepResult.rootBelowSupportSamples << ",\n"
        << "    \"misplaced_samples\": " << sleepResult.misplacedSamples << ",\n"
        << "    \"horizontal_locomotion_samples\": "
        << sleepResult.horizontalLocomotionSamples << ",\n"
        << "    \"max_base_ground_offset_cm\": "
        << sleepResult.maxBaseGroundOffset << ",\n"
        << "    \"max_base_above_support_cm\": "
        << sleepResult.maxBaseAboveSupport << ",\n"
        << "    \"max_root_above_support_cm\": "
        << sleepResult.maxRootAboveSupport << ",\n"
        << "    \"max_root_below_support_cm\": "
        << sleepResult.maxRootBelowSupport << ",\n"
        << "    \"max_root_horizontal_offset_cm\": "
        << sleepResult.maxRootHorizontalOffset << ",\n"
        << "    \"max_attachment_ground_delta_cm\": "
        << sleepResult.maxAttachmentGroundDelta << ",\n"
        << "    \"min_locomotion_upright_ratio\": "
        << sleepResult.minLocomotionUpright << ",\n"
        << "    \"passed\": " << sleepResult.passed() << "\n"
        << "  }\n";
    }
  else if(mode==Mode::OrcBehind) {
    out << "  \"cases\": [\n";
    for(size_t i=0;i<orcResults.size();++i) {
      const auto& value = orcResults[i];
      out << "    {\n"
          << "      \"variant\": \"" << jsonEscape(value.variant) << "\",\n"
          << "      \"instance\": \"" << jsonEscape(value.instance) << "\",\n"
          << "      \"assess_player_callback\": \"" << jsonEscape(value.assessPlayerCallback) << "\",\n"
          << "      \"assess_enemy_callback\": \"" << jsonEscape(value.assessEnemyCallback) << "\",\n"
          << "      \"spawned\": " << value.spawned << ",\n"
          << "      \"see_only\": " << value.seeOnly << ",\n"
          << "      \"initial_angle_degrees\": " << value.initialAngle << ",\n"
          << "      \"initially_behind\": " << value.initiallyBehind << ",\n"
          << "      \"initial_fov\": " << value.initialFov << ",\n"
          << "      \"initial_free_los\": " << value.initialFreeLos << ",\n"
          << "      \"initial_sense_cone\": " << value.initialSenseCone << ",\n"
          << "      \"initial_sense_free\": " << value.initialSenseFree << ",\n"
          << "      \"senses_mask\": " << value.sensesMask << ",\n"
          << "      \"npc_detection_radius_cm\": " << NpcDetectionRadius << ",\n"
          << "      \"nearby_other_npcs\": " << value.nearbyOtherNpcs << ",\n"
          << "      \"nearest_other_npc_at_start_cm\": " << value.nearestOtherNpc << ",\n"
          << "      \"initial_regular_perception_invoked\": " << value.initialRegularPerceptionInvoked << ",\n"
          << "      \"target_acquired_before_probe\": " << value.targetAcquiredBeforeProbe << ",\n"
          << "      \"fight_state_reached_before_probe\": " << value.fightStateReachedBeforeProbe << ",\n"
          << "      \"turn_observed_before_probe\": " << value.turnObservedBeforeProbe << ",\n"
          << "      \"selector_acquired_player\": " << value.selectorAcquiredPlayer << ",\n"
          << "      \"fight_state_reached\": " << value.fightStateReached << ",\n"
          << "      \"fight_state_forced\": " << value.fightStateForced << ",\n"
          << "      \"turned\": " << value.turned << ",\n"
          << "      \"detection_time_ms\": " << value.detectionTimeMs << ",\n"
          << "      \"fight_state_time_ms\": " << value.fightStateTimeMs << ",\n"
          << "      \"maximum_turn_degrees\": " << value.maximumTurnDegrees << ",\n"
          << "      \"passed\": " << value.passed() << "\n"
          << "    }" << (i+1<orcResults.size() ? "," : "") << "\n";
      }
    out << "  ]\n";
    }
  out << "}\n";
  out.flush();
  if(!out) {
    Log::e("[RUNTIME_TEST] unable to finish result file: ",outputPath);
    return false;
    }
  return true;
  }

float RuntimeTest::relativeAngle(const Npc& from, const Npc& to) {
  const Vec3 direction = to.position()-from.position();
  const float target = 180.f*std::atan2(direction.z,direction.x)/float(M_PI);
  float delta = std::fmod(target-from.rotation()+540.f,360.f)-180.f;
  return std::abs(delta);
  }
