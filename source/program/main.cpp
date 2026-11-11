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
static s32 gP2Port = -1;
}  // namespace coop

#define COOP_LOG(...) Logging.Log("[OdysseyCoop] " __VA_ARGS__)

HOOK_DEFINE_TRAMPOLINE(StageSceneInit) {
    static void Callback(StageScene* scene, const al::SceneInitInfo& info) {
        coop::gP1 = nullptr;
        coop::gP2 = nullptr;
        coop::gP2Port = -1;
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
        p2Info.trans[0] += 120.0f;  // spawn beside P1 (placement offset only; step 3 adds ground checks)

        PlayerActorBase* p2 = createPlayerFunction<PlayerActorHakoniwa>("Player2");
        if (p2 == nullptr) {
            COOP_LOG("createPlayerFunction returned null");
            coop::gSpawningP2 = false;
            return;
        }
        Orig(reinterpret_cast<PlayerActorHakoniwa*>(p2), actorInfo, p2Info);
        coop::gP2 = reinterpret_cast<al::LiveActor*>(p2);
        coop::gP2Port = p2Port;
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
                         smo100::alPlayerFunction_registerPlayer_bytes);
    if (!ok) {
        COOP_LOG("Not SMO 1.0.0 (hook-site bytes differ). No hooks installed.");
        return;
    }

    StageSceneInit::InstallAtOffset(smo100::StageScene_init);
    PlayerInitHook::InstallAtOffset(smo100::PlayerActorHakoniwa_initPlayer);
    RegisterPlayerHook::InstallAtOffset(smo100::alPlayerFunction_registerPlayer);
    COOP_LOG("Hooks installed (SMO 1.0.0).");
}

extern "C" NORETURN void exl_exception_entry() {
    EXL_ABORT("Default exception handler called!");
}
