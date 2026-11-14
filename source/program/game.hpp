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
template <typename T>
struct Vector2 { T x, y; };
using Vector2f = Vector2<float>;
class Heap;
}  // namespace sead
// GameDataHolderWriter is one GameDataHolder* passed by value (Ghidra: wearCostume reads
// param_1+0x20 = GameDataFile*).
struct GameDataHolderWriter {
    void* holder;
};

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
const sead::Vector3f& getVelocity(const LiveActor* actor);        // 0x8e2a34
bool isDead(const LiveActor* actor);                              // 0x8d9980
bool isInDeathArea(const LiveActor* actor);                       // 0x8d6bb8
class IUseAreaObj;
class AreaObj;
bool isInDeathArea(const IUseAreaObj* user, const sead::Vector3f& pos);  // 0x8050ec
class LayoutActor;
class IUseLayout;
class IUseLayoutAction;
class LayoutInitInfo;
bool isDead(const LayoutActor* layout);                               // 0x8b1f00
void killLayoutIfActive(LayoutActor* layout);                         // 0x8b1e98
sead::Vector3f getPaneLocalTrans(const IUseLayout* user, const char* pane);            // 0x8b2888
void setPaneLocalTrans(IUseLayout* user, const char* pane, const sead::Vector3f& t);   // 0x8b26ac
float getActionFrameMax(const IUseLayoutAction* user, const char* action, const char* group);  // 0x8b0ce0
void setPaneStringFormat(IUseLayout* user, const char* pane, const char* fmt, ...);    // 0x8b41d0
class IUseCollision;
class HitSensor;
LiveActor* getSensorHost(const HitSensor* sensor);  // 0x8f1ec0
class SensorMsg;
bool isMsgBindInit(const SensorMsg* msg);          // 0x8f96e4
bool isMsgBindStart(const SensorMsg* msg);         // 0x8f965c
sead::Heap* getSceneHeap();                        // 0x936e78
class IUseNerve;
class Nerve;
void setNerve(IUseNerve* user, const Nerve* nerve);  // 0x959af4
s32 getNerveStep(const IUseNerve* user);             // 0x959c8c
bool isFirstStep(const IUseNerve* user);             // 0x959cc8
class IUseCamera;
// 0x9d1c70: project a world position to screen pixels (Ghidra: sead::Camera::projectByMatrix with
// the scene camera/projection/viewport, then y flipped).
void calcScreenPosFromWorldPos(sead::Vector2f* out, const IUseCamera* user, const sead::Vector3f& pos);
// 0x9d144c: true if -margin <= x < width+margin and -margin <= y < height+margin.
bool isInScreen(const sead::Vector2f& pos, float margin);
const sead::Vector3f& getCameraPos(const IUseCamera* user, s32 index);  // 0x83bbe4
const sead::Vector3f& getCameraAt(const IUseCamera* user, s32 index);   // 0x83bc34
class CollisionPartsFilterBase;
class TriangleFilterBase;
s32 getMainControllerPort();                           // 0x85bfb0
PadRumbleKeeper* createPadRumbleKeeper(const LiveActor* actor, s32 port);  // 0x86b710
}  // namespace al

namespace rs {
// Player-aware ground check (0x571224). al::isOnGround must NOT be used on players:
// it dereferences actor+0x78 (al::Collider), which players don't have -> null crash.
bool isPlayerOnGround(const al::LiveActor* actor);
bool isInChangeStageArea(const al::LiveActor* actor, const sead::Vector3f* pos);  // 0x544de0
bool isActiveDemo(const al::LiveActor* actor);  // 0x5508d4
bool isSeparatePlay(const void* sceneObjHolder) asm(
    "_ZN2rs14isSeparatePlayEPKN2al18IUseSceneObjHolderE");  // 0x575af4
}  // namespace rs

namespace GameDataFunction {
void recoveryPlayerMax(const al::LiveActor* actor);  // 0x528780: shared life counter to max
// 0x527c50: find the ChangeStageArea at the actor (or pos) and request that stage change.
void reenterStage(GameDataHolderWriter writer);  // 0x527de8: holder+0x4a = 1
void findAreaAndChangeNextStage(GameDataHolderWriter writer, const al::LiveActor* actor,
                                const sead::Vector3f* pos);
}

namespace alCollisionUtil {
// 0x84c390. Ray from `start` along `arrow` (direction * length); true + *out on hit.
bool getHitPosOnArrow(const al::IUseCollision* user, sead::Vector3f* out,
                      const sead::Vector3f& start, const sead::Vector3f& arrow,
                      const al::CollisionPartsFilterBase* partsFilter,
                      const al::TriangleFilterBase* triFilter);
}  // namespace alCollisionUtil

namespace alPlayerFunction {
void registerPlayer(al::LiveActor* actor, al::PadRumbleKeeper* keeper);  // 0x9a4fe4
}

// The heart gauge layout (0x140 bytes). CounterLifeCtrl owns three of them: +0x18 normal,
// +0x20 bonus hearts, +0x28 kids mode. LayoutActor: IUseLayout at +0x8, IUseLayoutAction at +0x10.
// CounterLifeCtrl::setCount: gauge frame = max - max * (hp / maxHp) on action "Gauge", number text
// in pane "TxtLife"; root pane "All".
class CounterLife {
public:
    CounterLife(const char* name, const char* archive, const al::LayoutInitInfo& info);  // 0x1e53c4
    void start();            // 0x1e5750
    void setCount(float f);  // 0x1e5734
};

class PlayerActorBase;
class PlayerRecoverySafetyPoint {
public:
    // 0x460508. Vanilla PlayerActorHakoniwa::movement calls it with (trans, -gravity, nullptr).
    void setSafetyPoint(const sead::Vector3f& pos, const sead::Vector3f& up, const al::AreaObj* area);
};
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
    bool isClosetScene;           // +0x45  stage has a wardrobe (StageSceneStateCloset exists)
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
//  PlayerActorHakoniwa::getPlayerHackKeeper() returns *(this+0x208);
//  PlayerHackKeeper::startHack stores the captured actor at keeper+0x68;
//  PlayerActorHakoniwa::checkDeathArea uses *(this+0x198) as its PlayerDamageKeeper;
//  PlayerDamageKeeper's first field (+0x0) is its player actor;
//  PlayerJudgeDead::judge reads its player actor at this+0x8;
//  al::LiveActor's IUseCollision base sits at +0x38 (callers pass actor+0x38).
constexpr ptrdiff_t PlayerActorHakoniwa_hackKeeper = 0x208;
constexpr ptrdiff_t PlayerHackKeeper_hackActor = 0x68;
constexpr ptrdiff_t PlayerActorHakoniwa_damageKeeper = 0x198;
constexpr ptrdiff_t PlayerJudgeDead_player = 0x8;
constexpr ptrdiff_t LiveActor_IUseCollision = 0x38;
//  IUseAreaObj sits between IUseSceneObjHolder (+0x20, confirmed) and IUseCamera/IUseCollision.
constexpr ptrdiff_t LiveActor_IUseAreaObj = 0x28;
constexpr ptrdiff_t LiveActor_IUseCamera = 0x30;
//  PlayerRecoverySafetyPoint's first field (+0x0) is its player actor (isValid/setSafetyPoint).
constexpr ptrdiff_t PlayerRecoverySafetyPoint_player = 0x0;

// Camera (verified in Ghidra, 1.0.0):
//  al::CameraPoser::calcCameraPose: makeLookAtCameraPrev -> vtable makeLookAtCamera (base version
//  at 0xcc684 is an empty `ret`, used by posers that don't override it, e.g. follow cameras) ->
//  CameraParamMoveLimit::apply -> CameraArrowCollider::makeLookAtCamera (keeps camera out of walls).
//  sead::LookAtCamera: pos at +0x38, at +0x44, up +0x50 (from its constructor).
//  PlayerCameraTarget::calcTrans(out) is the follow point StageScene::init gives the camera for P1.
constexpr ptrdiff_t PlayerCameraTarget_calcTrans = 0xce9b8;
constexpr u64 PlayerCameraTarget_calcTrans_bytes = 0xa9014ff4d100c3ffULL;
constexpr ptrdiff_t CameraPoser_makeLookAtCamera = 0xcc684;
constexpr u64 CameraPoser_makeLookAtCamera_bytes = 0x2a1f03e0d65f03c0ULL;
constexpr ptrdiff_t LookAtCamera_pos = 0x38;
constexpr ptrdiff_t LookAtCamera_at = 0x44;

// Rescue bubble (verified in Ghidra): local routine called from checkDeathArea's recovery branch
// with (player, +0x148, +0x1f8, +0x1f0, +0x200, +0x3b8); player+0x270 = PlayerRecoverySafetyPoint.
constexpr ptrdiff_t PlayerRecoveryStart = 0x4273f4;
constexpr u64 PlayerRecoveryStart_bytes = 0xa90157f6a9bc5ff8ULL;
constexpr ptrdiff_t PlayerActorHakoniwa_safetyPoint = 0x270;

// Bubble flight (PlayerStateRecoveryDead, verified in Ghidra):
//  exeStart rises for 45 steps (isLessStep 0x2d) then setNerve(Recovery = main+0x1d7eff8,
//  whose execute() tail-calls exeRecovery). exeRecovery on its first step stores the flight
//  length in frames at +0x74 = clamp(ceil(dist/7.5), 60, 120) and the spin length at +0x78;
//  the path is evaluated from calcNerveRate(step, +0x74), so shortening +0x74 just flies faster.
//  The state's actor is at +0x18 (ActorStateBase).
constexpr ptrdiff_t PlayerStateRecoveryDead_exeStart = 0x479694;
constexpr u64 PlayerStateRecoveryDead_exeStart_bytes = 0xfd000be8d10103ffULL;
constexpr ptrdiff_t PlayerStateRecoveryDead_exeRecovery = 0x479884;
constexpr u64 PlayerStateRecoveryDead_exeRecovery_bytes = 0xfd002becd10303ffULL;
constexpr ptrdiff_t NrvRecoveryDeadRecovery = 0x1d7eff8;
constexpr ptrdiff_t ActorStateBase_actor = 0x18;
// PlayerRecoverySafetyPoint::endRecovery: called when the bubble lands (RecoveryDead::exeFall) or
// the state is killed. Its first field is the player.
// Death rules / crush / hearts (verified in Ghidra):
//  executePreMovementNerveChange kills the player when rs::isPressedCollision(player+0x170) is true.
//  LifeUpItem::receiveMsg stores the picker's HitSensor at item+0x110, then get() ->
//  isPlayerHitPointMax / recoveryPlayer(item) on the shared counter.
constexpr ptrdiff_t PlayerActorHakoniwa_collision = 0x170;
constexpr ptrdiff_t rs_isPressedCollision = 0x56a280;
constexpr u64 rs_isPressedCollision_bytes = 0xa9017bfdf81e0ff3ULL;
constexpr ptrdiff_t LifeUpItem_get = 0x1c9ea8;
constexpr u64 LifeUpItem_get_bytes = 0xa9017bfdf81e0ff3ULL;
constexpr ptrdiff_t LifeUpItem_pickerSensor = 0x110;
constexpr ptrdiff_t GameDataFunction_recoveryPlayer = 0x5286f0;
constexpr u64 GameDataFunction_recoveryPlayer_bytes = 0xa9017bfdf81e0ff3ULL;
constexpr ptrdiff_t GameDataFunction_isPlayerHitPointMax = 0x5286c0;
constexpr u64 GameDataFunction_isPlayerHitPointMax_bytes = 0x910003fda9bf7bfdULL;
// Pipes / doors (verified in Ghidra):
//  DokanPuppetController / DoorWarpPuppetController drive the player only through
//  IUsePlayerPuppet virtual calls (rs::setPuppetTrans etc. are thin virtual wrappers).
//  PlayerPuppet vtable at main+0x1d7ca18 (+0x10 = first slot): 0x00 start, 0x08 end, 0x10 cancel,
//  0x18 setTrans, 0x20 setPose, 0x28 setVelocity, 0x30 resetPosition, 0x60 startAction,
//  0x78 setAnimRate, 0x88 hide, 0x90 show, 0xa0/0xa8 silhouette, 0xb0/0xb8 shadow,
//  0xd8 setBindEndOnGround, 0xe8 setBindEndJump, 0xf8/0x100 collision check, 0x120/0x128 sensor.
//  PlayerPuppet::start stores binder sensor at +0x70 and player sensor at +0x78 (isBinding).
//  PlayerActorHakoniwa: puppet at +0x178, PlayerBindKeeper at +0x1f0 ([0] body sensor,
//  [+8] bound sensor, [+0x24] flag); Bind nerve = main+0x1d789e0 (execute -> exeBind);
//  entering Bind also sets *(player+0x138)+0x9b and +0xcc to 1.
//  rs::requestBindPlayer(pipe, sensor) binds getPlayerActor(pipe, 0), i.e. only P1 in vanilla.
constexpr ptrdiff_t Vtbl_PlayerPuppet = 0x1d7ca18 + 0x10;
constexpr ptrdiff_t PlayerPuppet_start_fn = 0x45f714;
constexpr ptrdiff_t Vptr_Dokan = 0x1cedb90 + 0x18;
constexpr ptrdiff_t Vptr_DokanMaze = 0x1cee9a8 + 0x18;
constexpr ptrdiff_t Vptr_DoorWarp = 0x1cf1018 + 0x18;
constexpr ptrdiff_t PlayerActorHakoniwa_puppet = 0x178;
constexpr ptrdiff_t PlayerActorHakoniwa_bindKeeper = 0x1f0;
constexpr ptrdiff_t PlayerActorHakoniwa_input138 = 0x138;
constexpr ptrdiff_t NrvPlayerBind = 0x1d789e0;
constexpr ptrdiff_t Dokan_attackSensor = 0x2576ec;
constexpr u64 Dokan_attackSensor_bytes = 0xf90023f5d101c3ffULL;
constexpr ptrdiff_t DokanStageChange_attackSensor = 0x25daf4;
constexpr u64 DokanStageChange_attackSensor_bytes = 0xf90023f5d101c3ffULL;
constexpr ptrdiff_t CounterLifeCtrl_ctor = 0x1e5bd0;
constexpr u64 CounterLifeCtrl_ctor_bytes = 0xa90157f6d10103ffULL;
// Resource loading (Luigi for P2). Verified in Ghidra: callers PlayerFunction::initMarioModelActor
// (+0x190), initMarioModelActor2D (+0xc4), initCapModelActor (+0xfc) pass "ObjectData/%s" names.
// al::findOrCreateActorResource(ActorResourceHolder*, const char* name, const char* suffix)
constexpr ptrdiff_t al_findOrCreateActorResource = 0x9cb618;
constexpr u64 al_findOrCreateActorResource_bytes = 0xa90157f6a9bc5ffcULL;
// al::findOrCreateActorResourceWithAnimResource(holder, name, animArc, suffix, bool isMergeAnim)
constexpr ptrdiff_t al_findOrCreateActorResourceWithAnimResource = 0x9cbc54;
constexpr u64 al_findOrCreateActorResourceWithAnimResource_bytes = 0xf9009bfcd10603ffULL;
// al::ResourceSystem::findOrCreateResource(const sead::SafeString& name, const char* ext)
constexpr ptrdiff_t ResourceSystem_findOrCreateResource = 0xa7d894;
constexpr u64 ResourceSystem_findOrCreateResource_bytes = 0xa9014ff4f81d0ff5ULL;
// al::findResource(const sead::SafeString&). rs::createPlayerSklRetargettingInfo (0x56f520)
// formats "ObjectData/%s" with al::getModelName(actor) and reads ->+0xb8 without a null check.
constexpr ptrdiff_t al_findResource = 0x9cb4e0;
// GOT slot holding the sead::SafeStringBase<char> vtable object (vtable = value + 0x10).
constexpr ptrdiff_t GOT_SafeStringBase_vtable = 0x1e4ebb8;
// Closet::receiveMsg(const al::SensorMsg*, al::HitSensor* other, al::HitSensor* self)
constexpr ptrdiff_t Closet_receiveMsg = 0x24e8f0;
constexpr u64 Closet_receiveMsg_bytes = 0xa90157f6fc1c0fe8ULL;
constexpr ptrdiff_t Closet_attackSensor = 0x24eaac;  // (HitSensor* own, HitSensor* other)
constexpr u64 Closet_attackSensor_bytes = 0xa9017bfda9be4ff4ULL;
constexpr ptrdiff_t Closet_isPlayerStart = 0x118;
// EventFlowNodeMessageBalloon::exeWait() (NPC talk balloon / talk start), node+0x18 = actor
constexpr ptrdiff_t EventFlowNodeMessageBalloon_exeWait = 0x1b79ec;
constexpr u64 EventFlowNodeMessageBalloon_exeWait_bytes = 0xfd0013e8d10143ffULL;
// rs::tryLockStartEventTalkDemo(al::EventFlowNode*) -> bool
constexpr ptrdiff_t rs_tryLockStartEventTalkDemo = 0x55a118;
constexpr u64 rs_tryLockStartEventTalkDemo_bytes = 0xa9017bfdf81e0ff3ULL;
// Capture start demo (type 3). PlayerHackKeeper+0x0 = owning player actor.
constexpr ptrdiff_t PlayerHackKeeper_startHackStartDemo = 0x449274;  // (keeper, actor) -> bool
constexpr u64 PlayerHackKeeper_startHackStartDemo_bytes = 0xa9017bfda9be4ff4ULL;
constexpr ptrdiff_t PlayerHackKeeper_startHackStartDemoPuppetable = 0x4492b8;
constexpr u64 PlayerHackKeeper_startHackStartDemoPuppetable_bytes = 0xa9014ff4d100c3ffULL;
// PlayerActorHakoniwa demo vfuncs (vptr+0x108 isEnableDemo, +0x120 start/+0x128 end puppetable)
constexpr ptrdiff_t PlayerActorHakoniwa_isEnableDemo = 0x421754;
constexpr u64 PlayerActorHakoniwa_isEnableDemo_bytes = 0xa9017bfdf81e0ff3ULL;
constexpr ptrdiff_t PlayerActorHakoniwa_startDemoPuppetable = 0x421b84;
constexpr u64 PlayerActorHakoniwa_startDemoPuppetable_bytes = 0xa9017bfdf81e0ff3ULL;
constexpr ptrdiff_t PlayerActorHakoniwa_endDemoPuppetable = 0x421e2c;
constexpr u64 PlayerActorHakoniwa_endDemoPuppetable_bytes = 0xf9000bf5d10103ffULL;
// TalkNpc::movement()
constexpr ptrdiff_t TalkNpc_movement = 0x3ddb38;
constexpr u64 TalkNpc_movement_bytes = 0xa90157f6fc1c0fe8ULL;
// al::PartsModel::initPartsFixFileNoRegister(parent, info, archive, ?, suffix)
constexpr ptrdiff_t PartsModel_initPartsFixFileNoRegister = 0x96cd78;
constexpr u64 PartsModel_initPartsFixFileNoRegister_bytes = 0xfd005be8d103c3ffULL;
// rs::isSeparatePlay(const al::IUseSceneObjHolder*) -> SMO's 2P mode flag
constexpr ptrdiff_t rs_isSeparatePlay = 0x575af4;
constexpr u64 rs_isSeparatePlay_bytes = 0x910003fda9bf7bfdULL;
constexpr ptrdiff_t NpcEventDirector_updateBalloon = 0x3a3ee4;
constexpr u64 NpcEventDirector_updateBalloon_bytes = 0x6d063befd10403ffULL;
// rs::checkTriggerDecideWithRequestIcon(al::LiveActor*, const sead::Vector3f&, float) (f in s0)
constexpr ptrdiff_t rs_checkTriggerDecideWithRequestIcon = 0x55b2fc;
constexpr u64 rs_checkTriggerDecideWithRequestIcon_bytes = 0xa9074ff4d10243ffULL;
constexpr ptrdiff_t NpcJointLookAtController_update = 0x3a65c4;
constexpr u64 NpcJointLookAtController_update_bytes = 0xfd0013e8d10143ffULL;
constexpr ptrdiff_t EventFlowExecutor_execute = 0x88b918;  // al::EventFlowExecutor::execute()
constexpr u64 EventFlowExecutor_execute_bytes = 0xa9014ff4f81d0ff5ULL;
constexpr ptrdiff_t rs_getNpcLookPos = 0x55ad2c;  // rs::getNpcLookPos(const al::LiveActor*)
constexpr u64 rs_getNpcLookPos_bytes = 0x910003fda9bf7bfdULL;
constexpr ptrdiff_t Closet_control = 0x24e62c;  // Closet::control()
constexpr u64 Closet_control_bytes = 0xa9017bfdf81e0ff3ULL;
constexpr ptrdiff_t rs_isTriggerUiDecide = 0x575ba8;     // (const al::IUseSceneObjHolder*)
constexpr u64 rs_isTriggerUiDecide_bytes = 0x910003fda9bf7bfdULL;
constexpr ptrdiff_t rs_isTriggerUiDecideGdh = 0x575bf8;  // (const GameDataHolder*)
constexpr u64 rs_isTriggerUiDecideGdh_bytes = 0x910003fda9bf7bfdULL;
constexpr ptrdiff_t rs_isTriggerUiCancel = 0x575c40;     // (const al::IUseSceneObjHolder*)
constexpr u64 rs_isTriggerUiCancel_bytes = 0xa9017bfdf81e0ff3ULL;
// NpcEventCtrlInfo::requestShowBalloonMessage(const NpcEventBalloonInfo&)
constexpr ptrdiff_t NpcEventCtrlInfo_requestShowBalloonMessage = 0x3a2f10;
constexpr u64 NpcEventCtrlInfo_requestShowBalloonMessage_bytes = 0x6d0233edd10243ffULL;
constexpr ptrdiff_t Shine_receiveMsg = 0x1cfb68;
constexpr u64 Shine_receiveMsg_bytes = 0x6d0423e9d10243ffULL;
constexpr ptrdiff_t StageSceneStateGetShine_kill = 0x4dbc5c;
constexpr u64 StageSceneStateGetShine_kill_bytes = 0xa9017bfdf81e0ff3ULL;
constexpr ptrdiff_t PlayerHolder_tryGetPlayer = 0x9a3df4;  // al::PlayerHolder::tryGetPlayer(int)
constexpr u64 PlayerHolder_tryGetPlayer_bytes = 0x6b01011ff9400408ULL;
constexpr ptrdiff_t PlayerHolder_getPlayer = 0x9a3de4;     // al::PlayerHolder::getPlayer(int)
constexpr u64 PlayerHolder_getPlayer_bytes = 0x937c7c29f9400008ULL;
constexpr ptrdiff_t ShoppingWatcherStateWear_appear = 0x316a88;
constexpr u64 ShoppingWatcherStateWear_appear_bytes = 0xa9017bfda9be4ff4ULL;
constexpr ptrdiff_t StageScene_dtor = 0x4c8474;  // StageScene::~StageScene() (D1 == D2)
constexpr u64 StageScene_dtor_bytes = 0xa9014ff4f81d0ff5ULL;
constexpr ptrdiff_t MessageSystem_getStageMessageHolder = 0x93d208;    // (const char*)
constexpr u64 MessageSystem_getStageMessageHolder_bytes = 0x71000d1fb9400808ULL;
constexpr ptrdiff_t MessageSystem_getSystemMessageHolder = 0x93cbbc;   // (const char*)
constexpr u64 MessageSystem_getSystemMessageHolder_bytes = 0x34000088b9400808ULL;
constexpr ptrdiff_t MessageSystem_getSystemMessageHolder2 = 0x93ce40;  // (const char*, const char*)
constexpr u64 MessageSystem_getSystemMessageHolder2_bytes = 0x34000088b9400808ULL;
// Luigi's voice: SE request path and the nn::atk wave lookup used while preparing a wave sound
constexpr ptrdiff_t al_SeKeeper_create = 0x9e65e8;  // static SeKeeper* create(...)
constexpr u64 al_SeKeeper_create_bytes = 0xa9015ff8a9bb67faULL;
constexpr ptrdiff_t al_SeKeeper_requestPlaySeIdLocal = 0x9e7888;
constexpr u64 al_SeKeeper_requestPlaySeIdLocal_bytes = 0xa9017bfda9be4ff4ULL;
constexpr ptrdiff_t al_SeRequest_setNew = 0xa98a80;
constexpr u64 al_SeRequest_setNew_bytes = 0xa90157f6f81c0ff7ULL;
constexpr ptrdiff_t al_SeRequest_playStart = 0xa98314;
constexpr u64 al_SeRequest_playStart_bytes = 0xa9017bfda9be4ff4ULL;
constexpr ptrdiff_t ProjectDemoDirector_requestStartDemo = 0xd5978;  // (const char*, DemoType) -> bool
constexpr u64 ProjectDemoDirector_requestStartDemo_bytes = 0xa90157f6a9bc5ff8ULL;
constexpr ptrdiff_t ProjectDemoDirector_requestEndDemo = 0xd5d5c;    // (const char*, DemoType)
constexpr u64 ProjectDemoDirector_requestEndDemo_bytes = 0xa90157f6f81c0ff7ULL;
constexpr ptrdiff_t MapObjStatePlayerHold_tryStartCarryFront = 0x2b6ec8;  // (SensorMsg*, HitSensor*) -> bool
constexpr ptrdiff_t MapObjStatePlayerHold_tryStartCarryFrontWallKeep = 0x2b6f08;
constexpr ptrdiff_t MapObjStatePlayerHold_tryStartCarryUp = 0x2b6f48;
constexpr u64 MapObjStatePlayerHold_tryStart_bytes = 0xa9017bfda9be4ff4ULL;
constexpr ptrdiff_t al_getPlayerPos_actor = 0x9a3f9c;  // al::getPlayerPos(const LiveActor*, int)
constexpr u64 al_getPlayerPos_actor_bytes = 0xa9017bfdf81e0ff3ULL;
constexpr ptrdiff_t rs_isPlayerHackRadiconNpc = 0x5706f0;  // (const LiveActor*) -> bool
constexpr u64 rs_isPlayerHackRadiconNpc_bytes = 0x910003fda9bf7bfdULL;
constexpr ptrdiff_t CheckpointFlag_init = 0x24972c;        // (const ActorInitInfo&)
constexpr u64 CheckpointFlag_init_bytes = 0xa90b5ff8d103c3ffULL;
constexpr ptrdiff_t CheckpointFlag_receiveMsg = 0x249ae0;
constexpr u64 CheckpointFlag_receiveMsg_bytes = 0xa90157f6a9bc5ff8ULL;
constexpr ptrdiff_t CheckpointFlag_nrvGet = 0x1ce6d30;     // nerve object (Ghidra 0x1de6d30)
constexpr ptrdiff_t ProjectDemoDirector_requestStartDemoReturnToHome = 0xd63c8;   // (const char*)
constexpr ptrdiff_t ProjectDemoDirector_requestStartDemoAppearFromHome = 0xd6240;
constexpr u64 PDD_requestStartDemoHome_bytes = 0xa9017bfdf81e0ff3ULL;
constexpr ptrdiff_t rs_startActionDemoPlayer = 0x56a8b8;
constexpr u64 rs_startActionDemoPlayer_bytes = 0xa9014ff4d100c3ffULL;
constexpr ptrdiff_t rs_setActionFrameDemoPlayer = 0x56aa04;
constexpr ptrdiff_t rs_setActionFrameRateDemoPlayer = 0x56a994;
constexpr u64 rs_setActionFrameDemoPlayer_bytes = 0xf90007f3fc1e0fe8ULL;
constexpr ptrdiff_t rs_hideDemoPlayer = 0x56aacc;
constexpr ptrdiff_t rs_showDemoPlayer = 0x56ab60;
constexpr u64 rs_hideShowDemoPlayer_bytes = 0xa9017bfdf81e0ff3ULL;
constexpr u64 JumpingRopeNpc_vtable = 0x1d5c288;     // vtable for JumpingRopeNpc + 0x10
constexpr u64 RaceManGoal_vtable = 0x1d636f0;
constexpr u64 RaceManRace_vtable = 0x1d63b48;
constexpr u64 SnowManRaceNpc_vtable = 0x1d6ab08;
constexpr ptrdiff_t atk_Util_GetWaveFileOfWaveSound = 0xac02c0;
constexpr u64 atk_Util_GetWaveFileOfWaveSound_bytes = 0xf90033f5d10243ffULL;  // bool, set in Closet::init (isPlayerStartCloset)
// al::getMainControllerPort() -> s32 (used by rs::isTriggerUiDecide/Cancel and the UI)
constexpr ptrdiff_t al_getMainControllerPort = 0x85bfb0;
constexpr u64 al_getMainControllerPort_bytes = 0xa9017bfdf81e0ff3ULL;
// StageSceneStateCloset::kill() (vtable +0x28, called at the end of exeEnd)
constexpr ptrdiff_t StageSceneStateCloset_kill = 0x4d5758;
constexpr u64 StageSceneStateCloset_kill_bytes = 0xa9017bfdf81e0ff3ULL;
// GameDataFunction::wearCostume/wearCap(GameDataHolderWriter, const char*) -> bool
constexpr ptrdiff_t GameDataFunction_wearCostume = 0x52b4ec;
constexpr u64 GameDataFunction_wearCostume_bytes = 0xa9017bfda9be4ff4ULL;
constexpr ptrdiff_t GameDataFunction_wearCap = 0x52b530;
constexpr u64 GameDataFunction_wearCap_bytes = 0xa9017bfda9be4ff4ULL;
// GameDataFunction::getCurrentCostumeTypeName/getCurrentCapTypeName(GameDataHolderAccessor)
constexpr ptrdiff_t GameDataFunction_getCurrentCostumeTypeName = 0x52b48c;
constexpr u64 GameDataFunction_getCurrentCostumeTypeName_bytes = 0xa9017bfdf81e0ff3ULL;
constexpr ptrdiff_t GameDataFunction_getCurrentCapTypeName = 0x52b4bc;
constexpr u64 GameDataFunction_getCurrentCapTypeName_bytes = 0xa9017bfdf81e0ff3ULL;
// ResourceSystem internals reached by the inlined TextureArc load in createResource (0xa7d2c8).
constexpr ptrdiff_t ResourceSystem_findResourceCore = 0xa7d760;  // (this, name, iterator*)
constexpr u64 ResourceSystem_findResourceCore_bytes = 0xa9015ff8a9bb67faULL;
constexpr ptrdiff_t ResourceSystem_findResourceCategory = 0xa7d8f8;  // (this, name)
constexpr u64 ResourceSystem_findResourceCategory_bytes = 0xa90157f6d10103ffULL;
constexpr ptrdiff_t ResourceSystem_createResource = 0xa7d2c8;  // (this, name, category, ext)
constexpr u64 ResourceSystem_createResource_bytes = 0xa9015ff8f81b0ffcULL;
// rs::tryChangeNextStage(GameDataHolder*, al::Scene*) -> bool
constexpr ptrdiff_t rs_tryChangeNextStage = 0x4d2c54;
constexpr u64 rs_tryChangeNextStage_bytes = 0xa9024ff4d10103ffULL;
// StageSceneStateCloset::initModel(const al::ActorInitInfo&)
constexpr ptrdiff_t StageSceneStateCloset_initModel = 0x4d5560;
constexpr u64 StageSceneStateCloset_initModel_bytes = 0xa9026ffcd10203ffULL;
constexpr u64 al_findResource_bytes = 0xa9017bfdf81e0ff3ULL;
constexpr ptrdiff_t CounterLifeCtrl_normalLayout = 0x18;
constexpr ptrdiff_t PlayerRecoverySafetyPoint_endRecovery = 0x4613a8;
constexpr u64 PlayerRecoverySafetyPoint_endRecovery_bytes = 0xa9017bfdf81e0ff3ULL;
constexpr ptrdiff_t RecoveryDead_flightFrames = 0x74;
constexpr ptrdiff_t RecoveryDead_spinFrames = 0x78;
constexpr ptrdiff_t RecoveryDead_safetyPoint = 0x20;  // PlayerRecoverySafetyPoint*
constexpr ptrdiff_t RecoveryDead_arcA = 0x48;
constexpr ptrdiff_t RecoveryDead_arcB = 0x4c;
constexpr ptrdiff_t RecoveryDead_start = 0x50;  // Vector3f, player trans when the flight began
constexpr ptrdiff_t RecoveryDead_up = 0x5c;     // Vector3f, -gravity
constexpr ptrdiff_t RecoveryDead_horiz = 0x68;  // Vector3f, horizontal start->target offset

constexpr ptrdiff_t PlayerFunction_isPlayerHitPointOne = 0x447bdc;
constexpr u64 PlayerFunction_isPlayerHitPointOne_bytes = 0xa9017bfdd10083ffULL;
constexpr ptrdiff_t PlayerRecoverySafetyPoint_isValid = 0x460c90;
constexpr u64 PlayerRecoverySafetyPoint_isValid_bytes = 0xa9017bfdf81e0ff3ULL;
constexpr ptrdiff_t rs_isPlayerEnableToSeeOddSpace = 0x570908;
constexpr u64 rs_isPlayerEnableToSeeOddSpace_bytes = 0xa9017bfdf81e0ff3ULL;

constexpr ptrdiff_t PlayerJudgeDead_judge = 0x458e14;
constexpr ptrdiff_t PlayerDamageKeeper_damage = 0x43f154;
constexpr ptrdiff_t PlayerDamageKeeper_damageForce = 0x43f304;
constexpr ptrdiff_t PlayerDamageKeeper_dead = 0x43f390;
constexpr ptrdiff_t PlayerActorHakoniwa_checkDeathArea = 0x427200;
constexpr u64 PlayerJudgeDead_judge_bytes = 0xa9017bfdf81e0ff3ULL;
constexpr u64 PlayerDamageKeeper_damage_bytes = 0xf9000bf3d100c3ffULL;
constexpr u64 PlayerDamageKeeper_damageForce_bytes = 0xf9000bf3d100c3ffULL;
constexpr u64 PlayerDamageKeeper_dead_bytes = 0xa9017bfdd10083ffULL;
constexpr u64 PlayerActorHakoniwa_checkDeathArea_bytes = 0xa90357f6d10183ffULL;

constexpr u64 StageScene_init_bytes = 0xa9016ffcfc190fe8ULL;
constexpr u64 PlayerActorHakoniwa_initPlayer_bytes = 0xa90167faa9ba6ffcULL;
constexpr u64 alPlayerFunction_registerPlayer_bytes = 0xa9017bfda9be4ff4ULL;
constexpr u64 StageScene_control_bytes = 0xa9035ff8d101c3ffULL;
constexpr u64 al_getPlayerActor_bytes = 0xa9017bfdf81e0ff3ULL;
constexpr u64 al_tryGetPlayerActor_bytes = 0xa9017bfdf81e0ff3ULL;
// NPC event scenes (EventDemoCtrl::tryStart*Demo) all go through these rs helpers
constexpr ptrdiff_t rs_requestStartDemoWithPlayerKeepBindTalk = 0x54f9e0;
constexpr ptrdiff_t rs_requestStartDemoKeepHackTalk = 0x54fa7c;
constexpr ptrdiff_t rs_requestStartDemoWithPlayerCinemaFrameTalk = 0x54fb18;
constexpr ptrdiff_t rs_requestStartDemoWithPlayerKeepCarryTalk = 0x54fbb4;
constexpr ptrdiff_t rs_requestStartDemoWithPlayerUseCoinTalk = 0x54fc50;
constexpr ptrdiff_t rs_requestStartDemoNormalTalk = 0x54fcec;
constexpr u64 rs_requestStartDemoTalk_bytes = 0xa9017bfda9be4ff4ULL;
// Every demo end (requestEndDemo, ...AppearFromHome, ...ReturnToHome, ...ScenarioCamera,
// ...ShineMainGet/GrandGet, ...RiseMapParts) goes through al::DemoDirector::requestEndDemo.
constexpr ptrdiff_t al_DemoDirector_requestEndDemo = 0x9cdf58;  // (this, const char*)
constexpr u64 al_DemoDirector_requestEndDemo_bytes = 0xa9017bfdf81e0ff3ULL;
// The Odyssey's own scenes (globe "Head to another kingdom?", take-off, paying moons)
constexpr ptrdiff_t ShineTowerRocket_tryStartDemo = 0x30d598;
constexpr u64 ShineTowerRocket_tryStartDemo_bytes = 0xa9017bfda9be4ff4ULL;
// rs::requestStartDemoWithPlayer(LiveActor*, bool): the wardrobe (Closet::receiveMsg, returns to
// 0x24ea40) and the Crazy Cap try-on (ShoppingWatcherStateWear::exeActionForDoshi, returns to 0x316cf0)
constexpr ptrdiff_t rs_requestStartDemoWithPlayer = 0x54f740;
constexpr u64 rs_requestStartDemoWithPlayer_bytes = 0xa9017bfda9be4ff4ULL;
constexpr ptrdiff_t Closet_receiveMsg_demoRet = 0x24ea40;
constexpr ptrdiff_t ShoppingWatcherStateWear_demoRet = 0x316cf0;
// HackCap::sendMsgStartHack(HitSensor* target): Cappy asks the target to be captured
constexpr ptrdiff_t HackCap_sendMsgStartHack = 0x3fc09c;
constexpr u64 HackCap_sendMsgStartHack_bytes = 0xa9014ff4f81d0ff5ULL;
// Outfits from the 1.1-1.4 updates, added to the 1.0.0 shop list (romfs ItemList) for Luigi only.
constexpr ptrdiff_t rs_isHaveCloth = 0x54c498;  // (GameDataHolderAccessor, const char*)
constexpr u64 rs_isHaveCloth_bytes = 0xf9437c02f9401000ULL;
constexpr ptrdiff_t rs_isHaveCap = 0x54c4a4;    // (GameDataHolderAccessor, const char*)
constexpr u64 rs_isHaveCap_bytes = 0xf9437802f9401000ULL;
constexpr ptrdiff_t ShopStateInShop_appear = 0x317538;
constexpr u64 ShopStateInShop_appear_bytes = 0xa9017bfda9be4ff4ULL;
constexpr ptrdiff_t al_createTextureInfo_path = 0x8b48cc;  // (const char* path, const char* bfres, const char* tex)
constexpr u64 al_createTextureInfo_path_bytes = 0xa91257fcd10543ffULL;
constexpr ptrdiff_t ShopLayoutInfo_loadTexture = 0x207e00;
constexpr u64 ShopLayoutInfo_loadTexture_bytes = 0xa90167faa9ba6ffcULL;
// al::ExecutorListActorDraw::ExecutorListActorDraw(const char* name, int capacity): one draw
// executor per distinct model name, appended with no bounds check
// (ExecutorListActorExecuteBase::registerActor, Ghidra 0x997330).
constexpr ptrdiff_t al_ExecutorListActorExecuteBase_ctor = 0x8972a4;   // (name, int capacity)
constexpr u64 al_ExecutorListActorExecuteBase_ctor_bytes = 0xa9017bfda9be4ff4ULL;
constexpr ptrdiff_t al_ExecutorListActorModelDrawBase_ctor = 0x893fe8;  // (name, int capacity, ExecuteSystemInitInfo&)
constexpr u64 al_ExecutorListActorModelDrawBase_ctor_bytes = 0xa9014ff4f81d0ff5ULL;
// al::ActorExecuteInfo (0x90 bytes): updaters at +0x10 (count +8, room for 4), drawers at +0x38
// (count +0x30, room for 11); addUpdater/addDrawer write with no bounds check.
constexpr ptrdiff_t al_ActorExecuteInfo_addDrawer = 0x891c18;
constexpr u64 al_ActorExecuteInfo_addDrawer_bytes = 0x8b080c08b9803008ULL;
constexpr ptrdiff_t al_ActorExecuteInfo_addUpdater = 0x891bfc;
constexpr u64 al_ActorExecuteInfo_addUpdater_bytes = 0x8b080c08b9800808ULL;
}  // namespace smo100
