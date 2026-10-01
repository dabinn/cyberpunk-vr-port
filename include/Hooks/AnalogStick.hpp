#pragma once
#include <algorithm>
#include <cmath>

namespace cvr::input {
inline float StickDeadzone(float value) {
    return std::isfinite(value) && value>=0 && value<=.30f ? value : .15f;
}
inline float StickFullInput(float value) {
    return std::isfinite(value) && value>=.80f && value<=1 ? value : .90f;
}
// TE6-testing-highlight's ApplyStickRange: independently remap each axis from
// its centre deadzone to the chosen full-travel point, preserving its sign.
inline float AnalogAxis(float value,float deadzone,float fullInput) {
    if(!std::isfinite(value))return 0;
    deadzone=StickDeadzone(deadzone);fullInput=StickFullInput(fullInput);
    const float magnitude=std::abs(value);
    if(magnitude<=deadzone)return 0;
    return std::copysign(std::min(1.f,(magnitude-deadzone)/(fullInput-deadzone)),value);
}
// This controls on-foot locomotion magnitude only. Stick response tuning is
// independent and still applies in fixed-speed and vehicle contexts.
inline bool UseAnalogMovement(int mode,bool vehicle) {return mode==1 && !vehicle;}
// Raw-travel callers use analog=true so the configured outer threshold remains
// independent of movement-speed policy. The false branch preserves the legacy
// 0.90 rule for any explicitly legacy caller.
inline bool AtFullTravel(float value,bool analog,float fullInput,bool negative=false) {
    if(!std::isfinite(value))return false;
    if(negative)value=-value;
    return analog?value>=StickFullInput(fullInput):value>.90f;
}
}
