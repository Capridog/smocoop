#include "lib.hpp"
#include "program/game.hpp"
#include "program/boss_vtables.hpp"
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

// Wardrobe for P2. P2's outfit lives here (not in the save file); empty = same as P1.
static char gP2Costume[64] = {};
static char gP2CapName[64] = {};
static bool gOutfitDirty = false;  // save Luigi's outfit file at the next stage load
static bool gClosetP2 = false;   // P2 is using the wardrobe right now (Closet bind -> state kill)
static bool gUiPortP2 = false;   // al::getMainControllerPort answers P2's port
static bool gLuigiCloset = false;
static bool gClosetStartP2Pending = false;  // P2 wore something: next stage start is Luigi's
static bool gClosetStartP2 = false;         // this stage: Luigi steps out of the booth
static bool gClosetSwapDone = false;
static bool gTalkP2 = false;
static bool gShopP2 = false;     // the last conversation (e.g. the Crazy Cap counter) was Luigi's
static bool gLeadP2 = false;     // Luigi grabbed a Moon: he is "player 0" until the Moon demo ends
static s32 gLeadFrames = 0;
static s32 gLeadNoDemo = 0;
static al::LiveActor* gNpcTarget = nullptr;   // TalkNpc::movement: who "player 0" is for this NPC
static al::LiveActor* gHackDemoStarter = nullptr;  // player starting a capture (HackStart demo)
static bool gSkippedHackDemo[2] = {false, false};  // P1/P2 skipped the other's HackStart demo       // P2 started the current NPC talk (UI follows P2's controller)
static s32 gTalkNoDemoFrames = 0;  // building the wardrobe's Luigi preview models right now

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

// SMO's own "2P mode" switch (pause menu) is GameDataHolder+0x245 (rs::isSeparatePlay 0x575af4).
// Co-op runs only while it is on. The game itself is kept in 1P mode (see SeparatePlayHook), so
// Cappy never goes to the second controller.
static bool CoopModeOn(const void* sceneObjHolder) {
    return sceneObjHolder != nullptr && rs::isSeparatePlay(sceneObjHolder);  // called from here: raw
}
static bool gReenterRequested = false;
static s32 gModeMismatchFrames = 0;
static s32 gModeFrames = 0;  // frames since stage init (counted in solo too)

static void ResetStageState() {
    gP1 = gP2 = gP2Cap = nullptr;
    gP2Port = -1;
    gP2Placed = false;
    gFramesSinceInit = 0;
    gP2Hp = kP2MaxHp;
    gP2RespawnPending = false;
    gGameOver = false;
    gForceP1Death = false;
    gReenterRequested = false;
    gModeMismatchFrames = 0;
    gModeFrames = 0;
    gTalkP2 = false;
    gShopP2 = false;
    gLeadP2 = false;
    gLeadFrames = gLeadNoDemo = 0;
    gTalkNoDemoFrames = 0;
    gNpcTarget = nullptr;
    gHackDemoStarter = nullptr;
    gSkippedHackDemo[0] = gSkippedHackDemo[1] = false;
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
namespace coop::closet {
static void Reset();
}  // namespace coop::closet
namespace coop::dim {
static void Reset();
}  // namespace coop::dim
namespace coop::text {
static void Reset();
}  // namespace coop::text
namespace coop::voice {
static void Reset();
static void Load();
}  // namespace coop::voice
namespace coop::boss {
static void Reset();
}  // namespace coop::boss
namespace coop::cut {
static void Reset();
}  // namespace coop::cut

// ---------------------------------------------------------------------------- spawning (step 2)

// ------------------------------------------------------------------------ Luigi's outfit file
// Luigi's outfit is not in the game's save file; it is kept in sd:/OdysseyCoop/p2outfit.txt
// ("body\ncap\n") through the SDK's nn::fs (imported from the sdk module).
struct FsHandle { u64 v; };
struct FsWriteOption { s32 flags; };
extern u32 FsMountSd(const char*) asm("_ZN2nn2fs19MountSdCardForDebugEPKc");
extern u32 FsCreateDirectory(const char*) asm("_ZN2nn2fs15CreateDirectoryEPKc");
extern u32 FsCreateFile(const char*, s64) asm("_ZN2nn2fs10CreateFileEPKcl");
extern u32 FsDeleteFile(const char*) asm("_ZN2nn2fs10DeleteFileEPKc");
extern u32 FsOpenFile(FsHandle*, const char*, s32) asm("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci");
extern u32 FsReadFile(u64*, FsHandle, s64, void*, u64) asm("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm");
extern u32 FsWriteFile(FsHandle, s64, const void*, u64, const FsWriteOption&) asm(
    "_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE");
extern u32 FsFlushFile(FsHandle) asm("_ZN2nn2fs9FlushFileENS0_10FileHandleE");
extern void FsCloseFile(FsHandle) asm("_ZN2nn2fs9CloseFileENS0_10FileHandleE");

namespace coop::outfit {
constexpr const char* kDir = "sd:/OdysseyCoop";
constexpr const char* kFile = "sd:/OdysseyCoop/p2outfit.txt";
static bool gMounted = false;
static bool gLoaded = false;

static bool Mount() {
    if (gMounted) return true;
    u32 rc = FsMountSd("sd");
    gMounted = rc == 0;
    if (!gMounted) COOP_LOG("Outfit file: SD card mount failed (0x%x)", rc);
    return gMounted;
}

static void Load() {
    if (gLoaded) return;
    gLoaded = true;
    if (!Mount()) return;
    FsHandle h{};
    if (FsOpenFile(&h, kFile, 1) != 0) { COOP_LOG("Outfit file: none yet"); return; }
    char buf[160] = {};
    u64 got = 0;
    FsReadFile(&got, h, 0, buf, sizeof(buf) - 1);
    FsCloseFile(h);
    buf[got < sizeof(buf) ? got : sizeof(buf) - 1] = 0;
    char* nl = nullptr;
    for (char* c = buf; *c; c++) if (*c == '\n') { nl = c; break; }
    if (nl == nullptr) return;
    *nl = 0;
    char* cap = nl + 1;
    for (char* c = cap; *c; c++) if (*c == '\n' || *c == '\r') { *c = 0; break; }
    size_t nb = std::strlen(buf), nc = std::strlen(cap);
    if (nb > 0 && nb < 64) std::memcpy(gP2Costume, buf, nb + 1);
    if (nc > 0 && nc < 64) std::memcpy(gP2CapName, cap, nc + 1);
    COOP_LOG("Outfit file: Luigi wears %s / %s", gP2Costume, gP2CapName);
}

static void Save() {
    if (!Mount()) return;
    char buf[160];
    size_t nb = std::strlen(gP2Costume), nc = std::strlen(gP2CapName);
    if (nb + nc + 2 >= sizeof(buf)) return;
    std::memcpy(buf, gP2Costume, nb);
    buf[nb] = '\n';
    std::memcpy(buf + nb + 1, gP2CapName, nc);
    buf[nb + 1 + nc] = '\n';
    u64 len = nb + nc + 2;
    FsCreateDirectory(kDir);
    FsDeleteFile(kFile);
    if (FsCreateFile(kFile, (s64)len) != 0) { COOP_LOG("Outfit file: create failed"); return; }
    FsHandle h{};
    if (FsOpenFile(&h, kFile, 2) != 0) { COOP_LOG("Outfit file: open failed"); return; }
    FsWriteOption opt{1};
    FsWriteFile(h, 0, buf, len, opt);
    FsFlushFile(h);
    FsCloseFile(h);
    COOP_LOG("Outfit file: saved %s / %s", gP2Costume, gP2CapName);
}
}  // namespace coop::outfit

HOOK_DEFINE_TRAMPOLINE(StageSceneInit) {
    static void Callback(StageScene* scene, const al::SceneInitInfo& info) {
        coop::ResetStageState();
        coop::outfit::Load();
        coop::voice::Load();
        if (coop::gOutfitDirty) { coop::gOutfitDirty = false; coop::outfit::Save(); }
        coop::dim::Reset();
        coop::gClosetP2 = false;
        coop::gUiPortP2 = false;
        coop::gClosetStartP2 = coop::gClosetStartP2Pending;
        coop::gClosetStartP2Pending = false;
        coop::gClosetSwapDone = false;
        coop::closet::Reset();
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
        if (sead::Heap* heap = al::getSceneHeap()) {
            // sead::Heap vtable slot 15 = getFreeSize (ExpHeap vtable 0x1dcbc68, Ghidra).
            using FreeFn = size_t (*)(sead::Heap*);
            FreeFn fn = (*reinterpret_cast<FreeFn**>(heap))[15];
            COOP_LOG("Scene heap free: %lu KB", (unsigned long)(fn(heap) / 1024));
        }
    }
};

// ------------------------------------------------------------------------ Luigi (P2 look)
// While P2's PlayerActorHakoniwa is initialised, every player archive it asks for is renamed to
// the Luigi copy in romfs ObjectData/P2Luigi (Super Luigi Odyssey files, Tall Luigi bodies).
// Names are rewritten where the game turns an archive name into a resource:
//   al::findOrCreateActorResource / ...WithAnimResource (the ActorResource cache key)
//   al::ResourceSystem::findOrCreateResource (TextureArc, byml lookups, model archives).
// Each Luigi archive holds both "<Mario name>.bfres" and "P2Luigi<X>.bfres", so a renamed
// resource finds its model. Renamed names are not in ResourceCategoryTable, so they load into
// the current scene category and are freed with the stage. P1 is never touched.
#include "program/luigi_files.hpp"
namespace coop::luigi {
constexpr bool kRedirectAnims = true;  // Luigi's own PlayerAnimation* (hands: no oversized fist)
static char gBuf[8][160];
static u32 gBufIdx = 0;
static u32 gRedirects = 0;

static bool InList(const char* const* list, size_t n, const char* key) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = std::strcmp(list[mid], key);
        if (c == 0) return true;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return false;
}

// "ObjectData/MarioHead" -> "ObjectData/P2Luigi/P2LuigiHead" (only if the Luigi file exists).
static const char* Redirect(const char* name) {
    if (!(gSpawningP2 || gLuigiCloset) || name == nullptr) return nullptr;
    constexpr const char kDir[] = "ObjectData/";
    if (std::strncmp(name, kDir, sizeof(kDir) - 1) != 0) return nullptr;
    const char* base = name + sizeof(kDir) - 1;
    const char* fmt = nullptr;
    const char* rest = nullptr;
    if (std::strncmp(base, "Mario", 5) == 0 &&
        InList(kLuigiParts, sizeof(kLuigiParts) / sizeof(kLuigiParts[0]), base + 5)) {
        fmt = "ObjectData/P2Luigi/P2Luigi"; rest = base + 5;
    } else if (InList(kLuigiOther, sizeof(kLuigiOther) / sizeof(kLuigiOther[0]), base) ||
               (kRedirectAnims &&
                InList(kLuigiAnims, sizeof(kLuigiAnims) / sizeof(kLuigiAnims[0]), base))) {
        fmt = "ObjectData/P2Luigi/P2Luigi_"; rest = base;
    } else {
        return nullptr;
    }
    size_t a = std::strlen(fmt), b = std::strlen(rest);
    if (a + b + 1 > sizeof(gBuf[0])) return nullptr;
    char* out = gBuf[gBufIdx++ & 7];
    std::memcpy(out, fmt, a);
    std::memcpy(out + a, rest, b + 1);
    if (gRedirects++ < 64) COOP_LOG("Luigi: %s -> %s", name, out);
    return out;
}

// A Luigi actor's model name is "P2Luigi<X>", and some code rebuilds "ObjectData/<model name>"
// from it (e.g. rs::createPlayerSklRetargettingInfo -> al::findResource). Those files live in
// the P2Luigi folder: "ObjectData/P2Luigi<X>" -> "ObjectData/P2Luigi/P2Luigi<X>". Any time.
static const char* FixFolder(const char* name) {
    if (name == nullptr) return nullptr;
    constexpr const char kPre[] = "ObjectData/P2Luigi";
    constexpr size_t n = sizeof(kPre) - 1;
    if (std::strncmp(name, kPre, n) != 0 || name[n] == '/') return nullptr;
    size_t len = std::strlen(name);
    if (len + 9 > sizeof(gBuf[0])) return nullptr;
    char* out = gBuf[gBufIdx++ & 7];
    std::memcpy(out, "ObjectData/P2Luigi/", 19);
    std::memcpy(out + 19, name + 11, len - 11 + 1);
    return out;
}

static const char* Rename(const char* name) {
    const char* r = Redirect(name);
    return r ? r : FixFolder(name);
}

struct SafeStr {  // sead::SafeStringBase<char>: vtable, mStringTop
    void* vtable;
    const char* str;
};
// The incoming name is often a StringTmp (BufferedSafeString): its vtable's
// assureTerminationImpl_ uses the buffer size field, which a 16-byte copy does not have. Use the
// plain sead::SafeStringBase<char> vtable the game itself uses for temporary names
// (GOT main+0x1e4ebb8, +0x10; seen in findOrCreateActorResourceWithAnimResource).
static SafeStr MakeSafe(const char* str) {
    u64 vt;
    std::memcpy(&vt, reinterpret_cast<const void*>(exl::util::modules::GetTargetStart() +
                                                   smo100::GOT_SafeStringBase_vtable),
                sizeof(vt));
    return SafeStr{reinterpret_cast<void*>(vt + 0x10), str};
}
}  // namespace coop::luigi

HOOK_DEFINE_TRAMPOLINE(FindOrCreateActorResourceHook) {
    static void* Callback(void* holder, const char* name, const char* suffix) {
        const char* r = coop::luigi::Rename(name);
        return Orig(holder, r ? r : name, suffix);
    }
};

HOOK_DEFINE_TRAMPOLINE(FindOrCreateActorResourceAnimHook) {
    static void* Callback(void* holder, const char* name, const char* animArc, const char* suffix,
                          bool isMergeAnim) {
        const char* r = coop::luigi::Rename(name);
        const char* ra = coop::luigi::Rename(animArc);
        return Orig(holder, r ? r : name, ra ? ra : animArc, suffix, isMergeAnim);
    }
};

HOOK_DEFINE_TRAMPOLINE(ResourceFindOrCreateHook) {
    static void* Callback(void* system, const coop::luigi::SafeStr* name, const char* ext) {
        const char* r = name ? coop::luigi::Rename(name->str) : nullptr;
        if (r == nullptr) return Orig(system, name, ext);
        coop::luigi::SafeStr renamed = coop::luigi::MakeSafe(r);
        return Orig(system, &renamed, ext);
    }
};

// ResourceSystem::createResource inlines the TextureArc load ("ObjectData/<TextureArc>" ->
// findResourceCore / findResourceCategory / createResource), bypassing findOrCreateResource.
// Without these, Luigi's parts kept Mario's MarioHeadTexture (red cap with the M).
HOOK_DEFINE_TRAMPOLINE(FindResourceCoreHook) {
    static void* Callback(void* system, const coop::luigi::SafeStr* name, void* outIter) {
        const char* r = name ? coop::luigi::Rename(name->str) : nullptr;
        if (r == nullptr) return Orig(system, name, outIter);
        coop::luigi::SafeStr renamed = coop::luigi::MakeSafe(r);
        return Orig(system, &renamed, outIter);
    }
};

HOOK_DEFINE_TRAMPOLINE(FindResourceCategoryHook) {
    static void* Callback(void* system, const coop::luigi::SafeStr* name) {
        const char* r = name ? coop::luigi::Rename(name->str) : nullptr;
        if (r == nullptr) return Orig(system, name);
        coop::luigi::SafeStr renamed = coop::luigi::MakeSafe(r);
        return Orig(system, &renamed);
    }
};

HOOK_DEFINE_TRAMPOLINE(CreateResourceHook) {
    static void* Callback(void* system, const coop::luigi::SafeStr* name, void* category,
                          const char* ext) {
        const char* r = name ? coop::luigi::Rename(name->str) : nullptr;
        if (r == nullptr) return Orig(system, name, category, ext);
        coop::luigi::SafeStr renamed = coop::luigi::MakeSafe(r);
        return Orig(system, &renamed, category, ext);
    }
};

// ------------------------------------------------------------------------ exits (ChangeStageArea)
// rs::tryChangeNextStage(GameDataHolder*, Scene*) 0x4d2c54 (Ghidra) only asks whether player 0 is
// in a ChangeStageArea, then GameDataFunction::findAreaAndChangeNextStage(writer, player, nullptr)
// and ends the stage when the holder's change flags (+0x49/+0x4a) are set. Let P2 use exits too.
HOOK_DEFINE_TRAMPOLINE(TryChangeNextStageHook) {
    static bool Callback(void* holder, void* scene) {
        if (coop::gP2 != nullptr && coop::gP1 != nullptr && !coop::gGameOver &&
            !al::isDead(coop::gP2) &&
            !rs::isInChangeStageArea(coop::gP1, nullptr) &&
            rs::isInChangeStageArea(coop::gP2, nullptr)) {
            COOP_LOG("Exit: P2 walked into a ChangeStageArea");
            GameDataFunction::findAreaAndChangeNextStage(GameDataHolderWriter{holder}, coop::gP2,
                                                         nullptr);
        }
        return Orig(holder, scene);
    }
};

HOOK_DEFINE_TRAMPOLINE(FindResourceHook) {
    static void* Callback(const coop::luigi::SafeStr* name) {
        const char* r = name ? coop::luigi::Rename(name->str) : nullptr;
        if (r == nullptr) return Orig(name);
        coop::luigi::SafeStr renamed = coop::luigi::MakeSafe(r);
        return Orig(&renamed);
    }
};

// ------------------------------------------------------------------------ wardrobe (closet)
// Ghidra (1.0.0): Closet::receiveMsg 0x24e8f0. Normal mode: a player sensor's BindStart opens the
// wardrobe when rs::checkTriggerDecideWithRequestIcon sees "decide" (rs::isTriggerUiDecide ->
// al::getMainControllerPort); BindInit then puppets *that* player. "Player start" mode (+0x118,
// right after an outfit change) binds whoever answers first and keeps one puppet at +0x110.
//  - start mode: only P1 may answer (P2 overwrote +0x110 and Mario stayed bound).
//  - normal mode, message from P2: the decide check reads P2's controller, and a BindInit from
//    P2 starts a P2 wardrobe session (UI on P2's controller, outfit stored for P2 only).
namespace coop::closet {
constexpr uintptr_t kStateBegin = 0x4d42b8;  // StageSceneStateCloset::tryCreate
constexpr uintptr_t kStateEnd = 0x4d7cf8;    // end of StageSceneStateCloset::~StageSceneStateCloset
static bool CalledFromClosetState(void* ret) {
    uintptr_t off = reinterpret_cast<uintptr_t>(ret) - exl::util::modules::GetTargetStart();
    return off >= kStateBegin && off < kStateEnd;
}
// Crazy Cap counter: ShopStateInShop (0x31703c..0x31ac30, Ghidra) wears a bought item through
// exeBuyWait / prepareChangeWear / exeEndWear -> GameDataFunction::wearCostume/wearCap.
constexpr uintptr_t kShopBegin = 0x317000;
constexpr uintptr_t kShopEnd = 0x31ac40;
static bool CalledFromShopState(void* ret) {
    uintptr_t off = reinterpret_cast<uintptr_t>(ret) - exl::util::modules::GetTargetStart();
    return off >= kShopBegin && off < kShopEnd;
}

// ---- Luigi preview models.
// StageSceneStateCloset::initModel (0x4d5560, Ghidra) builds one preview model per entry of
// rs::getClothList + rs::getCapList: entry array at state+0x1a0 (stride 0x4b0, count +0x1a8),
// entry = FixedSafeString name (cstr at +0x8), +0x58 model actor, +0x278 cloth info (non-null:
// body entry), +0x498 cap info (non-null: cap entry), +0x4a0 PlayerCostumeInfo*,
// +0x4a8 PlayerJointControlPartsDynamics*. Each model is PlayerFunction::initMarioModelActorCloset(
// &e+0x4a8, actor, info, body, cap, const, state+0xc8, state+0xd4, state+0xe0, state+0xe8).
// "MarioInvisible" entries use body = cap = "Mario". Afterwards we build Luigi models the same
// way for the outfits the player owns (rs::isHaveCloth/isHaveCap), while the scene heap keeps
// kMinFreeHeap free, and swap them into the entries for a P2 session.
constexpr uintptr_t kEntryStride = 0x4b0;
constexpr size_t kMinFreeHeap = 64u * 1024u * 1024u;
struct PreviewSlot {
    void* actor;
    void* info;
    void* dyn;
};
static u8* gState = nullptr;
static PreviewSlot* gLuigiSlots = nullptr;
static s32 gSlotCount = 0;
static void* gLuigiFollowMtx = nullptr;  // state+0xe0 as left by the Luigi models
static bool gSwapped = false;

static size_t SceneHeapFree() {
    sead::Heap* heap = al::getSceneHeap();
    if (heap == nullptr) return 0;
    using FreeFn = size_t (*)(sead::Heap*);
    return (*reinterpret_cast<FreeFn**>(heap))[15](heap);
}

static void SwapEntries() {
    if (gState == nullptr || gLuigiSlots == nullptr) return;
    u8* arr = Field<u8*>(gState, 0x1a0);
    for (s32 i = 0; i < gSlotCount; i++) {
        PreviewSlot& s = gLuigiSlots[i];
        if (s.actor == nullptr) continue;
        u8* e = arr + i * kEntryStride;
        PreviewSlot m{Field<void*>(e, 0x58), Field<void*>(e, 0x4a0), Field<void*>(e, 0x4a8)};
        SetField(e, 0x58, s.actor);
        SetField(e, 0x4a0, s.info);
        SetField(e, 0x4a8, s.dyn);
        s = m;
    }
    void* f = Field<void*>(gState, 0xe0);
    SetField(gState, 0xe0, gLuigiFollowMtx);
    gLuigiFollowMtx = f;
    gSwapped = !gSwapped;
}

static void SetLuigiPreview(bool on) {
    if (on != gSwapped) SwapEntries();
}

static void Reset() {
    gState = nullptr;
    gLuigiSlots = nullptr;
    gSlotCount = 0;
    gSwapped = false;
}
}  // namespace coop::closet

// Game functions used by the Luigi preview (signatures from the 1.0.0 symbols).
extern void* GameCreateMarioConst(const char*) asm("_ZN14PlayerFunction16createMarioConstEPKc");
extern void GameLiveActorCtor(void*, const char*) asm("_ZN2al9LiveActorC1EPKc");
extern void* GameInitMarioModelActorCloset(void** dyn, void* actor, const al::ActorInitInfo& info,
                                           const char* body, const char* cap, void* playerConst,
                                           void* v0, void* v1, void* followMtx, void* mtx) asm(
    "_ZN14PlayerFunction25initMarioModelActorClosetEPP31PlayerJointControlPartsDynamicsPN2al9LiveActorERKNS3_13ActorInitInfoEPKcSA_PK11PlayerConstPN4sead7Vector3IfEESH_PP30PlayerJointControlFollowMtxPtrPNSE_8Matrix34IfEE");
extern void GameAccessorCtor(GameDataHolderWriter* out, const void* sceneObjHolder) asm(
    "_ZN22GameDataHolderAccessorC1EPKN2al18IUseSceneObjHolderE");
extern bool GameIsHaveCloth(GameDataHolderWriter acc, const char* name) asm(
    "_ZN2rs11isHaveClothE22GameDataHolderAccessorPKc");
extern bool GameIsHaveCap(GameDataHolderWriter acc, const char* name) asm(
    "_ZN2rs9isHaveCapE22GameDataHolderAccessorPKc");

HOOK_DEFINE_TRAMPOLINE(ClosetInitModelHook) {
    static void Callback(u8* state, const al::ActorInitInfo& info) {
        using namespace coop::closet;
        Orig(state, info);
        gState = state;
        gLuigiSlots = nullptr;
        gSlotCount = 0;
        gSwapped = false;
        s32 p1Port = al::getPlayerControllerPort(0);
        s32 p2Port = al::getPlayerControllerPort(1);
        s32 n = coop::Field<s32>(state, 0x1a8);
        u8* arr = coop::Field<u8*>(state, 0x1a0);
        void* scene = coop::Field<void*>(state, 0x18);
        COOP_LOG("Wardrobe: initModel entries=%d ports p1=%d p2=%d", n, p1Port, p2Port);
        if (p2Port < 0 || p2Port == p1Port) return;  // no Luigi this stage
        if (n <= 0 || arr == nullptr || scene == nullptr) return;
        if (!coop::CoopModeOn(reinterpret_cast<u8*>(scene) + 0x20)) return;  // 2P mode off
        GameDataHolderWriter acc{nullptr};
        GameAccessorCtor(&acc, reinterpret_cast<u8*>(scene) + 0x20);

        gLuigiSlots = static_cast<PreviewSlot*>(operator new(sizeof(PreviewSlot) * n));
        std::memset(gLuigiSlots, 0, sizeof(PreviewSlot) * n);
        gSlotCount = n;
        void* marioFollow = coop::Field<void*>(state, 0xe0);
        void* playerConst = GameCreateMarioConst("");
        size_t before = SceneHeapFree();
        s32 built = 0, skipped = 0;
        coop::gLuigiCloset = true;
        for (s32 i = 0; i < n; i++) {
            u8* e = arr + i * kEntryStride;
            const char* name = coop::Field<const char*>(e, 0x8);
            if (name == nullptr) continue;
            bool isBody = coop::Field<void*>(e, 0x278) != nullptr;
            bool isCap = coop::Field<void*>(e, 0x498) != nullptr;
            const char* body = "Mario";
            const char* cap = "Mario";
            if (std::strcmp(name, "MarioInvisible") != 0) {
                if (isBody) body = name;
                if (isCap) cap = name;
            }
            bool owned = std::strcmp(name, "Mario") == 0 ||
                         (isBody && GameIsHaveCloth(acc, name)) ||
                         (isCap && GameIsHaveCap(acc, name));
            if (!owned) continue;
            if (SceneHeapFree() < kMinFreeHeap) { skipped++; continue; }
            void* actor = operator new(0x108);
            GameLiveActorCtor(actor, "LuigiClosetModel");
            void* dyn = nullptr;
            void* ci = GameInitMarioModelActorCloset(&dyn, actor, info, body, cap, playerConst,
                                                     state + 0xc8, state + 0xd4, state + 0xe0,
                                                     state + 0xe8);
            gLuigiSlots[i] = PreviewSlot{actor, ci, dyn};
            built++;
        }
        coop::gLuigiCloset = false;
        gLuigiFollowMtx = coop::Field<void*>(state, 0xe0);
        coop::SetField(state, 0xe0, marioFollow);
        COOP_LOG("Wardrobe: %d Luigi previews built (%d skipped for memory), heap %lu -> %lu KB",
                 built, skipped, (unsigned long)(before / 1024),
                 (unsigned long)(SceneHeapFree() / 1024));
    }
};

HOOK_DEFINE_TRAMPOLINE(ClosetReceiveMsgHook) {
    static bool Callback(al::LiveActor* self, const al::SensorMsg* msg, al::HitSensor* other,
                         al::HitSensor* own) {
        bool startMode = coop::Field<u8>(self, smo100::Closet_isPlayerStart) != 0;
        al::LiveActor* host = other ? al::getSensorHost(other) : nullptr;
        if (startMode && coop::gClosetStartP2 && coop::gP2 != nullptr) {
            // Luigi changed clothes: he is the one who steps out and poses.
            if (host == coop::gP1) return false;
            if (host != coop::gP2) return Orig(self, msg, other, own);
            if (!coop::gClosetSwapDone) return false;  // wait until Luigi stands at the booth
            bool prevT = coop::gToucherIsP2;
            coop::gToucherIsP2 = true;
            bool r = Orig(self, msg, other, own);
            coop::gToucherIsP2 = prevT;
            if (r && al::isMsgBindInit(msg)) COOP_LOG("Wardrobe: Luigi steps out of the booth");
            return r;
        }
        if (coop::gP2 == nullptr || other == nullptr || host != coop::gP2)
            return Orig(self, msg, other, own);
        if (startMode) return false;  // after Mario's outfit change the booth belongs to P1
        if (coop::gClosetP2 == false && coop::gP2Port < 0) return false;
        // Ghidra: rs::checkTriggerDecideWithRequestIcon -> rs::isPlayerEnableTalkGround(closet)
        // -> al::getPlayerActor(closet, 0), and the balloon request compares rs::getPlayerPos.
        // Answer P2 there (gToucherIsP2), so Luigi's prompt doesn't depend on where Mario is.
        bool prev = coop::gUiPortP2, prevT = coop::gToucherIsP2;
        coop::gUiPortP2 = true;
        coop::gToucherIsP2 = true;
        bool r = Orig(self, msg, other, own);
        coop::gUiPortP2 = prev;
        coop::gToucherIsP2 = prevT;
        static s32 sLogBudget = 40;
        if (al::isMsgBindStart(msg) && sLogBudget > 0) {
            sLogBudget--;
            COOP_LOG("Wardrobe: P2 BindStart -> %d", (int)r);
        }
        if (r && al::isMsgBindInit(msg)) {
            coop::gClosetP2 = true;
            coop::closet::SetLuigiPreview(true);
            COOP_LOG("Wardrobe: P2 session started");
        }
        return r;
    }
};

// Closet::attackSensor(own, other) 0x24eaac shows the "A" balloon when a player sensor touches
// it, using player 0's talk-ground state. For P2's sensor, ask about P2 and P2's controller.
HOOK_DEFINE_TRAMPOLINE(ClosetAttackSensorHook) {
    static bool Callback(al::LiveActor* self, al::HitSensor* own, al::HitSensor* other) {
        if (coop::gP2 == nullptr || other == nullptr || al::getSensorHost(other) != coop::gP2)
            return Orig(self, own, other);
        if (coop::Field<u8>(self, smo100::Closet_isPlayerStart) != 0 && !coop::gClosetStartP2)
            return false;
        bool prev = coop::gUiPortP2, prevT = coop::gToucherIsP2;
        coop::gUiPortP2 = true;
        coop::gToucherIsP2 = true;
        bool r = Orig(self, own, other);
        coop::gUiPortP2 = prev;
        coop::gToucherIsP2 = prevT;
        return r;
    }
};

// ------------------------------------------------------------------------ NPC talk for P2
// Ghidra: EventFlowNodeMessageBalloon::exeWait (0x1b79ec) decides whether a talk starts from
// player 0's view: rs::getPlayerPos(npc), rs::isPlayerEnableTalkGround/Swim(npc),
// rs::isPlayerInputTriggerStartTalk(npc) (player 0's own input port), then
// rs::tryLockStartEventTalkDemo(node) (0x55a118). When P2 is closer to the NPC than P1, run it
// from P2's view; if the talk locks, the talk UI follows P2's controller until the demo ends.
static float Dist2(const al::LiveActor* a, const al::LiveActor* b) {
    const sead::Vector3f& p = al::getTrans(a);
    const sead::Vector3f& q = al::getTrans(b);
    float dx = p.x - q.x, dy = p.y - q.y, dz = p.z - q.z;
    return dx * dx + dy * dy + dz * dz;
}

HOOK_DEFINE_TRAMPOLINE(TalkBalloonWaitHook) {
    static void Callback(u8* node) {
        al::LiveActor* npc = coop::Field<al::LiveActor*>(node, 0x18);
        if (coop::gP1 == nullptr || coop::gP2 == nullptr || npc == nullptr || coop::gTalkP2 ||
            Dist2(coop::gP2, npc) >= Dist2(coop::gP1, npc))
            return Orig(node);
        bool prevT = coop::gToucherIsP2, prevU = coop::gUiPortP2;
        coop::gToucherIsP2 = true;
        coop::gUiPortP2 = true;
        Orig(node);
        coop::gToucherIsP2 = prevT;
        coop::gUiPortP2 = prevU;
    }
};

HOOK_DEFINE_TRAMPOLINE(TryLockTalkDemoHook) {
    static bool Callback(void* node) {
        bool r = Orig(node);
        if (r) coop::gShopP2 = coop::gToucherIsP2 && coop::gP2 != nullptr;
        if (r && coop::gToucherIsP2 && coop::gP2 != nullptr) {
            coop::gTalkP2 = true;
            coop::gTalkNoDemoFrames = 0;
            COOP_LOG("Talk: P2 started a conversation");
        }
        return r;
    }
};


// ------------------------------------------------------------------------ cutscenes (3D World)
// ProjectDemoDirector::requestStartDemo(name, type) (Ghidra 0xd5978): types 3-8 and 10 put every
// player into a demo state (vptr+0x120/0x130/0x140/0x178/0x150/0x160) and the cutscene then moves
// only player 0 (boss intros, Moon get, kingdom scenes). The other brother would stand frozen in
// the shot. Hide him for the cutscene (rs::hideDemoPlayer resolves player 0 from the actor, which
// our redirect answers with that brother) and bring him back beside the lead when it ends.
// Captures (HackStart), Luigi's conversations and the wardrobe keep their own handling.
extern void RsHideDemoPlayer(const al::LiveActor*) asm("_ZN2rs14hideDemoPlayerEPKN2al9LiveActorE");
extern void RsShowDemoPlayer(const al::LiveActor*) asm("_ZN2rs14showDemoPlayerEPKN2al9LiveActorE");
namespace coop::cut {
static al::LiveActor* gHidden = nullptr;
static void Reset() { gHidden = nullptr; }
}  // namespace coop::cut
HOOK_DEFINE_TRAMPOLINE(DemoStartHook) {
    static bool Callback(void* dir, const char* name, s32 type) {
        bool capture = coop::gHackDemoStarter != nullptr;
        bool r = Orig(dir, name, type);
        if (!r || capture || type < 3 || type == 9 || coop::gTalkP2 || coop::gClosetP2) return r;
        if (coop::gP1 == nullptr || coop::gP2 == nullptr || coop::cut::gHidden != nullptr) return r;
        al::LiveActor* other = coop::gLeadP2 ? coop::gP1 : coop::gP2;
        s32 slot = other == coop::gP1 ? 0 : 1;
        if (al::isDead(other) || coop::gBubbleBusy[slot] > 60) return r;
        RsHideDemoPlayer(other);
        coop::cut::gHidden = other;
        COOP_LOG("Cutscene %s (type %d): P%d hidden", name ? name : "?", type, slot + 1);
        return r;
    }
};
HOOK_DEFINE_TRAMPOLINE(DemoEndHook) {
    static u64 Callback(void* dir, const char* name, s32 type) {
        u64 ret = Orig(dir, name, type);
        al::LiveActor* h = coop::cut::gHidden;
        if (h == nullptr) return ret;
        coop::cut::gHidden = nullptr;
        if (h != coop::gP1 && h != coop::gP2) return ret;
        RsShowDemoPlayer(h);
        al::LiveActor* lead = h == coop::gP1 ? coop::gP2 : coop::gP1;
        if (lead != nullptr && !al::isDead(lead)) coop::PlaceBeside(h, lead, "after cutscene");
        return ret;
    }
};
// ------------------------------------------------------------------------ captures for both
// Ghidra: a capture starts a HackStart demo: PlayerHackKeeper::startHackStartDemo(Puppetable)
// (0x449274/0x4492b8) -> rs::requestStartDemoHackStart -> ProjectDemoDirector::requestStartDemo
// (0xd5978) type 3, which requires isEnableDemo (vptr+0x108 = PlayerActorHakoniwa::isEnableDemo
// 0x421754) on every player and calls startDemoPuppetable (0x421b84) on every player; ending
// calls endDemoPuppetable (0x421e2c). startDemoPuppetable -> PlayerHackKeeper::sendMarioDemo:
// the other brother's captured enemy refuses HackMarioDemo -> sendMsgCancelHack -> he pops out.
// The capture swoop only concerns the brother capturing: the other one skips that demo.
static s32 PlayerSlot(const void* actor) {
    if (actor == nullptr) return -1;
    if (actor == coop::gP1) return 0;
    if (actor == coop::gP2) return 1;
    return -1;
}

HOOK_DEFINE_TRAMPOLINE(HackStartDemoHook) {
    static bool Callback(void* keeper, al::LiveActor* actor) {
        al::LiveActor* prev = coop::gHackDemoStarter;
        coop::gHackDemoStarter = coop::Field<al::LiveActor*>(keeper, 0x0);
        bool r = Orig(keeper, actor);
        coop::gHackDemoStarter = prev;
        return r;
    }
};

HOOK_DEFINE_TRAMPOLINE(HackStartDemoPuppetableHook) {
    static bool Callback(void* keeper, al::LiveActor* actor) {
        al::LiveActor* prev = coop::gHackDemoStarter;
        coop::gHackDemoStarter = coop::Field<al::LiveActor*>(keeper, 0x0);
        bool r = Orig(keeper, actor);
        coop::gHackDemoStarter = prev;
        return r;
    }
};

static bool IsOtherBrotherOfHackDemo(const void* player) {
    return coop::gHackDemoStarter != nullptr && coop::gP2 != nullptr && PlayerSlot(player) >= 0 &&
           player != coop::gHackDemoStarter && PlayerSlot(coop::gHackDemoStarter) >= 0;
}

HOOK_DEFINE_TRAMPOLINE(PlayerIsEnableDemoHook) {
    static bool Callback(void* player) {
        if (IsOtherBrotherOfHackDemo(player)) return true;  // doesn't take part, never blocks it
        return Orig(player);
    }
};

HOOK_DEFINE_TRAMPOLINE(PlayerStartDemoPuppetableHook) {
    static void Callback(void* player) {
        if (IsOtherBrotherOfHackDemo(player)) {
            coop::gSkippedHackDemo[PlayerSlot(player)] = true;
            COOP_LOG("Capture: %s keeps playing during the other's capture",
                     player == coop::gP1 ? "P1" : "P2");
            return;
        }
        Orig(player);
    }
};

HOOK_DEFINE_TRAMPOLINE(PlayerEndDemoPuppetableHook) {
    static void Callback(void* player) {
        s32 slot = PlayerSlot(player);
        if (slot >= 0 && coop::gSkippedHackDemo[slot]) {
            coop::gSkippedHackDemo[slot] = false;
            return;
        }
        Orig(player);
    }
};

// ------------------------------------------------------------------------ NPCs face the nearest
// TalkNpc::movement (0x3ddb38) runs the NPC's look-at, turning and event flow, all asking for
// player 0. Answer the brother it is talking with, otherwise the nearer brother.
HOOK_DEFINE_TRAMPOLINE(TalkNpcMovementHook) {
    static void Callback(al::LiveActor* npc) {
        if (coop::gP1 == nullptr || coop::gP2 == nullptr) return Orig(npc);
        al::LiveActor* target;
        if (coop::gTalkP2) target = coop::gP2;
        else if (rs::isActiveDemo(coop::gP1)) target = coop::gP1;
        else target = Dist2(coop::gP2, npc) < Dist2(coop::gP1, npc) ? coop::gP2 : coop::gP1;
        al::LiveActor* prev = coop::gNpcTarget;
        coop::gNpcTarget = target;
        Orig(npc);
        coop::gNpcTarget = prev;
    }
};

// ------------------------------------------------------------------------ Luigi's hands
// Hands are al::PartsModel sub-actors set up by al::PartsModel::initPartsFixFileNoRegister
// (0x96cd78; args: parent, info, archive, ?, suffix "HandL"/"HandR"). From the body's
// InitPartsFixInfoHand* byml it stores UseFollowMtxScale at +0x140 and UseLocalScale at +0x141;
// PartsModel::updatePose (0x96cfd0) copies the wrist joint's scale onto the hand when +0x140 is
// set. On Tall Luigi that scale balloons the fist in the jump animation. Luigi's hands keep
// their own size (scale 1) instead.
HOOK_DEFINE_TRAMPOLINE(PartsFixInitHook) {
    static void Callback(u8* self, al::LiveActor* parent, const al::ActorInitInfo& info,
                         const char* archive, const char* arg4, const char* suffix) {
        Orig(self, parent, info, archive, arg4, suffix);
        if (!(coop::gSpawningP2 || coop::gLuigiCloset) || suffix == nullptr) return;
        if (std::strcmp(suffix, "HandL") == 0 || std::strcmp(suffix, "HandR") == 0) {
            self[0x140] = 0;  // UseFollowMtxScale
            self[0x141] = 0;  // UseLocalScale
        }
    }
};

// ------------------------------------------------------------------------ 2P mode switch
// Ghidra: rs::isSeparatePlay(holder) 0x575af4 = GameDataHolder(sceneObj 0x12)+0x245. Callers are
// the pause menu (StageSceneStatePauseMenu 0x4e9d8c..0x4ebb54: shows/toggles "2P mode") and the
// vanilla Cappy-for-player-2 code (PlayerInput, PlayerInputFunction, HackCap, StageScene::init ->
// rs::setSeparatePlayMode, info windows...). Only the pause menu and this mod see the real
// value; everything else is told 1P so vanilla 2P Cappy never starts.
constexpr uintptr_t kPauseMenuBegin = 0x4e9d8c;
constexpr uintptr_t kPauseMenuEnd = 0x4ebb54;
constexpr uintptr_t kMainTextEnd = 0x1800000;
HOOK_DEFINE_TRAMPOLINE(SeparatePlayHook) {
    static bool Callback(const void* holder) {
        uintptr_t off = reinterpret_cast<uintptr_t>(__builtin_return_address(0)) -
                        exl::util::modules::GetTargetStart();
        if ((off >= kPauseMenuBegin && off < kPauseMenuEnd) || off >= kMainTextEnd)
            return Orig(holder);
        return false;
    }
};

namespace coop {
// Switching 2P mode mid-stage: re-enter the stage (GameDataFunction::reenterStage sets
// GameDataHolder+0x4a, picked up by rs::tryChangeNextStage) so Luigi joins or leaves.
static void CheckModeSwitch(StageScene* scene) {
    if (gReenterRequested || ++gModeFrames < 60) return;
    const u8* holder = reinterpret_cast<const u8*>(scene) + 0x20;
    bool want = CoopModeOn(holder);
    s32 p2Port = al::getPlayerControllerPort(1);
    if (want && (p2Port < 0 || p2Port == al::getPlayerControllerPort(0))) want = false;
    bool have = gP2 != nullptr;
    if (want == have) { gModeMismatchFrames = 0; return; }
    if (++gModeMismatchFrames < 20) return;
    GameDataHolderWriter w{nullptr};
    GameAccessorCtor(&w, holder);
    if (w.holder == nullptr) return;
    gReenterRequested = true;
    COOP_LOG("2P mode switched %s: re-entering the stage", want ? "on" : "off");
    GameDataFunction::reenterStage(w);
}
}  // namespace coop

// ------------------------------------------------------------------------ prompts + gaze for P2
// The "A" prompt and NPC gaze are decided from player 0's point of view in a few places
// (Ghidra 1.0.0):
//  - NpcEventDirector::updateBalloon (0x3a3ee4) pops the requested balloon (owner actor stored
//    in NpcEventCtrlInfo+0x38, ctrl at director+0x40) and hides/shows it from rs::getPlayerPos /
//    area checks of player 0.
//  - rs::checkTriggerDecideWithRequestIcon (0x55b2fc): talk-ground check, balloon request and
//    the "decide" button, all for player 0 (signs, hint art, music lists, wardrobe...).
//  - NpcJointLookAtController::update (0x3a65c4): looks at the sensor stored in its
//    PlayerEyeSensorHitHolder (+0x28 -> holder, holder+0x30 = HitSensor, HitSensor+0 = name);
//    it only switches when the other sensor is strictly nearer, so it sticks to Mario.
// When P2 is the nearer brother, run these from P2's side.
static bool P2Nearer(const al::LiveActor* actor) {
    return coop::gP1 != nullptr && coop::gP2 != nullptr && actor != nullptr &&
           !al::isDead(coop::gP2) && Dist2(coop::gP2, actor) < Dist2(coop::gP1, actor);
}

HOOK_DEFINE_TRAMPOLINE(UpdateBalloonHook) {
    static void Callback(u8* director) {
        u8* ctrl = coop::Field<u8*>(director, 0x40);
        al::LiveActor* owner = ctrl ? coop::Field<al::LiveActor*>(ctrl, 0x38) : nullptr;
        if (coop::gTalkP2 || !P2Nearer(owner)) return Orig(director);
        bool prevT = coop::gToucherIsP2;
        coop::gToucherIsP2 = true;
        Orig(director);
        coop::gToucherIsP2 = prevT;
    }
};

HOOK_DEFINE_TRAMPOLINE(DecideWithIconHook) {
    static bool Callback(float radius, al::LiveActor* actor, const sead::Vector3f& offset) {
        if (coop::gToucherIsP2 || !P2Nearer(actor)) return Orig(radius, actor, offset);
        bool prevT = coop::gToucherIsP2, prevU = coop::gUiPortP2;
        coop::gToucherIsP2 = true;
        coop::gUiPortP2 = true;
        bool r = Orig(radius, actor, offset);
        coop::gToucherIsP2 = prevT;
        coop::gUiPortP2 = prevU;
        return r;
    }
};

extern al::HitSensor* GameGetHitSensor(const al::LiveActor*, const char*) asm(
    "_ZN2al12getHitSensorEPKNS_9LiveActorEPKc");

HOOK_DEFINE_TRAMPOLINE(NpcLookAtUpdateHook) {
    static void Callback(u8* ctl) {
        al::LiveActor* npc = coop::Field<al::LiveActor*>(ctl, 0x8);
        u8* holder = coop::Field<u8*>(ctl, 0x28);
        if (npc != nullptr && holder != nullptr && coop::gP1 != nullptr && coop::gP2 != nullptr) {
            al::HitSensor* cur = coop::Field<al::HitSensor*>(holder, 0x30);
            if (cur != nullptr) {
                al::LiveActor* host = al::getSensorHost(cur);
                al::LiveActor* want = coop::gTalkP2 ? coop::gP2 : (P2Nearer(npc) ? coop::gP2 : coop::gP1);
                if ((host == coop::gP1 || host == coop::gP2) && host != want) {
                    const char* name = coop::Field<const char*>(cur, 0x0);
                    al::HitSensor* alt = name ? GameGetHitSensor(want, name) : nullptr;
                    if (alt != nullptr) coop::SetField(holder, 0x30, alt);
                    static s32 sLog = 20;
                    if (sLog > 0) {
                        sLog--;
                        COOP_LOG("Gaze: NPC %p now looks at %s (%s)", npc,
                                 want == coop::gP2 ? "P2" : "P1", alt ? "ok" : "no sensor");
                    }
                }
            }
        }
        Orig(ctl);
    }
};

// Event-flow NPCs (shopkeepers' ShoppingWatcher, Toads, Sphynx...): al::EventFlowExecutor::execute
// (0x88b918; actor at +0x8) runs the NPC's event nodes and EventFlowMovement (turning/walking
// toward "the player"). Same rule as TalkNpc::movement: the brother talking, else the nearer one.
HOOK_DEFINE_TRAMPOLINE(EventFlowExecuteHook) {
    static void Callback(u8* executor) {
        al::LiveActor* npc = coop::Field<al::LiveActor*>(executor, 0x8);
        if (coop::gP1 == nullptr || coop::gP2 == nullptr || npc == nullptr) return Orig(executor);
        al::LiveActor* target;
        if (coop::gTalkP2) target = coop::gP2;
        else if (rs::isActiveDemo(coop::gP1)) target = coop::gP1;
        else target = Dist2(coop::gP2, npc) < Dist2(coop::gP1, npc) ? coop::gP2 : coop::gP1;
        al::LiveActor* prev = coop::gNpcTarget;
        coop::gNpcTarget = target;
        Orig(executor);
        coop::gNpcTarget = prev;
    }
};

// NPC "look position": NpcEventDirector::execute (0x3a3b68) writes player 0's head position into
// the shared NpcEventCtrlInfo look slot every frame; rs::getNpcLookPos(npc) (0x55ad2c) hands it to
// NpcJointLookAtController::update and rs::tryUpdateNpcEyeLineAnim, which prefer it over the eye
// sensor. That is why shopkeepers' eyes stayed on Mario. Answer per NPC: Luigi's head when he is
// the one talking or the nearer brother.
extern bool GameCalcHeadJointPos(sead::Vector3f*, const al::LiveActor*) asm(
    "_ZN2rs30tryCalcPlayerModelHeadJointPosEPN4sead7Vector3IfEEPKN2al9LiveActorE");
extern const sead::Vector3f& GameGetPlayerHeadPos(const al::LiveActor*) asm(
    "_ZN2rs16getPlayerHeadPosEPKN2al9LiveActorE");
static sead::Vector3f gP2LookPos[4];
static u32 gP2LookIdx = 0;
HOOK_DEFINE_TRAMPOLINE(NpcLookPosHook) {
    static const sead::Vector3f* Callback(const al::LiveActor* npc) {
        const sead::Vector3f* r = Orig(npc);
        if (coop::gP1 == nullptr || coop::gP2 == nullptr || npc == nullptr) return r;
        bool p2 = coop::gTalkP2 || (!rs::isActiveDemo(coop::gP1) && P2Nearer(npc));
        if (!p2) return r;
        sead::Vector3f& out = gP2LookPos[gP2LookIdx++ & 3];
        if (!GameCalcHeadJointPos(&out, coop::gP2)) out = GameGetPlayerHeadPos(coop::gP2);
        return &out;
    }
};

// Closet::control (0x24e62c): in player-start mode its bind sensor follows rs::getPlayerPos(this)
// (player 0). When Luigi is the one coming out, follow Luigi; stop once the booth is done (+0x118
// cleared by exeWearEnd).
HOOK_DEFINE_TRAMPOLINE(ClosetControlHook) {
    static void Callback(al::LiveActor* self) {
        if (!coop::gClosetStartP2 || coop::gP2 == nullptr ||
            coop::Field<u8>(self, smo100::Closet_isPlayerStart) == 0)
            return Orig(self);
        bool prevT = coop::gToucherIsP2;
        coop::gToucherIsP2 = true;
        Orig(self);
        coop::gToucherIsP2 = prevT;
    }
};

// ------------------------------------------------------------------------ one A button per prompt
// Ghidra: rs::isTriggerUiDecide(IUseSceneObjHolder*) 0x575ba8 / (GameDataHolder*) 0x575bf8 and
// rs::isTriggerUiCancel 0x575c40 read GameDataHolder+0x245 (2P mode) directly: when it is on they
// accept the button from player port 0 OR 1. With co-op on that let Mario confirm Luigi's prompt
// (and Luigi Mario's). Read only the main controller port instead; MainControllerPortHook makes
// that Luigi's port while a prompt, talk or wardrobe belongs to him.
extern bool GameIsPadTriggerA(s32 port) asm("_ZN2al13isPadTriggerAEi");
extern bool GameIsPadTriggerB(s32 port) asm("_ZN2al13isPadTriggerBEi");
extern s32 GameMainControllerPort() asm("_ZN2al21getMainControllerPortEv");

HOOK_DEFINE_TRAMPOLINE(UiDecideHook) {
    static bool Callback(const void* holder) {
        if (!coop::CoopModeOn(holder)) return Orig(holder);
        return GameIsPadTriggerA(GameMainControllerPort());
    }
};
HOOK_DEFINE_TRAMPOLINE(UiDecideGdhHook) {
    static bool Callback(const u8* gdh) {
        if (gdh == nullptr || gdh[0x245] == 0) return Orig(gdh);
        return GameIsPadTriggerA(GameMainControllerPort());
    }
};
HOOK_DEFINE_TRAMPOLINE(UiCancelHook) {
    static bool Callback(const void* holder) {
        if (!coop::CoopModeOn(holder)) return Orig(holder);
        return GameIsPadTriggerB(GameMainControllerPort());
    }
};

// ------------------------------------------------------------------------ whose prompt is shown
// The game shows one prompt balloon at a time: NpcEventCtrlInfo::requestShowBalloonMessage
// (0x3a2f10) keeps one request per frame (owner actor at ctrl+0x38, priority at ctrl+0x6c vs
// info+0x34). With two brothers, among requests of the same priority keep the one whose brother is
// closest to his own object, so Luigi's prompt shows when he is at a counter even if Mario is
// standing near something else.
static float gReqDist = 0.0f;
HOOK_DEFINE_TRAMPOLINE(BalloonRequestHook) {
    static void Callback(u8* ctrl, const u8* info) {
        if (coop::gP1 == nullptr || coop::gP2 == nullptr || ctrl == nullptr || info == nullptr)
            return Orig(ctrl, info);
        al::LiveActor* actor = coop::Field<al::LiveActor*>(info, 0x0);
        al::LiveActor* who = coop::gToucherIsP2 ? coop::gP2 : coop::gP1;
        float d = actor ? Dist2(who, actor) : 1e30f;
        al::LiveActor* cur = coop::Field<al::LiveActor*>(ctrl, 0x38);
        if (cur == nullptr) {
            Orig(ctrl, info);
            if (coop::Field<al::LiveActor*>(ctrl, 0x38) == actor) gReqDist = d;
            return;
        }
        if (coop::Field<s32>(info, 0x34) != coop::Field<s32>(ctrl, 0x6c)) {
            Orig(ctrl, info);
            if (coop::Field<al::LiveActor*>(ctrl, 0x38) == actor) gReqDist = d;
            return;
        }
        if (d >= gReqDist) return;  // the current one is closer to its brother
        coop::SetField<al::LiveActor*>(ctrl, 0x38, nullptr);
        Orig(ctrl, info);
        if (coop::Field<al::LiveActor*>(ctrl, 0x38) == actor) gReqDist = d;
        else coop::SetField<al::LiveActor*>(ctrl, 0x38, cur);  // rejected (off camera etc.)
    }
};

// ------------------------------------------------------------------------ Moons for Luigi
// Ghidra: the Moon-get demo is played by "player 0" everywhere: StageSceneStateGetShine and
// CameraPoserGetMoon (+0x178) use rs::getDemoPlayerTrans/Quat, rs::startActionDemoPlayer and
// rs::replaceDemoPlayer, all al::getPlayerActor(actor, 0) -> the player's own model and animator;
// rs::getPlayerActor(scene) and ProjectDemoDirector use al::getPlayerActor(PlayerHolder, i) ->
// PlayerHolder::tryGetPlayer (0x9a3df4) / getPlayer (0x9a3de4). When Luigi collects the Moon
// (Shine::receiveMsg 0x1cfb68 gets ShineGet/ShineGet2D from his side), swap players 0 and 1
// until the Moon demo ends (StageSceneStateGetShine::kill 0x4dbc5c, or no demo for 1 s), so
// Luigi does the dance and the camera frames him while Mario waits in the demo.
extern bool GameIsMsgShineGet(const al::SensorMsg*) asm("_ZN2rs13isMsgShineGetEPKN2al9SensorMsgE");
extern bool GameIsMsgShineGet2D(const al::SensorMsg*) asm("_ZN2rs15isMsgShineGet2DEPKN2al9SensorMsgE");

static bool IsP2Side(const al::LiveActor* a) {
    return a != nullptr && coop::gP2 != nullptr &&
           (a == coop::gP2 || a == coop::gP2Cap || a == coop::P2HackActor());
}

HOOK_DEFINE_TRAMPOLINE(ShineReceiveMsgHook) {
    static bool Callback(al::LiveActor* self, const al::SensorMsg* msg, al::HitSensor* other,
                         al::HitSensor* own) {
        bool get = coop::gP2 != nullptr && (GameIsMsgShineGet(msg) || GameIsMsgShineGet2D(msg));
        if (!get) return Orig(self, msg, other, own);
        al::LiveActor* host = other ? al::getSensorHost(other) : nullptr;
        bool p2 = IsP2Side(host);
        bool prevLead = coop::gLeadP2;
        if (p2) coop::gLeadP2 = true;
        bool r = Orig(self, msg, other, own);
        if (p2) {
            if (r) {
                coop::gLeadFrames = 0;
                coop::gLeadNoDemo = 0;
                if (!prevLead) COOP_LOG("Moon: Luigi got it - he leads the Moon demo");
            } else {
                coop::gLeadP2 = prevLead;
            }
        } else if (r) {
            coop::gLeadP2 = false;  // Mario got it
        }
        return r;
    }
};

HOOK_DEFINE_TRAMPOLINE(GetShineKillHook) {
    static void Callback(void* state) {
        Orig(state);
        if (coop::gLeadP2) COOP_LOG("Moon: demo over, Mario is player 0 again");
        coop::gLeadP2 = false;
    }
};

HOOK_DEFINE_TRAMPOLINE(HolderTryGetPlayerHook) {
    static al::LiveActor* Callback(const void* holder, s32 index) {
        if (coop::gLeadP2 && (index == 0 || index == 1) && coop::gP1 && coop::gP2) index = 1 - index;
        return Orig(holder, index);
    }
};
HOOK_DEFINE_TRAMPOLINE(HolderGetPlayerHook) {
    static al::LiveActor* Callback(const void* holder, s32 index) {
        if (coop::gLeadP2 && (index == 0 || index == 1) && coop::gP1 && coop::gP2) index = 1 - index;
        return Orig(holder, index);
    }
};

// Crazy Cap "wear it now": after the reload ShoppingWatcherStateWear::appear (0x316a88, Ghidra)
// calls rs::requestBindPlayer(shopkeeper, sensor) -> binds player 0, and exeAction plays the
// pose on that puppet. When Luigi bought and wore the item, bind Luigi instead.
HOOK_DEFINE_TRAMPOLINE(ShopWearAppearHook) {
    static void Callback(void* state) {
        if (!coop::gClosetStartP2 || coop::gP2 == nullptr) return Orig(state);
        bool prevT = coop::gToucherIsP2;
        coop::gToucherIsP2 = true;
        Orig(state);
        coop::gToucherIsP2 = prevT;
        COOP_LOG("Shop: Luigi shows off his new outfit");
    }
};

// Leaving a stage: StageScene::~StageScene (D2 0x4c8474). Other scenes run afterwards (e.g.
// DemoChangeWorldScene, the Odyssey flight between kingdoms) while our camera hooks
// (PlayerCameraTarget::calcTrans, CameraPoser::makeLookAtCamera) still run every frame; with
// gP1/gP2 pointing at the destroyed players they read freed memory (crash in CameraDirector::
// update). Forget everything about the old stage before it is freed.
HOOK_DEFINE_TRAMPOLINE(StageSceneDtorHook) {
    static void* Callback(void* scene) {
        coop::ResetStageState();
        coop::cam::Reset();
        coop::echo::Reset();
        coop::hud::Reset();
        coop::closet::Reset();
        coop::dim::Reset();
        coop::text::Reset();
        coop::voice::Reset();
        coop::boss::Reset();
        coop::cut::Reset();
        coop::gClosetP2 = false;
        coop::gUiPortP2 = false;
        COOP_LOG("StageScene destroyed: co-op state cleared");
        return Orig(scene);
    }
};

// ------------------------------------------------------------------------ "Luigi" in dialogue
// Ghidra: text is fetched through al::MessageSystem::getStageMessageHolder(name) (0x93d208) and
// getSystemMessageHolder(name) (0x93cbbc) / (name, label) (0x93ce40) every time (no caching);
// a MessageHolder (0x10 bytes, C1 0x938f10) is filled by MessageHolder::init(archivePath, file)
// (0x938fc8: findOrCreateResource(archivePath) + "<file>.msbt"). The Luigi pack has
// LocalizedData/USen/MessageData/P2Luigi_StageMessage / P2Luigi_SystemMessage with the same
// files where the player is "Luigi" (CharacterName.msbt etc.). While Luigi leads a conversation,
// hand out holders built from those archives (English only; other languages stay vanilla).
extern void GameMessageHolderCtor(void*) asm("_ZN2al13MessageHolderC1Ev");
extern void GameMessageHolderInit(void*, const char*, const char*) asm(
    "_ZN2al13MessageHolder4initEPKcS2_");
extern const char16_t* GameMessageHolderTryGetText(const void*, const char*) asm(
    "_ZNK2al13MessageHolder10tryGetTextEPKc");
extern const char* GameLanguageString() asm("_ZN2al17getLanguageStringEv");
namespace coop::text {
struct Entry { char name[64]; void* holder; };
static Entry gStage[96];
static Entry gSystem[96];
static s32 gStageN = 0, gSystemN = 0;
static s32 gEnglish = -1;
static void Reset() { gStageN = gSystemN = 0; }
static bool Enabled() {
    if (!gTalkP2 || gP2 == nullptr) return false;
    if (gEnglish < 0) {
        const char* l = GameLanguageString();
        gEnglish = (l != nullptr && std::strcmp(l, "USen") == 0) ? 1 : 0;
        COOP_LOG("Text: language %s, Luigi dialogue %s", l ? l : "?", gEnglish ? "on" : "off");
    }
    return gEnglish == 1;
}
static void* Get(Entry* tab, s32& n, const char* archive, const char* name) {
    if (name == nullptr) return nullptr;
    size_t len = std::strlen(name);
    if (len == 0 || len >= 64) return nullptr;
    for (s32 i = 0; i < n; i++) if (std::strcmp(tab[i].name, name) == 0) return tab[i].holder;
    if (n >= 96) return nullptr;
    void* h = operator new(0x10);
    GameMessageHolderCtor(h);
    GameMessageHolderInit(h, archive, name);
    // MessageHolder+8 -> sead::MessageSetBase, whose +8 is the LMS message data. When the Luigi
    // archive lacks this file the data stays null and every lookup (e.g. Talkatoo's
    // isExistLabelInStageMessage) dereferences it: crash. Keep using the game's holder then.
    void* set = *reinterpret_cast<void**>(static_cast<u8*>(h) + 8);
    if (set == nullptr || *reinterpret_cast<void**>(static_cast<u8*>(set) + 8) == nullptr) {
        COOP_LOG("Text: no Luigi version of %s, using the original", name);
        h = nullptr;  // (the half-built holder is leaked once per file; tiny)
    }
    std::memcpy(tab[n].name, name, len + 1);
    tab[n].holder = h;
    n++;
    return h;
}
}  // namespace coop::text

HOOK_DEFINE_TRAMPOLINE(StageMessageHolderHook) {
    static void* Callback(const void* sys, const char* name) {
        void* van = Orig(sys, name);
        if (van == nullptr || !coop::text::Enabled()) return van;
        void* l = coop::text::Get(coop::text::gStage, coop::text::gStageN,
                                  "LocalizedData/USen/MessageData/P2Luigi_StageMessage", name);
        return l ? l : van;
    }
};
HOOK_DEFINE_TRAMPOLINE(SystemMessageHolderHook) {
    static void* Callback(const void* sys, const char* name) {
        void* van = Orig(sys, name);
        if (van == nullptr || !coop::text::Enabled()) return van;
        void* l = coop::text::Get(coop::text::gSystem, coop::text::gSystemN,
                                  "LocalizedData/USen/MessageData/P2Luigi_SystemMessage", name);
        return l ? l : van;
    }
};
HOOK_DEFINE_TRAMPOLINE(SystemMessageHolderLabelHook) {
    static void* Callback(const void* sys, const char* name, const char* label) {
        void* van = Orig(sys, name, label);
        if (van == nullptr || !coop::text::Enabled()) return van;
        void* l = coop::text::Get(coop::text::gSystem, coop::text::gSystemN,
                                  "LocalizedData/USen/MessageData/P2Luigi_SystemMessage", name);
        if (l == nullptr || (label && GameMessageHolderTryGetText(l, label) == nullptr)) return van;
        return l;
    }
};

// ------------------------------------------------------------------------ Luigi's voice
// Mario's voice lines are wave sounds in one wave-sound file of SeData.bfsar (FWSD, 19720
// bytes, 126 sounds WsPvDummy..WsPvWaitCold13). nn::atk resolves each clip synchronously while
// starting the sound: SeRequest::playStart -> SeadAudioPlayer::startAudio -> ... ->
// WaveSoundRuntime::PrepareImpl -> Util::GetWaveFileOfWaveSound(wsd, index, ...). When the request
// came from one of Luigi's SeKeepers, hand back the clip at that index from our own wave archive
// (romfs SoundData/P2LuigiPv.bfwar, built from the Super Mario 3D World Luigi set). Slots that
// archive leaves empty keep Mario's clip.
extern bool AlIsExistFile(const coop::luigi::SafeStr&) asm("_ZN2al11isExistFileERKN4sead14SafeStringBaseIcEE");
extern bool AlTryLoadFileToBuffer(const coop::luigi::SafeStr&, u8*, u32, s32) asm(
    "_ZN2al19tryLoadFileToBufferERKN4sead14SafeStringBaseIcEEPhji");
extern void AtkAttachMemoryPool(void*, void*, u64) asm(
    "_ZN2nn3atk11SoundSystem16AttachMemoryPoolEPNS_5audio14MemoryPoolTypeEPvm");
extern const sead::Vector3f* SeEmitterGetPosition(const void*) asm("_ZNK2al9SeEmitter11getPositionEv");
namespace coop::voice {
constexpr u32 kPvWsdSize = 19720;
constexpr u32 kPvCount = 126;
alignas(0x1000) static u8 gWar[10 << 20];  // room for long sleep and cheer lines
static bool gTried = false, gLoaded = false;
alignas(0x10) static u8 gPool[0x40];  // nn::audio::MemoryPoolType
static void* gKeepers[48];
static s32 gKeeperN = 0;
static void* gReqs[96];  // SeRequests currently filled from a Luigi keeper
static s32 gReqN = 0;
static bool gInLuigiRequest = false;
static bool gInLuigiPlay = false;

static void Load() {
    if (gTried) return;
    gTried = true;
    coop::luigi::SafeStr path = coop::luigi::MakeSafe("SoundData/P2LuigiPv.bfwar");
    if (!AlIsExistFile(path)) { COOP_LOG("voice: P2LuigiPv.bfwar missing"); return; }
    gLoaded = AlTryLoadFileToBuffer(path, gWar, sizeof(gWar), 0x40);
    u32 magic;
    std::memcpy(&magic, gWar, 4);
    if (gLoaded && magic != 0x52415746) gLoaded = false;  // "FWAR"
    // nn::audio only plays wave data that sits in an attached memory pool; the game's own sound
    // heap is one, our buffer is not. Attach it once (it lives for the whole session).
    if (gLoaded) AtkAttachMemoryPool(gPool, gWar, sizeof(gWar));
    COOP_LOG("voice: Luigi archive %s", gLoaded ? "loaded" : "failed");
}
static void Reset() { gKeeperN = 0; gReqN = 0; gInLuigiRequest = false; gInLuigiPlay = false; }
static void AddKeeper(void* k) {
    if (k == nullptr || gKeeperN >= 48) return;
    gKeepers[gKeeperN++] = k;
    COOP_LOG("voice: Luigi SeKeeper #%d %p", gKeeperN, k);
}
static bool IsLuigiKeeper(void* k) {
    for (s32 i = 0; i < gKeeperN; i++) if (gKeepers[i] == k) return true;
    return false;
}
static s32 FindReq(void* r) {
    for (s32 i = 0; i < gReqN; i++) if (gReqs[i] == r) return i;
    return -1;
}
static void MarkReq(void* r, bool luigi) {
    s32 i = FindReq(r);
    if (luigi) {
        if (i < 0 && gReqN < 96) gReqs[gReqN++] = r;
    } else if (i >= 0) {
        gReqs[i] = gReqs[--gReqN];
    }
}
static u32 Rd32(const u8* p) { u32 v; std::memcpy(&v, p, 4); return v; }
static s32 gLogBudget = 40;
static const void* LuigiWave(const void* wsd, u32 index) {
    if (!gLoaded || wsd == nullptr || index >= kPvCount) return nullptr;
    const u8* w = static_cast<const u8*>(wsd);
    if (Rd32(w) != 0x44535746 /* FWSD */ || Rd32(w + 0xc) != kPvWsdSize) {
        if (gLogBudget > 0 && Rd32(w + 0xc) > 19000 && Rd32(w + 0xc) < 21000) {
            gLogBudget--; COOP_LOG("voice: wsd size %u idx %u (not voice file?)", Rd32(w + 0xc), index);
        }
        return nullptr;
    }
    // FWAR: section refs at 0x14 (INFO) and 0x20 (FILE): {u16 id, u16 pad, u32 offset, u32 size}
    const u8* info = gWar + Rd32(gWar + 0x18) + 8;
    if (index >= Rd32(info)) return nullptr;
    const u8* e = info + 4 + 12 * index;
    s32 off; std::memcpy(&off, e + 4, 4);
    if (off < 0 || Rd32(e + 8) == 0) return nullptr;
    return gWar + Rd32(gWar + 0x24) + 8 + off;
}
// Fallback when the request was not tagged: the emitter's position is at Luigi (not at Mario).
static bool NearLuigi(void* emitter) {
    if (emitter == nullptr || coop::gP1 == nullptr || coop::gP2 == nullptr || al::isDead(coop::gP2))
        return false;
    const sead::Vector3f* p = SeEmitterGetPosition(emitter);
    if (p == nullptr) return false;
    const sead::Vector3f& l = al::getTrans(coop::gP2);
    const sead::Vector3f& m = al::getTrans(coop::gP1);
    auto d2 = [&](const sead::Vector3f& a) {
        f32 x = p->x - a.x, y = p->y - a.y, z = p->z - a.z; return x * x + y * y + z * z; };
    f32 dl = d2(l);
    if (dl == 0.0f && p->x == 0.0f && p->y == 0.0f && p->z == 0.0f) return false;
    return dl < d2(m) && dl < 400.0f * 400.0f;
}
}  // namespace coop::voice

HOOK_DEFINE_TRAMPOLINE(SeKeeperCreateHook) {
    static void* Callback(void* a, void* b, const char* c, void* d, void* e, void* f, void* g) {
        void* k = Orig(a, b, c, d, e, f, g);
        if (coop::gSpawningP2) coop::voice::AddKeeper(k);
        return k;
    }
};
HOOK_DEFINE_TRAMPOLINE(SeKeeperRequestIdLocalHook) {
    static u64 Callback(void* keeper, void* param, const char* name) {
        bool prev = coop::voice::gInLuigiRequest;
        coop::voice::gInLuigiRequest = coop::voice::IsLuigiKeeper(keeper);
        u64 r = Orig(keeper, param, name);
        coop::voice::gInLuigiRequest = prev;
        return r;
    }
};
HOOK_DEFINE_TRAMPOLINE(SeRequestSetNewHook) {
    static void Callback(void* req, const void* param, s32 a) {
        Orig(req, param, a);
        coop::voice::MarkReq(req, coop::voice::gInLuigiRequest);
    }
};
HOOK_DEFINE_TRAMPOLINE(SeRequestPlayStartHook) {
    static u64 Callback(void* req, void* player) {
        bool prev = coop::voice::gInLuigiPlay;
        bool luigi = coop::voice::FindReq(req) >= 0;
        if (!luigi) luigi = coop::voice::NearLuigi(*reinterpret_cast<void**>(static_cast<u8*>(req) + 0x48));
        coop::voice::gInLuigiPlay = luigi;
        u64 r = Orig(req, player);
        coop::voice::gInLuigiPlay = prev;
        return r;
    }
};
HOOK_DEFINE_TRAMPOLINE(GetWaveFileOfWaveSoundHook) {
    static const void* Callback(const void* wsd, u32 index, const void* archive, const void* loader) {
        if (coop::voice::gInLuigiPlay) {
            const void* w = coop::voice::LuigiWave(wsd, index);
            if (w != nullptr && coop::voice::gLogBudget > 0) {
                coop::voice::gLogBudget--;
                COOP_LOG("voice: Luigi wave idx %u -> %s", index, w ? "swapped" : "kept");
            }
            if (w) return w;
        } else if (coop::voice::gLogBudget > 20 && wsd != nullptr) {
            u32 sz; std::memcpy(&sz, static_cast<const u8*>(wsd) + 0xc, 4);
            if (sz == coop::voice::kPvWsdSize) { coop::voice::gLogBudget--; COOP_LOG("voice: Mario voice idx %u", index); }
        }
        return Orig(wsd, index, archive, loader);
    }
};
HOOK_DEFINE_TRAMPOLINE(MainControllerPortHook) {
    static s32 Callback() {
        if ((coop::gUiPortP2 || coop::gClosetP2 || coop::gTalkP2) && coop::gP2Port >= 0)
            return coop::gP2Port;
        return Orig();
    }
};

HOOK_DEFINE_TRAMPOLINE(ClosetStateKillHook) {
    static void Callback(void* state) {
        Orig(state);
        if (coop::gClosetP2) COOP_LOG("Wardrobe: P2 session ended");
        coop::gClosetP2 = false;
        coop::closet::SetLuigiPreview(false);
    }
};

static void CopyName(char* dst, const char* src) {
    size_t n = src ? std::strlen(src) : 0;
    if (n > 63) n = 63;
    if (n) std::memcpy(dst, src, n);
    dst[n] = 0;
}

HOOK_DEFINE_TRAMPOLINE(WearCostumeHook) {
    static bool Callback(void* writer, const char* name) {
        if (coop::gShopP2 && coop::gP2 != nullptr &&
            coop::closet::CalledFromShopState(__builtin_return_address(0))) {
            CopyName(coop::gP2Costume, name);
            coop::gClosetStartP2Pending = true;
            COOP_LOG("Shop: Luigi wears his new body %s (Mario keeps his)", coop::gP2Costume);
            coop::gOutfitDirty = true;
            return true;
        }
        if (coop::gClosetP2 && coop::closet::CalledFromClosetState(__builtin_return_address(0))) {
            CopyName(coop::gP2Costume, name);
            coop::gClosetStartP2Pending = true;
            COOP_LOG("Wardrobe: P2 body -> %s (save file untouched)", coop::gP2Costume);
            coop::gOutfitDirty = true;
            return true;
        }
        return Orig(writer, name);
    }
};

HOOK_DEFINE_TRAMPOLINE(WearCapHook) {
    static bool Callback(void* writer, const char* name) {
        if (coop::gShopP2 && coop::gP2 != nullptr &&
            coop::closet::CalledFromShopState(__builtin_return_address(0))) {
            CopyName(coop::gP2CapName, name);
            coop::gClosetStartP2Pending = true;
            COOP_LOG("Shop: Luigi wears his new cap %s (Mario keeps his)", coop::gP2CapName);
            coop::gOutfitDirty = true;
            return true;
        }
        if (coop::gClosetP2 && coop::closet::CalledFromClosetState(__builtin_return_address(0))) {
            CopyName(coop::gP2CapName, name);
            coop::gClosetStartP2Pending = true;
            COOP_LOG("Wardrobe: P2 cap -> %s (save file untouched)", coop::gP2CapName);
            coop::gOutfitDirty = true;
            return true;
        }
        return Orig(writer, name);
    }
};

// While P2 browses, the wardrobe marks P2's current outfit as "worn".
HOOK_DEFINE_TRAMPOLINE(GetCostumeNameHook) {
    static const char* Callback(void* accessor) {
        if (coop::gClosetP2 && coop::gP2Costume[0] &&
            coop::closet::CalledFromClosetState(__builtin_return_address(0)))
            return coop::gP2Costume;
        return Orig(accessor);
    }
};

HOOK_DEFINE_TRAMPOLINE(GetCapNameHook) {
    static const char* Callback(void* accessor) {
        if (coop::gClosetP2 && coop::gP2CapName[0] &&
            coop::closet::CalledFromClosetState(__builtin_return_address(0)))
            return coop::gP2CapName;
        return Orig(accessor);
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
        if (!coop::CoopModeOn(reinterpret_cast<const u8*>(self) + 0x20)) {
            COOP_LOG("2P mode is off; Mario plays solo.");
            return;
        }

        coop::gSpawningP2 = true;
        PlayerInitInfo p2Info;
        std::memcpy(&p2Info, &playerInfo, sizeof(p2Info));
        p2Info.controllerPort = p2Port;
        if (coop::gP2Costume[0]) p2Info.costumeName = coop::gP2Costume;
        if (coop::gP2CapName[0]) p2Info.capName = coop::gP2CapName;
        COOP_LOG("P2 outfit: body %s, cap %s", p2Info.costumeName ? p2Info.costumeName : "-",
                 p2Info.capName ? p2Info.capName : "-");
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

// Moon lead: while Luigi's Moon demo runs, players 0 and 1 swap for everyone except Mario's own
// pieces (Mario, his Cappy, what he is capturing still see Mario as player 0).
static al::LiveActor* LeadSwap(const al::LiveActor* caller, s32 index) {
    if (!coop::gLeadP2 || coop::gP1 == nullptr || coop::gP2 == nullptr) return nullptr;
    if (index != 0 && index != 1) return nullptr;
    if (caller != nullptr && index == 0) {
        al::LiveActor* p1Cap = coop::Field<al::LiveActor*>(coop::gP1, smo100::PlayerActorHakoniwa_hackCap);
        void* keeper = coop::Field<void*>(coop::gP1, smo100::PlayerActorHakoniwa_hackKeeper);
        al::LiveActor* p1Hack = keeper ? coop::Field<al::LiveActor*>(keeper, smo100::PlayerHackKeeper_hackActor) : nullptr;
        if (caller == coop::gP1 || caller == p1Cap || (p1Hack != nullptr && caller == p1Hack))
            return coop::gP1;
    }
    return index == 0 ? coop::gP2 : coop::gP1;
}

// ------------------------------------------------------------------------ bosses (3D World)
// Bosses aim at "player 0" through rs::getPlayerPos / getPlayerBodyPos / getPlayerVelocity ->
// al::getPlayerActor(boss, 0) (Ghidra: 0x56f608, 0x56f678, 0x56fa7c), so vanilla they only ever go
// after Mario. In 3D World a boss goes after the nearer brother: each boss actor keeps a target,
// switching when the other brother is clearly closer (and at least 3 s after the last switch), or
// at once when its target is down or in a bubble. Cutscenes keep the vanilla answer.
namespace coop::boss {
struct Slot { const al::LiveActor* boss; al::LiveActor* target; s32 since; };
static Slot gSlots[32];
static s32 gFrame = 0;
static void Reset() { for (auto& s : gSlots) s = {nullptr, nullptr, 0}; gFrame = 0; }
static bool IsBoss(const al::LiveActor* a) {
    u64 vt = *reinterpret_cast<const u64*>(a) - exl::util::modules::GetTargetStart();
    s32 lo = 0, hi = sizeof(kBossVtables) / sizeof(kBossVtables[0]) - 1;
    while (lo <= hi) { s32 m = (lo + hi) / 2; if (kBossVtables[m] == vt) return true;
        if (kBossVtables[m] < vt) lo = m + 1; else hi = m - 1; }
    return false;
}
static bool Usable(s32 i) {
    al::LiveActor* p = coop::PlayerAt(i);
    return p != nullptr && !al::isDead(p) && coop::gBubbleBusy[i] <= 60;
}
static float Dist2(const al::LiveActor* a, const al::LiveActor* b) {
    const sead::Vector3f& p = al::getTrans(a); const sead::Vector3f& q = al::getTrans(b);
    float x = p.x - q.x, y = p.y - q.y, z = p.z - q.z; return x * x + y * y + z * z;
}
static al::LiveActor* Target(const al::LiveActor* boss) {
    if (coop::gP1 == nullptr || coop::gP2 == nullptr || coop::gGameOver) return nullptr;
    if (rs::isActiveDemo(coop::gP1) || rs::isActiveDemo(coop::gP2)) return nullptr;
    if (!IsBoss(boss)) return nullptr;
    Slot* s = nullptr;
    for (auto& e : gSlots) if (e.boss == boss) { s = &e; break; }
    if (s == nullptr) {
        for (auto& e : gSlots) if (e.boss == nullptr) { s = &e; break; }
        if (s == nullptr) s = &gSlots[gFrame & 31];
        *s = {boss, nullptr, gFrame};
    }
    bool ok0 = Usable(0), ok1 = Usable(1);
    if (!ok0 && !ok1) return nullptr;
    float d0 = ok0 ? Dist2(boss, coop::gP1) : 3.4e38f, d1 = ok1 ? Dist2(boss, coop::gP2) : 3.4e38f;
    al::LiveActor* nearer = d0 <= d1 ? coop::gP1 : coop::gP2;
    s32 cur = s->target == coop::gP1 ? 0 : (s->target == coop::gP2 ? 1 : -1);
    if (cur < 0 || !(cur == 0 ? ok0 : ok1)) { s->target = nearer; s->since = gFrame; }
    else if (s->target != nearer && gFrame - s->since > 180) {
        float dc = cur == 0 ? d0 : d1, dn = cur == 0 ? d1 : d0;
        if (dn < dc * 0.42f) { s->target = nearer; s->since = gFrame; }  // other is ~35% closer
    }
    return s->target;
}
}  // namespace coop::boss

static al::LiveActor* RedirectOwnPlayer(const al::LiveActor* caller, s32 index) {
    if (auto* l = LeadSwap(caller, index)) return l;
    if (index != 0 || coop::gP2 == nullptr || caller == nullptr) return nullptr;
    if (auto* b = coop::boss::Target(caller)) return b == coop::gP1 ? nullptr : b;
    if (coop::gToucherIsP2) return coop::gP2;
    if (coop::gNpcTarget == coop::gP2) return coop::gP2;
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

// 3D World bubble. PlayerStateRecoveryDead::exeRecovery (0x479884, Ghidra) moves the player each
// frame by setting velocity = path(step / flightFrames) - trans, and when step >= flightFrames
// (+0x74) it snaps to the path end and goes to the Fall nerve (exeFall: endRecovery, then fall with
// gravity until on ground). For co-op bubbles:
//  - flightFrames is made huge, so the bubble never lands by itself;
//  - after the vanilla update we replace the velocity: the bubble drifts gently to a spot beside
//    the brother it is rescued by and keeps following him;
//  - it pops only when that brother touches it: on that frame the path is collapsed onto the
//    bubble's position (start +0x50 = pos, horizontal +0x68 = 0, arc +0x48/+0x4c = 0,
//    flightFrames = current step), so the vanilla end puts him right there and he drops down.
constexpr float kBubbleHover = 90.0f;      // height above the rescuer's feet
constexpr float kBubbleSide = 150.0f;      // distance beside him
constexpr float kBubbleTouch = 130.0f;     // rescuer within this distance pops it
constexpr float kBubbleEase = 0.05f;       // fraction of the remaining way per frame
constexpr float kBubbleMaxSpeed = 12.0f;   // units per frame near the rescuer
constexpr float kBubbleFarSpeed = 30.0f;   // when far away (catch-up)
constexpr s32 kBubbleNoPopFrames = 30;
static bool gBubblePop[2] = {false, false};
static sead::Vector3f gBubblePopPos[2] = {};

extern void GameSetVelocity(al::LiveActor*, const sead::Vector3f&) asm(
    "_ZN2al11setVelocityEPNS_9LiveActorERKN4sead7Vector3IfEE");

static sead::Vector3f BubbleHoverPoint(s32 slot, al::LiveActor* rescuer) {
    const sead::Vector3f& t = al::getTrans(rescuer);
    sead::Vector3f side = {gAimOffset[slot].x, 0.0f, gAimOffset[slot].z};
    float len = __builtin_sqrtf(side.x * side.x + side.z * side.z);
    if (len < 1.0f) {
        al::calcSideDir(&side, rescuer);
        side.y = 0.0f;
        len = __builtin_sqrtf(side.x * side.x + side.z * side.z);
        if (len < 0.001f) { side = {1.0f, 0.0f, 0.0f}; len = 1.0f; }
    }
    return {t.x + side.x / len * kBubbleSide, t.y + kBubbleHover, t.z + side.z / len * kBubbleSide};
}

HOOK_DEFINE_TRAMPOLINE(RecoveryFlyHook) {
    static void Callback(void* state) {
        auto* nerveUser = reinterpret_cast<al::IUseNerve*>(state);
        bool first = al::isFirstStep(nerveUser);
        al::LiveActor* rescuer = BubbleTargetOfState(state);
        s32 slot = BubbleSlot(state);
        auto* actor = coop::Field<al::LiveActor*>(state, smo100::ActorStateBase_actor);

        if (!first && rescuer != nullptr && gBubblePop[slot]) {
            // Collapse the path onto the bubble's position: the vanilla end drops him here.
            gBubblePop[slot] = false;
            coop::SetField<sead::Vector3f>(state, smo100::RecoveryDead_start, gBubblePopPos[slot]);
            coop::SetField<sead::Vector3f>(state, smo100::RecoveryDead_horiz, {0.0f, 0.0f, 0.0f});
            coop::SetField<float>(state, smo100::RecoveryDead_arcA, 0.0f);
            coop::SetField<float>(state, smo100::RecoveryDead_arcB, 0.0f);
            coop::SetField<s32>(state, smo100::RecoveryDead_flightFrames,
                                al::getNerveStep(nerveUser));
            auto* sp = coop::Field<PlayerRecoverySafetyPoint*>(state, smo100::RecoveryDead_safetyPoint);
            sead::Vector3f up1 = {0.0f, 1.0f, 0.0f};
            sp->setSafetyPoint(gBubblePopPos[slot], up1, nullptr);
            COOP_LOG("Bubble: P%d popped by P%d", slot + 1, 2 - slot);
            Orig(state);
            return;
        }

        Orig(state);
        if (rescuer == nullptr || actor == nullptr) return;

        if (first) {
            sead::Vector3f spot = coop::FindSpotBeside(rescuer);
            const sead::Vector3f& t = al::getTrans(rescuer);
            gAimOffset[slot] = {spot.x - t.x, spot.y - t.y, spot.z - t.z};
            gBubblePop[slot] = false;
            coop::SetField<s32>(state, smo100::RecoveryDead_flightFrames, 0x3fffffff);
            s32 spin = coop::Field<s32>(state, smo100::RecoveryDead_spinFrames) / 2;
            coop::SetField<s32>(state, smo100::RecoveryDead_spinFrames, spin < 1 ? 1 : spin);
            return;
        }

        // Still floating: stay "in a bubble" for the rest of the mod.
        if (coop::gBubbleBusy[slot] < 60 * 10) coop::gBubbleBusy[slot] = 60 * 10;
        if (actor == coop::gP1 && coop::gForcedRecovery[0] < 60 * 10) coop::gForcedRecovery[0] = 60 * 10;
        if (actor == coop::gP2 && coop::gForcedRecovery[1] < 60 * 10) coop::gForcedRecovery[1] = 60 * 10;

        const sead::Vector3f& pos = al::getTrans(actor);
        sead::Vector3f goal = BubbleHoverPoint(slot, rescuer);
        sead::Vector3f d = {goal.x - pos.x, goal.y - pos.y, goal.z - pos.z};
        float dist = __builtin_sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
        float speed = dist * kBubbleEase;
        float cap = dist > 2500.0f ? kBubbleFarSpeed : kBubbleMaxSpeed;
        if (speed > cap) speed = cap;
        sead::Vector3f v = {0.0f, 0.0f, 0.0f};
        if (dist > 0.5f) v = {d.x / dist * speed, d.y / dist * speed, d.z / dist * speed};
        GameSetVelocity(actor, v);

        // Touch to pop (3D World): the rescuer's body reaches the bubble.
        if (al::getNerveStep(nerveUser) < kBubbleNoPopFrames || al::isDead(rescuer)) return;
        s32 rescuerSlot = rescuer == coop::gP1 ? 0 : 1;
        if (coop::gBubbleBusy[rescuerSlot] > 60) return;  // rescuer is in a bubble himself
        const sead::Vector3f& r = al::getTrans(rescuer);
        float dx = r.x - pos.x, dy = (r.y + 80.0f) - (pos.y + 80.0f), dz = r.z - pos.z;
        if (dx * dx + dy * dy + dz * dz < kBubbleTouch * kBubbleTouch) {
            sead::Vector3f pp = pos;
            if (pp.y < r.y + 40.0f) pp.y = r.y + 40.0f;  // never pop below the rescuer's feet
            gBubblePopPos[slot] = pp;
            gBubblePop[slot] = true;
        }
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

// ------------------------------------------------------------------------ brother collision
// 3D World style: the brothers are solid to each other (you stop against your brother, the one
// standing still isn't shoved), and landing on his head bounces you up. Applied once per frame
// after the scene update. Skipped while either is in a bubble, capturing, bound to a pipe/door/
// booth, dead, or a demo is running.
extern void GameSetTrans(al::LiveActor*, const sead::Vector3f&) asm(
    "_ZN2al8setTransEPNS_9LiveActorERKN4sead7Vector3IfEE");
extern sead::Vector3f* GameGetVelocityPtr(al::LiveActor*) asm(
    "_ZN2al14getVelocityPtrEPNS_9LiveActorE");
namespace coop::collide {
constexpr float kRadius = 90.0f;       // centre-to-centre horizontal distance kept
constexpr float kHeight = 150.0f;      // vertical gap at/above which they don't block
constexpr float kHeadMin = 110.0f;     // feet this far above the other's feet = on his head
constexpr float kHeadMax = 190.0f;
constexpr float kHeadRadius = 70.0f;
constexpr float kBounce = 26.0f;       // upward speed of a head bounce
static s32 gBounceCooldown[2] = {0, 0};

static bool Free(s32 i) {
    al::LiveActor* p = PlayerAt(i);
    if (p == nullptr || al::isDead(p) || gBubbleBusy[i] > 0) return false;
    void* keeper = Field<void*>(p, smo100::PlayerActorHakoniwa_hackKeeper);
    if (keeper && Field<al::LiveActor*>(keeper, smo100::PlayerHackKeeper_hackActor)) return false;
    void* pup = Field<void*>(p, smo100::PlayerActorHakoniwa_puppet);
    if (pup && Field<void*>(pup, 0x70) && Field<void*>(pup, 0x78)) return false;  // bound
    return true;
}

constexpr float kCapHop = 22.0f;       // upward speed when the brother's Cappy hits you
constexpr float kCapTouch = 95.0f;     // Cappy centre to body centre
constexpr float kCapAway = 140.0f;     // Cappy this far from its owner's head = thrown
static bool gCapHit[2] = {false, false};  // this throw already hopped the other brother

// A brother's thrown Cappy that touches the other brother makes him hop (once per throw).
static void CapHop() {
    for (s32 thrower = 0; thrower < 2; thrower++) {
        al::LiveActor* owner = PlayerAt(thrower);
        al::LiveActor* target = PlayerAt(1 - thrower);
        al::LiveActor* cap = Field<al::LiveActor*>(owner, smo100::PlayerActorHakoniwa_hackCap);
        if (cap == nullptr || al::isDead(cap)) { gCapHit[thrower] = false; continue; }
        const sead::Vector3f& c = al::getTrans(cap);
        const sead::Vector3f& o = al::getTrans(owner);
        float ox = c.x - o.x, oy = c.y - (o.y + 150.0f), oz = c.z - o.z;
        if (ox * ox + oy * oy + oz * oz < kCapAway * kCapAway) { gCapHit[thrower] = false; continue; }
        if (gCapHit[thrower] || !Free(1 - thrower)) continue;
        const sead::Vector3f& t = al::getTrans(target);
        float tx = c.x - t.x, ty = c.y - (t.y + 80.0f), tz = c.z - t.z;
        if (tx * tx + ty * ty + tz * tz > kCapTouch * kCapTouch) continue;
        sead::Vector3f* v = GameGetVelocityPtr(target);
        if (v == nullptr) continue;
        if (v->y < kCapHop) v->y = kCapHop;
        gCapHit[thrower] = true;
        gBounceCooldown[1 - thrower] = 20;
    }
}

static void Update() {
    coop::boss::gFrame++;
    for (s32 i = 0; i < 2; i++) if (gBounceCooldown[i] > 0) gBounceCooldown[i]--;
    if (gP1 == nullptr || gP2 == nullptr || gGameOver || gLeadP2 || gTalkP2) return;
    if (!gP2Placed) return;  // no pushing apart while Luigi is still being placed at stage start
    if (rs::isActiveDemo(gP1) || rs::isActiveDemo(gP2)) return;
    CapHop();
    if (!Free(0) || !Free(1)) return;
    sead::Vector3f a = al::getTrans(gP1), b = al::getTrans(gP2);
    float dx = b.x - a.x, dz = b.z - a.z, dy = b.y - a.y;
    float h2 = dx * dx + dz * dz;

    // Head bounce: the higher brother, falling, lands on the other's head.
    for (s32 top = 0; top < 2; top++) {
        al::LiveActor* hi = PlayerAt(top);
        float rise = top == 0 ? -dy : dy;  // hi.y - lo.y
        if (gBounceCooldown[top] > 0 || rise < kHeadMin || rise > kHeadMax) continue;
        if (h2 > kHeadRadius * kHeadRadius) continue;
        sead::Vector3f* v = GameGetVelocityPtr(hi);
        if (v == nullptr || v->y > 0.0f) continue;
        v->y = kBounce;
        gBounceCooldown[top] = 20;
        return;
    }

    if (dy > kHeight || dy < -kHeight || h2 >= kRadius * kRadius) return;
    float d = __builtin_sqrtf(h2);
    float nx, nz;
    if (d < 0.01f) { nx = 1.0f; nz = 0.0f; d = 0.0f; }
    else { nx = dx / d; nz = dz / d; }
    float push = kRadius - d;
    // Whoever is moving toward the other is the one stopped.
    const sead::Vector3f& va = al::getVelocity(gP1);
    const sead::Vector3f& vb = al::getVelocity(gP2);
    float inA = va.x * nx + va.z * nz;        // P1 moving toward P2
    float inB = -(vb.x * nx + vb.z * nz);     // P2 moving toward P1
    if (inA < 0.0f) inA = 0.0f;
    if (inB < 0.0f) inB = 0.0f;
    float wa = 0.5f, wb = 0.5f;
    if (inA + inB > 0.5f) { wa = inA / (inA + inB); wb = 1.0f - wa; }
    sead::Vector3f na = {a.x - nx * push * wa, a.y, a.z - nz * push * wa};
    sead::Vector3f nb = {b.x + nx * push * wb, b.y, b.z + nz * push * wb};
    if (wa > 0.0f) GameSetTrans(gP1, na);
    if (wb > 0.0f) GameSetTrans(gP2, nb);
}
}  // namespace coop::collide

// ------------------------------------------------------------------------ leaving 2D together
// When one brother leaves an 8-bit wall section (rs::isPlayer2D goes false) while the other is
// still in it, bring the other one out too: after the leader's pop-out (~0.5 s) he is placed
// beside him in 3D; outside the 2D area his dimension keeper switches him back to 3D. Pipe exits
// are left to the pipe follow (echo).
extern bool GameIsPlayer2D(const al::LiveActor*) asm("_ZN2rs10isPlayer2DEPKN2al9LiveActorE");
namespace coop::dim {
static bool gWas2D[2] = {false, false};
static s32 gTimer = 0;
static s32 gLeader = -1;
static void Update() {
    bool now[2];
    for (s32 i = 0; i < 2; i++) {
        al::LiveActor* p = PlayerAt(i);
        now[i] = p != nullptr && !al::isDead(p) && GameIsPlayer2D(p);
    }
    for (s32 i = 0; i < 2; i++) {
        if (gWas2D[i] && !now[i] && now[1 - i] && !coop::echo::Active() &&
            gBubbleBusy[1 - i] == 0 && gTimer == 0) {
            gLeader = i;
            gTimer = 30;
            COOP_LOG("2D: P%d left the wall section, bringing P%d out", i + 1, 2 - i);
        }
    }
    if (gTimer > 0 && --gTimer == 0 && gLeader >= 0) {
        s32 f = 1 - gLeader;
        if (now[f] && !now[gLeader]) PlaceBeside(PlayerAt(f), PlayerAt(gLeader), "left 2D together");
        gLeader = -1;
    }
    gWas2D[0] = now[0];
    gWas2D[1] = now[1];
}
static void Reset() { gWas2D[0] = gWas2D[1] = false; gTimer = 0; gLeader = -1; }
}  // namespace coop::dim

HOOK_DEFINE_TRAMPOLINE(StageSceneControl) {
    static void Callback(StageScene* scene) {
        Orig(scene);
        coop::hud::Update();
        coop::cam::Update();
        coop::CheckModeSwitch(scene);
        if (coop::gP2 == nullptr || coop::gP1 == nullptr) return;

        coop::collide::Update();
        coop::dim::Update();
        if (coop::gLeadP2) {
            bool demo = rs::isActiveDemo(coop::gP1) || rs::isActiveDemo(coop::gP2);
            ++coop::gLeadFrames;
            if (demo) coop::gLeadNoDemo = 0;
            else if (coop::gLeadFrames > 120 && ++coop::gLeadNoDemo > 60) {
                coop::gLeadP2 = false;
                COOP_LOG("Moon: no demo running, Mario is player 0 again");
            }
        }
        if (coop::gTalkP2) {
            if (rs::isActiveDemo(coop::gP1)) {
                coop::gTalkNoDemoFrames = 0;
            } else if (++coop::gTalkNoDemoFrames > 10) {
                coop::gTalkP2 = false;
                COOP_LOG("Talk: P2 conversation ended");
            }
        }

        if (coop::gP2RespawnPending) {
            coop::gP2RespawnPending = false;
            coop::gP2Hp = coop::kP2MaxHp;
            coop::PlaceP2BesideP1("out of hearts");
        }

        if (coop::gClosetStartP2 && !coop::gClosetSwapDone) {
            // Luigi takes Mario's spot in front of the booth; Mario waits beside him.
            coop::gClosetSwapDone = true;
            sead::Vector3f spot = al::getTrans(coop::gP1);
            al::resetPosition(coop::gP2, spot);
            al::setVelocityZero(coop::gP2);
            coop::PlaceBeside(coop::gP1, coop::gP2, "Luigi's outfit change");
            coop::gP2Placed = true;
        }
        if (coop::gP2Placed) return;
        if (coop::gFramesSinceInit == 0) {
            // Both spawn on the same start point. Put Luigi beside Mario from the very first frame
            // (3D World style) so they never appear inside each other and slide apart.
            sead::Vector3f pre = coop::FindSpotBesideP1();  // ground-checked spot next to him
            al::resetPosition(coop::gP2, pre);
            al::setVelocityZero(coop::gP2);
            coop::gIgnoreJumps = 3;
        }
        if (++coop::gFramesSinceInit > 60 * 20) { coop::gP2Placed = true; return; }
        if (!rs::isPlayerOnGround(coop::gP1)) return;
        {
            // Already standing next to Mario: keep him where he is (no visible snap).
            const sead::Vector3f& a = al::getTrans(coop::gP1);
            const sead::Vector3f& b = al::getTrans(coop::gP2);
            float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
            float h2 = dx * dx + dz * dz;
            if (rs::isPlayerOnGround(coop::gP2) && h2 > 60.0f * 60.0f && h2 < 300.0f * 300.0f &&
                dy * dy < 80.0f * 80.0f) {
                coop::gP2Placed = true;
                COOP_LOG("P2 already beside P1 at stage start");
                return;
            }
        }
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
              CheckBytes(CounterLifeCtrl_ctor, CounterLifeCtrl_ctor_bytes) &&
              CheckBytes(al_findOrCreateActorResource, al_findOrCreateActorResource_bytes) &&
              CheckBytes(al_findOrCreateActorResourceWithAnimResource,
                         al_findOrCreateActorResourceWithAnimResource_bytes) &&
              CheckBytes(ResourceSystem_findOrCreateResource, ResourceSystem_findOrCreateResource_bytes) &&
              CheckBytes(al_findResource, al_findResource_bytes) &&
              CheckBytes(Closet_receiveMsg, Closet_receiveMsg_bytes) &&
              CheckBytes(al_getMainControllerPort, al_getMainControllerPort_bytes) &&
              CheckBytes(StageSceneStateCloset_kill, StageSceneStateCloset_kill_bytes) &&
              CheckBytes(GameDataFunction_wearCostume, GameDataFunction_wearCostume_bytes) &&
              CheckBytes(GameDataFunction_wearCap, GameDataFunction_wearCap_bytes) &&
              CheckBytes(GameDataFunction_getCurrentCostumeTypeName,
                         GameDataFunction_getCurrentCostumeTypeName_bytes) &&
              CheckBytes(GameDataFunction_getCurrentCapTypeName,
                         GameDataFunction_getCurrentCapTypeName_bytes) &&
              CheckBytes(ResourceSystem_findResourceCore, ResourceSystem_findResourceCore_bytes) &&
              CheckBytes(ResourceSystem_findResourceCategory,
                         ResourceSystem_findResourceCategory_bytes) &&
              CheckBytes(ResourceSystem_createResource, ResourceSystem_createResource_bytes) &&
              CheckBytes(rs_tryChangeNextStage, rs_tryChangeNextStage_bytes) &&
              CheckBytes(StageSceneStateCloset_initModel, StageSceneStateCloset_initModel_bytes) &&
              CheckBytes(Closet_attackSensor, Closet_attackSensor_bytes) &&
              CheckBytes(EventFlowNodeMessageBalloon_exeWait,
                         EventFlowNodeMessageBalloon_exeWait_bytes) &&
              CheckBytes(rs_tryLockStartEventTalkDemo, rs_tryLockStartEventTalkDemo_bytes) &&
              CheckBytes(PlayerHackKeeper_startHackStartDemo, PlayerHackKeeper_startHackStartDemo_bytes) &&
              CheckBytes(PlayerHackKeeper_startHackStartDemoPuppetable,
                         PlayerHackKeeper_startHackStartDemoPuppetable_bytes) &&
              CheckBytes(PlayerActorHakoniwa_isEnableDemo, PlayerActorHakoniwa_isEnableDemo_bytes) &&
              CheckBytes(PlayerActorHakoniwa_startDemoPuppetable,
                         PlayerActorHakoniwa_startDemoPuppetable_bytes) &&
              CheckBytes(PlayerActorHakoniwa_endDemoPuppetable,
                         PlayerActorHakoniwa_endDemoPuppetable_bytes) &&
              CheckBytes(TalkNpc_movement, TalkNpc_movement_bytes) &&
              CheckBytes(PartsModel_initPartsFixFileNoRegister,
                         PartsModel_initPartsFixFileNoRegister_bytes) &&
              CheckBytes(rs_isSeparatePlay, rs_isSeparatePlay_bytes) &&
              CheckBytes(NpcEventDirector_updateBalloon, NpcEventDirector_updateBalloon_bytes) &&
              CheckBytes(rs_checkTriggerDecideWithRequestIcon,
                         rs_checkTriggerDecideWithRequestIcon_bytes) &&
              CheckBytes(NpcJointLookAtController_update, NpcJointLookAtController_update_bytes) &&
              CheckBytes(EventFlowExecutor_execute, EventFlowExecutor_execute_bytes) &&
              CheckBytes(rs_getNpcLookPos, rs_getNpcLookPos_bytes) &&
              CheckBytes(Closet_control, Closet_control_bytes) &&
              CheckBytes(rs_isTriggerUiDecide, rs_isTriggerUiDecide_bytes) &&
              CheckBytes(rs_isTriggerUiDecideGdh, rs_isTriggerUiDecideGdh_bytes) &&
              CheckBytes(rs_isTriggerUiCancel, rs_isTriggerUiCancel_bytes) &&
              CheckBytes(NpcEventCtrlInfo_requestShowBalloonMessage,
                         NpcEventCtrlInfo_requestShowBalloonMessage_bytes) &&
              CheckBytes(Shine_receiveMsg, Shine_receiveMsg_bytes) &&
              CheckBytes(StageSceneStateGetShine_kill, StageSceneStateGetShine_kill_bytes) &&
              CheckBytes(PlayerHolder_tryGetPlayer, PlayerHolder_tryGetPlayer_bytes) &&
              CheckBytes(PlayerHolder_getPlayer, PlayerHolder_getPlayer_bytes) &&
              CheckBytes(ShoppingWatcherStateWear_appear, ShoppingWatcherStateWear_appear_bytes) &&
              CheckBytes(StageScene_dtor, StageScene_dtor_bytes) &&
              CheckBytes(MessageSystem_getStageMessageHolder, MessageSystem_getStageMessageHolder_bytes) &&
              CheckBytes(MessageSystem_getSystemMessageHolder, MessageSystem_getSystemMessageHolder_bytes) &&
              CheckBytes(MessageSystem_getSystemMessageHolder2, MessageSystem_getSystemMessageHolder2_bytes) &&
              CheckBytes(al_SeKeeper_create, al_SeKeeper_create_bytes) &&
              CheckBytes(al_SeKeeper_requestPlaySeIdLocal, al_SeKeeper_requestPlaySeIdLocal_bytes) &&
              CheckBytes(al_SeRequest_setNew, al_SeRequest_setNew_bytes) &&
              CheckBytes(al_SeRequest_playStart, al_SeRequest_playStart_bytes) &&
              CheckBytes(atk_Util_GetWaveFileOfWaveSound, atk_Util_GetWaveFileOfWaveSound_bytes) &&
              CheckBytes(ProjectDemoDirector_requestStartDemo, ProjectDemoDirector_requestStartDemo_bytes) &&
              CheckBytes(ProjectDemoDirector_requestEndDemo, ProjectDemoDirector_requestEndDemo_bytes);
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
    FindOrCreateActorResourceHook::InstallAtOffset(al_findOrCreateActorResource);
    FindOrCreateActorResourceAnimHook::InstallAtOffset(al_findOrCreateActorResourceWithAnimResource);
    ResourceFindOrCreateHook::InstallAtOffset(ResourceSystem_findOrCreateResource);
    FindResourceHook::InstallAtOffset(al_findResource);
    ClosetReceiveMsgHook::InstallAtOffset(Closet_receiveMsg);
    MainControllerPortHook::InstallAtOffset(al_getMainControllerPort);
    ClosetStateKillHook::InstallAtOffset(StageSceneStateCloset_kill);
    WearCostumeHook::InstallAtOffset(GameDataFunction_wearCostume);
    WearCapHook::InstallAtOffset(GameDataFunction_wearCap);
    GetCostumeNameHook::InstallAtOffset(GameDataFunction_getCurrentCostumeTypeName);
    GetCapNameHook::InstallAtOffset(GameDataFunction_getCurrentCapTypeName);
    FindResourceCoreHook::InstallAtOffset(ResourceSystem_findResourceCore);
    FindResourceCategoryHook::InstallAtOffset(ResourceSystem_findResourceCategory);
    CreateResourceHook::InstallAtOffset(ResourceSystem_createResource);
    TryChangeNextStageHook::InstallAtOffset(rs_tryChangeNextStage);
    ClosetInitModelHook::InstallAtOffset(StageSceneStateCloset_initModel);
    ClosetAttackSensorHook::InstallAtOffset(Closet_attackSensor);
    TalkBalloonWaitHook::InstallAtOffset(EventFlowNodeMessageBalloon_exeWait);
    TryLockTalkDemoHook::InstallAtOffset(rs_tryLockStartEventTalkDemo);
    HackStartDemoHook::InstallAtOffset(PlayerHackKeeper_startHackStartDemo);
    HackStartDemoPuppetableHook::InstallAtOffset(PlayerHackKeeper_startHackStartDemoPuppetable);
    PlayerIsEnableDemoHook::InstallAtOffset(PlayerActorHakoniwa_isEnableDemo);
    PlayerStartDemoPuppetableHook::InstallAtOffset(PlayerActorHakoniwa_startDemoPuppetable);
    PlayerEndDemoPuppetableHook::InstallAtOffset(PlayerActorHakoniwa_endDemoPuppetable);
    TalkNpcMovementHook::InstallAtOffset(TalkNpc_movement);
    PartsFixInitHook::InstallAtOffset(PartsModel_initPartsFixFileNoRegister);
    SeparatePlayHook::InstallAtOffset(rs_isSeparatePlay);
    UpdateBalloonHook::InstallAtOffset(NpcEventDirector_updateBalloon);
    DecideWithIconHook::InstallAtOffset(rs_checkTriggerDecideWithRequestIcon);
    NpcLookAtUpdateHook::InstallAtOffset(NpcJointLookAtController_update);
    EventFlowExecuteHook::InstallAtOffset(EventFlowExecutor_execute);
    NpcLookPosHook::InstallAtOffset(rs_getNpcLookPos);
    ClosetControlHook::InstallAtOffset(Closet_control);
    UiDecideHook::InstallAtOffset(rs_isTriggerUiDecide);
    UiDecideGdhHook::InstallAtOffset(rs_isTriggerUiDecideGdh);
    UiCancelHook::InstallAtOffset(rs_isTriggerUiCancel);
    BalloonRequestHook::InstallAtOffset(NpcEventCtrlInfo_requestShowBalloonMessage);
    ShineReceiveMsgHook::InstallAtOffset(Shine_receiveMsg);
    GetShineKillHook::InstallAtOffset(StageSceneStateGetShine_kill);
    HolderTryGetPlayerHook::InstallAtOffset(PlayerHolder_tryGetPlayer);
    HolderGetPlayerHook::InstallAtOffset(PlayerHolder_getPlayer);
    ShopWearAppearHook::InstallAtOffset(ShoppingWatcherStateWear_appear);
    StageSceneDtorHook::InstallAtOffset(StageScene_dtor);
    StageMessageHolderHook::InstallAtOffset(MessageSystem_getStageMessageHolder);
    SystemMessageHolderHook::InstallAtOffset(MessageSystem_getSystemMessageHolder);
    SystemMessageHolderLabelHook::InstallAtOffset(MessageSystem_getSystemMessageHolder2);
    SeKeeperCreateHook::InstallAtOffset(al_SeKeeper_create);
    SeKeeperRequestIdLocalHook::InstallAtOffset(al_SeKeeper_requestPlaySeIdLocal);
    SeRequestSetNewHook::InstallAtOffset(al_SeRequest_setNew);
    SeRequestPlayStartHook::InstallAtOffset(al_SeRequest_playStart);
    GetWaveFileOfWaveSoundHook::InstallAtOffset(atk_Util_GetWaveFileOfWaveSound);
    DemoStartHook::InstallAtOffset(ProjectDemoDirector_requestStartDemo);
    DemoEndHook::InstallAtOffset(ProjectDemoDirector_requestEndDemo);
    COOP_LOG("Hooks installed (SMO 1.0.0, step 4 camera + bubble).");
}

extern "C" NORETURN void exl_exception_entry() {
    EXL_ABORT("Default exception handler called!");
}
