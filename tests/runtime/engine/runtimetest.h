#pragma once

#include <Tempest/Vec>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class Gothic;
class Marvin;
class Npc;
class World;

// Opt-in, data-driven integration harness. It is not part of normal game builds.
class RuntimeTest final {
  public:
    RuntimeTest(Gothic& owner, std::string_view name, std::string_view output);
    ~RuntimeTest();

    void tick(uint64_t dt);

  private:
    enum class Mode : uint8_t {
      Invalid,
      EnemyHeal,
      OrcBehind,
      NpcSleepPlacement,
      };

    enum class Phase : uint8_t {
      Intro,
      Warmup,
      Observe,
      Result,
      Done,
      };

    struct HealResult {
      std::string requested;
      std::string instance;
      std::string displayName;
      std::string assessDamageCallback;
      bool        spawned             = false;
      bool        combatReached       = false;
      bool        fightStateForced    = false;
      bool        attackAnimationSeen = false;
      uint32_t    samples             = 0;
      uint32_t    fightSamples        = 0;
      uint32_t    callbackInvocations = 0;
      uint32_t    healEvents          = 0;
      uint32_t    frontFovFalse       = 0;
      uint32_t    freeLosFalse        = 0;
      uint32_t    legacyFovFalse      = 0;
      float       maxHeadOffset       = 0.f;
      float       maxDistance         = 0.f;
      std::vector<int32_t> hpDeltas;

      bool passed() const;
      };

    struct OrcResult {
      std::string instance;
      std::string variant;
      std::string assessPlayerCallback;
      std::string assessEnemyCallback;
      bool        spawned            = false;
      bool        seeOnly            = false;
      bool        initiallyBehind    = false;
      bool        initialFov         = false;
      bool        initialFreeLos     = false;
      int32_t     initialSenseCone   = 0;
      int32_t     initialSenseFree   = 0;
      int32_t     sensesMask         = 0;
      size_t      nearbyOtherNpcs    = 0;
      float       nearestOtherNpc    = 0.f;
      float       initialAngle       = 0.f;
      bool        initialRegularPerceptionInvoked = false;
      bool        targetAcquiredBeforeProbe = false;
      bool        fightStateReachedBeforeProbe = false;
      bool        turnObservedBeforeProbe = false;
      bool        selectorAcquiredPlayer = false;
      bool        fightStateReached  = false;
      bool        fightStateForced   = false;
      bool        turned             = false;
      uint64_t    detectionTimeMs    = 0;
      uint64_t    fightStateTimeMs   = 0;
      float       maximumTurnDegrees = 0.f;

      bool passed() const;
      };

    struct SleepPlacementResult {
      std::string instance;
      bool        found                         = false;
      bool        gotoBedStateSeen              = false;
      bool        sleepStateSeen                = false;
      bool        bedFixtureSeen                = false;
      bool        bedAttached                   = false;
      bool        liePoseSeen                   = false;
      bool        locomotionSeen                = false;
      bool        doorClassSeen                 = false;
      bool        doorSemanticsSeen             = false;
      uint32_t    samples                       = 0;
      uint32_t    attachedSamples               = 0;
      uint32_t    settledSamples                = 0;
      uint32_t    baseFloatingSamples           = 0;
      uint32_t    rootFloatingSamples           = 0;
      uint32_t    rootBelowSupportSamples       = 0;
      uint32_t    misplacedSamples              = 0;
      uint32_t    horizontalLocomotionSamples   = 0;
      float       maxTransitionRootAbove        = 0.f;
      float       maxBaseAboveSupport           = 0.f;
      float       maxRootAboveSupport           = 0.f;
      float       maxRootBelowSupport           = 0.f;
      float       maxBaseGroundOffset           = 0.f;
      float       maxRootHorizontalOffset       = 0.f;
      float       maxAttachmentGroundDelta      = 0.f;
      float       minLocomotionUpright          = 1.f;

      bool passed() const;
      };

    struct QuarantinedNpc {
      Npc*          npc = nullptr;
      Tempest::Vec3 position;
      float         rotation = 0.f;
      };

    void initialize(World& world, Npc& player);
    void tickEnemyHeal(uint64_t dt);
    void tickOrcBehind(uint64_t dt);
    void tickNpcSleepPlacement(uint64_t dt);

    void beginHealCase();
    void sampleHealCase();
    void finishHealCase();

    void beginOrcCase(bool seeOnly);
    void prepareOrcCase();
    void sampleOrcCase();
    void finishOrcCase();

    void sampleSleepPlacement();
    void frameSleepCamera();
    Npc* findNpc(std::string_view instance, std::string_view displayName) const;

    Npc* insertNpc(const std::vector<std::string_view>& candidates, std::string& instance);
    bool isNpcInstance(std::string_view name) const;
    void removeSubject();
    void frameSubjectCamera();
    void quarantineNearbyNpcs();
    void restoreQuarantinedNpcs();

    void enter(Phase next);
    void showStatus();
    void finish();
    bool writeResult(bool passed) const;

    bool chooseClearFixture(const Tempest::Vec3& preferredAnchor, float preferredAngle);
    size_t countOtherNpcs(const Tempest::Vec3& center, float radius,
                          const Npc* fixtureSubject = nullptr) const;
    float nearestOtherNpcDistance(const Tempest::Vec3& center,
                                  const Npc* fixtureSubject = nullptr) const;
    bool legacyCanSeeNpc(const Npc& from, const Npc& to) const;
    static float relativeAngle(const Npc& from, const Npc& to);
    static std::string jsonEscape(std::string_view text);

    Gothic&                 owner;
    std::unique_ptr<Marvin> marvin;
    Mode                    mode       = Mode::Invalid;
    Phase                   phase      = Phase::Intro;
    World*                  world      = nullptr;
    Npc*                    player     = nullptr;
    Npc*                    subject    = nullptr;

    std::string             testName;
    std::string             outputPath;
    Tempest::Vec3           initialAnchor;
    Tempest::Vec3           anchor;
    Tempest::Vec3           forward = {1.f,0.f,0.f};
    size_t                  fixtureScore = 0;
    size_t                  fixtureScoreMax = 0;
    size_t                  fixtureNpcCount = 0;
    float                   fixtureNpcClearance = 0.f;
    size_t                  fixtureQuarantinedNpcCount = 0;
    bool                    fixtureClear = false;
    std::vector<QuarantinedNpc> quarantinedNpcs;

    uint64_t                phaseTime   = 0;
    uint64_t                nextAction  = 0;
    uint64_t                nextMessage = 0;
    bool                    initialized = false;
    bool                    resultPass  = false;

    size_t                  caseIndex = 0;
    HealResult              currentHeal;
    std::vector<HealResult> healResults;
    OrcResult               currentOrc;
    std::vector<OrcResult>  orcResults;
    Npc*                    sleepSubject = nullptr;
    SleepPlacementResult    sleepResult;
    std::string             sleepLastSignature;
  };
