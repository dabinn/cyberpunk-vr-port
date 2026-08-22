#pragma once

// Moving the authored ADS arm pose from the cyclopean camera onto the eye the player actually sights
// with. See src/Anim/AdsEyeAlign.cpp for the geometry and for why the pivot is not the eye.

#include <cstdint>

namespace cvr::anim {

// Apply a desired WEAPON model rotation without touching the authored grip: the rotation goes onto
// the weapon's parent (RightHand) and WeaponRight keeps its own local transform. Pass
// weaponLocalOverride when the caller has the tick's RAW local rotation cached, so repeated passes
// compose from the same source. False when the rig is not the expected hand -> weapon chain.
bool WriteWeaponModelRotViaRightHand(uint8_t* boneBuf, int weaponIdx,
                                     const float* desiredWeaponModel,
                                     const float* weaponLocalOverride = nullptr);

// Record the authored arm pose for this tick and compute fixed-origin hand/elbow targets from it.
// Head Aim applies in hip and ADS states; non-VRIK applies only to opt-in right-eye ADS alignment.
void PrepareAimArmTargets(uint8_t* boneBuf);

// Rotation-only two-bone solve onto the targets PrepareAimArmTargets computed. Runs after the weapon
// writer, because the right hand's rotation is read back from the pose it left.
void SolvePreparedAimArms(uint8_t* boneBuf);

}  // namespace cvr::anim
