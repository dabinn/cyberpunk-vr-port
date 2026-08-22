// AdsEyeAlign -- the game's aim-down-sights pose is centred between the eyes, and you look through one.
//
// Ported from dabinn's TofuExpress (73bdf668, "feat(aiming): align non-VRIK and Head Aim ADS poses
// with the right eye").
//
// THE PROBLEM. The authored ADS animation puts the sight in front of the CAMERA, which in this port is
// the cyclopean point between the eyes. On a flat screen that is the screen centre and it is correct.
// In VR the player sights with one eye, half an IPD to the side, so the sight sits beside the line of
// sight rather than on it -- and the closer the sight is to the face, the larger the miss. Nothing
// about the weapon's own pose is wrong; it is aligned to a viewpoint nobody is looking from.
//
// THE FIX, and it is a re-anchoring rather than a correction: take the whole authored arm pose and
// move it so that its camera-relative geometry hangs off the RIGHT EYE instead of the centre. The
// pose keeps its shape -- grip, wrist, elbow bend, the animation's own motion -- and only the frame
// it is expressed in changes.
//
//   THE PIVOT IS THE CYCLOPEAN POINT, NOT THE EYE. Hand and elbow positions are taken relative to the
//   camera (the centre), rotated by the live head delta, and re-anchored at the eye. Pre-shifting the
//   source by half an IPD as well would cancel exactly the dominant-eye correction this exists for.
//
//   THE GAMEPLAY ORIGIN IS FIXED IN THE RECENTER/BODY FRAME. Rendered head translation remains full
//   6DoF, but it does not feed back into weapon/arm targets. Head aim consumes only the final HMD
//   orientation, so physical leaning cannot introduce a second pivot or make the weapon chase the
//   headset. The right-eye offset is fixed in the same centred frame.
//
//   HEAD AIM ALWAYS ROTATES, non-VRIK does not. Under head aim the weapon follows the HMD in hip and
//   ADS states; the right-eye toggle only selects the ADS destination origin. In non-VRIK hand aim
//   the game still owns the direction, so only the opt-in right-eye translation is applied.
//
// THE CENTRED CAMERA FRAME IS RECOVERED BY DIVIDING OUT THE HMD, not by reading the engine camera:
// the render view is composed as gameHeading * mappedHmd, and the engine's camera orientation may
// already contain the HMD. The raw head orientation published beside the view packet is the exact
// factor to remove, so this holds even when the player is looking away from their body.
//
// AND THE GRIP IS NEVER OVERWRITTEN. Where a weapon rotation has to be applied, it is applied to the
// hand and the authored WeaponRight local transform is left alone:
//
//     desiredWeaponModel = desiredHandModel * weaponLocal
//         =>  desiredHandModel = desiredWeaponModel * inverse(weaponLocal)
//
// Writing the weapon bone directly would have replaced the authored grip with whatever rotation the
// aim wanted, which is how a pistol ends up held sideways.
//
// TWO DEPARTURES FROM THE ORIGINAL. His repeat-pass cache is keyed on SharedPose(13) -- a component
// of the right controller quaternion, which happens to change most frames; here it is keyed on the
// entity tick, the quantity that actually means "a new pose". And the arm solve is rotation-only in
// both versions, which is deliberate: writing a hand TRANSLATION would stretch the wrist off the
// forearm.

#include "Anim/AdsEyeAlign.hpp"

#include "Anim/AdsMuzzleStabilizer.hpp"
#include "Anim/CharacterRig.hpp"
#include "Anim/HeadAimWeapon.hpp"
#include "Anim/VrikHook.hpp"
#include "Anim/VrikState.hpp"
#include "Core/VrCoreShared.hpp"   // g_isAiming
#include "Utils/SharedSlots.hpp"

#include <cmath>
#include <cstdint>

extern float* g_pSharedHands;

namespace cvr::anim {

namespace {

// One entry per pose buffer the hook visits in a tick.
struct AimArmPose {
    uint8_t* boneBuf = nullptr;
    float tick = -1.0f;
    int bone[6] = {-1, -1, -1, -1, -1, -1};   // right upper/fore/hand, left upper/fore/hand
    int clavicle[2] = {-1, -1};
    float localRot[6][4] = {};
    float clavicleLocalRot[2][4] = {};
    float rawPos[6][3] = {};
    float rawRot[6][4] = {};
    float targetHand[2][3] = {};
    float targetElbow[2][3] = {};
    float targetLeftRot[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    bool headAim = false;
    bool valid = false;
};
AimArmPose g_aimArmPose[4];

float AimDist3(const float* a, const float* b) {
    const float x = a[0] - b[0];
    const float y = a[1] - b[1];
    const float z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

// Build the Head Aim gameplay frame. The render camera remains fully 6DoF, but weapon/arm aim
// deliberately ignores base-relative HMD translation: the centred camera plus its fixed view
// offsets is the head origin, and the recenter-time right-eye offset stays fixed in that body frame.
// Only the final HMD orientation is live. This is equivalent to a virtual body that follows every
// physical head translation, without actually moving the player entity or collision capsule.
bool CurrentFixedAimFrame(float* outHeadCentreModel, float* outRightEyeModel,
                          float* outViewModel, float* outCentreModelRot) {
    float camModelPos[3];
    float unusedCamRot[4];
    if (!g_viewPktValid || !VRIK_ComputeCamModel(camModelPos, unusedCamRot)) return false;

    float viewQ[4] = { g_viewPkt[0], g_viewPkt[1], g_viewPkt[2], g_viewPkt[3] };
    VRIK_QuatNorm(viewQ);

    float entQ[4] = { g_VREntityQI, g_VREntityQJ, g_VREntityQK, g_VREntityQR };
    VRIK_QuatNorm(entQ);
    float invEnt[4];
    VRIK_QuatConj(entQ, invEnt);
    VRIK_QuatMul(invEnt, viewQ, outViewModel);
    VRIK_QuatNorm(outViewModel);

    // Divide out the exact raw HMD quaternion published beside this view -- axis-mapped the same way
    // the compose maps it -- rather than comparing against the engine camera, which may already carry
    // the HMD. What remains is the centred game-camera frame the authored weapon pose was made in.
    float hmdGame[4] = { g_viewPkt[13], -g_viewPkt[15], g_viewPkt[14], g_viewPkt[16] };
    if ((hmdGame[0] * hmdGame[0] + hmdGame[1] * hmdGame[1] +
         hmdGame[2] * hmdGame[2] + hmdGame[3] * hmdGame[3]) < 1e-6f) {
        hmdGame[0] = 0.0f; hmdGame[1] = 0.0f; hmdGame[2] = 0.0f; hmdGame[3] = 1.0f;
    } else {
        VRIK_QuatNorm(hmdGame);
    }
    float invHmdGame[4];
    VRIK_QuatConj(hmdGame, invHmdGame);
    float centreWorld[4];
    VRIK_QuatMul(viewQ, invHmdGame, centreWorld);
    VRIK_QuatMul(invEnt, centreWorld, outCentreModelRot);
    VRIK_QuatNorm(outCentreModelRot);

    // [120..122] is the fixed Tracking-Camera + camera-bake + eye-bake offset in centred
    // game-camera axes. It belongs to the recenter origin; unlike [108..110], it contains no live
    // HMD position. Rotate it only by the centred camera frame, never by the live HMD orientation.
    float fixedViewLocal[3] = {0.0f, 0.0f, 0.0f};
    if (SharedPose(123) == 1.0f) {
        fixedViewLocal[0] = SharedPose(120);
        fixedViewLocal[1] = SharedPose(121);
        fixedViewLocal[2] = SharedPose(122);
    }
    float fixedViewModel[3];
    VRIK_QuatRotateVec(outCentreModelRot, fixedViewLocal, fixedViewModel);
    for (int k = 0; k < 3; ++k) outHeadCentreModel[k] = camModelPos[k] + fixedViewModel[k];

    // Keep the recenter-time right eye fixed in the body frame. Rotating this offset by the live
    // HMD quaternion would reintroduce an IPD-radius positional orbit and another pivot problem.
    const float rightEyeLocal[3] = {SharedPose(95), 0.0f, 0.0f};
    float rightEyeOffsetModel[3];
    VRIK_QuatRotateVec(outCentreModelRot, rightEyeLocal, rightEyeOffsetModel);
    for (int k = 0; k < 3; ++k) {
        outRightEyeModel[k] = outHeadCentreModel[k] + rightEyeOffsetModel[k];
    }
    return true;
}

// Move the upper-arm socket only when the authored shoulder cannot reach the Head Aim target. The
// socket rides the end of the clavicle at a fixed radius, so rotate that bone by the minimum
// required angle instead of translating or stretching the shoulder girdle.
void AssistAimShoulder(uint8_t* boneBuf, int upperIdx, int foreIdx, int handIdx,
                       const float* targetHand) {
    if (upperIdx < 0 || foreIdx < 0 || handIdx < 0) return;
    const int clavicleIdx = g_VRBoneParent[upperIdx];
    if (clavicleIdx < 0 || clavicleIdx >= VRIK_FKCount()) return;
    const int clavicleParent = g_VRBoneParent[clavicleIdx];

    const float upperLen = AimDist3(g_fkPos[upperIdx], g_fkPos[foreIdx]);
    const float foreLen = AimDist3(g_fkPos[foreIdx], g_fkPos[handIdx]);
    const float armReach = upperLen + foreLen - 1e-4f;
    if (upperLen < 1e-4f || foreLen < 1e-4f || armReach < 1e-4f) return;
    if (AimDist3(g_fkPos[upperIdx], targetHand) <= armReach) return;

    float shoulderFromPivot[3] = {g_fkPos[upperIdx][0] - g_fkPos[clavicleIdx][0],
                                  g_fkPos[upperIdx][1] - g_fkPos[clavicleIdx][1],
                                  g_fkPos[upperIdx][2] - g_fkPos[clavicleIdx][2]};
    float targetFromPivot[3] = {targetHand[0] - g_fkPos[clavicleIdx][0],
                                targetHand[1] - g_fkPos[clavicleIdx][1],
                                targetHand[2] - g_fkPos[clavicleIdx][2]};
    const float clavicleLen = VRIK_Norm3(shoulderFromPivot);
    const float targetDist = VRIK_Norm3(targetFromPivot);
    if (clavicleLen < 1e-4f || targetDist < 1e-4f) return;

    float cosReach = (clavicleLen * clavicleLen + targetDist * targetDist - armReach * armReach) /
                     (2.0f * clavicleLen * targetDist);
    if (cosReach < -1.0f) cosReach = -1.0f;
    if (cosReach > 1.0f) cosReach = 1.0f;
    const float maxReachSeparation = std::acos(cosReach);
    float cosCurrent = VRIK_Dot3(shoulderFromPivot, targetFromPivot);
    if (cosCurrent < -1.0f) cosCurrent = -1.0f;
    if (cosCurrent > 1.0f) cosCurrent = 1.0f;
    const float currentSeparation = std::acos(cosCurrent);
    float required = currentSeparation - maxReachSeparation;
    if (required <= 1e-4f || currentSeparation <= 1e-4f) return;

    constexpr float kMaxAimClavicleAssist = 0.610865238f; // 35 degrees
    if (required > kMaxAimClavicleAssist) required = kMaxAimClavicleAssist;
    float fullDelta[4];
    VRIK_QuatFromTo(shoulderFromPivot, targetFromPivot, fullDelta);
    float assistDelta[4];
    VRIK_QuatScale(fullDelta, required / currentSeparation, assistDelta);
    float newClavicleModel[4];
    VRIK_QuatMul(assistDelta, g_fkRot[clavicleIdx], newClavicleModel);
    VRIK_QuatNorm(newClavicleModel);
    const float identity[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    VRIK_WriteLocalRot(
        boneBuf, clavicleIdx,
        (clavicleParent >= 0 && clavicleParent < VRIK_FKCount()) ? g_fkRot[clavicleParent] : identity,
        newClavicleModel);
    VRIK_ComputeFK(boneBuf, VRIK_FKCount());
}

// Rotation-only two-bone solve for an authored arm pose. Segment translations are untouched, so this
// cannot stretch the wrist away from the forearm the way writing the hand's position would.
void SolveAimArm(uint8_t* boneBuf, int upperIdx, int foreIdx, int handIdx,
                 const float rawPos[3][3], const float rawRot[3][4],
                 const float* targetHand, const float* elbowHint, const float* targetHandRot) {
    float upVec[3] = { rawPos[1][0] - rawPos[0][0], rawPos[1][1] - rawPos[0][1],
                       rawPos[1][2] - rawPos[0][2] };
    float foreVec[3] = { rawPos[2][0] - rawPos[1][0], rawPos[2][1] - rawPos[1][1],
                         rawPos[2][2] - rawPos[1][2] };
    const float upLen = VRIK_Norm3(upVec), foreLen = VRIK_Norm3(foreVec);
    float toHand[3] = { targetHand[0] - rawPos[0][0], targetHand[1] - rawPos[0][1],
                        targetHand[2] - rawPos[0][2] };
    const float dist = VRIK_Norm3(toHand);
    if (upLen < 1e-4f || foreLen < 1e-4f || dist < 1e-4f) return;

    const float minD = std::fabs(upLen - foreLen) + 1e-4f, maxD = upLen + foreLen - 1e-4f;
    float reach = dist;
    if (reach < minD) reach = minD;
    if (reach > maxD) reach = maxD;
    const float along = (upLen * upLen - foreLen * foreLen + reach * reach) / (2.0f * reach);
    const float height = std::sqrt(std::fmax(0.0f, upLen * upLen - along * along));
    const float linePoint[3] = { rawPos[0][0] + toHand[0] * along,
                                 rawPos[0][1] + toHand[1] * along,
                                 rawPos[0][2] + toHand[2] * along };
    float bend[3] = { elbowHint[0] - linePoint[0], elbowHint[1] - linePoint[1],
                      elbowHint[2] - linePoint[2] };
    float proj = VRIK_Dot3(bend, toHand);
    bend[0] -= toHand[0] * proj; bend[1] -= toHand[1] * proj; bend[2] -= toHand[2] * proj;
    if (VRIK_Norm3(bend) < 1e-4f) {
        // The hint collapsed onto the shoulder-to-hand line: fall back to the authored elbow, which
        // is the pose's own bend direction.
        bend[0] = rawPos[1][0] - linePoint[0];
        bend[1] = rawPos[1][1] - linePoint[1];
        bend[2] = rawPos[1][2] - linePoint[2];
        proj = VRIK_Dot3(bend, toHand);
        bend[0] -= toHand[0] * proj; bend[1] -= toHand[1] * proj; bend[2] -= toHand[2] * proj;
        if (VRIK_Norm3(bend) < 1e-4f) return;
    }
    const float newElbow[3] = { linePoint[0] + bend[0] * height,
                                linePoint[1] + bend[1] * height,
                                linePoint[2] + bend[2] * height };
    float desiredUp[3] = { newElbow[0] - rawPos[0][0], newElbow[1] - rawPos[0][1],
                           newElbow[2] - rawPos[0][2] };
    VRIK_Norm3(desiredUp);
    float desiredFore[3] = { targetHand[0] - newElbow[0], targetHand[1] - newElbow[1],
                             targetHand[2] - newElbow[2] };
    VRIK_Norm3(desiredFore);

    float dUp[4], newUp[4];
    VRIK_QuatFromTo(upVec, desiredUp, dUp);
    VRIK_QuatMul(dUp, rawRot[0], newUp);
    VRIK_QuatNorm(newUp);
    float dFore[4], newFore[4];
    VRIK_QuatFromTo(foreVec, desiredFore, dFore);
    VRIK_QuatMul(dFore, rawRot[1], newFore);
    VRIK_QuatNorm(newFore);

    const int upParent = g_VRBoneParent[upperIdx];
    const float identity[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    VRIK_WriteLocalRot(boneBuf, upperIdx,
                       (upParent >= 0 && upParent < VRIK_FKCount()) ? g_fkRot[upParent] : identity,
                       newUp);
    VRIK_WriteLocalRot(boneBuf, foreIdx, newUp, newFore);
    VRIK_WriteLocalRot(boneBuf, handIdx, newFore, targetHandRot);
}

}  // namespace

bool WriteWeaponModelRotViaRightHand(uint8_t* boneBuf, int weaponIdx,
                                     const float* desiredWeaponModel,
                                     const float* weaponLocalOverride) {
    const int handIdx = static_cast<int>(g_VRRightBoneIdx);
    if (weaponIdx < 0 || weaponIdx >= VRIK_FKCount() ||
        handIdx < 0 || handIdx >= VRIK_FKCount() ||
        g_VRBoneParent[weaponIdx] != handIdx) {
        return false;
    }
    const int handParent = g_VRBoneParent[handIdx];
    if (handParent < 0 || handParent >= handIdx) return false;

    float weaponLocal[4];
    if (weaponLocalOverride) {
        weaponLocal[0] = weaponLocalOverride[0]; weaponLocal[1] = weaponLocalOverride[1];
        weaponLocal[2] = weaponLocalOverride[2]; weaponLocal[3] = weaponLocalOverride[3];
    } else {
        const float* local =
            reinterpret_cast<const float*>(boneBuf + weaponIdx * 48 + VRIK_ROT_OFF);
        weaponLocal[0] = local[0]; weaponLocal[1] = local[1];
        weaponLocal[2] = local[2]; weaponLocal[3] = local[3];
    }
    VRIK_QuatNorm(weaponLocal);

    float invWeaponLocal[4];
    VRIK_QuatConj(weaponLocal, invWeaponLocal);
    float desiredHandModel[4];
    VRIK_QuatMul(desiredWeaponModel, invWeaponLocal, desiredHandModel);
    VRIK_QuatNorm(desiredHandModel);
    VRIK_WriteLocalRot(boneBuf, handIdx, g_fkRot[handParent], desiredHandModel);
    return true;
}

void PrepareAimArmTargets(uint8_t* boneBuf) {
    const bool headAim = IsHeadAimWeaponActive();
    const bool nonVrik = g_pSharedHands && g_VRBind <= 0 && CyberpunkVR_NonVrikAdsStabilizer &&
                         g_pSharedHands[vrshared::kWeaponFlag] > 0.5f;
    const bool aiming = g_isAiming;
    const bool alignmentEnabled = g_pSharedHands &&
                                  g_pSharedHands[vrshared::kAdsRightEyeAlignment] > 0.5f;
    const bool headEyeAlignment = headAim && aiming && alignmentEnabled;
    const bool nonVrikEyeAlignment = nonVrik && aiming && alignmentEnabled;
    const bool active = headAim || nonVrikEyeAlignment;

    AimArmPose* pose = nullptr;
    for (auto& entry : g_aimArmPose) {
        entry.valid = false;
        if (entry.boneBuf == boneBuf) { pose = &entry; break; }
        if (!pose && entry.boneBuf == nullptr) pose = &entry;
    }
    if (!active || !pose) return;
    if (pose->boneBuf != boneBuf) { pose->boneBuf = boneBuf; pose->tick = -1.0f; }

    const int bone[6] = { g_VRRightUpperArmIdx, g_VRRightForeArmIdx, g_VRRightBoneIdx,
                          g_VRLeftUpperArmIdx,  g_VRLeftForeArmIdx,  g_VRLeftBoneIdx };
    for (int i = 0; i < 6; ++i) if (bone[i] < 0 || bone[i] >= VRIK_FKCount()) return;
    const int clavicle[2] = {g_VRBoneParent[bone[0]], g_VRBoneParent[bone[3]]};

    // THE TICK, not a controller quaternion component. The hook visits this buffer several times per
    // tick: the first pass records the authored rotations, the repeats restore them, so every pass
    // re-anchors the same original pose instead of re-anchoring its own output.
    const float tick = g_pSharedHands[vrshared::kEntitySeq];
    bool samePose = (pose->tick == tick);
    for (int i = 0; i < 6; ++i) samePose = samePose && pose->bone[i] == bone[i];
    for (int side = 0; side < 2; ++side) {
        samePose = samePose && pose->clavicle[side] == clavicle[side];
    }
    if (samePose) {
        for (int side = 0; side < 2; ++side) {
            if (clavicle[side] < 0 || clavicle[side] >= VRIK_FKCount()) continue;
            float* q = reinterpret_cast<float*>(boneBuf + clavicle[side] * 48 + VRIK_ROT_OFF);
            q[0] = pose->clavicleLocalRot[side][0]; q[1] = pose->clavicleLocalRot[side][1];
            q[2] = pose->clavicleLocalRot[side][2]; q[3] = pose->clavicleLocalRot[side][3];
        }
        for (int i = 0; i < 6; ++i) {
            float* q = reinterpret_cast<float*>(boneBuf + bone[i] * 48 + VRIK_ROT_OFF);
            q[0] = pose->localRot[i][0]; q[1] = pose->localRot[i][1];
            q[2] = pose->localRot[i][2]; q[3] = pose->localRot[i][3];
        }
    } else {
        for (int side = 0; side < 2; ++side) {
            pose->clavicle[side] = clavicle[side];
            if (clavicle[side] < 0 || clavicle[side] >= VRIK_FKCount()) continue;
            const float* q =
                reinterpret_cast<const float*>(boneBuf + clavicle[side] * 48 + VRIK_ROT_OFF);
            pose->clavicleLocalRot[side][0] = q[0]; pose->clavicleLocalRot[side][1] = q[1];
            pose->clavicleLocalRot[side][2] = q[2]; pose->clavicleLocalRot[side][3] = q[3];
        }
        for (int i = 0; i < 6; ++i) {
            const float* q =
                reinterpret_cast<const float*>(boneBuf + bone[i] * 48 + VRIK_ROT_OFF);
            pose->bone[i] = bone[i];
            pose->localRot[i][0] = q[0]; pose->localRot[i][1] = q[1];
            pose->localRot[i][2] = q[2]; pose->localRot[i][3] = q[3];
        }
        pose->tick = tick;
    }

    VRIK_ComputeFK(boneBuf, VRIK_FKCount());
    for (int i = 0; i < 6; ++i) {
        for (int k = 0; k < 3; ++k) pose->rawPos[i][k] = g_fkPos[bone[i]][k];
        for (int k = 0; k < 4; ++k) pose->rawRot[i][k] = g_fkRot[bone[i]][k];
    }

    float fixedHeadCentre[3], fixedRightEye[3], viewModel[4], centreRot[4];
    if (!CurrentFixedAimFrame(fixedHeadCentre, fixedRightEye, viewModel, centreRot)) return;
    float invCentre[4];
    VRIK_QuatConj(centreRot, invCentre);
    float liveDelta[4];
    VRIK_QuatMul(viewModel, invCentre, liveDelta);
    VRIK_QuatNorm(liveDelta);

    float rotationPivot[3];
    float postRotationTranslation[3] = {0.0f, 0.0f, 0.0f};
    float delta[4];
    for (int k = 0; k < 3; ++k) rotationPivot[k] = fixedHeadCentre[k];
    if (headEyeAlignment || nonVrikEyeAlignment) {
        for (int k = 0; k < 3; ++k) {
            postRotationTranslation[k] = fixedRightEye[k] - fixedHeadCentre[k];
        }
    }
    if (headAim) {
        for (int k = 0; k < 4; ++k) delta[k] = liveDelta[k];
    } else if (nonVrikEyeAlignment) {
        delta[0] = 0.0f; delta[1] = 0.0f; delta[2] = 0.0f; delta[3] = 1.0f;
    } else {
        return;
    }

    const int handSlot[2] = {2, 5}, elbowSlot[2] = {1, 4};
    for (int side = 0; side < 2; ++side) {
        const float relH[3] = {pose->rawPos[handSlot[side]][0] - rotationPivot[0],
                               pose->rawPos[handSlot[side]][1] - rotationPivot[1],
                               pose->rawPos[handSlot[side]][2] - rotationPivot[2]};
        const float relE[3] = {pose->rawPos[elbowSlot[side]][0] - rotationPivot[0],
                               pose->rawPos[elbowSlot[side]][1] - rotationPivot[1],
                               pose->rawPos[elbowSlot[side]][2] - rotationPivot[2]};
        float rotH[3], rotE[3];
        VRIK_QuatRotateVec(delta, relH, rotH);
        VRIK_QuatRotateVec(delta, relE, rotE);
        for (int k = 0; k < 3; ++k) {
            pose->targetHand[side][k] =
                rotationPivot[k] + rotH[k] + postRotationTranslation[k];
            pose->targetElbow[side][k] =
                rotationPivot[k] + rotE[k] + postRotationTranslation[k];
        }
    }
    VRIK_QuatMul(delta, pose->rawRot[5], pose->targetLeftRot);
    VRIK_QuatNorm(pose->targetLeftRot);
    pose->headAim = headAim;
    pose->valid = true;
}

void SolvePreparedAimArms(uint8_t* boneBuf) {
    AimArmPose* pose = nullptr;
    for (auto& entry : g_aimArmPose) {
        if (entry.boneBuf == boneBuf) { pose = &entry; break; }
    }
    if (!pose || !pose->valid) return;

    // The RIGHT hand keeps whatever rotation the weapon writer just gave it -- head aim's view
    // rotation, or the stabilizer's correction -- so it is read back from the FK rather than
    // re-derived here. The left hand takes the rotated authored grip.
    VRIK_ComputeFK(boneBuf, VRIK_FKCount());
    const float rightHandRot[4] = { g_fkRot[pose->bone[2]][0], g_fkRot[pose->bone[2]][1],
                                    g_fkRot[pose->bone[2]][2], g_fkRot[pose->bone[2]][3] };
    if (pose->headAim) {
        AssistAimShoulder(boneBuf, pose->bone[0], pose->bone[1], pose->bone[2],
                          pose->targetHand[0]);
    }
    float rightPos[3][3], rightRot[3][4];
    for (int joint = 0; joint < 3; ++joint) {
        for (int k = 0; k < 3; ++k) rightPos[joint][k] = g_fkPos[pose->bone[joint]][k];
        for (int k = 0; k < 4; ++k) rightRot[joint][k] = g_fkRot[pose->bone[joint]][k];
    }
    SolveAimArm(boneBuf, pose->bone[0], pose->bone[1], pose->bone[2],
                rightPos, rightRot,
                pose->targetHand[0], pose->targetElbow[0], rightHandRot);
    VRIK_ComputeFK(boneBuf, VRIK_FKCount());
    if (pose->headAim) {
        AssistAimShoulder(boneBuf, pose->bone[3], pose->bone[4], pose->bone[5],
                          pose->targetHand[1]);
    }
    float leftPos[3][3], leftRot[3][4];
    for (int joint = 0; joint < 3; ++joint) {
        const int boneIdx = pose->bone[joint + 3];
        for (int k = 0; k < 3; ++k) leftPos[joint][k] = g_fkPos[boneIdx][k];
        for (int k = 0; k < 4; ++k) leftRot[joint][k] = g_fkRot[boneIdx][k];
    }
    SolveAimArm(boneBuf, pose->bone[3], pose->bone[4], pose->bone[5],
                leftPos, leftRot,
                pose->targetHand[1], pose->targetElbow[1], pose->targetLeftRot);
}

}  // namespace cvr::anim
