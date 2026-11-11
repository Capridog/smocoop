#include "lib.hpp"
#include "program/game.hpp"
#include "program/loggers.hpp"

#include <cstring>

// ---------------------------------------------------------------------------------------------
// Odyssey Co-op (rebuild) — step 2: spawn Player 2 as a second PlayerActorHakoniwa and give it
// the second controller. Targets SMO 1.0.0 only; refuses to install on any other build.
// ---------------------------------------------------------------------------------------------

namespace coop {
static bool gInStageSceneInit = false;  // only spawn P2 while StageScene::init runs
static bool gSpawningP2 = false;        // re-entrancy guard
static al::LiveActor* gP1 = nullptr;
static al::LiveActor* gP2 = nullptr;
static al::LiveActor* gP2Cap = nullptr;  // P2's own Cappy (HackCap)
static s32 gP2Port = -1;
static bool gP2Placed = false;
static s32 gFramesSinceInit = 0;

template <typename T>
static T Field(const void* base, ptrdiff_t off) {
    T v;
    std::memcpy(&v, reinterpret_cast<const u8*>(base) + off, sizeof(T));
    return v;
}
}  // namespace coop

#define COOP_LOG(...) Logging.Log("[OdysseyCoop] " __VA_ARGS__)

HOOK_DEFINE_TRAMPOLINE(StageSceneInit) {
    static void Callback(StageScene* scene, const al::SceneInitInfo& info) {
        coop::gP1 = nullptr;
        coop::gP2 = nullptr;
        coop::gP2Cap = nullptr;
        coop::gP2Port = -1;
        coop::gP2Placed = false;
        coop::gFramesSinceInit = 0;
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
        COOP_LOG("P2 Cappy: %p", coop::gP2Cap);
        coop::gSpawningP2 = false;
        COOP_LOG("P2 created and initialised: %p (port %d)", p2, p2Port);
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

// Vanilla code asks for "player 0" from the caller's point of view (e.g. Cappy's return target
// is rs::getPlayerHeadPos(cap) -> al::getPlayerActor(cap, 0), verified in Ghidra). When P2 or
// P2's own Cappy asks, the answer must be P2, otherwise P2's Cappy flies back to Mario (P1).
static al::LiveActor* RedirectOwnPlayer(const al::LiveActor* caller, s32 index) {
    if (index == 0 && coop::gP2 != nullptr && caller != nullptr &&
        (caller == coop::gP2 || caller == coop::gP2Cap))
        return coop::gP2;
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

// Place P2 beside P1 once P1 has landed after the stage entrance (entrance demos move P1 after
// StageScene::init, so a spawn offset alone is not enough).
HOOK_DEFINE_TRAMPOLINE(StageSceneControl) {
    static void Callback(StageScene* scene) {
        Orig(scene);
        if (coop::gP2 == nullptr || coop::gP1 == nullptr || coop::gP2Placed) return;
        if (++coop::gFramesSinceInit > 60 * 20) { coop::gP2Placed = true; return; }
        if (!al::isOnGround(coop::gP1, 0)) return;
        const sead::Vector3f& p1 = al::getTrans(coop::gP1);
        sead::Vector3f side;
        al::calcSideDir(&side, coop::gP1);
        sead::Vector3f dst = {p1.x + side.x * 150.0f, p1.y + 20.0f, p1.z + side.z * 150.0f};
        al::resetPosition(coop::gP2, dst);
        al::setVelocityZero(coop::gP2);
        coop::gP2Placed = true;
        COOP_LOG("P2 placed beside P1 at (%.0f, %.0f, %.0f) after %d frames", dst.x, dst.y, dst.z,
                 coop::gFramesSinceInit);
    }
};

static bool CheckBytes(ptrdiff_t offset, u64 expected) {
    u64 actual = 0;
    std::memcpy(&actual, reinterpret_cast<const void*>(exl::util::modules::GetTargetStart() + offset),
                sizeof(actual));
    return actual == expected;
}

extern "C" void exl_main(void* x0, void* x1) {
    exl::hook::Initialize();

    bool ok = CheckBytes(smo100::StageScene_init, smo100::StageScene_init_bytes) &&
              CheckBytes(smo100::PlayerActorHakoniwa_initPlayer,
                         smo100::PlayerActorHakoniwa_initPlayer_bytes) &&
              CheckBytes(smo100::alPlayerFunction_registerPlayer,
                         smo100::alPlayerFunction_registerPlayer_bytes) &&
              CheckBytes(smo100::StageScene_control, smo100::StageScene_control_bytes) &&
              CheckBytes(smo100::al_getPlayerActor, smo100::al_getPlayerActor_bytes) &&
              CheckBytes(smo100::al_tryGetPlayerActor, smo100::al_tryGetPlayerActor_bytes);
    if (!ok) {
        COOP_LOG("Not SMO 1.0.0 (hook-site bytes differ). No hooks installed.");
        return;
    }

    StageSceneInit::InstallAtOffset(smo100::StageScene_init);
    PlayerInitHook::InstallAtOffset(smo100::PlayerActorHakoniwa_initPlayer);
    RegisterPlayerHook::InstallAtOffset(smo100::alPlayerFunction_registerPlayer);
    StageSceneControl::InstallAtOffset(smo100::StageScene_control);
    GetPlayerActorHook::InstallAtOffset(smo100::al_getPlayerActor);
    TryGetPlayerActorHook::InstallAtOffset(smo100::al_tryGetPlayerActor);
    COOP_LOG("Hooks installed (SMO 1.0.0).");
}

extern "C" NORETURN void exl_exception_entry() {
    EXL_ABORT("Default exception handler called!");
}
