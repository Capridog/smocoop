#pragma once
// Minimal declarations of SMO 1.0.0 (build 3CA12DFA...) functions this module calls.
// Every symbol below was checked against the 1.0.0 dynamic symbol table and in Ghidra.
// They are linked dynamically against the game's main executable at load time.

#include <cstddef>
#include <cstdint>

namespace sead {
template <typename T>
struct Vector3 { T x, y, z; };
using Vector3f = Vector3<float>;
}  // namespace sead

namespace al {
class ActorInitInfo;
class SceneInitInfo;
class LiveActor;
class PadRumbleKeeper;

s32 getPlayerControllerPort(s32 playerIndex);          // 0x85bfec
LiveActor* getPlayerActor(const LiveActor* actor, s32 index);     // 0x9a3f70 (hooked)
const sead::Vector3f& getTrans(const LiveActor* actor);           // 0x8ee66c
void calcSideDir(sead::Vector3f* out, const LiveActor* actor);    // 0x8ee6ec
void resetPosition(LiveActor* actor, const sead::Vector3f& trans);// 0x8e28b8
void setVelocityZero(LiveActor* actor);                           // 0x8e2c68
s32 getMainControllerPort();                           // 0x85bfb0
PadRumbleKeeper* createPadRumbleKeeper(const LiveActor* actor, s32 port);  // 0x86b710
}  // namespace al

namespace rs {
// Player-aware ground check (0x571224). al::isOnGround must NOT be used on players:
// it dereferences actor+0x78 (al::Collider), which players don't have -> null crash.
bool isPlayerOnGround(const al::LiveActor* actor);
}  // namespace rs

namespace alPlayerFunction {
void registerPlayer(al::LiveActor* actor, al::PadRumbleKeeper* keeper);  // 0x9a4fe4
}

class PlayerActorBase;
class PlayerActorHakoniwa;
class StageScene;

// PlayerInitInfo is not defined in OdysseyDecomp. Layout taken from Ghidra:
// StageScene::init builds it on the stack, rs::initPlayerActorInfo reads +0x08 / +0x10 /
// +0x28 / +0x34, and PlayerActorBase stores +0x10 as its port (actor +0x118).
struct PlayerInitInfo {
    void* gamePadSystem;          // +0x00
    const void* viewMtxPtr;       // +0x08
    s32 controllerPort;           // +0x10
    u32 _pad14;                   // +0x14
    const char* costumeName;      // +0x18
    const char* capName;          // +0x20
    float trans[3];               // +0x28
    float quat[4];                // +0x34  (x, y, z, w)
    bool flag44;                  // +0x44
    bool isClosetScene;           // +0x45
    u8 _pad46[2];                 // +0x46
};
static_assert(sizeof(PlayerInitInfo) == 0x48);
static_assert(offsetof(PlayerInitInfo, controllerPort) == 0x10);
static_assert(offsetof(PlayerInitInfo, trans) == 0x28);
static_assert(offsetof(PlayerInitInfo, quat) == 0x34);

// Factory entry the game itself uses for "PlayerActorHakoniwa" (0x4b5dc8).
template <typename T>
PlayerActorBase* createPlayerFunction(const char* name);
template <>
PlayerActorBase* createPlayerFunction<PlayerActorHakoniwa>(const char* name);

namespace smo100 {
// Hook sites (main-relative, 1.0.0) and the first 8 bytes expected there.
constexpr ptrdiff_t StageScene_init = 0x4c861c;
constexpr ptrdiff_t PlayerActorHakoniwa_initPlayer = 0x41b334;
constexpr ptrdiff_t alPlayerFunction_registerPlayer = 0x9a4fe4;
constexpr ptrdiff_t StageScene_control = 0x4cc348;
constexpr ptrdiff_t al_getPlayerActor = 0x9a3f70;
constexpr ptrdiff_t al_tryGetPlayerActor = 0x9a3fe4;

// Field offsets verified in Ghidra (1.0.0):
//  PlayerActorHakoniwa::initPlayer stores the new HackCap at this+0x148 (param_1[0x29]).
//  HackCap::HackCap stores its owning player actor at this+0x128 (param_1[0x25]).
constexpr ptrdiff_t PlayerActorHakoniwa_hackCap = 0x148;
constexpr ptrdiff_t HackCap_player = 0x128;

constexpr u64 StageScene_init_bytes = 0xa9016ffcfc190fe8ULL;
constexpr u64 PlayerActorHakoniwa_initPlayer_bytes = 0xa90167faa9ba6ffcULL;
constexpr u64 alPlayerFunction_registerPlayer_bytes = 0xa9017bfda9be4ff4ULL;
constexpr u64 StageScene_control_bytes = 0xa9035ff8d101c3ffULL;
constexpr u64 al_getPlayerActor_bytes = 0xa9017bfdf81e0ff3ULL;
constexpr u64 al_tryGetPlayerActor_bytes = 0xa9017bfdf81e0ff3ULL;
}  // namespace smo100
