#include "lib.hpp"
#include "program/game.hpp"
#include "program/loggers.hpp"

#include <cstring>
#include <new>

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
static bool gGameOver = false;      // both brothers down: real death in progress
static bool gForceP1Death = false;  // send P1 into the vanilla death sequence next frame
static bool gToucherIsP2 = false;   // a pipe is processing P2's touch right now

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
    gGameOver = false;
    gForceP1Death = false;
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

static const al::IUseAreaObj* AreaUser(const al::LiveActor* a) {
    return reinterpret_cast<const al::IUseAreaObj*>(reinterpret_cast<const u8*>(a) +
                                                    smo100::LiveActor_IUseAreaObj);
}

// Find a safe spot beside `anchor`: right, left, behind, front; each must have no wall between the
// anchor and the spot, ground within 600 units below it, and not be inside a death area.
// Falls back to the anchor's own position.
static sead::Vector3f FindSpotBeside(al::LiveActor* anchor) {
    const sead::Vector3f p1 = al::getTrans(anchor);
    sead::Vector3f side;
    al::calcSideDir(&side, anchor);
    const sead::Vector3f dirs[4] = {{side.x, 0, side.z},
                                    {-side.x, 0, -side.z},
                                    {side.z, 0, -side.x},
                                    {-side.z, 0, side.x}};
    for (const auto& d : dirs) {
        const float dist = 150.0f;
        sead::Vector3f eye = {p1.x, p1.y + 60.0f, p1.z};
        sead::Vector3f toSpot = {d.x * dist, 0.0f, d.z * dist};
        sead::Vector3f tmp;
        if (RayHit(anchor, eye, toSpot, &tmp)) continue;  // wall in the way
        sead::Vector3f top = {p1.x + toSpot.x, p1.y + 150.0f, p1.z + toSpot.z};
        sead::Vector3f down = {0.0f, -600.0f, 0.0f};
        sead::Vector3f ground;
        if (!RayHit(anchor, top, down, &ground)) continue;  // no floor (ledge / pit)
        sead::Vector3f spot = {ground.x, ground.y + 5.0f, ground.z};
        if (al::isInDeathArea(AreaUser(anchor), spot)) continue;  // floor is inside a kill volume
        return spot;
    }
    return {p1.x, p1.y + 5.0f, p1.z};
}
static sead::Vector3f FindSpotBesideP1() { return FindSpotBeside(gP1); }

// Actor a player is currently capturing (nullptr if none).
static al::LiveActor* HackActorOf(al::LiveActor* player) {
    if (!player) return nullptr;
    void* keeper = Field<void*>(player, smo100::PlayerActorHakoniwa_hackKeeper);
    return keeper ? Field<al::LiveActor*>(keeper, smo100::PlayerHackKeeper_hackActor) : nullptr;
}

// ---- rescue bubble: reuse the game's own pit-recovery (verified in Ghidra) ----
// checkDeathArea's recovery branch calls a local routine at main+0x4273f4 with
// (player, player+0x148 HackCap, +0x1f8 carry keeper, +0x1f0 bind keeper, +0x200 equipment user,
//  +0x3b8 PlayerStateAbyss). It cancels carry/bind/equip, prepares PlayerStateAbyss recovery and
// switches the player to the Abyss nerve, which floats the player in the bubble to the safety
// point (PlayerActorHakoniwa+0x270). We point the safety point beside the other brother first.
static s32 gForcedRecovery[2] = {0, 0};  // frames during which isValid() is forced true
static bool gBubbleRequest[2] = {false, false};
static s32 gBubbleBusy[2] = {0, 0};  // >0 while a bubble is in flight or just landed
// Runs inside the player's own checkDeathArea (same place vanilla starts pit recovery).
static void StartBubbleTo(al::LiveActor* who, al::LiveActor* to, s32 whoIdx) {
    using Fn = void (*)(al::LiveActor*, void*, void*, void*, void*, void*);
    auto fn = reinterpret_cast<Fn>(exl::util::modules::GetTargetStart() + smo100::PlayerRecoveryStart);
    auto* sp = Field<PlayerRecoverySafetyPoint*>(who, smo100::PlayerActorHakoniwa_safetyPoint);
    sead::Vector3f spot = FindSpotBeside(to);
    sead::Vector3f up = {0.0f, 1.0f, 0.0f};
    sp->setSafetyPoint(spot, up, nullptr);
    gForcedRecovery[whoIdx] = 60 * 10;
    gBubbleBusy[whoIdx] = 60 * 10;  // cleared (to a 1 s grace) when the bubble lands
    fn(who, Field<void*>(who, 0x148), Field<void*>(who, 0x1f8), Field<void*>(who, 0x1f0),
       Field<void*>(who, 0x200), Field<void*>(who, 0x3b8));
    COOP_LOG("Bubble: P%d floating to P%d at (%.0f, %.0f, %.0f)", whoIdx + 1,
             2 - whoIdx, spot.x, spot.y, spot.z);
}

// ---- 3D World death rules ----
// A brother who would die goes into a bubble to the other one, as long as the other one is
// available (alive and not in a bubble himself). If the other one is already in a bubble, it's a
// real death: Mario goes through the game's own fall-death sequence (black screen, coins lost,
// restart), which is what the stage reacts to.
static al::LiveActor* PlayerAt(s32 i) { return i == 0 ? gP1 : gP2; }
static bool PartnerAvailable(s32 i) {
    al::LiveActor* other = PlayerAt(1 - i);
    if (other == nullptr || PlayerAt(i) == nullptr || gGameOver) return false;
    return !al::isDead(other) && gBubbleBusy[1 - i] <= 60;  // >60 = bubble in flight
}
static void RequestBubble(s32 i, const char* why) {
    if (gBubbleRequest[i] || gBubbleBusy[i] > 60) return;
    gBubbleRequest[i] = true;
    gForcedRecovery[i] = 60 * 10;
    gBubbleBusy[i] = 60 * 10;
    COOP_LOG("Bubble requested for P%d (%s)", i + 1, why);
}
static void RequestGameOver(const char* why) {
    if (gGameOver) return;
    gGameOver = true;
    gForceP1Death = true;
    COOP_LOG("Both brothers down (%s): real death", why);
}
static void CallRecoveryRoutine(al::LiveActor* who) {
    using Fn = void (*)(al::LiveActor*, void*, void*, void*, void*, void*);
    auto fn = reinterpret_cast<Fn>(exl::util::modules::GetTargetStart() + smo100::PlayerRecoveryStart);
    fn(who, Field<void*>(who, 0x148), Field<void*>(who, 0x1f8), Field<void*>(who, 0x1f0),
       Field<void*>(who, 0x200), Field<void*>(who, 0x3b8));
}

static s32 gIgnoreJumps = 0;  // frames during which position jumps are our own teleports
static void PlaceBeside(al::LiveActor* who, al::LiveActor* anchor, const char* why) {
    sead::Vector3f dst = FindSpotBeside(anchor);
    al::resetPosition(who, dst);
    al::setVelocityZero(who);
    gIgnoreJumps = 3;
    COOP_LOG("P%d placed beside P%d (%s) at (%.0f, %.0f, %.0f)", who == gP1 ? 1 : 2,
             anchor == gP1 ? 1 : 2, why, dst.x, dst.y, dst.z);
}
static void PlaceP2BesideP1(const char* why) {
    gIgnoreJumps = 3;
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
namespace coop::echo {
static void Reset();
static void Install(al::LiveActor* player, s32 idx);
}  // namespace coop::echo
namespace coop::hud {
static void Reset();
}  // namespace coop::hud

// ---------------------------------------------------------------------------- spawning (step 2)

HOOK_DEFINE_TRAMPOLINE(StageSceneInit) {
    static void Callback(StageScene* scene, const al::SceneInitInfo& info) {
        coop::ResetStageState();
        coop::cam::Reset();
        coop::echo::Reset();
        coop::hud::Reset();
        coop::gInStageSceneInit = true;
        Orig(scene, info);
        coop::gInStageSceneInit = false;
        if (coop::gP1 != nullptr && coop::gP2 != nullptr) {
            coop::echo::Install(coop::gP1, 0);
            coop::echo::Install(coop::gP2, 1);
        }
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
        // Note: PlayerInitInfo+0x45 only means "this stage has a wardrobe" (shops, the Odyssey);
        // it is passed to the model init. P2 is spawned there too.

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
    if (coop::gToucherIsP2) return coop::gP2;
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
            coop::gP2Hp = coop::kP2MaxHp;
            if (coop::PartnerAvailable(1)) coop::RequestBubble(1, "out of hearts");
            else coop::RequestGameOver("P2 out of hearts while P1 in a bubble");
        }
    }
}

static bool IsP1Keeper(void* keeper) {
    return coop::gP1 != nullptr && coop::gP2 != nullptr && keeper != nullptr &&
           coop::Field<al::LiveActor*>(keeper, 0x0) == coop::gP1;
}
static bool P1OnLastHeart();  // defined after HitPointOneHook

// P1's last heart: refill and bubble to P2 instead of dying (if P2 is available).
static bool TrySaveP1(void* keeper, s32 damageType, const char* why, bool startBubble) {
    if (!IsP1Keeper(keeper) || !P1OnLastHeart() || !coop::PartnerAvailable(0)) return false;
    coop::SetField<u8>(keeper, 0x10, 1);
    coop::SetField<s32>(keeper, 0x14, damageType);
    GameDataFunction::recoveryPlayerMax(coop::gP1);
    if (startBubble) coop::RequestBubble(0, why);  // pits/quicksand: vanilla recovery already bubbles
    else COOP_LOG("P1 on last heart in a pit: refilled, game's own bubble takes him to P2");
    return true;
}

HOOK_DEFINE_TRAMPOLINE(DamageHook) {
    static void Callback(void* keeper, s32 damageType) {
        if (coop::Field<u8>(keeper, 0x1c) == 0 && TrySaveP1(keeper, damageType, "out of hearts", true))
            return;
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
        if (TrySaveP1(keeper, damageType, "last heart in a pit", false)) return;
        if (!IsP2Keeper(keeper)) return Orig(keeper, damageType);
        // vanilla only uses damageForce for the pit/quicksand "lose a heart and recover" path
        P2TakeHit(keeper, damageType, true);
    }
};

HOOK_DEFINE_TRAMPOLINE(DeadHook) {
    static void Callback(void* keeper) {
        if (!IsP2Keeper(keeper)) return Orig(keeper);
        if (coop::PartnerAvailable(1)) coop::RequestBubble(1, "would have died");
        else coop::RequestGameOver("P2 died while P1 in a bubble");
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
        // P1 takes the "lose a heart and recover" path while P2 can catch him
        if (coop::gP1 != nullptr && actor == coop::gP1 && coop::PartnerAvailable(0)) return false;
        return Orig(actor);
    }
};
static bool P1OnLastHeart() { return HitPointOneHook::Orig(coop::gP1); }

HOOK_DEFINE_TRAMPOLINE(SafetyPointValidHook) {
    static bool Callback(PlayerRecoverySafetyPoint* sp) {
        auto* owner = coop::Field<al::LiveActor*>(sp, smo100::PlayerRecoverySafetyPoint_player);
        if (coop::gP2 == nullptr || coop::gP1 == nullptr) return Orig(sp);
        sead::Vector3f up = {0.0f, 1.0f, 0.0f};
        if (coop::gGameOver) return Orig(sp);
        if (owner == coop::gP2) {
            sead::Vector3f spot = coop::FindSpotBeside(coop::gP1);
            sp->setSafetyPoint(spot, up, nullptr);
            return true;
        }
        if (owner == coop::gP1 && (coop::gForcedRecovery[0] > 0 || coop::PartnerAvailable(0))) {
            // P1 is bubbled to P2 (off-screen rule, or pit/quicksand while P2 can catch him)
            sead::Vector3f spot = coop::FindSpotBeside(coop::gP2);
            sp->setSafetyPoint(spot, up, nullptr);
            return true;
        }
        return Orig(sp);
    }
};

// --------------------------------------------------------- pipes & doors: follow with animation
// The pipe/door moves the brother who entered purely through IUsePlayerPuppet virtual calls. Both
// brothers' puppets get a copy of the vtable whose mutating slots record the call. While a brother
// travels through an in-stage pipe or door, every call is replayed on the other brother's puppet
// 25 frames later, so he walks in, travels and pops out right behind with the same animation.
namespace coop::echo {
constexpr s32 kDelay = 25;
constexpr s32 kSlots = 38;
enum Kind : u8 { K0, KVec3, KQuat, KStr, KFloat, KVec3Int, KFlags2D };
struct Entry { u32 frame; u16 slot; Kind kind; float f[4]; s32 i; const char* str; void* strVptr; };
struct SafeStr { void* vptr; const char* str; };
static Entry gQ[1024];
static u32 gHead = 0, gTail = 0;
static u64 gTable[2 + kSlots];
static void* gOrig[kSlots];
static bool gTableReady = false;
static void* gPup[2] = {nullptr, nullptr};
static s32 gLeader = -1;
static u32 gFrame = 0, gStart = 0;
static bool gFollowerStarted = false, gStartReq = false;
static s32 gDepth = 0;

static bool Active() { return gLeader >= 0; }
static s32 LeaderIdx() { return gLeader; }

// 2D sections: the pipe switches the player between 3D and 2D with PuppetFunction2D3D helpers,
// which are not virtual calls but plain flag writes (verified in Ghidra): validate2D ->
// *(puppet+0x28)+0x8, keepOn2D/endKeepOn2D -> *(puppet+0x28)+0xb, snap2D -> puppet+0xae.
// They are sampled on the leader every frame and replayed like the other calls.
static s32 gLastFlags = -1;
static s32 ReadFlags2D(void* pup) {
    void* dim = coop::Field<void*>(pup, 0x28);
    if (dim == nullptr) return -1;
    return coop::Field<u8>(dim, 0x8) | (coop::Field<u8>(dim, 0xb) << 8) |
           (coop::Field<u8>(pup, 0xae) << 16);
}
static void WriteFlags2D(void* pup, s32 flags) {
    void* dim = coop::Field<void*>(pup, 0x28);
    if (dim == nullptr || flags < 0) return;
    coop::SetField<u8>(dim, 0x8, (u8)(flags & 0xff));
    coop::SetField<u8>(dim, 0xb, (u8)((flags >> 8) & 0xff));
    coop::SetField<u8>(pup, 0xae, (u8)((flags >> 16) & 0xff));
}
static bool IsBinding(void* pup) {
    return coop::Field<void*>(pup, 0x70) != nullptr && coop::Field<void*>(pup, 0x78) != nullptr;
}
static void End(const char* why) {
    if (gLeader >= 0) COOP_LOG("Pipe/door follow ended (%s)", why);
    gLeader = -1; gHead = gTail = 0; gStartReq = false; gFollowerStarted = false;
    coop::gIgnoreJumps = 10;
}
static void Reset() { End("stage change"); gPup[0] = gPup[1] = nullptr; gDepth = 0; }

static Entry* Push(void* self, s32 slot, Kind kind) {
    if (gLeader < 0 || self != gPup[gLeader] || gDepth > 0) return nullptr;
    if (gHead - gTail >= 1024) return nullptr;
    Entry* e = &gQ[gHead++ % 1024];
    *e = {};
    e->frame = gFrame; e->slot = (u16)slot; e->kind = kind;
    return e;
}

template <int S> static void T0(void* self) {
    Push(self, S, K0);
    gDepth++; ((void (*)(void*))gOrig[S])(self); gDepth--;
}
template <int S> static void TV3(void* self, const sead::Vector3f& v) {
    if (Entry* e = Push(self, S, KVec3)) { e->f[0] = v.x; e->f[1] = v.y; e->f[2] = v.z; }
    gDepth++; ((void (*)(void*, const sead::Vector3f&))gOrig[S])(self, v); gDepth--;
}
template <int S> static void TQ(void* self, const float* q) {
    if (Entry* e = Push(self, S, KQuat)) std::memcpy(e->f, q, sizeof(float) * 4);
    gDepth++; ((void (*)(void*, const float*))gOrig[S])(self, q); gDepth--;
}
template <int S> static void TStr(void* self, const SafeStr& str) {
    if (Entry* e = Push(self, S, KStr)) { e->str = str.str; e->strVptr = str.vptr; }
    gDepth++; ((void (*)(void*, const SafeStr&))gOrig[S])(self, str); gDepth--;
}
template <int S> static void TF(void* self, float f) {
    if (Entry* e = Push(self, S, KFloat)) e->f[0] = f;
    gDepth++; ((void (*)(void*, float))gOrig[S])(self, f); gDepth--;
}
template <int S> static void TV3I(void* self, const sead::Vector3f& v, s32 i) {
    if (Entry* e = Push(self, S, KVec3Int)) { e->f[0] = v.x; e->f[1] = v.y; e->f[2] = v.z; e->i = i; }
    gDepth++; ((void (*)(void*, const sead::Vector3f&, s32))gOrig[S])(self, v, i); gDepth--;
}

static bool IsInStagePipeOrDoor(al::HitSensor* binder) {
    al::LiveActor* host = binder ? al::getSensorHost(binder) : nullptr;
    if (!host) return false;
    uintptr_t vptr = coop::Field<uintptr_t>(host, 0) - exl::util::modules::GetTargetStart();
    return vptr == (uintptr_t)smo100::Vptr_Dokan || vptr == (uintptr_t)smo100::Vptr_DokanMaze ||
           vptr == (uintptr_t)smo100::Vptr_DoorWarp;
}

static void TStart(void* self, al::HitSensor* binder, al::HitSensor* player) {
    ((void (*)(void*, al::HitSensor*, al::HitSensor*))gOrig[0])(self, binder, player);
    if (gLeader >= 0 || coop::gP1 == nullptr || coop::gP2 == nullptr || coop::gGameOver) return;
    s32 idx = self == gPup[0] ? 0 : (self == gPup[1] ? 1 : -1);
    if (idx < 0 || !IsInStagePipeOrDoor(binder)) return;
    gLeader = idx; gStart = gFrame; gHead = gTail = 0;
    gFollowerStarted = false; gStartReq = false;
    gLastFlags = ReadFlags2D(self);
    COOP_LOG("Pipe/door: P%d went in, P%d follows %d frames behind", idx + 1, 2 - idx, kDelay);
}

static void Install(al::LiveActor* player, s32 idx) {
    void* pup = coop::Field<void*>(player, smo100::PlayerActorHakoniwa_puppet);
    gPup[idx] = nullptr;
    if (pup == nullptr) return;
    uintptr_t base = exl::util::modules::GetTargetStart();
    u64* vt = coop::Field<u64*>(pup, 0);
    if (!gTableReady) {
        if ((uintptr_t)vt != base + smo100::Vtbl_PlayerPuppet ||
            vt[0] != base + smo100::PlayerPuppet_start_fn) {
            COOP_LOG("PlayerPuppet vtable not as expected; pipe follow disabled");
            return;
        }
        std::memcpy(gTable, vt - 2, sizeof(gTable));
        for (s32 i = 0; i < kSlots; i++) gOrig[i] = (void*)vt[i];
        u64* t = gTable + 2;
        t[0] = (u64)&TStart;
        t[1] = (u64)&T0<1>;    t[2] = (u64)&T0<2>;
        t[3] = (u64)&TV3<3>;   t[4] = (u64)&TQ<4>;   t[5] = (u64)&TV3<5>;  t[6] = (u64)&TV3<6>;
        t[12] = (u64)&TStr<12>; t[15] = (u64)&TF<15>;
        t[17] = (u64)&T0<17>;  t[18] = (u64)&T0<18>; t[20] = (u64)&T0<20>; t[21] = (u64)&T0<21>;
        t[22] = (u64)&T0<22>;  t[23] = (u64)&T0<23>; t[27] = (u64)&T0<27>; t[29] = (u64)&TV3I<29>;
        t[31] = (u64)&T0<31>;  t[32] = (u64)&T0<32>; t[36] = (u64)&T0<36>; t[37] = (u64)&T0<37>;
        gTableReady = true;
    } else if ((uintptr_t)vt != base + smo100::Vtbl_PlayerPuppet) {
        return;
    }
    coop::SetField<u64*>(pup, 0, gTable + 2);
    gPup[idx] = pup;
}

static void Replay(const Entry& e, void* fp) {
    sead::Vector3f v = {e.f[0], e.f[1], e.f[2]};
    switch (e.kind) {
    case K0:
        // a cancel on the leader ends the follower's bind properly (end = BindEnd msg + cancel)
        ((void (*)(void*))gOrig[e.slot == 2 ? 1 : e.slot])(fp);
        break;
    case KVec3: ((void (*)(void*, const sead::Vector3f&))gOrig[e.slot])(fp, v); break;
    case KQuat: ((void (*)(void*, const float*))gOrig[e.slot])(fp, e.f); break;
    case KStr: {
        SafeStr str = {e.strVptr, e.str};
        ((void (*)(void*, const SafeStr&))gOrig[e.slot])(fp, str);
        break;
    }
    case KFloat: ((void (*)(void*, float))gOrig[e.slot])(fp, e.f[0]); break;
    case KFlags2D: WriteFlags2D(fp, e.i); break;
    case KVec3Int: ((void (*)(void*, const sead::Vector3f&, s32))gOrig[e.slot])(fp, v, e.i); break;
    }
}

// Runs inside the follower's own update (checkDeathArea hook): put him into the game's Bind state
// bound to his own body sensor (so nothing is ever sent to the pipe on his behalf).
static bool TryStartFollower(al::LiveActor* actor) {
    if (gLeader < 0 || !gStartReq || actor != coop::PlayerAt(1 - gLeader)) return false;
    gStartReq = false;
    s32 f = 1 - gLeader;
    void* pup = gPup[f];
    void* keeper = coop::Field<void*>(actor, smo100::PlayerActorHakoniwa_bindKeeper);
    if (pup == nullptr || keeper == nullptr || coop::HackActorOf(actor) != nullptr ||
        coop::gBubbleBusy[f] > 0 || coop::gGameOver || al::isDead(actor) || IsBinding(pup) ||
        coop::Field<void*>(keeper, 0x8) != nullptr) {
        End("follower busy");
        return false;
    }
    auto* body = coop::Field<al::HitSensor*>(keeper, 0x0);
    coop::SetField<al::HitSensor*>(keeper, 0x8, body);
    coop::SetField<u8>(keeper, 0x24, 0);
    ((void (*)(void*, al::HitSensor*, al::HitSensor*))gOrig[0])(pup, body, body);
    void* in = coop::Field<void*>(actor, smo100::PlayerActorHakoniwa_input138);
    if (in) { coop::SetField<u8>(in, 0x9b, 1); coop::SetField<u8>(in, 0xcc, 1); }
    al::setNerve(reinterpret_cast<al::IUseNerve*>(actor),
                 reinterpret_cast<const al::Nerve*>(exl::util::modules::GetTargetStart() +
                                                    smo100::NrvPlayerBind));
    gFollowerStarted = true;
    COOP_LOG("Pipe/door: P%d now following through", f + 1);
    return true;
}

// once per frame
static void Update() {
    gFrame++;
    if (gLeader < 0) return;
    if (gFrame - gStart > 60 * 30) {
        void* fp = gPup[1 - gLeader];
        if (gFollowerStarted && fp && IsBinding(fp)) ((void (*)(void*))gOrig[1])(fp);
        End("timeout");
        return;
    }
    {
        s32 flags = ReadFlags2D(gPup[gLeader]);
        if (flags != gLastFlags) {
            gLastFlags = flags;
            if (Entry* e = Push(gPup[gLeader], 0xff, KFlags2D)) e->i = flags;
            COOP_LOG("Pipe/door: leader 2D flags now %06x", flags);
        }
    }
    if (!gFollowerStarted) {
        if (gFrame - gStart >= (u32)kDelay) gStartReq = true;
        return;
    }
    void* fp = gPup[1 - gLeader];
    if (!IsBinding(fp)) { End("follower left the bind"); return; }
    while (gTail != gHead && gQ[gTail % 1024].frame + kDelay <= gFrame) {
        Entry e = gQ[gTail++ % 1024];
        Replay(e, fp);
        if (e.kind == K0 && (e.slot == 1 || e.slot == 2)) { End("follower came out"); return; }
    }
}
}  // namespace coop::echo

// Vanilla pipes only ever bind player 0 (rs::requestBindPlayer -> getPlayerActor(pipe, 0)). While
// a pipe handles a touch from P2, "player 0" means P2, so P2 can enter pipes too.
HOOK_DEFINE_TRAMPOLINE(DokanAttackHook) {
    static void Callback(void* self, al::HitSensor* own, al::HitSensor* other) {
        coop::gToucherIsP2 = coop::gP2 != nullptr && other != nullptr &&
                             al::getSensorHost(other) == coop::gP2;
        Orig(self, own, other);
        coop::gToucherIsP2 = false;
    }
};
HOOK_DEFINE_TRAMPOLINE(DokanStageChangeAttackHook) {
    static void Callback(void* self, al::HitSensor* own, al::HitSensor* other) {
        coop::gToucherIsP2 = coop::gP2 != nullptr && other != nullptr &&
                             al::getSensorHost(other) == coop::gP2;
        Orig(self, own, other);
        coop::gToucherIsP2 = false;
    }
};

// Bubble requests are executed from inside the player's own checkDeathArea(), exactly where the
// vanilla pit-recovery branch runs (it returns true after starting recovery). Calling the recovery
// routine from outside the player's update (first attempt) did not stick.
HOOK_DEFINE_TRAMPOLINE(CheckDeathAreaHook) {
    static bool Callback(PlayerActorHakoniwa* self) {
        auto* actor = reinterpret_cast<al::LiveActor*>(self);
        if (coop::gP1 != nullptr && coop::gP2 != nullptr) {
            if (coop::echo::TryStartFollower(actor)) return true;
            if (actor == coop::gP1 && coop::gForceP1Death) {
                // vanilla fall-death: life to zero + Abyss state with no valid safety point
                coop::gForceP1Death = false;
                coop::gForcedRecovery[0] = 0;
                DeadHook::Orig(coop::Field<void*>(actor, smo100::PlayerActorHakoniwa_damageKeeper));
                coop::CallRecoveryRoutine(actor);
                COOP_LOG("P1 sent into the game's death sequence");
                return true;
            }
            if (actor == coop::gP2 && !coop::gGameOver && al::isInDeathArea(actor) &&
                !coop::PartnerAvailable(1))
                coop::RequestGameOver("P2 fell while P1 in a bubble");
            s32 idx = actor == coop::gP1 ? 0 : (actor == coop::gP2 ? 1 : -1);
            if (idx >= 0 && coop::gBubbleRequest[idx] && !coop::gGameOver) {
                coop::gBubbleRequest[idx] = false;
                coop::StartBubbleTo(actor, idx == 0 ? coop::gP2 : coop::gP1, idx);
                return true;
            }
        }
        return Orig(self);
    }
};

// Faster co-op bubble: the game's rescue bubble rises for 45 frames and then flies 60-120 frames.
// For bubbles started by the co-op off-screen rule: rise 24 frames, fly in 30-60 frames (half).
constexpr s32 kCoopRiseFrames = 24;
constexpr s32 kCoopFlyMin = 30;
constexpr s32 kCoopFlyMax = 60;
static s32 CoopBubbleIndexOfState(const void* state) {
    auto* actor = coop::Field<al::LiveActor*>(state, smo100::ActorStateBase_actor);
    for (s32 i = 0; i < 2; i++) {
        al::LiveActor* p = i == 0 ? coop::gP1 : coop::gP2;
        if (p != nullptr && actor == p && coop::gForcedRecovery[i] > 0) return i;
    }
    return -1;
}

HOOK_DEFINE_TRAMPOLINE(RecoveryStartHook) {
    static void Callback(void* state) {
        if (al::isFirstStep(reinterpret_cast<al::IUseNerve*>(state))) {
            auto* a = coop::Field<al::LiveActor*>(state, smo100::ActorStateBase_actor);
            for (s32 i = 0; i < 2; i++)
                if (a != nullptr && a == coop::PlayerAt(i) && coop::gBubbleBusy[i] < 60 * 10)
                    coop::gBubbleBusy[i] = 60 * 10;  // any bubble (pit too) counts as "in a bubble"
        }
        Orig(state);
        if (CoopBubbleIndexOfState(state) < 0) return;
        auto* nerveUser = reinterpret_cast<al::IUseNerve*>(state);
        if (al::getNerveStep(nerveUser) >= kCoopRiseFrames) {
            auto* nrv = reinterpret_cast<const al::Nerve*>(exl::util::modules::GetTargetStart() +
                                                           smo100::NrvRecoveryDeadRecovery);
            al::setNerve(nerveUser, nrv);
        }
    }
};

// Which brother a bubble should end next to: P2's bubbles (pit/quicksand or off-screen) always go
// to P1; P1 only gets a co-op bubble (off-screen rule) and goes to P2.
static al::LiveActor* BubbleTargetOfState(const void* state) {
    auto* actor = coop::Field<al::LiveActor*>(state, smo100::ActorStateBase_actor);
    if (coop::gP1 == nullptr || coop::gP2 == nullptr) return nullptr;
    if (actor == coop::gP2) return coop::gP1;
    if (actor == coop::gP1 && coop::gForcedRecovery[0] > 0) return coop::gP2;
    return nullptr;
}

// The bubble's path is fixed on its first frame (start +0x50, up +0x5c, horizontal offset +0x68,
// vertical arc a*t^2 + b*t with a=+0x48, b=+0x4c, a+b = vertical offset; verified in Ghidra), so a
// brother who keeps moving would be met at his OLD spot. Every few frames we re-aim the safety point
// at the brother's current position and update the horizontal offset and arc end height.
// Smooth re-aiming: on the first frame we pick a safe spot beside the brother and remember its
// offset from him; afterwards the aim point is (brother's position + that offset), eased toward
// every frame, so the bubble curves smoothly instead of hopping between recalculated spots.
static sead::Vector3f gAimOffset[2] = {};
static sead::Vector3f gAim[2] = {};

static void ApplyBubbleAim(void* state, const sead::Vector3f& spot) {
    auto* sp = coop::Field<PlayerRecoverySafetyPoint*>(state, smo100::RecoveryDead_safetyPoint);
    sead::Vector3f up1 = {0.0f, 1.0f, 0.0f};
    sp->setSafetyPoint(spot, up1, nullptr);
    auto start = coop::Field<sead::Vector3f>(state, smo100::RecoveryDead_start);
    auto up = coop::Field<sead::Vector3f>(state, smo100::RecoveryDead_up);
    sead::Vector3f v = {spot.x - start.x, spot.y - start.y, spot.z - start.z};
    float vert = v.x * up.x + v.y * up.y + v.z * up.z;
    sead::Vector3f horiz = {v.x - up.x * vert, v.y - up.y * vert, v.z - up.z * vert};
    coop::SetField<sead::Vector3f>(state, smo100::RecoveryDead_horiz, horiz);
    float a = coop::Field<float>(state, smo100::RecoveryDead_arcA);
    float b = coop::Field<float>(state, smo100::RecoveryDead_arcB);
    coop::SetField<float>(state, smo100::RecoveryDead_arcB, b + (vert - (a + b)));
}

static s32 BubbleSlot(const void* state) {
    auto* actor = coop::Field<al::LiveActor*>(state, smo100::ActorStateBase_actor);
    return actor == coop::gP1 ? 0 : 1;
}

HOOK_DEFINE_TRAMPOLINE(RecoveryFlyHook) {
    static void Callback(void* state) {
        auto* nerveUser = reinterpret_cast<al::IUseNerve*>(state);
        bool first = al::isFirstStep(nerveUser);
        al::LiveActor* target = BubbleTargetOfState(state);
        s32 slot = BubbleSlot(state);
        if (!first && target != nullptr) {
            const sead::Vector3f& t = al::getTrans(target);
            sead::Vector3f want = {t.x + gAimOffset[slot].x, t.y + gAimOffset[slot].y,
                                   t.z + gAimOffset[slot].z};
            sead::Vector3f& aim = gAim[slot];
            aim.x += (want.x - aim.x) * 0.2f;
            aim.y += (want.y - aim.y) * 0.2f;
            aim.z += (want.z - aim.z) * 0.2f;
            ApplyBubbleAim(state, aim);
        }
        Orig(state);
        if (!first) return;
        if (target != nullptr) {
            sead::Vector3f spot = coop::FindSpotBeside(target);
            const sead::Vector3f& t = al::getTrans(target);
            gAimOffset[slot] = {spot.x - t.x, spot.y - t.y, spot.z - t.z};
            gAim[slot] = spot;
            ApplyBubbleAim(state, spot);
        }
        if (CoopBubbleIndexOfState(state) < 0) return;
        s32 fly = coop::Field<s32>(state, smo100::RecoveryDead_flightFrames) / 2;
        if (fly < kCoopFlyMin) fly = kCoopFlyMin;
        if (fly > kCoopFlyMax) fly = kCoopFlyMax;
        s32 spin = coop::Field<s32>(state, smo100::RecoveryDead_spinFrames) / 2;
        if (spin < 1) spin = 1;
        coop::SetField<s32>(state, smo100::RecoveryDead_flightFrames, fly);
        coop::SetField<s32>(state, smo100::RecoveryDead_spinFrames, spin);
    }
};

// Bubble landed: allow new bubbles for that brother again after a 1 s grace.
HOOK_DEFINE_TRAMPOLINE(EndRecoveryHook) {
    static void Callback(PlayerRecoverySafetyPoint* sp) {
        Orig(sp);
        auto* owner = coop::Field<al::LiveActor*>(sp, smo100::PlayerRecoverySafetyPoint_player);
        for (s32 i = 0; i < 2; i++) {
            al::LiveActor* p = i == 0 ? coop::gP1 : coop::gP2;
            if (p != nullptr && owner == p && coop::gBubbleBusy[i] > 60) {
                coop::gBubbleBusy[i] = 60;
                COOP_LOG("Bubble: P%d landed", i + 1);
            }
        }
    }
};

// Crushed: executePreMovementNerveChange kills a player when rs::isPressedCollision(player+0x170)
// is true. If the partner can catch him, bubble out of the crush instead.
HOOK_DEFINE_TRAMPOLINE(PressedHook) {
    static bool Callback(const void* collision) {
        bool pressed = Orig(collision);
        if (!pressed || coop::gGameOver || coop::gP1 == nullptr || coop::gP2 == nullptr)
            return pressed;
        s32 idx = -1;
        for (s32 i = 0; i < 2; i++)
            if (collision == coop::Field<void*>(coop::PlayerAt(i),
                                                smo100::PlayerActorHakoniwa_collision))
                idx = i;
        if (idx < 0) return pressed;
        if (coop::PartnerAvailable(idx)) {
            coop::RequestBubble(idx, "crushed");
            return false;
        }
        if (idx == 1) {
            coop::RequestGameOver("P2 crushed while P1 in a bubble");
            return false;
        }
        return pressed;  // P1 crushed while P2 is in a bubble: vanilla death
    }
};

// Hearts: LifeUpItem::get -> isPlayerHitPointMax / recoveryPlayer on the shared counter. When P2
// picked it up, use P2's own hearts instead.
static bool gPickerIsP2 = false;
HOOK_DEFINE_TRAMPOLINE(LifeUpGetHook) {
    static void Callback(al::LiveActor* item) {
        auto* sensor = coop::Field<al::HitSensor*>(item, smo100::LifeUpItem_pickerSensor);
        gPickerIsP2 = coop::gP2 != nullptr && sensor != nullptr && al::getSensorHost(sensor) == coop::gP2;
        Orig(item);
        gPickerIsP2 = false;
    }
};
HOOK_DEFINE_TRAMPOLINE(HitPointMaxHook) {
    static bool Callback(void* accessor) {
        if (gPickerIsP2) return coop::gP2Hp >= coop::kP2MaxHp;
        return Orig(accessor);
    }
};
HOOK_DEFINE_TRAMPOLINE(RecoveryPlayerHook) {
    static void Callback(const al::LiveActor* item) {
        if (!gPickerIsP2) return Orig(item);
        if (coop::gP2Hp < coop::kP2MaxHp) coop::gP2Hp++;
        COOP_LOG("P2 picked up a heart, HP now %d", coop::gP2Hp);
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
constexpr float kZoomFull = 2000.0f;   // separation where zoom-out is at maximum
constexpr float kLeaderSep = 2000.0f;  // split-up distance: camera picks the leader
constexpr float kLeaderRelease = 1500.0f;  // back together: shared view again
constexpr float kBubbleSep = 1500.0f;  // minimum separation for a bubble (never when close)
constexpr s32 kBubbleDelay = 60;       // frames off-screen before bubbling (1 s grace)
constexpr s32 kBubbleCooldown = 90;    // ~1.5 s: lets the bubble arrive before re-checking
constexpr float kZoomMax = 1.9f;       // max distance multiplier
constexpr float kLerp = 0.08f;         // per-frame smoothing (zoom)
constexpr float kShareLerp = 0.12f;    // per-frame smoothing (who the camera looks at)
static s32 gFarFrames = 0;
static s32 gFarIdx = -1;
constexpr float kWarpJump = 500.0f;    // per-frame move that can only be a teleport
static sead::Vector3f gPrevRaw[2] = {};
static bool gPrevHack[2] = {false, false};
static bool gHaveRaw = false;
static s32 gWarpFollow = -1;           // brother who still has to follow through a pipe/door
static s32 gWarpWait = 0;
static s32 gBubbleCooldown = 0;
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
// Off-screen test against the real scene camera: behind the camera, or projected outside the screen.
static bool IsOffScreen(al::LiveActor* actor) {
    auto* camUser = reinterpret_cast<const al::IUseCamera*>(reinterpret_cast<const u8*>(actor) +
                                                            smo100::LiveActor_IUseCamera);
    sead::Vector3f p = al::getTrans(actor);
    p.y += 80.0f;  // roughly the middle of the body, not the feet
    const sead::Vector3f& cp = al::getCameraPos(camUser, 0);
    const sead::Vector3f& ca = al::getCameraAt(camUser, 0);
    float fx = ca.x - cp.x, fy = ca.y - cp.y, fz = ca.z - cp.z;
    if ((p.x - cp.x) * fx + (p.y - cp.y) * fy + (p.z - cp.z) * fz <= 0.0f) return true;  // behind
    sead::Vector2f sp;
    al::calcScreenPosFromWorldPos(&sp, camUser, p);
    return !al::isInScreen(sp, 0.0f);
}
static void Reset() {
    gZoom = 1.0f; gShare = 0.0f; gMoveP1 = gMoveP2 = 0.0f; gHavePrev = false; gLeader = -1;
    gFarFrames = 0; gBubbleCooldown = 0;
    gHaveRaw = false; gWarpFollow = -1; gWarpWait = 0;
    coop::gForcedRecovery[0] = coop::gForcedRecovery[1] = 0;
    coop::gBubbleRequest[0] = coop::gBubbleRequest[1] = false;
    coop::gBubbleBusy[0] = coop::gBubbleBusy[1] = 0;
}
// called once per frame from StageSceneControl
static void Update() {
    if (!Active()) { gLeader = -1; gHavePrev = false; gHaveRaw = false; return; }
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

    coop::echo::Update();
    if (coop::echo::Active()) {  // animated follow in progress: no fallback warp, no bubble
        coop::gIgnoreJumps = 10;
        gBubbleCooldown = 90;
        gFarFrames = 0;
        gWarpFollow = -1;
    }
    // Pipes / doors inside a stage: the game teleports the brother who went in (a position jump
    // far beyond any normal movement). The other brother follows him through: he is put beside
    // him as soon as he is out and standing, instead of waiting for the off-screen bubble.
    {
        sead::Vector3f raw[2] = {al::getTrans(coop::gP1), al::getTrans(coop::gP2)};
        bool hack[2] = {coop::HackActorOf(coop::gP1) != nullptr,
                        coop::HackActorOf(coop::gP2) != nullptr};
        if (gHaveRaw && coop::gIgnoreJumps == 0 && !coop::gGameOver && gWarpFollow < 0) {
            for (s32 i = 0; i < 2; i++) {
                if (hack[i] || gPrevHack[i] || coop::gBubbleBusy[i] > 0) continue;
                if (Dist(raw[i], gPrevRaw[i]) > kWarpJump) {
                    gWarpFollow = 1 - i;
                    gWarpWait = 0;
                    COOP_LOG("Warp: P%d went through a pipe/door, P%d follows", i + 1, 2 - i);
                    break;
                }
            }
        }
        if (coop::gIgnoreJumps > 0) coop::gIgnoreJumps--;
        for (s32 i = 0; i < 2; i++) { gPrevRaw[i] = raw[i]; gPrevHack[i] = hack[i]; }
        gHaveRaw = true;

        if (gWarpFollow >= 0) {
            gWarpWait++;
            gBubbleCooldown = 90;  // no off-screen bubble while following through
            gFarFrames = 0;
            al::LiveActor* who = coop::PlayerAt(gWarpFollow);
            al::LiveActor* anchor = coop::PlayerAt(1 - gWarpFollow);
            bool ready = gWarpWait >= 20 && rs::isPlayerOnGround(anchor);
            if (ready || gWarpWait > 60 * 5) {
                if (!hack[gWarpFollow] && coop::gBubbleBusy[gWarpFollow] == 0)
                    coop::PlaceBeside(who, anchor, "followed through pipe/door");
                gWarpFollow = -1;
            }
        }
    }

    sead::Vector3f d;
    float sep = Separation(&d);
    if (gLeader < 0 && sep > kLeaderSep) {
        gLeader = gMoveP2 > gMoveP1 ? 1 : 0;
        COOP_LOG("Camera: brothers split up, following P%d", gLeader + 1);
    } else if (gLeader >= 0 && sep < kLeaderRelease) {
        gLeader = -1;
        COOP_LOG("Camera: brothers back together, sharing view");
    }

    // 3D World: whoever is left behind off-screen is bubbled to the leader.
    for (s32 i = 0; i < 2; i++)
        if (coop::gForcedRecovery[i] > 0) coop::gForcedRecovery[i]--;
    if (gBubbleCooldown > 0) gBubbleCooldown--;
    // As soon as one brother is off-screen (and the other is on-screen), bubble him to the other.
    // A 2-frame confirmation only filters single-frame projection noise (~0.03 s).
    s32 offIdx = -1;
    if (sep > kBubbleSep) {
        bool off1 = IsOffScreen(coop::gP1), off2 = IsOffScreen(coop::gP2);
        if (off1 != off2) offIdx = off1 ? 0 : 1;
    }
    gFarFrames = (offIdx >= 0 && offIdx == gFarIdx) ? gFarFrames + 1 : (offIdx >= 0 ? 1 : 0);
    gFarIdx = offIdx;
    for (s32 i = 0; i < 2; i++)
        if (coop::gBubbleBusy[i] > 0) coop::gBubbleBusy[i]--;
    if (!coop::gGameOver && offIdx >= 0 && gFarFrames >= kBubbleDelay && gBubbleCooldown == 0 &&
        !coop::gBubbleRequest[offIdx] && coop::gBubbleBusy[offIdx] == 0) {
        al::LiveActor* who = offIdx == 0 ? coop::gP1 : coop::gP2;
        al::LiveActor* to = offIdx == 0 ? coop::gP2 : coop::gP1;
        if (coop::HackActorOf(who) == nullptr && coop::HackActorOf(to) == nullptr) {
            coop::gBubbleRequest[offIdx] = true;
            coop::gForcedRecovery[offIdx] = 60 * 10;
            COOP_LOG("Bubble requested for P%d (off-screen)", offIdx + 1);
            gBubbleCooldown = kBubbleCooldown;
            gFarFrames = 0;
        }
    }
}
}  // namespace coop::cam

HOOK_DEFINE_TRAMPOLINE(CameraTargetHook) {
    static void Callback(const void* target, sead::Vector3f* out) {
        Orig(target, out);
        using namespace coop::cam;
        if (!Active()) { gShare = 0.0f; return; }
        sead::Vector3f d;
        Separation(&d);
        // together: look at the midpoint; split up: look at the leader (switches in ~0.5 s)
        float want = gLeader < 0 ? 0.5f : (gLeader == 1 ? 1.0f : 0.0f);
        if (coop::echo::Active()) want = coop::echo::LeaderIdx() == 1 ? 1.0f : 0.0f;
        gShare += (want - gShare) * kShareLerp;
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

// ------------------------------------------------------------------------- P2 hearts on screen
// A second copy of the game's own heart gauge, shown to the left of Mario's whenever his is
// shown, fed from P2's hearts.
namespace coop::hud {
static void* gCtrl = nullptr;        // CounterLifeCtrl of this stage
static CounterLife* gP2Life = nullptr;
static s32 gShownHp = -1;
constexpr float kOffsetX = -150.0f;

static al::LayoutActor* AsLayout(void* p) { return reinterpret_cast<al::LayoutActor*>(p); }
static al::IUseLayout* LayoutOf(void* p) {
    return reinterpret_cast<al::IUseLayout*>(reinterpret_cast<u8*>(p) + 0x8);
}
static al::IUseLayoutAction* ActionOf(void* p) {
    return reinterpret_cast<al::IUseLayoutAction*>(reinterpret_cast<u8*>(p) + 0x10);
}
static void Reset() { gCtrl = nullptr; gP2Life = nullptr; gShownHp = -1; }
static void ApplyCount() {
    float max = al::getActionFrameMax(ActionOf(gP2Life), "Gauge", "Gauge");
    gP2Life->setCount(max - max * ((float)coop::gP2Hp / (float)coop::kP2MaxHp));
    al::setPaneStringFormat(LayoutOf(gP2Life), "TxtLife", "%d", coop::gP2Hp);
    gShownHp = coop::gP2Hp;
}
static void Update() {
    if (gCtrl == nullptr || gP2Life == nullptr) return;
    void* primary = coop::Field<void*>(gCtrl, smo100::CounterLifeCtrl_normalLayout);
    bool show = coop::gP2 != nullptr && primary != nullptr && !al::isDead(AsLayout(primary));
    if (!show) {
        al::killLayoutIfActive(AsLayout(gP2Life));
        return;
    }
    if (al::isDead(AsLayout(gP2Life))) {
        gP2Life->start();
        ApplyCount();
    } else if (gShownHp != coop::gP2Hp) {
        ApplyCount();
    }
    sead::Vector3f t = al::getPaneLocalTrans(LayoutOf(primary), "All");
    t.x += kOffsetX;
    al::setPaneLocalTrans(LayoutOf(gP2Life), "All", t);
}
}  // namespace coop::hud

HOOK_DEFINE_TRAMPOLINE(CounterLifeCtrlCtorHook) {
    static void Callback(void* self, const al::LayoutInitInfo& info, const void* playerHolder,
                         const void* subCamera) {
        Orig(self, info, playerHolder, subCamera);
        coop::hud::gCtrl = self;
        void* mem = ::operator new(0x140);
        coop::hud::gP2Life = new (mem) CounterLife("CounterLifeP2", "CounterLife", info);
        coop::hud::gShownHp = -1;
        COOP_LOG("P2 heart gauge created");
    }
};

// --------------------------------------------------------------------- per-frame (placement)

HOOK_DEFINE_TRAMPOLINE(StageSceneControl) {
    static void Callback(StageScene* scene) {
        Orig(scene);
        coop::hud::Update();
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
              CheckBytes(CameraPoser_makeLookAtCamera, CameraPoser_makeLookAtCamera_bytes) &&
              CheckBytes(PlayerRecoveryStart, PlayerRecoveryStart_bytes) &&
              CheckBytes(PlayerActorHakoniwa_checkDeathArea, PlayerActorHakoniwa_checkDeathArea_bytes) &&
              CheckBytes(PlayerStateRecoveryDead_exeStart, PlayerStateRecoveryDead_exeStart_bytes) &&
              CheckBytes(PlayerStateRecoveryDead_exeRecovery, PlayerStateRecoveryDead_exeRecovery_bytes) &&
              CheckBytes(PlayerRecoverySafetyPoint_endRecovery, PlayerRecoverySafetyPoint_endRecovery_bytes) &&
              CheckBytes(rs_isPressedCollision, rs_isPressedCollision_bytes) &&
              CheckBytes(LifeUpItem_get, LifeUpItem_get_bytes) &&
              CheckBytes(GameDataFunction_recoveryPlayer, GameDataFunction_recoveryPlayer_bytes) &&
              CheckBytes(GameDataFunction_isPlayerHitPointMax, GameDataFunction_isPlayerHitPointMax_bytes) &&
              CheckBytes(Dokan_attackSensor, Dokan_attackSensor_bytes) &&
              CheckBytes(DokanStageChange_attackSensor, DokanStageChange_attackSensor_bytes) &&
              CheckBytes(CounterLifeCtrl_ctor, CounterLifeCtrl_ctor_bytes);
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
    CheckDeathAreaHook::InstallAtOffset(PlayerActorHakoniwa_checkDeathArea);
    RecoveryStartHook::InstallAtOffset(PlayerStateRecoveryDead_exeStart);
    RecoveryFlyHook::InstallAtOffset(PlayerStateRecoveryDead_exeRecovery);
    EndRecoveryHook::InstallAtOffset(PlayerRecoverySafetyPoint_endRecovery);
    PressedHook::InstallAtOffset(rs_isPressedCollision);
    LifeUpGetHook::InstallAtOffset(LifeUpItem_get);
    RecoveryPlayerHook::InstallAtOffset(GameDataFunction_recoveryPlayer);
    HitPointMaxHook::InstallAtOffset(GameDataFunction_isPlayerHitPointMax);
    DokanAttackHook::InstallAtOffset(Dokan_attackSensor);
    DokanStageChangeAttackHook::InstallAtOffset(DokanStageChange_attackSensor);
    CounterLifeCtrlCtorHook::InstallAtOffset(CounterLifeCtrl_ctor);
    COOP_LOG("Hooks installed (SMO 1.0.0, step 4 camera + bubble).");
}

extern "C" NORETURN void exl_exception_entry() {
    EXL_ABORT("Default exception handler called!");
}
