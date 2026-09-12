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
//   THE ANCHOR IS LATCHED, the live IPD orbit is not. The eye offset is captured in the vanilla
//   camera's frame when ADS begins, so walking, crouching and vehicle motion still carry the arms
//   with the player, while PHYSICAL head translation afterwards stays free -- that is how a player
//   fine-adjusts a sight, by moving their head, and it must not drag the weapon along.
//
//   HEAD AIM ALSO ROTATES, non-VRIK does not. Under head aim the weapon follows the HMD, so the arms
//   must orbit the eye with it (delta = the live rotation since the anchor). In non-VRIK hand aim the
//   game still owns the direction, so only the translation is re-anchored (delta = identity).
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
#include "Camera/CameraState.hpp"
#include "Core/LiveControls.hpp"
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
    int shoulderBone[2] = {-1, -1};             // right/left clavicle-like shoulder bones
    float localRot[6][4] = {};
    float shoulderLocalPos[2][3] = {};
    float shoulderLocalRot[2][4] = {};
    float rawPos[6][3] = {};
    float rawRot[6][4] = {};
    float targetHand[2][3] = {};
    float targetElbow[2][3] = {};
    float targetLeftRot[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    bool solveRight = false;
    bool solveLeft = false;
    bool shoulderConstraint = false;
    bool valid = false;
};
AimArmPose g_aimArmPose[4];
WeaponShoulderConstraintDiag g_weaponShoulderConstraintDiag{};

// Build the Head Aim gameplay frame. The render camera remains fully 6DoF, but weapon/arm aim
// deliberately ignores base-relative HMD translation: the centred camera plus its fixed view
// offsets is the head origin. The final HMD orientation stays live; Head Aim ADS uses that live
// orientation again below when placing the dominant-eye offset around this fixed head centre.
bool CurrentFixedAimFrame(float* outHeadCentreModel, float* outRightEyeModel,
                          float* outViewModel, float* outCentreModelRot) {
    float camModelPos[3];
    float unusedCamRot[4];
    float camModelEntityQuat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float pairedCamModelRot[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    if (!g_viewPktValid) return false;
    if (!VRIK_ComputeCamModel(
            camModelPos, unusedCamRot, camModelEntityQuat, pairedCamModelRot)) return false;

    // With HeadTranslationInPatch the camera component already contains the room-scale head
    // displacement, so VRIK_ComputeCamModel() necessarily carries it into camModelPos. Head Aim's
    // gameplay pivot must stay on the body/recenter frame: remove the same displacement here using
    // the exact entity basis paired with camModelPos, matching the body-anchor correction in
    // Hooked_AnimPoseApply().
    if (CyberpunkVR_HeadTranslationInPatch &&
        g_headDeltaValid.load(std::memory_order_acquire)) {
        VRIK_QuatNorm(camModelEntityQuat);
        float invCamEntity[4];
        VRIK_QuatConj(camModelEntityQuat, invCamEntity);
        const float k = 1.0f / 131072.0f;
        const float headDeltaWorld[3] = {
            g_headDeltaFP[0].load(std::memory_order_relaxed) * k,
            g_headDeltaFP[1].load(std::memory_order_relaxed) * k,
            g_headDeltaFP[2].load(std::memory_order_relaxed) * k };
        float headDeltaModel[3];
        VRIK_QuatRotateVec(invCamEntity, headDeltaWorld, headDeltaModel);
        for (int i = 0; i < 3; ++i) camModelPos[i] -= headDeltaModel[i];
    }

    // In ordinary gameplay PatchCamera applies MAIN's +/-half-IPD directly to the camera component
    // before SerializeSetup fills the located-camera buffer. VRIK's coherent camera/entity pair is
    // therefore an EYE position even though LocateCamera later labels that same buffer as the head
    // centre. Undo the exact paired-eye lever here so Head Aim rotates about the cyclopean gameplay
    // centre. Use the paired camera rotation, not the fresh HMD rotation: it is the orientation that
    // produced the camera position consumed above.
    if (CyberpunkVR_IpdInWorldPos) {
        const float mainEyeLocal[3] = {
            (CyberpunkVR_MainIsRightEye ? 1.0f : -1.0f) * GetDesiredHalfIpd(), 0.0f, 0.0f };
        float mainEyeModel[3];
        VRIK_QuatRotateVec(pairedCamModelRot, mainEyeLocal, mainEyeModel);
        for (int i = 0; i < 3; ++i) camModelPos[i] -= mainEyeModel[i];
    }

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

    // [120..122] is the fixed Tracking-Camera + camera-bake/vehicle offset in centred game-camera
    // axes. It belongs to the recenter origin; unlike [108..110], it contains no live
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

struct ShoulderAngles {
    float protraction = 0.0f;
    float elevation = 0.0f;
    float twist = 0.0f;
    float total = 0.0f;
};

float ClampShoulderAngle(float value, float lo, float hi) {
    return value < lo ? lo : (value > hi ? hi : value);
}

void AxisAngleQuat(const float* axis, float angle, float* out) {
    const float s = std::sin(angle * 0.5f);
    out[0] = axis[0] * s;
    out[1] = axis[1] * s;
    out[2] = axis[2] * s;
    out[3] = std::cos(angle * 0.5f);
}

void NlerpQuatShortest(const float* a, const float* b, float t, float* out) {
    const float dot = a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3];
    const float sign = dot < 0.0f ? -1.0f : 1.0f;
    for (int k = 0; k < 4; ++k) out[k] = a[k] * (1.0f - t) + b[k] * sign * t;
    VRIK_QuatNorm(out);
}

bool MeasureShoulderLocal(bool isLeft, const float* localRot, ShoulderAngles& out,
                          float* outRestAxisModel = nullptr,
                          float* outParentRestModelRot = nullptr) {
    const volatile int valid = isLeft ? g_VRLeftShoulderRestValid : g_VRRightShoulderRestValid;
    if (!valid) return false;
    const float* restSrc = isLeft ? g_VRLeftClavicleRestRot : g_VRRightClavicleRestRot;
    const float* upperRest = isLeft ? g_VRLeftUpperArmRestPos : g_VRRightUpperArmRestPos;
    const float* parentSrc = isLeft ? g_VRLeftShoulderParentRestModelRot
                                    : g_VRRightShoulderParentRestModelRot;

    float rest[4] = {restSrc[0], restSrc[1], restSrc[2], restSrc[3]};
    float cur[4] = {localRot[0], localRot[1], localRot[2], localRot[3]};
    float parentRest[4] = {parentSrc[0], parentSrc[1], parentSrc[2], parentSrc[3]};
    VRIK_QuatNorm(rest);
    VRIK_QuatNorm(cur);
    VRIK_QuatNorm(parentRest);

    float invRest[4];
    VRIK_QuatConj(rest, invRest);
    float deltaParent[4];
    VRIK_QuatMul(cur, invRest, deltaParent);
    VRIK_QuatNorm(deltaParent);

    float invParentRest[4];
    VRIK_QuatConj(parentRest, invParentRest);
    float tmp[4], deltaModel[4];
    VRIK_QuatMul(parentRest, deltaParent, tmp);
    VRIK_QuatMul(tmp, invParentRest, deltaModel);
    VRIK_QuatNorm(deltaModel);
    if (deltaModel[3] < 0.0f) {
        for (float& v : deltaModel) v = -v;
    }

    float axisLocal[3] = {upperRest[0], upperRest[1], upperRest[2]};
    if (VRIK_Norm3(axisLocal) < 1e-5f) return false;
    float axisParent[3];
    VRIK_QuatRotateVec(rest, axisLocal, axisParent);
    float restAxisModel[3];
    VRIK_QuatRotateVec(parentRest, axisParent, restAxisModel);
    if (VRIK_Norm3(restAxisModel) < 1e-5f) return false;
    float currentAxisModel[3];
    VRIK_QuatRotateVec(deltaModel, restAxisModel, currentAxisModel);
    if (VRIK_Norm3(currentAxisModel) < 1e-5f) return false;

    const float restZ = ClampShoulderAngle(restAxisModel[2], -1.0f, 1.0f);
    const float curZ = ClampShoulderAngle(currentAxisModel[2], -1.0f, 1.0f);
    out.elevation = std::asin(curZ) - std::asin(restZ);

    float restH[3] = {restAxisModel[0], restAxisModel[1], 0.0f};
    float curH[3] = {currentAxisModel[0], currentAxisModel[1], 0.0f};
    if (VRIK_Norm3(restH) < 1e-5f || VRIK_Norm3(curH) < 1e-5f) return false;
    float crossH[3];
    VRIK_Cross3(restH, curH, crossH);
    float dotH = ClampShoulderAngle(VRIK_Dot3(restH, curH), -1.0f, 1.0f);
    const float azimuth = std::atan2(crossH[2], dotH);
    const float sideSign = restAxisModel[0] >= 0.0f ? 1.0f : -1.0f;
    out.protraction = azimuth * sideSign;

    float swing[4];
    VRIK_QuatFromTo(restAxisModel, currentAxisModel, swing);
    float invSwing[4];
    VRIK_QuatConj(swing, invSwing);
    float twist[4];
    VRIK_QuatMul(invSwing, deltaModel, twist);
    VRIK_QuatNorm(twist);
    if (twist[3] < 0.0f) {
        for (float& v : twist) v = -v;
    }
    const float twistProj = twist[0]*restAxisModel[0] + twist[1]*restAxisModel[1]
                          + twist[2]*restAxisModel[2];
    out.twist = 2.0f * std::atan2(twistProj, twist[3]);

    const float xyz = std::sqrt(deltaModel[0]*deltaModel[0] + deltaModel[1]*deltaModel[1]
                              + deltaModel[2]*deltaModel[2]);
    out.total = 2.0f * std::atan2(xyz, deltaModel[3]);

    if (outRestAxisModel) {
        for (int k = 0; k < 3; ++k) outRestAxisModel[k] = restAxisModel[k];
    }
    if (outParentRestModelRot) {
        for (int k = 0; k < 4; ++k) outParentRestModelRot[k] = parentRest[k];
    }
    return true;
}

bool ApplyShoulderConstraint(uint8_t* boneBuf, bool isLeft,
                             const float* targetHand, float minArmReach, float maxArmReach,
                             ShoulderAngles& rawAngles, ShoulderAngles& constrainedAngles) {
    if (!boneBuf) return false;
    const int clavicle = isLeft ? g_VRLeftClavicleIdx : g_VRRightClavicleIdx;
    const int upper = isLeft ? g_VRLeftUpperArmIdx : g_VRRightUpperArmIdx;
    if (clavicle < 0 || upper < 0 ||
        clavicle >= VRIK_FKCount() || upper >= VRIK_FKCount()) return false;

    float* localRot = reinterpret_cast<float*>(boneBuf + clavicle * 48 + VRIK_ROT_OFF);
    float* localPos = reinterpret_cast<float*>(boneBuf + clavicle * 48 + VRIK_TRANS_OFF);
    float rawLocal[4] = {localRot[0], localRot[1], localRot[2], localRot[3]};
    const float rawPos[3] = {localPos[0], localPos[1], localPos[2]};
    VRIK_QuatNorm(rawLocal);
    float restAxisModel[3], parentRest[4];
    if (!MeasureShoulderLocal(isLeft, localRot, rawAngles, restAxisModel, parentRest)) return false;

    constexpr float kDeg = 0.01745329251994329577f;
    const float protraction = ClampShoulderAngle(rawAngles.protraction, -20.0f*kDeg, 20.0f*kDeg);
    const float elevation = ClampShoulderAngle(rawAngles.elevation, -8.0f*kDeg, 30.0f*kDeg);
    // The rig-specific sign of posterior clavicle roll has not been established yet, so keep this
    // axis symmetric until telemetry from real weapon poses tells us which sign deserves more range.
    const float twist = ClampShoulderAngle(rawAngles.twist, -25.0f*kDeg, 25.0f*kDeg);

    const float sideSign = restAxisModel[0] >= 0.0f ? 1.0f : -1.0f;
    const float up[3] = {0.0f, 0.0f, 1.0f};
    float qPro[4];
    AxisAngleQuat(up, protraction * sideSign, qPro);
    float proDir[3];
    VRIK_QuatRotateVec(qPro, restAxisModel, proDir);
    float horizontal[3] = {proDir[0], proDir[1], 0.0f};
    if (VRIK_Norm3(horizontal) < 1e-5f) return false;
    const float restElev = std::asin(ClampShoulderAngle(restAxisModel[2], -1.0f, 1.0f));
    const float targetElev = restElev + elevation;
    const float ce = std::cos(targetElev), se = std::sin(targetElev);
    float targetAxis[3] = {horizontal[0]*ce, horizontal[1]*ce, se};
    VRIK_Norm3(targetAxis);

    float swing[4];
    VRIK_QuatFromTo(restAxisModel, targetAxis, swing);
    float twistQ[4];
    AxisAngleQuat(restAxisModel, twist, twistQ);
    float constrainedDeltaModel[4];
    VRIK_QuatMul(swing, twistQ, constrainedDeltaModel);
    VRIK_QuatNorm(constrainedDeltaModel);

    float invParentRest[4];
    VRIK_QuatConj(parentRest, invParentRest);
    float tmp[4], constrainedDeltaParent[4];
    VRIK_QuatMul(invParentRest, constrainedDeltaModel, tmp);
    VRIK_QuatMul(tmp, parentRest, constrainedDeltaParent);
    VRIK_QuatNorm(constrainedDeltaParent);

    const float* restSrc = isLeft ? g_VRLeftClavicleRestRot : g_VRRightClavicleRestRot;
    const float* restPos = isLeft ? g_VRLeftClavicleRestPos : g_VRRightClavicleRestPos;
    float rest[4] = {restSrc[0], restSrc[1], restSrc[2], restSrc[3]};
    VRIK_QuatNorm(rest);
    VRIK_QuatMul(constrainedDeltaParent, rest, localRot);
    VRIK_QuatNorm(localRot);

    // FPP weapon/melee animations are allowed to translate the clavicle socket by many centimetres.
    // That is useful for a flat-screen composition but impossible for a human shoulder girdle. Keep
    // the socket on its authored reference position; the reach fallback below may release only the
    // amount required for the fixed-length arm to reach the original hand target.
    localPos[0] = restPos[0];
    localPos[1] = restPos[1];
    localPos[2] = restPos[2];

    // A strict clavicle limit can move the upper-arm root far enough that a fixed-length arm can no
    // longer reach the authored support grip. In that case relax only as much as required for the
    // original hand target to become reachable again. The authored pose at t=1 is the guaranteed
    // fallback, so this never needs to stretch the arm or move the weapon target.
    if (targetHand && minArmReach >= 0.0f && maxArmReach > minArmReach) {
        float hardLocal[4] = {localRot[0], localRot[1], localRot[2], localRot[3]};
        auto setBlendAndMeasureReach = [&](float t) {
            float blended[4];
            NlerpQuatShortest(hardLocal, rawLocal, t, blended);
            localRot[0] = blended[0]; localRot[1] = blended[1];
            localRot[2] = blended[2]; localRot[3] = blended[3];
            localPos[0] = restPos[0] + (rawPos[0] - restPos[0]) * t;
            localPos[1] = restPos[1] + (rawPos[1] - restPos[1]) * t;
            localPos[2] = restPos[2] + (rawPos[2] - restPos[2]) * t;
            VRIK_ComputeFK(boneBuf, VRIK_FKCount());
            float toTarget[3] = {targetHand[0] - g_fkPos[upper][0],
                                 targetHand[1] - g_fkPos[upper][1],
                                 targetHand[2] - g_fkPos[upper][2]};
            return VRIK_Norm3(toTarget);
        };
        auto reachable = [&](float distance) {
            return distance >= minArmReach && distance <= maxArmReach;
        };

        const float hardDistance = setBlendAndMeasureReach(0.0f);
        if (!reachable(hardDistance)) {
            constexpr int kScanSteps = 32;
            float low = 0.0f, high = 1.0f;
            bool found = false;
            for (int i = 1; i <= kScanSteps; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(kScanSteps);
                if (reachable(setBlendAndMeasureReach(t))) {
                    low = static_cast<float>(i - 1) / static_cast<float>(kScanSteps);
                    high = t;
                    found = true;
                    break;
                }
            }
            if (found) {
                for (int i = 0; i < 10; ++i) {
                    const float mid = (low + high) * 0.5f;
                    if (reachable(setBlendAndMeasureReach(mid))) high = mid;
                    else low = mid;
                }
                setBlendAndMeasureReach(high);
            } else {
                setBlendAndMeasureReach(1.0f);
            }
        }
    }
    return MeasureShoulderLocal(isLeft, localRot, constrainedAngles);
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
    const bool nonVrikWeapon = g_pSharedHands && g_VRBind <= 0 &&
                               g_pSharedHands[vrshared::kWeaponFlag] > 0.5f;
    const bool nonVrik = nonVrikWeapon && CyberpunkVR_NonVrikAdsStabilizer;
    const bool shoulderTest = nonVrikWeapon && !headAim &&
                              g_liveControls.xrWeaponShoulderConstraintTest != 0;
    if (!shoulderTest) g_weaponShoulderConstraintDiag.valid = false;
    const bool aiming = g_isAiming;
    const bool alignmentEnabled = g_pSharedHands &&
                                  g_pSharedHands[vrshared::kAdsRightEyeAlignment] > 0.5f;
    const bool headEyeAlignment = headAim && aiming && alignmentEnabled;
    const bool nonVrikEyeAlignment = nonVrik && aiming && alignmentEnabled;
    const bool active = headAim || nonVrikEyeAlignment || shoulderTest;

    AimArmPose* pose = nullptr;
    for (auto& entry : g_aimArmPose) {
        entry.valid = false;
        entry.solveRight = false;
        entry.solveLeft = false;
        entry.shoulderConstraint = false;
        if (entry.boneBuf == boneBuf) { pose = &entry; break; }
        if (!pose && entry.boneBuf == nullptr) pose = &entry;
    }
    if (!active || !pose) return;
    if (pose->boneBuf != boneBuf) { pose->boneBuf = boneBuf; pose->tick = -1.0f; }

    const int bone[6] = { g_VRRightUpperArmIdx, g_VRRightForeArmIdx, g_VRRightBoneIdx,
                          g_VRLeftUpperArmIdx,  g_VRLeftForeArmIdx,  g_VRLeftBoneIdx };
    const int shoulderBone[2] = { g_VRRightClavicleIdx, g_VRLeftClavicleIdx };
    for (int i = 0; i < 6; ++i) if (bone[i] < 0 || bone[i] >= VRIK_FKCount()) return;

    // THE TICK, not a controller quaternion component. The hook visits this buffer several times per
    // tick: the first pass records the authored rotations, the repeats restore them, so every pass
    // re-anchors the same original pose instead of re-anchoring its own output.
    const float tick = g_pSharedHands[vrshared::kEntitySeq];
    bool samePose = (pose->tick == tick);
    for (int i = 0; i < 6; ++i) samePose = samePose && pose->bone[i] == bone[i];
    for (int side = 0; side < 2; ++side) {
        samePose = samePose && pose->shoulderBone[side] == shoulderBone[side];
    }
    if (samePose) {
        for (int i = 0; i < 6; ++i) {
            float* q = reinterpret_cast<float*>(boneBuf + bone[i] * 48 + VRIK_ROT_OFF);
            q[0] = pose->localRot[i][0]; q[1] = pose->localRot[i][1];
            q[2] = pose->localRot[i][2]; q[3] = pose->localRot[i][3];
        }
        for (int side = 0; side < 2; ++side) {
            const int idx = shoulderBone[side];
            if (idx < 0 || idx >= VRIK_FKCount()) continue;
            float* p = reinterpret_cast<float*>(boneBuf + idx * 48 + VRIK_TRANS_OFF);
            p[0] = pose->shoulderLocalPos[side][0];
            p[1] = pose->shoulderLocalPos[side][1];
            p[2] = pose->shoulderLocalPos[side][2];
            float* q = reinterpret_cast<float*>(boneBuf + idx * 48 + VRIK_ROT_OFF);
            q[0] = pose->shoulderLocalRot[side][0];
            q[1] = pose->shoulderLocalRot[side][1];
            q[2] = pose->shoulderLocalRot[side][2];
            q[3] = pose->shoulderLocalRot[side][3];
        }
    } else {
        for (int i = 0; i < 6; ++i) {
            const float* q =
                reinterpret_cast<const float*>(boneBuf + bone[i] * 48 + VRIK_ROT_OFF);
            pose->bone[i] = bone[i];
            pose->localRot[i][0] = q[0]; pose->localRot[i][1] = q[1];
            pose->localRot[i][2] = q[2]; pose->localRot[i][3] = q[3];
        }
        for (int side = 0; side < 2; ++side) {
            const int idx = shoulderBone[side];
            pose->shoulderBone[side] = idx;
            if (idx < 0 || idx >= VRIK_FKCount()) continue;
            const float* p = reinterpret_cast<const float*>(boneBuf + idx * 48 + VRIK_TRANS_OFF);
            pose->shoulderLocalPos[side][0] = p[0];
            pose->shoulderLocalPos[side][1] = p[1];
            pose->shoulderLocalPos[side][2] = p[2];
            const float* q =
                reinterpret_cast<const float*>(boneBuf + idx * 48 + VRIK_ROT_OFF);
            pose->shoulderLocalRot[side][0] = q[0];
            pose->shoulderLocalRot[side][1] = q[1];
            pose->shoulderLocalRot[side][2] = q[2];
            pose->shoulderLocalRot[side][3] = q[3];
        }
        pose->tick = tick;
    }

    VRIK_ComputeFK(boneBuf, VRIK_FKCount());
    for (int i = 0; i < 6; ++i) {
        for (int k = 0; k < 3; ++k) pose->rawPos[i][k] = g_fkPos[bone[i]][k];
        for (int k = 0; k < 4; ++k) pose->rawRot[i][k] = g_fkRot[bone[i]][k];
    }

    pose->shoulderConstraint = shoulderTest;
    if (shoulderTest && !headAim && !nonVrikEyeAlignment) {
        for (int side = 0; side < 2; ++side) {
            const int base = side * 3;
            for (int k = 0; k < 3; ++k) {
                pose->targetHand[side][k] = pose->rawPos[base + 2][k];
                pose->targetElbow[side][k] = pose->rawPos[base + 1][k];
            }
        }
        for (int k = 0; k < 4; ++k) pose->targetLeftRot[k] = pose->rawRot[5][k];
        pose->solveRight = true;
        pose->solveLeft = true;
        pose->valid = true;
        return;
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
    if (headEyeAlignment) {
        // The gameplay head centre stays fixed so physical leaning remains an eye-box adjustment,
        // but the eye is not fixed relative to the body when the head rotates. Rotate only the
        // half-IPD lever by the live view orientation: this keeps the ADS sight in front of the
        // right eye both when entering ADS at an arbitrary head angle and while turning in ADS,
        // without restoring the much larger synthetic neck-pivot orbit.
        const float rightEyeLocal[3] = {SharedPose(95), 0.0f, 0.0f};
        VRIK_QuatRotateVec(viewModel, rightEyeLocal, postRotationTranslation);
    } else if (nonVrikEyeAlignment) {
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
    pose->solveRight = true;
    pose->solveLeft = true;
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
    float rightHandRot[4] = { g_fkRot[pose->bone[2]][0], g_fkRot[pose->bone[2]][1],
                              g_fkRot[pose->bone[2]][2], g_fkRot[pose->bone[2]][3] };
    // Head Aim rotates the authored arm around the head centre, so targetHand[0] is the wrist
    // position this solve will actually realize. Apply finite-distance ballistics here, after that
    // target exists, rather than in the earlier weapon writer that only knows the pre-reanchor wrist.
    if (IsHeadAimWeaponActive()) {
        ApplyWristTargetAdsBallisticCorrection(boneBuf, pose->targetHand[0], rightHandRot);
    }
    if (pose->shoulderConstraint) {
        g_weaponShoulderConstraintDiag.valid = false;
        g_weaponShoulderConstraintDiag.right.valid = false;
        g_weaponShoulderConstraintDiag.left.valid = false;
    }

    auto solveSide = [&](int side, const float* targetHandRot) {
        const bool isLeft = side != 0;
        const int base = side * 3;
        const int clavicle = isLeft ? g_VRLeftClavicleIdx : g_VRRightClavicleIdx;
        const float (*solvePos)[3] = &pose->rawPos[base];
        const float (*solveRot)[4] = &pose->rawRot[base];
        float constrainedPos[3][3] = {};
        float constrainedRot[3][4] = {};
        ShoulderAngles rawAngles{}, constrainedAngles{};
        bool constraintApplied = false;
        WeaponShoulderSideDiag* diag =
            isLeft ? &g_weaponShoulderConstraintDiag.left : &g_weaponShoulderConstraintDiag.right;

        if (pose->shoulderConstraint && clavicle >= 0 && clavicle < VRIK_FKCount()) {
            VRIK_ComputeFK(boneBuf, VRIK_FKCount());
            for (int k = 0; k < 3; ++k) {
                diag->targetHand[k] = pose->targetHand[side][k];
                diag->rawUpperArm[k] = pose->rawPos[base][k];
                diag->rawClavicle[k] = g_fkPos[clavicle][k];
            }
            const float* rest = isLeft ? g_VRLeftClavicleRestRot : g_VRRightClavicleRestRot;
            for (int k = 0; k < 4; ++k) diag->referenceLocalRot[k] = rest[k];
            float upperVec[3] = {pose->rawPos[base + 1][0] - pose->rawPos[base][0],
                                 pose->rawPos[base + 1][1] - pose->rawPos[base][1],
                                 pose->rawPos[base + 1][2] - pose->rawPos[base][2]};
            float foreVec[3] = {pose->rawPos[base + 2][0] - pose->rawPos[base + 1][0],
                                pose->rawPos[base + 2][1] - pose->rawPos[base + 1][1],
                                pose->rawPos[base + 2][2] - pose->rawPos[base + 1][2]};
            const float upperLen = VRIK_Norm3(upperVec);
            const float foreLen = VRIK_Norm3(foreVec);
            const float minReach = std::fabs(upperLen - foreLen) + 1e-4f;
            const float maxReach = upperLen + foreLen - 1e-4f;
            constraintApplied = ApplyShoulderConstraint(
                boneBuf, isLeft, pose->targetHand[side], minReach, maxReach,
                rawAngles, constrainedAngles);
        }

        if (constraintApplied) {
            constexpr float kRadToDeg = 57.29577951308232f;
            diag->rawClavicleDeltaDeg = rawAngles.total * kRadToDeg;
            diag->constrainedClavicleDeltaDeg = constrainedAngles.total * kRadToDeg;
            diag->rawProtractionDeg = rawAngles.protraction * kRadToDeg;
            diag->constrainedProtractionDeg = constrainedAngles.protraction * kRadToDeg;
            diag->rawElevationDeg = rawAngles.elevation * kRadToDeg;
            diag->constrainedElevationDeg = constrainedAngles.elevation * kRadToDeg;
            diag->rawTwistDeg = rawAngles.twist * kRadToDeg;
            diag->constrainedTwistDeg = constrainedAngles.twist * kRadToDeg;

            VRIK_ComputeFK(boneBuf, VRIK_FKCount());
            for (int i = 0; i < 3; ++i) {
                for (int k = 0; k < 3; ++k) constrainedPos[i][k] = g_fkPos[pose->bone[base + i]][k];
                for (int k = 0; k < 4; ++k) constrainedRot[i][k] = g_fkRot[pose->bone[base + i]][k];
            }
            for (int k = 0; k < 3; ++k) {
                diag->constrainedUpperArm[k] = constrainedPos[0][k];
                diag->constrainedClavicle[k] = g_fkPos[clavicle][k];
            }
            solvePos = constrainedPos;
            solveRot = constrainedRot;
        }

        SolveAimArm(boneBuf, pose->bone[base], pose->bone[base + 1], pose->bone[base + 2],
                    solvePos, solveRot, pose->targetHand[side], pose->targetElbow[side], targetHandRot);

        if (constraintApplied) {
            VRIK_ComputeFK(boneBuf, VRIK_FKCount());
            const float* finalHand = g_fkPos[pose->bone[base + 2]];
            float errSq = 0.0f;
            for (int k = 0; k < 3; ++k) {
                diag->finalHand[k] = finalHand[k];
                const float d = finalHand[k] - diag->targetHand[k];
                errSq += d * d;
            }
            diag->handPositionError = std::sqrt(errSq);

            float finalRot[4] = { g_fkRot[pose->bone[base + 2]][0], g_fkRot[pose->bone[base + 2]][1],
                                  g_fkRot[pose->bone[base + 2]][2], g_fkRot[pose->bone[base + 2]][3] };
            float targetRot[4] = {targetHandRot[0], targetHandRot[1], targetHandRot[2], targetHandRot[3]};
            VRIK_QuatNorm(finalRot);
            VRIK_QuatNorm(targetRot);
            float dot = std::fabs(finalRot[0]*targetRot[0] + finalRot[1]*targetRot[1]
                                + finalRot[2]*targetRot[2] + finalRot[3]*targetRot[3]);
            if (dot > 1.0f) dot = 1.0f;
            diag->handRotationErrorDeg = 2.0f * std::acos(dot) * 57.295779513f;
            diag->valid = true;
            g_weaponShoulderConstraintDiag.valid = true;
        }
    };

    if (pose->solveRight) solveSide(0, rightHandRot);
    if (pose->solveLeft) solveSide(1, pose->targetLeftRot);
}

bool GetWeaponShoulderConstraintDiag(WeaponShoulderConstraintDiag& out) {
    out = g_weaponShoulderConstraintDiag;
    return out.valid;
}

}  // namespace cvr::anim
