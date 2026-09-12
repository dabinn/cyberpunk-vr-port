#pragma once

// Moving the authored ADS arm pose from the cyclopean camera onto the eye the player actually sights
// with. See src/Anim/AdsEyeAlign.cpp for the geometry and for why the pivot is not the eye.

#include <cstdint>

namespace cvr::anim {

struct WeaponShoulderSideDiag {
    bool valid = false;
    float targetHand[3] = {};
    float finalHand[3] = {};
    float rawClavicle[3] = {};
    float constrainedClavicle[3] = {};
    float rawUpperArm[3] = {};
    float constrainedUpperArm[3] = {};
    float handPositionError = 0.0f;
    float handRotationErrorDeg = 0.0f;
    float rawClavicleDeltaDeg = 0.0f;
    float constrainedClavicleDeltaDeg = 0.0f;
    float rawProtractionDeg = 0.0f;
    float constrainedProtractionDeg = 0.0f;
    float rawElevationDeg = 0.0f;
    float constrainedElevationDeg = 0.0f;
    float rawTwistDeg = 0.0f;
    float constrainedTwistDeg = 0.0f;
    float referenceLocalRot[4] = {0.0f, 0.0f, 0.0f, 1.0f};
};

struct WeaponShoulderConstraintDiag {
    bool valid = false;
    WeaponShoulderSideDiag right{};
    WeaponShoulderSideDiag left{};
};

// Apply a desired WEAPON model rotation without touching the authored grip: the rotation goes onto
// the weapon's parent (RightHand) and WeaponRight keeps its own local transform. Pass
// weaponLocalOverride when the caller has the tick's RAW local rotation cached, so repeated passes
// compose from the same source. False when the rig is not the expected hand -> weapon chain.
bool WriteWeaponModelRotViaRightHand(uint8_t* boneBuf, int weaponIdx,
                                     const float* desiredWeaponModel,
                                     const float* weaponLocalOverride = nullptr);

// Record the authored arm pose for this tick and compute eye-anchored hand/elbow targets from it.
// A no-op unless the player is aiming under head aim or non-VRIK hand aim.
void PrepareAimArmTargets(uint8_t* boneBuf);

// Rotation-only two-bone solve onto the targets PrepareAimArmTargets computed. Runs after the weapon
// writer, because the right hand's rotation is read back from the pose it left.
void SolvePreparedAimArms(uint8_t* boneBuf);

// Last sample from the experimental non-VRIK weapon-shoulder A/B. Diagnostics only; the returned
// data never participates in the solve.
bool GetWeaponShoulderConstraintDiag(WeaponShoulderConstraintDiag& out);

}  // namespace cvr::anim
