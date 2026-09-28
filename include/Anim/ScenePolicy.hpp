#pragma once
#include <RED4ext/Scripting/Natives/Generated/GameplayTier.hpp>

namespace cvr::anim {
inline bool IsControllerVrikEnabled(int bindingMode,bool headAimActive) {
    return bindingMode==4 && !headAimActive;
}

// Keep the stored UI values: -1/0 disable, 1..4 select Tier2..Tier5.
// The engine enum is one-based and must not be compared to the UI index.
inline bool ShouldSuspendVrik(int engineTier,int configuredTier) {
    if(configuredTier<1 || configuredTier>4)return false;
    const int minimum=static_cast<int>(RED4ext::GameplayTier::Tier1_FullGameplay)+configuredTier;
    return engineTier>=minimum && engineTier<=static_cast<int>(RED4ext::GameplayTier::Tier5_Cinematic);
}
inline bool ShouldSuspendVrik(int engineTier,bool inVehicle,int onFootTier,int vehicleTier) {
    return ShouldSuspendVrik(engineTier,inVehicle ? vehicleTier : onFootTier);
}
inline int ResolveVehicleVrikSuspendTier(int onFootTier,int vehicleTier,bool explicitlyConfigured) {
    const int tier=explicitlyConfigured ? vehicleTier : onFootTier;
    return tier < -1 ? -1 : (tier > 4 ? 4 : tier);
}
}
