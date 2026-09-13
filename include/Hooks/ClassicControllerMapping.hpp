#pragma once

#include <windows.h>
#include <Xinput.h>
#include <cstdint>

struct VRControllerState;

namespace cvr::input {

enum class ClassicContext : uint8_t {
    None,
    GeneralOnFoot,
    Vehicle,
    Scanner,
};

struct ClassicComposeInput {
    ClassicContext context = ClassicContext::None;
    float leftX = 0.0f;
    float leftY = 0.0f;
    float rightX = 0.0f;
    float rightY = 0.0f;
    bool swapTriggersAndGrips = false;
    bool claimLeftGrip = false;
    bool claimRightGrip = false;
    bool claimLeftTrigger = false;
    int rightTriggerOverride = 0; // 0 = pass, 1 = swallow, 2 = force
    uint16_t claimedButtons = 0;
    uint16_t extraButtons = 0;
};

ClassicContext ResolveClassicContext(bool scannerActive,
                                     bool classicScanner,
                                     bool mounted,
                                     bool classicGeneral,
                                     bool classicVehicle);

bool IsClassicContext(ClassicContext context);

void ComposeClassicControllerState(XINPUT_STATE& output,
                                   const XINPUT_STATE& physical,
                                   const VRControllerState& vr,
                                   const ClassicComposeInput& input);

} // namespace cvr::input
