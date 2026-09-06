#pragma once

// The non-VRIK ADS muzzle stabilizer -- see src/Anim/AdsMuzzleStabilizer.cpp for what it corrects and
// why the correction is a closed loop rather than an override.

#include <cstdint>

namespace cvr::anim {

// Runs inside the player's pose apply, after the engine's animation has written the pose and before
// anything of ours replaces it. Safe to call every pass: it caches the tick's raw rotation and
// composes from that, so repeated passes cannot accumulate.
void ApplyNonVrikAdsMuzzleStabilizer(uint8_t* boneBuf);

// Update the mode/target lifecycle once per pose tick, before any orientation owner writes.
void UpdateAdsBallisticCorrection();
// Apply the finite-distance ADS correction about the RightHand/wrist pivot. The desired weapon
// rotation is supplied in model space; weaponLocalOverride preserves the tick's authored grip.
void ApplyAdsBallisticCorrectionToWeapon(uint8_t* boneBuf, int weaponIdx,
                                         const float* wristModelPos, float* weaponModelRot,
                                         const float* weaponLocalOverride = nullptr);
// Apply the same geometry at a path's final wrist target, before that arm solver writes the wrist.
// Used by full VRIK and Head Aim, whose final wrist positions are not the raw animated FK wrist.
void ApplyWristTargetAdsBallisticCorrection(uint8_t* boneBuf, const float* wristTargetModel,
                                            float* handModelRot);

}  // namespace cvr::anim

// 1 = correct the drift the vanilla aim-in animation adds to the muzzle direction (default).
extern "C" __declspec(dllexport) extern int32_t  CyberpunkVR_NonVrikAdsStabilizer;
extern "C" __declspec(dllexport) extern uint64_t CyberpunkVR_DebugAdsStabApplies;
extern "C" __declspec(dllexport) extern uint64_t CyberpunkVR_DebugAdsStabHipCaptures;
