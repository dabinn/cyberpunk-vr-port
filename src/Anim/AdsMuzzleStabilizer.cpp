// AdsMuzzleStabilizer -- the muzzle keeps pointing where it pointed, through the vanilla ADS raise.
//
// Ported from dabinn's TofuExpress (797a2a95, "fix(ads): prevent laser dot shifting during non-VRIK
// ADS"). The problem is specific to the NON-VRIK arms: raising the sights plays the game's authored
// aim-in animation, and that animation does not merely move the weapon closer to the eye, it CHANGES
// THE DIRECTION the barrel points. On a flat screen nobody notices, because the game then draws its
// reticle wherever the weapon ends up. In VR the aim point is derived from the real muzzle transform,
// so the dot -- and the bullet -- slide away from where the player was aiming a moment earlier.
//
// The shape of the fix is a closed loop rather than an override, and that is the whole trick: the
// vanilla animation is left to play, and only the residual DIRECTION error it introduces is taken
// back out.
//
//   HIP FIRE records the muzzle direction that is trustworthy, in GAME-HEADING space. Heading space
//   is the right frame because it follows mouse/stick turning while excluding physical HMD rotation:
//   a hip aim should travel with the right stick and stay put when only the player's head moves.
//
//   WHILE AIMING the recorded direction is rotated back into the world, the angular error against the
//   live muzzle is measured, and 35% of it is applied per tick to the WeaponRight bone -- in MODEL
//   space, converted through the entity quaternion. Bounded at 15 degrees total, so a wrong reference
//   can never throw the weapon across the screen.
//
//   WHEN AIM-IN ENDS the accumulated correction is converted into a WEAPON-LOCAL delta and frozen.
//   That conversion is what keeps ADS alive: a frozen model-space rotation would fight breathing,
//   sway and right-stick aiming, while the same rotation expressed in the weapon's own frame rides
//   along with all three.
//
// Two traps are handled explicitly, both learned the hard way upstream:
//
//   * The pose hook visits the same buffer several times per entity tick. Composing the correction
//     onto an already-corrected pass multiplies it, and the skew accumulates until it is permanent --
//     so the tick's RAW WeaponRight local rotation is cached and every pass composes from that.
//   * The hip reference must not be learned from the lowered weapon or from the raise transition, or
//     it records a direction the player never aimed. Weapon PSM 5 is the real ranged Ready state; Safe
//     and PublicSafeToReady are excluded by the redscript that publishes those states.
//
// DEPARTURES FROM THE ORIGINAL. His version carries the redscript values and the enable flag in
// shared-memory slots [158..162]; those numbers are already taken in this tree (the B and Y buttons,
// the trigger channel) and, more to the point, everything here lives in ONE plugin now -- the natives
// that receive the redscript values and this consumer are the same DLL, so they are plain globals.
// The aiming flag is g_isAiming, which the camera hook already refreshes, rather than a slot. And the
// heading comes from the LATCHED view packet instead of a live slot read, because this tree has
// measured that mixing a latched view with a directly-read heading is what produced the snap-turn arm
// double.

#include "Anim/AdsMuzzleStabilizer.hpp"

#include "Anim/AdsSightAim.hpp"
#include "Anim/CharacterRig.hpp"
#include "Anim/AdsEyeAlign.hpp"
#include "Anim/HeadAimWeapon.hpp"
#include "Anim/VrikHook.hpp"
#include "Anim/VrikState.hpp"
#include "Camera/CameraState.hpp"
#include "Core/VrCoreShared.hpp"   // g_isAiming
#include "Utils/SharedSlots.hpp"

#include <cmath>
#include <cstdint>

extern float* g_pSharedHands;
extern volatile float g_provMuzzleQ[4];

// 1 = correct the non-VRIK ADS muzzle drift (default). The knob exists because this writes a bone
// every tick while aiming, and a feature that writes bones should be switchable without a rebuild.
extern "C" __declspec(dllexport) int32_t  CyberpunkVR_NonVrikAdsStabilizer = 1;
// How often the correction is actually applied, and how often the reference was refused. Both are
// answers to "is it running at all", which cost a round trip to guess at.
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugAdsStabApplies = 0;
extern "C" __declspec(dllexport) uint64_t CyberpunkVR_DebugAdsStabHipCaptures = 0;

namespace cvr::anim {

namespace {

constexpr float kBallisticAngleLimit = 10.0f * 0.01745329252f;

struct AdsBallisticState {
    float tick = -1.0f;
    float solveTick = -1.0f;
    int owner = -1;
    int weapon = -1;
    int transitionTicks = 0;
    bool aiming = false;
    bool targetValid = false;
    bool frozen = false;
    bool sawAimIn = false;
    bool freezeAfterSolve = false;
    bool originLocalValid = false;
    bool lastAppliedValid = false;
    float target[3] = {};
    float originLocal[3] = {};
    float correctionLocal[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float lastAppliedWorld[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float feedbackForward[3] = {0.0f, 1.0f, 0.0f};
};
AdsBallisticState s_ballistic;

void SetIdentity(float* q) {
    q[0] = 0.0f; q[1] = 0.0f; q[2] = 0.0f; q[3] = 1.0f;
}

float WrapPi(float a) {
    constexpr float kPi = 3.14159265359f;
    constexpr float kTwoPi = 6.28318530718f;
    while (a > kPi) a -= kTwoPi;
    while (a < -kPi) a += kTwoPi;
    return a;
}

void AxisAngleQuat(const float* axis, float angle, float* out) {
    const float h = angle * 0.5f;
    const float s = std::sin(h);
    out[0] = axis[0] * s;
    out[1] = axis[1] * s;
    out[2] = axis[2] * s;
    out[3] = std::cos(h);
    VRIK_QuatNorm(out);
}

void ModelPointToWorld(const float* model, const float* entityQ, float* world) {
    float r[3];
    VRIK_QuatRotateVec(entityQ, model, r);
    world[0] = g_VREntityPosX + r[0];
    world[1] = g_VREntityPosY + r[1];
    world[2] = g_VREntityPosZ + r[2];
}

// Find the smallest rotation about the normal of the current ray/target-ray plane that makes the
// rigidly rotated firing ray pass through target. Origin rotates around the real wrist pivot too.
bool SolvePivotBallisticRotation(const float* pivot, const float* origin,
                                 const float* forwardIn, const float* target,
                                 float* axisOut, float* angleOut) {
    float forward[3] = {forwardIn[0], forwardIn[1], forwardIn[2]};
    if (VRIK_Norm3(forward) < 0.5f) return false;

    float toTarget[3] = {target[0] - origin[0], target[1] - origin[1], target[2] - origin[2]};
    if (VRIK_Norm3(toTarget) < 1e-4f) return false;

    float axis[3];
    VRIK_Cross3(forward, toTarget, axis);
    const float axisLen = VRIK_Norm3(axis);
    if (axisLen < 1e-5f) {
        axisOut[0] = 1.0f; axisOut[1] = 0.0f; axisOut[2] = 0.0f;
        *angleOut = 0.0f;
        return true;
    }

    // u is the in-plane direction 90 degrees from the original firing direction.
    float u[3];
    VRIK_Cross3(axis, forward, u);
    if (VRIK_Norm3(u) < 0.5f) return false;

    const float q[3] = {target[0] - pivot[0], target[1] - pivot[1], target[2] - pivot[2]};
    const float r[3] = {origin[0] - pivot[0], origin[1] - pivot[1], origin[2] - pivot[2]};
    const float qf = VRIK_Dot3(forward, q);
    const float qu = VRIK_Dot3(u, q);
    const float ru = VRIK_Dot3(u, r);
    const float radius = std::sqrt(qf * qf + qu * qu);
    if (radius < 1e-5f) return false;

    float k = ru / radius;
    if (k < -1.0001f || k > 1.0001f) return false;
    if (k < -1.0f) k = -1.0f;
    if (k > 1.0f) k = 1.0f;

    const float alpha = std::atan2(qf, qu);
    const float beta = std::acos(k);
    const float candidates[2] = {WrapPi(beta - alpha), WrapPi(-beta - alpha)};

    bool have = false;
    float chosen = 0.0f;
    for (float theta : candidates) {
        float qRot[4]; AxisAngleQuat(axis, theta, qRot);
        float rRot[3], fRot[3];
        VRIK_QuatRotateVec(qRot, r, rRot);
        VRIK_QuatRotateVec(qRot, forward, fRot);
        const float v[3] = {target[0] - (pivot[0] + rRot[0]),
                            target[1] - (pivot[1] + rRot[1]),
                            target[2] - (pivot[2] + rRot[2])};
        if (VRIK_Dot3(fRot, v) <= 0.0f) continue;
        if (!have || std::fabs(theta) < std::fabs(chosen)) {
            chosen = theta;
            have = true;
        }
    }
    if (!have) return false;

    if (chosen > kBallisticAngleLimit) chosen = kBallisticAngleLimit;
    if (chosen < -kBallisticAngleLimit) chosen = -kBallisticAngleLimit;
    axisOut[0] = axis[0]; axisOut[1] = axis[1]; axisOut[2] = axis[2];
    *angleOut = chosen;
    return true;
}

bool ReadSurfaceTarget(float* target) {
    for (int retry = 0; retry < 3; ++retry) {
        const auto seq = static_cast<uint32_t>(g_pSharedHands[vrshared::kBarrelRaySeq]);
        if (!seq || (seq & 1u)) continue;
        std::atomic_thread_fence(std::memory_order_acquire);
        float p[3] = {g_pSharedHands[vrshared::kBarrelRayHitX],
                      g_pSharedHands[vrshared::kBarrelRayHitX + 1],
                      g_pSharedHands[vrshared::kBarrelRayHitX + 2]};
        const bool valid = g_pSharedHands[vrshared::kBarrelRayHitValid] > 0.5f;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq != static_cast<uint32_t>(g_pSharedHands[vrshared::kBarrelRaySeq])) continue;
        if (!valid || !std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]))
            return false;
        for (int k = 0; k < 3; ++k) target[k] = p[k];
        return true;
    }
    return false;
}

} // namespace


void UpdateAdsBallisticCorrection() {
    auto& s = s_ballistic;
    if (!g_pSharedHands) { s = {}; return; }
    const float tick = g_pSharedHands[vrshared::kEntitySeq];
    const int owner = IsHeadAimWeaponActive() ? 1 : (g_VRBind == 4 ? 2 : (g_VRBind > 0 ? 3 : 0));
    const int weapon = static_cast<int>(g_VRSmokeCigIdx);
    if (s.tick == tick && s.owner == owner && s.weapon == weapon) return;

    // Remove the previous ballistic rotation from the published muzzle direction before the old
    // non-VRIK animation stabilizer sees it; otherwise that independent loop learns to cancel us.
    const float forward[3] = {0.0f, 1.0f, 0.0f};
    float feedback[3] = {g_pSharedHands[24], g_pSharedHands[25], g_pSharedHands[26]};
    if (s.lastAppliedValid) {
        float muzzleQ[4] = {::g_provMuzzleQ[0], ::g_provMuzzleQ[1],
                            ::g_provMuzzleQ[2], ::g_provMuzzleQ[3]};
        VRIK_QuatNorm(muzzleQ);
        float invCorrection[4], baseQ[4];
        VRIK_QuatConj(s.lastAppliedWorld, invCorrection);
        VRIK_QuatMul(invCorrection, muzzleQ, baseQ);
        VRIK_QuatNorm(baseQ);
        VRIK_QuatRotateVec(baseQ, forward, feedback);
    }

    const bool enabled = overlay::g_drawBarrelCross && g_liveControls.xrLaserDotMode == 2 &&
        g_liveControls.xrHideLaserDotAds != 0 &&
        owner != 3 && g_pSharedHands[vrshared::kWeaponFlag] > 0.5f && g_pSharedHands[27] > 0.5f;
    if (!enabled || owner != s.owner || weapon != s.weapon) s = {};
    for (int k = 0; k < 3; ++k) s.feedbackForward[k] = feedback[k];
    s.tick = tick;
    s.owner = owner;
    s.weapon = weapon;
    s.lastAppliedValid = false;
    if (!enabled) return;

    const bool aiming = g_isAiming;
    const bool aimIn = g_VRAimInRemaining > 0.001f;
    if (!aiming) {
        SetIdentity(s.correctionLocal);
        s.solveTick = -1.0f;
        s.frozen = false;
        s.freezeAfterSolve = false;
        s.sawAimIn = false;
        s.transitionTicks = 0;
        s.originLocalValid = false;
        s.targetValid = std::lround(g_VRWeaponPsmState) == 5 &&
            g_VRWeaponRaiseTransition == 0 &&
            g_pSharedHands[vrshared::kBarrelRayActive] > 0.5f &&
            ReadSurfaceTarget(s.target);
    } else if (s.targetValid) {
        ++s.transitionTicks;
        if (aimIn) s.sawAimIn = true;
        s.freezeAfterSolve = !s.frozen &&
            ((s.sawAimIn && !aimIn) || (!s.sawAimIn && s.transitionTicks >= 20));
    }
    s.aiming = aiming;
}

void ApplyAdsBallisticCorrectionToWeapon(uint8_t* boneBuf, int weaponIdx,
                                         const float* wristModelPos, float* weaponModelRot,
                                         const float* weaponLocalOverride) {
    auto& s = s_ballistic;
    if (!VrAdsSightAimActive() || g_liveControls.xrLaserDotMode != 2 ||
        !s.targetValid || !s.aiming || !boneBuf || !wristModelPos || !weaponModelRot) return;

    const int handIdx = static_cast<int>(g_VRRightBoneIdx);
    if (weaponIdx < 0 || weaponIdx >= VRIK_FKCount() || handIdx < 0 ||
        handIdx >= VRIK_FKCount() || g_VRBoneParent[weaponIdx] != handIdx) return;

    float weaponLocal[4];
    if (weaponLocalOverride) {
        for (int k = 0; k < 4; ++k) weaponLocal[k] = weaponLocalOverride[k];
    } else {
        const float* q = reinterpret_cast<const float*>(boneBuf + weaponIdx * 48 + VRIK_ROT_OFF);
        for (int k = 0; k < 4; ++k) weaponLocal[k] = q[k];
    }
    VRIK_QuatNorm(weaponLocal);
    float invWeaponLocal[4], handModelRot[4];
    VRIK_QuatConj(weaponLocal, invWeaponLocal);
    VRIK_QuatMul(weaponModelRot, invWeaponLocal, handModelRot);
    VRIK_QuatNorm(handModelRot);

    const float* weaponLocalPos =
        reinterpret_cast<const float*>(boneBuf + weaponIdx * 48 + VRIK_TRANS_OFF);
    float weaponOffsetModel[3];
    VRIK_QuatRotateVec(handModelRot, weaponLocalPos, weaponOffsetModel);
    const float weaponModelPos[3] = {wristModelPos[0] + weaponOffsetModel[0],
                                     wristModelPos[1] + weaponOffsetModel[1],
                                     wristModelPos[2] + weaponOffsetModel[2]};

    float entityQ[4] = {g_VREntityQI, g_VREntityQJ, g_VREntityQK, g_VREntityQR};
    VRIK_QuatNorm(entityQ);
    float weaponWorldRot[4];
    VRIK_QuatMul(entityQ, weaponModelRot, weaponWorldRot);
    VRIK_QuatNorm(weaponWorldRot);
    float wristWorld[3], weaponWorldPos[3];
    ModelPointToWorld(wristModelPos, entityQ, wristWorld);
    ModelPointToWorld(weaponModelPos, entityQ, weaponWorldPos);

    if (!s.originLocalValid && !s.frozen) {
        float sight[3];
        if (!VrReadSightOrigin(sight) || g_pSharedHands[203] <= 0.5f) return;
        const float rel[3] = {sight[0] - weaponWorldPos[0], sight[1] - weaponWorldPos[1],
                              sight[2] - weaponWorldPos[2]};
        float invWeaponWorld[4];
        VRIK_QuatConj(weaponWorldRot, invWeaponWorld);
        VRIK_QuatRotateVec(invWeaponWorld, rel, s.originLocal);
        s.originLocalValid = true;
    }
    if (!s.originLocalValid) return;

    if (!s.frozen && s.solveTick != s.tick) {
        float originOffsetWorld[3], originWorld[3], firingForward[3];
        VRIK_QuatRotateVec(weaponWorldRot, s.originLocal, originOffsetWorld);
        originWorld[0] = weaponWorldPos[0] + originOffsetWorld[0];
        originWorld[1] = weaponWorldPos[1] + originOffsetWorld[1];
        originWorld[2] = weaponWorldPos[2] + originOffsetWorld[2];
        const float localForward[3] = {0.0f, 1.0f, 0.0f};
        VRIK_QuatRotateVec(weaponWorldRot, localForward, firingForward);

        float axisWorld[3], theta = 0.0f;
        if (SolvePivotBallisticRotation(wristWorld, originWorld, firingForward, s.target,
                                        axisWorld, &theta)) {
            float invWeaponWorld[4], axisLocal[3];
            VRIK_QuatConj(weaponWorldRot, invWeaponWorld);
            VRIK_QuatRotateVec(invWeaponWorld, axisWorld, axisLocal);
            if (VRIK_Norm3(axisLocal) > 0.5f) AxisAngleQuat(axisLocal, theta, s.correctionLocal);
            else SetIdentity(s.correctionLocal);
        } else {
            SetIdentity(s.correctionLocal);
        }
        s.solveTick = s.tick;
        if (s.freezeAfterSolve) {
            s.frozen = true;
            s.freezeAfterSolve = false;
        }
    }

    float result[4];
    VRIK_QuatMul(weaponModelRot, s.correctionLocal, result);
    VRIK_QuatNorm(result);
    for (int k = 0; k < 4; ++k) weaponModelRot[k] = result[k];

    // Save the equivalent world-space delta so next tick's published muzzle orientation can have
    // only this ballistic layer removed before the independent animation stabilizer reads it.
    float invWeaponWorld[4], tmp[4];
    VRIK_QuatConj(weaponWorldRot, invWeaponWorld);
    VRIK_QuatMul(weaponWorldRot, s.correctionLocal, tmp);
    VRIK_QuatMul(tmp, invWeaponWorld, s.lastAppliedWorld);
    VRIK_QuatNorm(s.lastAppliedWorld);
    s.lastAppliedValid = true;
}

void ApplyWristTargetAdsBallisticCorrection(uint8_t* boneBuf, const float* wristTargetModel,
                                            float* handModelRot) {
    if (!VrAdsSightAimActive() || g_liveControls.xrLaserDotMode != 2 ||
        !s_ballistic.targetValid || !wristTargetModel) return;
    const int weapon = static_cast<int>(g_VRSmokeCigIdx);
    if (weapon < 0 || weapon >= VRIK_FKCount() ||
        g_VRBoneParent[weapon] != g_VRRightBoneIdx) return;
    const float* local = reinterpret_cast<const float*>(boneBuf + weapon * 48 + VRIK_ROT_OFF);
    float weaponRot[4], invLocal[4], result[4];
    VRIK_QuatMul(handModelRot, local, weaponRot);
    ApplyAdsBallisticCorrectionToWeapon(boneBuf, weapon, wristTargetModel, weaponRot);
    VRIK_QuatConj(local, invLocal);
    VRIK_QuatMul(weaponRot, invLocal, result);
    VRIK_QuatNorm(result);
    for (int k = 0; k < 4; ++k) handModelRot[k] = result[k];
}

void ApplyNonVrikAdsMuzzleStabilizer(uint8_t* boneBuf) {
    // One entry per pose buffer the hook visits, so each buffer keeps its own raw rotation for the
    // tick. Four is one more than the three the player pass has been measured to use.
    struct RawWeaponPoseCache {
        uint8_t* boneBuf = nullptr;
        float tick = -1.0f;
        int weaponIdx = -1;
        int handIdx = -1;
        // BOTH the weapon's and the hand's authored local rotations. The correction is applied to the
        // hand, so the hand's raw value is as much a part of "the pose this tick started from" as the
        // weapon's (dabinn, TofuExpress 73bdf668).
        float weaponLocalRot[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        float handLocalRot[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    };
    static float s_hipAimHeading[3] = {0.0f, 1.0f, 0.0f};
    static float s_totalModelCorrection[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    static float s_frozenWeaponLocalCorrection[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    static float s_lastTick = -1.0f;
    static bool s_hipAimValid = false;
    static bool s_prevAiming = false;
    static bool s_adsCorrectionFrozen = false;
    static bool s_adsSawAimIn = false;
    static bool s_freezeLocalCorrectionPending = false;
    static int s_adsTransitionTicks = 0;
    static int s_adsConvergedTicks = 0;
    static RawWeaponPoseCache s_rawPoseCache[4];
    static int s_rawPoseReplace = 0;

    // Off, VRIK-driven arms, no weapon out, or no muzzle published: reset to identity so nothing is
    // carried into the next time it applies. A stale correction is worse than none.
    // Head aim writes WeaponRight's rotation outright, so there is no vanilla direction error
    // left to correct -- and two writers on one bone is a fight, not a fix.
    if (!g_pSharedHands || IsHeadAimWeaponActive() || g_VRBind > 0 ||
        !CyberpunkVR_NonVrikAdsStabilizer ||
        g_pSharedHands[vrshared::kWeaponFlag] <= 0.5f ||
        g_pSharedHands[27] <= 0.5f) {
        s_totalModelCorrection[0] = 0.0f; s_totalModelCorrection[1] = 0.0f;
        s_totalModelCorrection[2] = 0.0f; s_totalModelCorrection[3] = 1.0f;
        s_frozenWeaponLocalCorrection[0] = 0.0f; s_frozenWeaponLocalCorrection[1] = 0.0f;
        s_frozenWeaponLocalCorrection[2] = 0.0f; s_frozenWeaponLocalCorrection[3] = 1.0f;
        s_prevAiming = false;
        s_adsCorrectionFrozen = false;
        s_freezeLocalCorrectionPending = false;
        return;
    }

    // WeaponRight, which is what this global resolves to (the name is a leftover from the smoking
    // pose, which rides the same bone).
    const int weaponIdx = static_cast<int>(g_VRSmokeCigIdx);
    if (weaponIdx < 0 || weaponIdx >= VRIK_FKCount()) return;

    float muzzle[3] = {s_ballistic.feedbackForward[0], s_ballistic.feedbackForward[1],
                       s_ballistic.feedbackForward[2]};
    if (VRIK_Norm3(muzzle) < 0.5f) return;
    float entQ[4] = { g_VREntityQI, g_VREntityQJ, g_VREntityQK, g_VREntityQR };
    VRIK_QuatNorm(entQ);

    // The heading from the packet latched for THIS solve, not a live read -- see the file header.
    if (!g_viewPktValid) return;
    const float heading = g_viewPkt[8];

    const bool aiming = g_isAiming;
    const float tick = g_pSharedHands[vrshared::kEntitySeq];
    if (tick != s_lastTick) {
        s_lastTick = tick;
        const int weaponPsm = static_cast<int>(std::lround(g_VRWeaponPsmState));
        const bool weaponPsmReady = (weaponPsm == 5);
        const bool safeToReady = (g_VRWeaponRaiseTransition != 0);
        const bool aimInRunning = (g_VRAimInRemaining > 0.001f);

        if (aiming && !s_prevAiming) {
            s_totalModelCorrection[0] = 0.0f; s_totalModelCorrection[1] = 0.0f;
            s_totalModelCorrection[2] = 0.0f; s_totalModelCorrection[3] = 1.0f;
            s_frozenWeaponLocalCorrection[0] = 0.0f; s_frozenWeaponLocalCorrection[1] = 0.0f;
            s_frozenWeaponLocalCorrection[2] = 0.0f; s_frozenWeaponLocalCorrection[3] = 1.0f;
            s_adsCorrectionFrozen = false;
            s_adsSawAimIn = aimInRunning;
            s_freezeLocalCorrectionPending = false;
            s_adsTransitionTicks = 0;
            s_adsConvergedTicks = 0;
        } else if (!aiming && s_prevAiming) {
            s_adsCorrectionFrozen = false;
            s_adsSawAimIn = false;
            s_freezeLocalCorrectionPending = false;
            s_adsTransitionTicks = 0;
            s_adsConvergedTicks = 0;
        }

        if (!aiming) {
            s_totalModelCorrection[0] = 0.0f; s_totalModelCorrection[1] = 0.0f;
            s_totalModelCorrection[2] = 0.0f; s_totalModelCorrection[3] = 1.0f;

            // Weapon PSM 5 is the actual ranged Ready state. In Safe, and through the
            // PublicSafeToReady raise, the previous reference is preserved rather than replaced with
            // the lowered pose or a pose mid-transition.
            if (weaponPsmReady && !safeToReady) {
                const float hs = std::sin(heading * 0.5f);
                const float hc = std::cos(heading * 0.5f);
                const float invHeadingQ[4] = {0.0f, 0.0f, -hs, hc};
                float local[3];
                VRIK_QuatRotateVec(invHeadingQ, muzzle, local);
                if (VRIK_Norm3(local) > 0.5f) {
                    // Ready is the authority: whatever the barrel dot shows this tick IS the aim.
                    // Combat motion and turn sway are valid samples, not outliers to be filtered.
                    s_hipAimHeading[0] = local[0];
                    s_hipAimHeading[1] = local[1];
                    s_hipAimHeading[2] = local[2];
                    s_hipAimValid = true;
                    ++CyberpunkVR_DebugAdsStabHipCaptures;
                }
            }
        } else if (s_hipAimValid && !s_adsCorrectionFrozen) {
            if (aimInRunning) s_adsSawAimIn = true;
            const float headingQ[4] = {
                0.0f, 0.0f, std::sin(heading * 0.5f), std::cos(heading * 0.5f)};
            float desiredWorld[3];
            VRIK_QuatRotateVec(headingQ, s_hipAimHeading, desiredWorld);
            VRIK_Norm3(desiredWorld);

            float errorWorld[4];
            VRIK_QuatFromTo(muzzle, desiredWorld, errorWorld);
            float alignment = muzzle[0] * desiredWorld[0] + muzzle[1] * desiredWorld[1] +
                              muzzle[2] * desiredWorld[2];
            if (alignment > 1.0f) alignment = 1.0f;
            if (alignment < -1.0f) alignment = -1.0f;
            const float errorRadians = std::acos(alignment);
            ++s_adsTransitionTicks;
            if (errorRadians < 0.2f * 0.01745329252f) {
                if (s_adsConvergedTicks < 1000) ++s_adsConvergedTicks;
            } else {
                s_adsConvergedTicks = 0;
            }

            // 35% of the error per tick, in model space (entity-conjugated), accumulated and clamped.
            float stepWorld[4];
            VRIK_QuatScale(errorWorld, 0.35f, stepWorld);
            float invEnt[4];
            VRIK_QuatConj(entQ, invEnt);
            float tmp[4], stepModel[4];
            VRIK_QuatMul(invEnt, stepWorld, tmp);
            VRIK_QuatMul(tmp, entQ, stepModel);
            VRIK_QuatNorm(stepModel);

            float next[4];
            VRIK_QuatMul(stepModel, s_totalModelCorrection, next);
            VRIK_QuatNorm(next);
            constexpr float kMaxCorrectionRadians = 15.0f * 0.01745329252f;
            float w = std::fabs(next[3]);
            if (w > 1.0f) w = 1.0f;
            const float angle = 2.0f * std::acos(w);
            if (angle > kMaxCorrectionRadians) {
                VRIK_QuatScale(next, kMaxCorrectionRadians / angle, next);
            }
            s_totalModelCorrection[0] = next[0]; s_totalModelCorrection[1] = next[1];
            s_totalModelCorrection[2] = next[2]; s_totalModelCorrection[3] = next[3];

            // AimInTimeRemaining is authored by AimingStateEvents for the real weapon ADS
            // transition, so it is the exact end of the thing being corrected. The convergence
            // fallback is only for weapons whose AimInTime is zero or missing.
            if ((s_adsSawAimIn && !aimInRunning) ||
                (!s_adsSawAimIn && s_adsTransitionTicks >= 20 && s_adsConvergedTicks >= 3)) {
                s_adsCorrectionFrozen = true;
                s_freezeLocalCorrectionPending = true;
            }
        }
        s_prevAiming = aiming;
    }

    if (!aiming || !s_hipAimValid) return;

    // ALWAYS COMPOSE FROM THE TICK'S RAW ROTATION. The hook visits this buffer several times per
    // tick; composing onto an already-corrected pass multiplies the correction and the skew becomes
    // permanent -- in ADS and in the hip pose that follows it.
    RawWeaponPoseCache* rawPose = nullptr;
    for (auto& entry : s_rawPoseCache) {
        if (entry.boneBuf == boneBuf) { rawPose = &entry; break; }
    }
    if (!rawPose) {
        rawPose = &s_rawPoseCache[s_rawPoseReplace++ & 3];
        rawPose->boneBuf = boneBuf;
        rawPose->tick = -1.0f;
    }
    const int handIdx = static_cast<int>(g_VRRightBoneIdx);
    if (handIdx < 0 || handIdx >= VRIK_FKCount() || g_VRBoneParent[weaponIdx] != handIdx) return;
    if (rawPose->tick != tick || rawPose->weaponIdx != weaponIdx || rawPose->handIdx != handIdx) {
        const float* rawWeaponLocal =
            reinterpret_cast<const float*>(boneBuf + weaponIdx * 48 + VRIK_ROT_OFF);
        const float* rawHandLocal =
            reinterpret_cast<const float*>(boneBuf + handIdx * 48 + VRIK_ROT_OFF);
        rawPose->weaponLocalRot[0] = rawWeaponLocal[0]; rawPose->weaponLocalRot[1] = rawWeaponLocal[1];
        rawPose->weaponLocalRot[2] = rawWeaponLocal[2]; rawPose->weaponLocalRot[3] = rawWeaponLocal[3];
        rawPose->handLocalRot[0] = rawHandLocal[0]; rawPose->handLocalRot[1] = rawHandLocal[1];
        rawPose->handLocalRot[2] = rawHandLocal[2]; rawPose->handLocalRot[3] = rawHandLocal[3];
        VRIK_QuatNorm(rawPose->weaponLocalRot);
        VRIK_QuatNorm(rawPose->handLocalRot);
        rawPose->tick = tick;
        rawPose->weaponIdx = weaponIdx;
        rawPose->handIdx = handIdx;
    }

    VRIK_ComputeFK(boneBuf, VRIK_FKCount());
    const int handParent = g_VRBoneParent[handIdx];
    if (handParent < 0 || handParent >= handIdx) return;
    // The weapon's model rotation this tick STARTED at: hand-from-its-parent, then the authored grip.
    float rawHandModel[4];
    VRIK_QuatMul(g_fkRot[handParent], rawPose->handLocalRot, rawHandModel);
    VRIK_QuatNorm(rawHandModel);
    float rawModel[4];
    VRIK_QuatMul(rawHandModel, rawPose->weaponLocalRot, rawModel);
    VRIK_QuatNorm(rawModel);

    float correctedModel[4];
    if (s_adsCorrectionFrozen && !s_freezeLocalCorrectionPending) {
        // Frozen: the correction rides in the WEAPON's frame, so breathing, sway and right-stick
        // aiming all still move the weapon normally.
        VRIK_QuatMul(rawModel, s_frozenWeaponLocalCorrection, correctedModel);
    } else {
        VRIK_QuatMul(s_totalModelCorrection, rawModel, correctedModel);
        if (s_freezeLocalCorrectionPending) {
            float invRawModel[4];
            VRIK_QuatConj(rawModel, invRawModel);
            VRIK_QuatMul(invRawModel, correctedModel, s_frozenWeaponLocalCorrection);
            VRIK_QuatNorm(s_frozenWeaponLocalCorrection);
            s_freezeLocalCorrectionPending = false;
        }
    }
    VRIK_QuatNorm(correctedModel);
    // Through the hand, with the tick's RAW grip as the factor to divide out -- so the grip survives
    // and repeated passes cannot compound.
    ApplyAdsBallisticCorrectionToWeapon(boneBuf, weaponIdx, g_fkPos[handIdx], correctedModel,
                                        rawPose->weaponLocalRot);
    WriteWeaponModelRotViaRightHand(boneBuf, weaponIdx, correctedModel, rawPose->weaponLocalRot);
    ++CyberpunkVR_DebugAdsStabApplies;
}

}  // namespace cvr::anim
