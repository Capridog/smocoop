#include "lib.hpp"
#include "program/game.hpp"
#include "program/loggers.hpp"

#include <cstring>

// ---------------------------------------------------------------------------------------------
// Odyssey Co-op (rebuild). Targets SMO 1.0.0 only; refuses to install on any other build.
//  step 2: spawn Player 2 as a second PlayerActorHakoniwa on the second controller.
//  step 3: per-player HP/death for P2, ground-checked placement, P2-owned lookups.
// All offsets/fields used here are listed (with how they were verified) in game.hpp.
// ---------------------------------------------------------------------------------------------

#define COOP_LOG(...) Logging.Log("[OdysseyCoop] " __VA_ARGS__)

namespace coop {
static bool gInStageSceneInit = false;  // only spawn P2 while StageScene::init runs
static bool gSpawningP2 = false;        // re-entrancy guard
static al::LiveActor* gP1 = nullptr;
static al::LiveActor* gP2 = nullptr;
static al::LiveActor* gP2Cap = nullptr;  // P2's own Cappy (HackCap)
static s32 gP2Port = -1;
static bool gP2Placed = false;
static s32 gFramesSinceInit = 0;

// P2's own hit points. P1 keeps using the game's shared life counter (HUD hearts).
constexpr s32 kP2MaxHp = 3;
static s32 gP2Hp = kP2MaxHp;
static bool gP2RespawnPending = false;

template <typename T>
static T Field(const void* base, ptrdiff_t off) {
    T v;
    std::memcpy(&v, reinterpret_cast<const u8*>(base) + off, sizeof(T));
    return v;
}
template <typename T>
static void SetField(void* base, ptrdiff_t off, T v) {
    std::memcpy(reinterpret_cast<u8*>(base) + off, &v, sizeof(T));
}

static void ResetStageState() {
    gP1 = gP2 = gP2Cap = nullptr;
    gP2Port = -1;
    gP2Placed = false;
    gFramesSinceInit = 0;
    gP2Hp = kP2MaxHp;
    gP2RespawnPending = false;
}

// Actor P2 is currently capturing (nullptr if none).
static al::LiveActor* P2HackActor() {
    if (!gP2) return nullptr;
    void* keeper = Field<void*>(gP2, smo100::PlayerActorHakoniwa_hackKeeper);
    return keeper ? Field<al::LiveActor*>(keeper, smo100::PlayerHackKeeper_hackActor) : nullptr;
}

static bool RayHit(const al::LiveActor* user, const sead::Vector3f& from, const sead::Vector3f& arrow,
                   sead::Vector3f* hit) {
    auto* col = reinterpret_cast<const al::IUseCollision*>(
        reinterpret_cast<const u8*>(user) + smo100::LiveActor_IUseCollision);
    return alCollisionUtil::getHitPosOnArrow(col, hit, from, arrow, nullptr, nullptr);
}

static const al::IUseAreaObj* P1AreaUser() {
    return reinterpret_cast<const al::IUseAreaObj*>(reinterpret_cast<const u8*>(gP1) +
                                                    smo100::LiveActor_IUseAreaObj);
}

// Find a safe spot beside P1: right, left, behind, front; each must have no wall between P1 and the
// spot and must have ground within 600 units below it. Falls back to P1's own position.
static sead::Vector3f FindSpotBesideP1() {
    const sead::Vector3f p1 = al::getTrans(gP1);
    sead::Vector3f side;
    al::calcSideDir(&side, gP1);
    const sead::Vector3f dirs[4] = {{side.x, 0, side.z},
                                    {-side.x, 0, -side.z},
                                    {side.z, 0, -side.x},
                                    {-side.z, 0, side.x}};
    for (const auto& d : dirs) {
        const float dist = 150.0f;
        sead::Vector3f eye = {p1.x, p1.y + 60.0f, p1.z};
        sead::Vector3f toSpot = {d.x * dist, 0.0f, d.z * dist};
        sead::Vector3f tmp;
        if (RayHit(gP1, eye, toSpot, &tmp)) continue;  // wall in the way
        sead::Vector3f top = {p1.x + toSpot.x, p1.y + 150.0f, p1.z + toSpot.z};
        sead::Vector3f down = {0.0f, -600.0f, 0.0f};
        sead::Vector3f ground;
        if (!RayHit(gP1, top, down, &ground)) continue;  // no floor (ledge / pit)
        sead::Vector3f spot = {ground.x, ground.y + 5.0f, ground.z};
        if (al::isInDeathArea(P1AreaUser(), spot)) continue;  // floor is inside a pit/kill volume
        return spot;
    }
    return {p1.x, p1.y + 5.0f, p1.z};
}

static void PlaceP2BesideP1(const char* why) {
    sead::Vector3f dst = FindSpotBesideP1();
    al::resetPosition(gP2, dst);
    al::setVelocityZero(gP2);
    COOP_LOG("P2 placed beside P1 (%s) at (%.0f, %.0f, %.0f)", why, dst.x, dst.y, dst.z);
}
}  // namespace coop

namespace coop::cam {
static void Reset();
static void Update();
}  // namespace coop::cam

// ---------------------------------------------------------------------------- spawning (step 2)

HOOK_DEFINE_TRAMPOLINE(StageSceneInit) {
    static void Callback(StageScene* scene, const al::SceneInitInfo& info) {
        coop::ResetStageState();
        coop::cam::Reset();
        coop::gInStageSceneInit = true;
        Orig(scene, info);
        coop::gInStageSceneInit = false;
        COOP_LOG("StageScene::init done. P1=%p P2=%p P2port=%d", coop::gP1, coop::gP2,
                 coop::gP2Port);
    }
};

HOOK_DEFINE_TRAMPOLINE(PlayerInitHook) {
    static void Callback(PlayerActorHakoniwa* self, const al::ActorInitInfo& actorInfo,
                         const PlayerInitInfo& playerInfo) {
        Orig(self, actorInfo, playerInfo);

        if (!coop::gInStageSceneInit || coop::gSpawningP2 || coop::gP2 != nullptr)
            return;
        if (playerInfo.isClosetScene)
            return;  // dressing-room preview player: never duplicate

        s32 p2Port = al::getPlayerControllerPort(1);
        s32 p1Port = playerInfo.controllerPort;
        COOP_LOG("P1 initPlayer: P1 port=%d, controller order 1 -> port %d", p1Port, p2Port);
        if (p2Port < 0 || p2Port == p1Port) {
            COOP_LOG("No second controller connected; staying single-player.");
            return;
        }

        coop::gSpawningP2 = true;
        PlayerInitInfo p2Info;
        std::memcpy(&p2Info, &playerInfo, sizeof(p2Info));
        p2Info.controllerPort = p2Port;
        p2Info.trans[0] += 150.0f;  // initial offset; final placement happens in StageSceneControl

        PlayerActorBase* p2 = createPlayerFunction<PlayerActorHakoniwa>("Player2");
        if (p2 == nullptr) {
            COOP_LOG("createPlayerFunction returned null");
            coop::gSpawningP2 = false;
            return;
        }
        Orig(reinterpret_cast<PlayerActorHakoniwa*>(p2), actorInfo, p2Info);
        coop::gP2 = reinterpret_cast<al::LiveActor*>(p2);
        coop::gP2Port = p2Port;
        auto* cap = coop::Field<al::LiveActor*>(p2, smo100::PlayerActorHakoniwa_hackCap);
        if (cap && coop::Field<al::LiveActor*>(cap, smo100::HackCap_player) == coop::gP2)
            coop::gP2Cap = cap;
        coop::gSpawningP2 = false;
        COOP_LOG("P2 created and initialised: %p (port %d), Cappy %p", p2, p2Port, coop::gP2Cap);
    }
};

HOOK_DEFINE_TRAMPOLINE(RegisterPlayerHook) {
    static void Callback(al::LiveActor* actor, al::PadRumbleKeeper* keeper) {
        Orig(actor, keeper);
        if (!coop::gInStageSceneInit || coop::gP2 == nullptr || coop::gP1 != nullptr)
            return;
        coop::gP1 = actor;  // first registration in this StageScene is P1
        if (actor == coop::gP2)
            return;
        Orig(coop::gP2, al::createPadRumbleKeeper(coop::gP2, coop::gP2Port));
        COOP_LOG("Registered P2 as player index 1.");
    }
};

// ------------------------------------------------------------------------ player lookups (3b)
// Vanilla code asks for "player 0" from the caller's point of view (e.g. Cappy's return target is
// rs::getPlayerHeadPos(cap) -> al::getPlayerActor(cap, 0)). When P2, P2's Cappy, or the enemy P2
// is capturing asks, the answer must be P2. Everyone else still gets the vanilla answer.

static al::LiveActor* RedirectOwnPlayer(const al::LiveActor* caller, s32 index) {
    if (index != 0 || coop::gP2 == nullptr || caller == nullptr) return nullptr;
    if (caller == coop::gP2 || caller == coop::gP2Cap) return coop::gP2;
    if (caller == coop::P2HackActor()) return coop::gP2;
    return nullptr;
}

HOOK_DEFINE_TRAMPOLINE(GetPlayerActorHook) {
    static al::LiveActor* Callback(const al::LiveActor* caller, s32 index) {
        if (auto* p = RedirectOwnPlayer(caller, index)) return p;
        return Orig(caller, index);
    }
};

HOOK_DEFINE_TRAMPOLINE(TryGetPlayerActorHook) {
    static al::LiveActor* Callback(const al::LiveActor* caller, s32 index) {
        if (auto* p = RedirectOwnPlayer(caller, index)) return p;
        return Orig(caller, index);
    }
};

// ------------------------------------------------------------------- per-player death (3a)
// Vanilla: PlayerDamageKeeper::damage/damageForce/dead write the ONE shared life counter in
// GameDataFile, and PlayerJudgeDead fires for every player when it reaches zero -> both die.
// For P2 we keep a private HP instead, mirror the keeper's own bookkeeping (fields verified in
// Ghidra), and respawn P2 beside P1 instead of letting P2 die or restart the stage.

static bool IsP2Keeper(void* keeper) {
    return coop::gP2 != nullptr && keeper != nullptr &&
           coop::Field<al::LiveActor*>(keeper, 0x0) == coop::gP2;
}

static void P2TakeHit(void* keeper, s32 damageType, bool fromRecovery) {
    coop::SetField<u8>(keeper, 0x10, 1);
    coop::SetField<s32>(keeper, 0x14, damageType);
    coop::gP2Hp = coop::gP2Hp > 0 ? coop::gP2Hp - 1 : 0;
    coop::SetField<s32>(keeper, 0x24, coop::gP2Hp);
    coop::SetField<s32>(keeper, 0x28, 0);
    if (coop::Field<s32>(keeper, 0x2c) < 0x78) coop::SetField<s32>(keeper, 0x2c, 0);
    COOP_LOG("P2 hit, HP now %d", coop::gP2Hp);
    if (coop::gP2Hp <= 0) {
        if (fromRecovery) {
            // pit/quicksand recovery already moves P2 beside P1 (safety point); just refill
            coop::gP2Hp = coop::kP2MaxHp;
            COOP_LOG("P2 out of hearts during recovery; refilled to %d", coop::gP2Hp);
        } else {
            coop::gP2RespawnPending = true;  // refill + move beside P1 next frame
        }
    }
}

HOOK_DEFINE_TRAMPOLINE(DamageHook) {
    static void Callback(void* keeper, s32 damageType) {
        if (!IsP2Keeper(keeper)) return Orig(keeper, damageType);
        // vanilla skips the life change while +0x1c (prevent-damage) is set
        if (coop::Field<u8>(keeper, 0x1c) == 0) {
            P2TakeHit(keeper, damageType, false);
        } else {
            coop::SetField<u8>(keeper, 0x10, 1);
            coop::SetField<s32>(keeper, 0x14, damageType);
        }
    }
};

HOOK_DEFINE_TRAMPOLINE(DamageForceHook) {
    static void Callback(void* keeper, s32 damageType) {
        if (!IsP2Keeper(keeper)) return Orig(keeper, damageType);
        // vanilla only uses damageForce for the pit/quicksand "lose a heart and recover" path
        P2TakeHit(keeper, damageType, true);
    }
};

HOOK_DEFINE_TRAMPOLINE(DeadHook) {
    static void Callback(void* keeper) {
        if (!IsP2Keeper(keeper)) return Orig(keeper);
        COOP_LOG("P2 would have died; respawning beside P1 instead.");
        coop::gP2RespawnPending = true;
    }
};

HOOK_DEFINE_TRAMPOLINE(JudgeDeadHook) {
    static bool Callback(const void* judge) {
        auto* actor = coop::Field<al::LiveActor*>(judge, smo100::PlayerJudgeDead_player);
        if (coop::gP2 != nullptr && actor == coop::gP2)
            return al::isDead(coop::gP2);  // ignore the shared life counter for P2
        return Orig(judge);
    }
};

// Pits, quicksand, lava: vanilla already has a "lose a heart and get put back at the safety point"
// path (checkDeathArea / executePreMovementNerveChange -> damageForce + recovery via
// PlayerStateAbyss). It is only taken when PlayerRecoverySafetyPoint::isValid() is true and the
// SHARED life counter is not 1 (PlayerFunction::isPlayerHitPointOne). For P2 we force that path
// and move P2's safety point beside P1, so P2 is recovered next to Mario with the game's own
// animation instead of dying.
HOOK_DEFINE_TRAMPOLINE(HitPointOneHook) {
    static bool Callback(const al::LiveActor* actor) {
        if (coop::gP2 != nullptr && actor == coop::gP2) return false;
        return Orig(actor);
    }
};

HOOK_DEFINE_TRAMPOLINE(SafetyPointValidHook) {
    static bool Callback(PlayerRecoverySafetyPoint* sp) {
        auto* owner = coop::Field<al::LiveActor*>(sp, smo100::PlayerRecoverySafetyPoint_player);
        if (coop::gP2 == nullptr || coop::gP1 == nullptr || owner != coop::gP2) return Orig(sp);
        sead::Vector3f spot = coop::FindSpotBesideP1();
        sead::Vector3f up = {0.0f, 1.0f, 0.0f};
        sp->setSafetyPoint(spot, up, nullptr);
        return true;
    }
};

// Moe-Eye: hidden platforms ask "is player 0 wearing the glasses?" (verified in Ghidra:
// rs::isPlayerEnableToSeeOddSpace -> getPlayerActor(x,0) -> current hack == Megane). Show them when
// either player is.
HOOK_DEFINE_TRAMPOLINE(SeeOddSpaceHook) {
    static bool Callback(const al::LiveActor* actor) {
        if (Orig(actor)) return true;
        return coop::gP2 != nullptr && Orig(coop::gP2);
    }
};


// ------------------------------------------------------------------------ shared camera (step 4)
// 1) The follow point (PlayerCameraTarget::calcTrans, P1's camera target) is pulled toward the
//    midpoint between the brothers.  2) Cameras that use the base CameraPoser::makeLookAtCamera are
//    pulled back along their view line as the brothers separate. The game's CameraArrowCollider runs
//    right after, so the zoomed camera is still kept out of walls.
namespace coop::cam {
constexpr float kZoomStart = 500.0f;   // separation where zoom-out begins
constexpr float kZoomFull = 3000.0f;   // separation where zoom-out is at maximum
constexpr float kZoomMax = 1.9f;       // max distance multiplier
constexpr float kLerp = 0.08f;         // per-frame smoothing
constexpr float kShareEnd = 5000.0f;   // by this separation the camera is fully on the leader
static float gZoom = 1.0f;
static float gShare = 0.0f;            // 0 = P1, 0.5 = midpoint, 1 = P2

// 3D World rule: when the brothers split up, the camera goes with whoever went off.
// Each frame we keep a smoothed "how much has this player been moving" value; the moment the
// separation passes kZoomFull, the brother who has been moving more is the leader, and the camera
// stays with him until the two are close again.
static float gMoveP1 = 0.0f, gMoveP2 = 0.0f;
static sead::Vector3f gPrevP1 = {}, gPrevP2 = {};
static bool gHavePrev = false;
static s32 gLeader = -1;  // -1 none, 0 = P1, 1 = P2

static sead::Vector3f FocusPos(al::LiveActor* player, al::LiveActor* hack) {
    return al::getTrans(hack ? hack : player);
}
static float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
static bool Active() { return coop::gP1 != nullptr && coop::gP2 != nullptr && !al::isDead(coop::gP2); }
static float Separation(sead::Vector3f* delta) {
    sead::Vector3f a = al::getTrans(coop::gP1);
    sead::Vector3f b = FocusPos(coop::gP2, coop::P2HackActor());
    *delta = {b.x - a.x, b.y - a.y, b.z - a.z};
    return __builtin_sqrtf(delta->x * delta->x + delta->z * delta->z);
}
static float Dist(const sead::Vector3f& a, const sead::Vector3f& b) {
    float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
    return __builtin_sqrtf(x * x + y * y + z * z);
}
static void Reset() {
    gZoom = 1.0f; gShare = 0.0f; gMoveP1 = gMoveP2 = 0.0f; gHavePrev = false; gLeader = -1;
}
// called once per frame from StageSceneControl
static void Update() {
    if (!Active()) { gLeader = -1; gHavePrev = false; return; }
    sead::Vector3f p1 = al::getTrans(coop::gP1);
    sead::Vector3f p2 = FocusPos(coop::gP2, coop::P2HackActor());
    if (gHavePrev) {
        float m1 = Dist(p1, gPrevP1), m2 = Dist(p2, gPrevP2);
        if (m1 > 200.0f) m1 = 0.0f;  // ignore teleports / recoveries
        if (m2 > 200.0f) m2 = 0.0f;
        gMoveP1 += (m1 - gMoveP1) * 0.05f;  // ~1 second memory
        gMoveP2 += (m2 - gMoveP2) * 0.05f;
    }
    gPrevP1 = p1; gPrevP2 = p2; gHavePrev = true;

    sead::Vector3f d;
    float sep = Separation(&d);
    if (gLeader < 0 && sep > kZoomFull) {
        gLeader = gMoveP2 > gMoveP1 ? 1 : 0;
        COOP_LOG("Camera: brothers split up, following P%d", gLeader + 1);
    } else if (gLeader >= 0 && sep < kZoomFull * 0.8f) {
        gLeader = -1;
        COOP_LOG("Camera: brothers back together, sharing view");
    }
}
}  // namespace coop::cam

HOOK_DEFINE_TRAMPOLINE(CameraTargetHook) {
    static void Callback(const void* target, sead::Vector3f* out) {
        Orig(target, out);
        using namespace coop::cam;
        if (!Active()) { gShare = 0.0f; return; }
        sead::Vector3f d;
        float sep = Separation(&d);
        // share the view 50/50 up to kZoomFull, then move onto the leader by kShareEnd
        float toLeader = Clamp01((sep - kZoomFull) / (kShareEnd - kZoomFull));
        float leaderShare = gLeader == 1 ? 1.0f : 0.0f;
        float want = gLeader < 0 ? 0.5f : 0.5f + (leaderShare - 0.5f) * toLeader;
        gShare += (want - gShare) * kLerp;
        out->x += d.x * gShare;
        out->y += d.y * gShare;
        out->z += d.z * gShare;
    }
};

HOOK_DEFINE_TRAMPOLINE(MakeLookAtHook) {
    static void Callback(const void* poser, void* lookAt) {
        Orig(poser, lookAt);
        using namespace coop::cam;
        float want = 1.0f;
        if (Active()) {
            sead::Vector3f d;
            float sep = Separation(&d);
            want = 1.0f + (kZoomMax - 1.0f) * Clamp01((sep - kZoomStart) / (kZoomFull - kZoomStart));
        }
        gZoom += (want - gZoom) * kLerp;
        if (gZoom < 1.001f) return;
        auto pos = coop::Field<sead::Vector3f>(lookAt, smo100::LookAtCamera_pos);
        auto at = coop::Field<sead::Vector3f>(lookAt, smo100::LookAtCamera_at);
        sead::Vector3f v = {pos.x - at.x, pos.y - at.y, pos.z - at.z};
        float len2 = v.x * v.x + v.y * v.y + v.z * v.z;
        if (len2 < 300.0f * 300.0f) return;  // first-person / very close cameras: leave alone
        sead::Vector3f np = {at.x + v.x * gZoom, at.y + v.y * gZoom, at.z + v.z * gZoom};
        coop::SetField<sead::Vector3f>(lookAt, smo100::LookAtCamera_pos, np);
    }
};

// --------------------------------------------------------------------- per-frame (placement)

HOOK_DEFINE_TRAMPOLINE(StageSceneControl) {
    static void Callback(StageScene* scene) {
        Orig(scene);
        coop::cam::Update();
        if (coop::gP2 == nullptr || coop::gP1 == nullptr) return;

        if (coop::gP2RespawnPending) {
            coop::gP2RespawnPending = false;
            coop::gP2Hp = coop::kP2MaxHp;
            coop::PlaceP2BesideP1("out of hearts");
        }

        if (coop::gP2Placed) return;
        if (++coop::gFramesSinceInit > 60 * 20) { coop::gP2Placed = true; return; }
        if (!rs::isPlayerOnGround(coop::gP1)) return;
        coop::PlaceP2BesideP1("stage start");
        coop::gP2Placed = true;
    }
};

// ------------------------------------------------------------------------------------ install

static bool CheckBytes(ptrdiff_t offset, u64 expected) {
    u64 actual = 0;
    std::memcpy(&actual, reinterpret_cast<const void*>(exl::util::modules::GetTargetStart() + offset),
                sizeof(actual));
    if (actual != expected)
        COOP_LOG("Byte check failed at main+0x%lx: %016lx != %016lx", (long)offset,
                 (unsigned long)actual, (unsigned long)expected);
    return actual == expected;
}

extern "C" void exl_main(void* x0, void* x1) {
    exl::hook::Initialize();

    using namespace smo100;
    bool ok = CheckBytes(StageScene_init, StageScene_init_bytes) &&
              CheckBytes(PlayerActorHakoniwa_initPlayer, PlayerActorHakoniwa_initPlayer_bytes) &&
              CheckBytes(alPlayerFunction_registerPlayer, alPlayerFunction_registerPlayer_bytes) &&
              CheckBytes(StageScene_control, StageScene_control_bytes) &&
              CheckBytes(al_getPlayerActor, al_getPlayerActor_bytes) &&
              CheckBytes(al_tryGetPlayerActor, al_tryGetPlayerActor_bytes) &&
              CheckBytes(PlayerJudgeDead_judge, PlayerJudgeDead_judge_bytes) &&
              CheckBytes(PlayerDamageKeeper_damage, PlayerDamageKeeper_damage_bytes) &&
              CheckBytes(PlayerDamageKeeper_damageForce, PlayerDamageKeeper_damageForce_bytes) &&
              CheckBytes(PlayerDamageKeeper_dead, PlayerDamageKeeper_dead_bytes) &&
              CheckBytes(PlayerFunction_isPlayerHitPointOne, PlayerFunction_isPlayerHitPointOne_bytes) &&
              CheckBytes(PlayerRecoverySafetyPoint_isValid, PlayerRecoverySafetyPoint_isValid_bytes) &&
              CheckBytes(rs_isPlayerEnableToSeeOddSpace, rs_isPlayerEnableToSeeOddSpace_bytes) &&
              CheckBytes(PlayerCameraTarget_calcTrans, PlayerCameraTarget_calcTrans_bytes) &&
              CheckBytes(CameraPoser_makeLookAtCamera, CameraPoser_makeLookAtCamera_bytes);
    if (!ok) {
        COOP_LOG("Not SMO 1.0.0 (hook-site bytes differ). No hooks installed.");
        return;
    }

    StageSceneInit::InstallAtOffset(StageScene_init);
    PlayerInitHook::InstallAtOffset(PlayerActorHakoniwa_initPlayer);
    RegisterPlayerHook::InstallAtOffset(alPlayerFunction_registerPlayer);
    StageSceneControl::InstallAtOffset(StageScene_control);
    GetPlayerActorHook::InstallAtOffset(al_getPlayerActor);
    TryGetPlayerActorHook::InstallAtOffset(al_tryGetPlayerActor);
    JudgeDeadHook::InstallAtOffset(PlayerJudgeDead_judge);
    DamageHook::InstallAtOffset(PlayerDamageKeeper_damage);
    DamageForceHook::InstallAtOffset(PlayerDamageKeeper_damageForce);
    DeadHook::InstallAtOffset(PlayerDamageKeeper_dead);
    HitPointOneHook::InstallAtOffset(PlayerFunction_isPlayerHitPointOne);
    SafetyPointValidHook::InstallAtOffset(PlayerRecoverySafetyPoint_isValid);
    SeeOddSpaceHook::InstallAtOffset(rs_isPlayerEnableToSeeOddSpace);
    CameraTargetHook::InstallAtOffset(PlayerCameraTarget_calcTrans);
    MakeLookAtHook::InstallAtOffset(CameraPoser_makeLookAtCamera);
    COOP_LOG("Hooks installed (SMO 1.0.0, step 4 camera).");
}

extern "C" NORETURN void exl_exception_entry() {
    EXL_ABORT("Default exception handler called!");
}
