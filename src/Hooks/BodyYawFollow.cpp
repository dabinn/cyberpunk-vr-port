#include "Utils/DebugGate.hpp"
// BodyYawFollow -- PHYSICAL BODY ROTATION: the character turns under the headset, the view does not
// turn with it. Gated by "Physical body rotation" in the overlay (vrport.ini
// xr_physical_body_rotation), off by default.
//
// THE GAME TURNS THE CHARACTER; THIS ONLY ASKS IT TO. The angle goes into the engine's own per-frame
// heading delta on foot -- the channel the snap turn already uses (src/Hooks/OnFootDeltaHead.cpp) --
// so the entity yaw moves, and with it the drawn mesh, the collision, the aim and the movement
// direction, because the engine moved them itself. That is not "through input": nothing synthesises a
// mouse or a stick; it is the same heading accumulator the snap turn writes.
//
// TWO ROUTES WERE TRIED FIRST AND BOTH FAILED. They are recorded because each looked obviously right:
//
//   1. state+0x1D0 at the store site sub_140336390. The only write to that field in the whole
//      function, so it looked like the source -- and it is a copy nothing propagates: with -45 deg
//      sitting in it, the game's own body heading stayed at the engine yaw to five digits.
//   2. The components' world rotations, pre-multiplied by Rz(offset) from PatchCamera's stub on
//      UpdateWorldTransforms. This one DID reach the transforms -- measured with a live scan: all 102
//      of the player's transform records carried our angle, and the body visibly turned.
//
//      IT WAS REJECTED FOR THE WRONG REASON, and the correction belongs here because the wrong reason
//      was recorded as measured. The hands rode along, and that was blamed on the transform route --
//      but the cause was a mismatch in LocateCamera that this route did not create and does not
//      depend on: the published view orientation is composed from `bodyGameForward`, which is the
//      camera component's pre-write quaternion, and the camera INHERITS its yaw from the parent
//      component this route rotates. So the published view carried E+offset while the camera we
//      composed (from the census value) carried E, and the hand offset from the head came out rotated
//      by the whole offset. The heading route had the same defect with the signs swapped -- published
//      E, drawn E-realign -- and fixing it once at the source fixed the hands in both.
//
//      What DOES rule this route out is the gameplay half, which no frame fix reaches: the engine
//      derives aim, movement direction, cover and the collision capsule from its own heading, not from
//      these transforms, so the body turns in the picture while the character still shoots and walks
//      the old way. Second, we do not own the set: 102 records plus the components, of which 2 pass
//      our hook per frame (measured 144/s at 72 fps), and whether the one the skinning uses is among
//      them -- and whether the engine recomputes it after our write -- is a race with its own pass
//      order, not an invariant. It remains the right route for a PURELY VISUAL body turn.
//
// WHERE THE CANCELLATION LIVES, AND WHY NOT IN THE RECENTER BASE. The heading also feeds the camera,
// so injecting into it would swing the view. The old on-foot code cancelled that with
// RotateBaseYaw(step): the frame loop reports the head relative to that base both ways --
//     relPos = RotateVector(conj(base.ori), headPos - base.pos)
//     relOri = conj(base.ori) * headOri
// -- so the head's orientation and its room position each lose what the heading gained, and the view,
// the play space and the head-local hand poses all stay put. Correct in the algebra, wrong in the
// ORDER: the heading changes inside the game tick while the base only takes effect on the next XR
// cycle, so for one frame the view swings by the whole step. That is the camera drift this feature
// was always reported to have.
//
// So the base is left alone -- recentring keeps working exactly as before -- and the cancellation is
// done on our side, in the same frame, where the view is composed:
//
//     body   yaw = E            (engine's own, ours included: E = E0 + realign)
//     view   yaw = E - realign  (PatchCamera, LocateCamera's head-offset recipe)
//     solve  yaw = E            (world->model in the pose path)
//
// The view is then exactly what it would have been had the body never turned, the play space is
// anchored to the heading that existed at recenter, and the hands need no compensation of their own:
// their poses are head-local against an untouched base, and the solve converting with the body's TRUE
// yaw puts the model-space target back at the controller. That last point is also self-correcting --
// the solve reads the yaw the engine actually ended up at (from the census), so a heading the engine
// clamps or eases still leaves the hands on the controllers.

#include "Core/VrCoreShared.hpp"
#include "Core/LiveControls.hpp"
#include "Hooks/Hook.hpp"
#include "Runtimes/OpenXRManager.hpp"
#include "Anim/CharacterRig.hpp"   // g_VREntityPos*: the player's world position for the publish
#include "Anim/VrikState.hpp"
#include "Camera/CameraLink.hpp"
#include "Camera/NativeCameraPair.hpp"
#include "Hooks/RoomscaleMove.hpp"
#include "Runtimes/BodyYawFollower.hpp"
#include "Camera/CameraState.hpp"
#include "Runtimes/LookDownCone.hpp"
#include "Runtimes/HybridBodyYaw.hpp"
#include <mutex>

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <chrono>

extern void Log(const char* fmt, ...);

// ---- controls -----------------------------------------------------------------------------------
//
// The switch itself lives in LiveControls (xr_physical_body_rotation, persisted); this mirror exists
// so the pose path and the camera write have one plain symbol to test on their hot paths.
extern "C" __declspec(dllexport) int   CyberpunkVR_BodyYawFollow        = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugVrikNativePairPublished = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugVrikNativePairRejected = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugVrikNativePairPhaseMiss = 0;
// FIVE DEGREES OF FREE LOOK, per the roomscale movement requirement.
//
// Glances inside the cone leave the body still. Crossing it starts a short
// body-only catch-up toward the centre, restoring the full free-look zone.
// Camera tracking and its same-frame yaw cancellation remain immediate.
extern "C" __declspec(dllexport) float CyberpunkVR_BodyYawFollowDeadDeg = 5.0f;

namespace {
cvr::roomscale::BodyYawFollower s_yawFollower;
cvr::body::TrackedBodyYaw s_trackedBody;
cvr::body::RotationMode s_bodyRotationMode=cvr::body::RotationMode::Off;
cvr::body::HybridBodyYaw s_hybridBody;
std::mutex s_yawFollowerMutex;
std::atomic<uint32_t> s_nativePairSeq{0};
std::mutex s_nativePairWriter;
struct AtomicNativePair {
    std::atomic<float> camQuat[4]{},entityQuat[4]{},cameraMinusEntity[3]{};
    std::atomic<uint32_t> valid{0},consumerEpoch{0}; // epoch is diagnostic only
    std::atomic<float> bodyCameraMinusEntity[3]{};
    std::atomic<uint32_t> unavailable{0};
    std::atomic<uintptr_t> owner{0};
    std::atomic<uint64_t> origin{0},stampMs{0};
};
AtomicNativePair s_nativePair{};
struct CurrentBodyFrame {
    std::mutex mutex;
    float position[3]{},rotation[4]{0,0,0,1},trackingYaw{};
    uintptr_t owner{};
    uint64_t stamp{};
} s_currentBodyFrame;
}

bool VRIK_ReadCurrentBodyFrame(float* p,float* q,float* trackingYaw) {
    std::lock_guard lock(s_currentBodyFrame.mutex);
    const auto now=GetTickCount64();
    if(!s_currentBodyFrame.owner || s_currentBodyFrame.owner!=cvr::roomscale::PlayerIdentity() ||
       now<s_currentBodyFrame.stamp || now-s_currentBodyFrame.stamp>250)return false;
    for(int k=0;k<3;++k)p[k]=s_currentBodyFrame.position[k];
    for(int k=0;k<4;++k)q[k]=s_currentBodyFrame.rotation[k];
    *trackingYaw=s_currentBodyFrame.trackingYaw;return true;
}

void VRIK_PublishNativeCameraPair(const int32_t* centre,const int32_t* bodyBase,
        const int32_t* entity,const float* cameraQuat,const float* entityQuat,
        uintptr_t owner,uint64_t origin,bool ownerStable) {
    using namespace cvr::camera;
    const auto pair=MakeCameraEntitySample({centre[0],centre[1],centre[2]},
        {bodyBase[0],bodyBase[1],bodyBase[2]},{entity[0],entity[1],entity[2]},cameraQuat,entityQuat);
    std::lock_guard lock(s_nativePairWriter);
    s_nativePairSeq.fetch_add(1,std::memory_order_acq_rel);
    for(int k=0;k<4;++k) {
        s_nativePair.camQuat[k].store(pair.cameraQuat[k],std::memory_order_relaxed);
        s_nativePair.entityQuat[k].store(pair.entityQuat[k],std::memory_order_relaxed);
    }
    for(int k=0;k<3;++k) {
        s_nativePair.cameraMinusEntity[k].store(pair.cameraMinusEntity[k],std::memory_order_relaxed);
        s_nativePair.bodyCameraMinusEntity[k].store(pair.bodyMinusEntity[k],std::memory_order_relaxed);
    }
    s_nativePair.valid.store(ownerStable && pair.valid,std::memory_order_relaxed);
    s_nativePair.unavailable.store(!ownerStable,std::memory_order_relaxed);
    s_nativePair.consumerEpoch.store(g_VrikFrameEpoch.load(std::memory_order_relaxed),std::memory_order_relaxed);
    s_nativePair.owner.store(owner,std::memory_order_relaxed);
    s_nativePair.origin.store(origin,std::memory_order_relaxed);
    s_nativePair.stampMs.store(GetTickCount64(),std::memory_order_relaxed);
    s_nativePairSeq.fetch_add(1,std::memory_order_release);
    if(ownerStable && pair.valid)CVR_DIAGNOSTIC(++CyberpunkVR_DebugVrikNativePairPublished);
    else CVR_DIAGNOSTIC(++CyberpunkVR_DebugVrikNativePairRejected);
}

bool VRIK_ReadNativeTransformSnapshot(VrikTransformSnapshot* out) {
    if(!out)return false;
    for(int attempt=0;attempt<4;++attempt) {
        const uint32_t before=s_nativePairSeq.load(std::memory_order_acquire);
        if(!before)return false;
        if(before&1u)continue;
        VrikTransformSnapshot value{};
        for(int k=0;k<4;++k) {
            value.camQuat[k]=s_nativePair.camQuat[k].load(std::memory_order_relaxed);
            value.entityQuat[k]=s_nativePair.entityQuat[k].load(std::memory_order_relaxed);
        }
        for(int k=0;k<3;++k) {
            value.cameraMinusEntity[k]=s_nativePair.cameraMinusEntity[k].load(std::memory_order_relaxed);
            value.bodyCameraMinusEntity[k]=s_nativePair.bodyCameraMinusEntity[k].load(std::memory_order_relaxed);
        }
        value.valid=s_nativePair.valid.load(std::memory_order_relaxed);
        value.unavailable=s_nativePair.unavailable.load(std::memory_order_relaxed);
        const auto owner=s_nativePair.owner.load(std::memory_order_relaxed);
        const auto origin=s_nativePair.origin.load(std::memory_order_relaxed);
        const auto stamp=s_nativePair.stampMs.load(std::memory_order_relaxed);
        if(before!=s_nativePairSeq.load(std::memory_order_acquire))continue;
        using namespace cvr::camera;
        const auto state=CameraPacketStatus(owner,origin,stamp,cvr::roomscale::PlayerIdentity(),
            OpenXRManager::Get().GetTrackingOriginSerial(),GetTickCount64(),value.valid!=0,value.unavailable!=0);
        value.valid=state==CameraPacketState::Valid;
        value.unavailable=state==CameraPacketState::Unavailable;
        *out=value;return true;
    }
    return false;
}

// THE ACCUMULATED REALIGN, radians, game space about +Z: how much of the engine's current heading is
// ours rather than the player's own turning. LOAD-BEARING -- the view is composed from
// (engine yaw - this), and if it is wrong the view drifts by the error.
extern "C" __declspec(dllexport) float CyberpunkVR_BodyYawRealignRad = 0.0f;

// Readable live: the same realign in degrees, the head-against-body residual, and the two counters.
extern "C" __declspec(dllexport) float    CyberpunkVR_DebugBodyFollowOffsetDeg = 0.0f;
extern "C" __declspec(dllexport) float    CyberpunkVR_DebugBodyFollowErrDeg = 0.0f;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugBodyFollowCalls = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugBodyFollowApplied = 0;

// THE BODY'S TRUE YAW, radians, game convention -- the engine's own value, read at the store site.
// The pose path converts world->model with this while the follower is on: model space IS the entity
// frame, so the entity's actual angle is the only correct converter, and the camera heading (which we
// deliberately hold back by the realign) is not it.
extern "C" __declspec(dllexport) float CyberpunkVR_BodyYawFinalRad = 0.0f;
extern "C" __declspec(dllexport) int   CyberpunkVR_BodyYawFinalValid = 0;
// The player's frame, published so nothing in the pose path has to ask CET for it.
extern "C" __declspec(dllexport) float CyberpunkVR_PlayerEntityPos[3]  = { 0.0f, 0.0f, 0.0f };
extern "C" __declspec(dllexport) float CyberpunkVR_PlayerEntityQuat[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
extern "C" __declspec(dllexport) int   CyberpunkVR_PlayerEntityValid = 0;

namespace {

// The HMD's yaw relative to the recenter base, radians, about the XR vertical (+Y).
bool HeadYawRelBase(float* outYaw,float* outDownDegrees,float* outBend,uint64_t* origin,bool* yawDefined) {
    OpenXRHeadPose hp{};
    // This starts the next native move. Read the same input latch as CCT, not
    // the completed previous move that camera/IK consumers still own.
    if (!OpenXRManager::Get().AcquireFrameHeadSample(&hp,nullptr,nullptr,nullptr,false) || !hp.valid) return false;
    *origin = hp.originSerial;
    *outDownDegrees=cvr::roomscale::HeadDownDegrees(hp.oriX,hp.oriY,hp.oriZ,hp.oriW);
    *outBend=hp.bodyBend.angle;
    *yawDefined=cvr::roomscale::HeadYaw(hp.oriX, hp.oriY, hp.oriZ, hp.oriW, outYaw);
    const float norm=hp.oriX*hp.oriX+hp.oriY*hp.oriY+hp.oriZ*hp.oriZ+hp.oriW*hp.oriW;
    return std::isfinite(norm) && norm>.5f && norm<1.5f;
}

}  // namespace

// THE STEP TO INJECT INTO THE ENGINE'S HEADING THIS FRAME, radians. Called once per frame from the
// on-foot heading hook, which is the game's own turn channel.
//
// THE ERROR IS HEAD-AGAINST-BODY, and that is what makes the loop closed. Measuring the HMD against
// the recenter base has no feedback in it -- the body turning does not change that number -- and the
// realign ran away at 119 deg in two seconds, wrapping through 180. What closes is the residual:
//
//     residual = hmdYawRelBase - realign
//
// because the view is (engine yaw - realign) * mappedHmd while the body carries the engine yaw, so
// the engine's own value cancels out of the difference and what is left is how far the head is turned
// relative to the body. MAPPING = +1, observed rather than derived: the axis map (XR y -> game z)
// predicts it, one build contradicted it, and that build had the recenter base spinning the whole
// world -- a direction cannot be judged against a rotating world.
// GIVE THE REALIGN BACK. Every consumer subtracts this accumulator unconditionally -- the camera
// write (PatchCamera.cpp), the body forward LocateCamera publishes, and the movement axes
// (OnFootMoveXY.cpp) -- so a value left standing after the follower stops issuing steps is a
// permanent yaw error in all three, and nothing self-corrects it: the loop that would unwind it is
// the same loop that is no longer running.
//
// THE BRANCH BELOW COULD NOT DO THIS, which is why this function exists. BodyYawFollowStep is
// called from exactly one place, OnFootDeltaHead.cpp, and that site returns BEFORE the call in both
// states that need the release -- `if (g_isInVehicle) return;` at the top and the `if (!bodyRot)`
// early-out -- while also being what SETS CyberpunkVR_BodyYawFollow. So by the time control reaches
// the step the flag is always 1 and the guard inside it is dead code.
//
// Two states need it, and the difference matters:
//
//   THE FEATURE SWITCHED OFF -- the view stops cancelling, which is a one-time step of whatever had
//   accumulated. That is the intended behaviour (the body keeps the rotation it was given and the
//   view stops pretending it did not happen); the alternative is carrying the offset for the session.
//
//   MOUNTED -- and here the freeze was the real defect. The follower does not run in a vehicle, so
//   the accumulator holds the value it had on foot while PatchCamera keeps subtracting it from a yaw
//   that now comes from the CAR. Enter a car looking aside and the whole drive is spent looking that
//   far off the road (the cone is 25 deg, so the residue is head yaw minus 25), with no way back:
//   the only thing that unwinds the accumulator is the on-foot loop. Released here, the vehicle view
//   is composed from the car's own heading and nothing else, and stepping out re-converges normally.
namespace {
void PublishFollowerState() {
    CyberpunkVR_BodyYawFollow=s_yawFollower.Enabled() ? 1 : 0;
    CyberpunkVR_BodyYawRealignRad=s_yawFollower.Offset();
    CyberpunkVR_DebugBodyFollowOffsetDeg=s_yawFollower.Offset()*57.2957795f;
}
void RefreshFollowerState() {
    const auto mode=static_cast<cvr::body::RotationMode>(g_liveControls.xrBodyRotationMode);
    s_yawFollower.SetEnabled(mode!=cvr::body::RotationMode::Off &&
                            cvr::roomscale::PhysicalBodyHeadingOwned());
    if(mode!=s_bodyRotationMode || !s_yawFollower.Enabled()){
        s_trackedBody.Reset(s_yawFollower.Offset(),mode!=cvr::body::RotationMode::Hybrid);
        s_hybridBody.Reset();s_bodyRotationMode=mode;
    }
    PublishFollowerState();
}
}

bool BodyYawFollowActive() {
    std::lock_guard lock(s_yawFollowerMutex);
    RefreshFollowerState();
    return s_yawFollower.Enabled();
}
float BodyYawFollowOffset() {
    std::lock_guard lock(s_yawFollowerMutex);
    RefreshFollowerState();
    return s_yawFollower.Offset();
}
extern "C" void BodyYawFollowRelease() {
    std::lock_guard lock(s_yawFollowerMutex);
    s_yawFollower.Reset();
    s_trackedBody.Reset(0,s_bodyRotationMode!=cvr::body::RotationMode::Hybrid);
    s_hybridBody.Reset();
    PublishFollowerState();
}

extern "C" float BodyYawFollowStep() {
    // Acquire XR state before taking the follower lock: camera consumers may
    // read the offset while publishing that same XR frame.
    float hmdYaw{},headDownDegrees{},bodyBend{};
    uint64_t origin{};
    bool yawDefined=false;
    const bool haveHead=HeadYawRelBase(&hmdYaw,&headDownDegrees,&bodyBend,&origin,&yawDefined);
    cvr::body::TrackingFrame bodyFrame{};
    const bool haveBody=cvr::body::NeedsBodyTracking(static_cast<cvr::body::RotationMode>(g_liveControls.xrBodyRotationMode)) &&
        OpenXRManager::Get().GetBodyTrackingFrame(&bodyFrame);
    std::lock_guard lock(s_yawFollowerMutex);
    CVR_DIAGNOSTIC(++CyberpunkVR_DebugBodyFollowCalls);
    RefreshFollowerState();
    if(!s_yawFollower.Enabled() || !cvr::roomscale::PhysicalBodyAllowed() || !haveHead){
        s_trackedBody.Suspend();s_hybridBody.Suspend();return 0;
    }
    const auto now=static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    CVR_DIAGNOSTIC(CyberpunkVR_DebugBodyFollowErrDeg=yawDefined?
        std::remainder(hmdYaw-s_yawFollower.Offset(),6.28318530718f)*57.2957795f:0);
    cvr::body::YawEstimate result{};
    if(cvr::body::NeedsBodyTracking(s_bodyRotationMode)){
        if(haveBody && bodyFrame.origin==origin)result=s_trackedBody.Update(bodyFrame);
        else s_trackedBody.Suspend();
    }
    if(s_bodyRotationMode==cvr::body::RotationMode::Hybrid){
        const auto command=s_hybridBody.Step({hmdYaw,headDownDegrees,bodyBend,s_yawFollower.Offset(),
            g_liveControls.xrBodyFreeLookDeg,result,yawDefined,result.valid && s_trackedBody.Ready(),now,origin});
        CyberpunkVR_BodyYawFollowDeadDeg=command.coneDegrees;
        const float step=s_yawFollower.Step(command.targetYaw,0,now,origin);
        PublishFollowerState();
        if(step!=0)CVR_DIAGNOSTIC(++CyberpunkVR_DebugBodyFollowApplied);
        return step;
    }
    if(s_bodyRotationMode==cvr::body::RotationMode::Tracked){
        CyberpunkVR_BodyYawFollowDeadDeg=0; // no standing, look-down or swimming cone in this mode
        const float target=result.valid?result.yaw:s_yawFollower.Offset();
        const float step=s_yawFollower.Step(target,0,now,origin);
        PublishFollowerState();
        if(step!=0)CVR_DIAGNOSTIC(++CyberpunkVR_DebugBodyFollowApplied);
        return step;
    }
    if(!yawDefined)return 0;
    CyberpunkVR_BodyYawFollowDeadDeg=cvr::roomscale::BodyFreeLookCone(
        cvr::swimming::Active(),g_liveControls.xrBodyFreeLookSwimDeg,
        headDownDegrees,g_liveControls.xrBodyFreeLookDeg,g_liveControls.xrBodyFreeLookDownDeg,bodyBend);
    const float step=s_yawFollower.Step(hmdYaw,CyberpunkVR_BodyYawFollowDeadDeg*.01745329252f,now,origin);
    PublishFollowerState();
    if(step!=0)CVR_DIAGNOSTIC(++CyberpunkVR_DebugBodyFollowApplied);
    return step;
}

// Called from the body-yaw store site (src/Hooks/BodyYawCensus.cpp), once per frame for the player.
// Publishes the body's own transform for the pose path; writes nothing into the game.
// THE POSITION COMES FROM THE ENGINE, IN THE SAME INSTANT AS THE YAW.
//
// It used to be copied from g_VREntityPos*, the CET push, while the yaw beside it came straight off
// the store site. That was corrected here, but it did not make g_lastLocate* a matching camera:
// LocateCamera publishes after animation, so animation sees camera(N-1) beside this entity(N).
// VRIK_ComputeCamModel therefore no longer subtracts these two absolutes; it consumes the coherent
// relative pair from one SetVRPlayerYaw push. This current engine position remains the right source
// for the camera-mount and script consumers below.
extern "C" void BodyYawFollowTick(float engineZ, float engineW, const float* enginePos) {
    float wz = engineZ, ww = engineW;
    if (ww < 0.0f) { wz = -wz; ww = -ww; }
    if (wz == 0.0f && ww == 0.0f) return;
    if (!enginePos) return;
    const float yaw = 2.0f * std::atan2(wz, ww);
    const float h = yaw * 0.5f;
    const float currentEntityQuat[4] = { 0.0f, 0.0f, std::sin(h), std::cos(h) };
    {
        std::lock_guard lock(s_currentBodyFrame.mutex);
        for(int k=0;k<3;++k)s_currentBodyFrame.position[k]=enginePos[k];
        for(int k=0;k<4;++k)s_currentBodyFrame.rotation[k]=currentEntityQuat[k];
        s_currentBodyFrame.trackingYaw=yaw-BodyYawFollowOffset();
        s_currentBodyFrame.owner=cvr::roomscale::PlayerIdentity();
        s_currentBodyFrame.stamp=GetTickCount64();
    }

    // Camera/owner pairing is published by the completed camera write itself.
    // This callback only publishes the current engine body orientation/position.
    CyberpunkVR_BodyYawFinalRad = yaw;
    CyberpunkVR_BodyYawFinalValid = 1;
    CyberpunkVR_PlayerEntityPos[0] = enginePos[0];
    CyberpunkVR_PlayerEntityPos[1] = enginePos[1];
    CyberpunkVR_PlayerEntityPos[2] = enginePos[2];
    CyberpunkVR_PlayerEntityQuat[0] = 0.0f;
    CyberpunkVR_PlayerEntityQuat[1] = 0.0f;
    CyberpunkVR_PlayerEntityQuat[2] = currentEntityQuat[2];
    CyberpunkVR_PlayerEntityQuat[3] = currentEntityQuat[3];
    CyberpunkVR_PlayerEntityValid = 1;
}

// FOR THE RECORD, a door that is real but not the one in: sub_140336390 calls [vt+0x40] on its state
// provider (vtable 0x142AEDBD8) right before storing the transform, handing it r8 = &position,
// r9 = &quaternion, and on that class the slot is a bare `retn` -- an adjust-my-transform hook the
// engine invokes every frame and nobody implements. Claiming it worked, but it never fired for the
// player (0 calls against 6552 counted player frames), so the player's state uses a provider of a
// different class. Still an extension point for other characters.
