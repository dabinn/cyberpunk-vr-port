#include "Hooks/ClassicControllerMapping.hpp"
#include "Runtimes/OpenXRManager.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cvr::input {
namespace {

SHORT FloatToShort(float value) {
    value = std::clamp(value, -1.0f, 1.0f);
    return value >= 0.0f
        ? static_cast<SHORT>(value * 32767.0f)
        : static_cast<SHORT>(value * 32768.0f);
}

BYTE FloatToByte(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    return static_cast<BYTE>(value * 255.0f);
}

void MergeAxis(SHORT& output, float vrValue) {
    const SHORT vrAxis = FloatToShort(vrValue);
    if (std::abs(static_cast<int>(vrAxis)) > std::abs(static_cast<int>(output))) {
        output = vrAxis;
    }
}

} // namespace

ClassicContext ResolveClassicContext(bool scannerActive,
                                     bool classicScanner,
                                     bool mounted,
                                     bool classicGeneral,
                                     bool classicVehicle) {
    if (scannerActive) {
        return classicScanner ? ClassicContext::Scanner : ClassicContext::None;
    }
    if (mounted) {
        return classicVehicle ? ClassicContext::Vehicle : ClassicContext::None;
    }
    return classicGeneral ? ClassicContext::GeneralOnFoot : ClassicContext::None;
}

bool IsClassicContext(ClassicContext context) {
    return context != ClassicContext::None;
}

void ComposeClassicControllerState(XINPUT_STATE& output,
                                   const XINPUT_STATE& physical,
                                   const VRControllerState& vr,
                                   const ClassicComposeInput& input) {
    if (!IsClassicContext(input.context)) return;

    output = physical;

    uint16_t buttons = static_cast<uint16_t>(vr.buttons &
        ~(XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER));
    buttons &= static_cast<uint16_t>(~input.claimedButtons);

    BYTE leftTrigger = 0;
    BYTE rightTrigger = 0;
    if (input.swapTriggersAndGrips) {
        if (!input.claimLeftTrigger && vr.leftTrigger >= 0.7f) {
            buttons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
        }
        if (input.rightTriggerOverride != 1 && vr.rightTrigger >= 0.7f) {
            buttons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        }
        if (!input.claimLeftGrip) leftTrigger = FloatToByte(vr.leftGrip);
        if (!input.claimRightGrip) rightTrigger = FloatToByte(vr.rightGrip);
    } else {
        if (!input.claimLeftGrip && vr.leftGrip >= 0.7f) {
            buttons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
        }
        if (!input.claimRightGrip && vr.rightGrip >= 0.7f) {
            buttons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        }
        if (!input.claimLeftTrigger) leftTrigger = FloatToByte(vr.leftTrigger);
        if (input.rightTriggerOverride == 2) {
            rightTrigger = 255;
        } else if (input.rightTriggerOverride == 0) {
            rightTrigger = FloatToByte(vr.rightTrigger);
        }
    }

    output.Gamepad.wButtons |= static_cast<uint16_t>(buttons | input.extraButtons);
    output.Gamepad.bLeftTrigger = std::max(output.Gamepad.bLeftTrigger, leftTrigger);
    output.Gamepad.bRightTrigger = std::max(output.Gamepad.bRightTrigger, rightTrigger);
    MergeAxis(output.Gamepad.sThumbLX, input.leftX);
    MergeAxis(output.Gamepad.sThumbLY, input.leftY);
    MergeAxis(output.Gamepad.sThumbRX, input.rightX);
    MergeAxis(output.Gamepad.sThumbRY, input.rightY);

    static bool initialized = false;
    static DWORD packet = 0;
    static XINPUT_GAMEPAD previous{};
    if (!initialized) {
        packet = physical.dwPacketNumber;
        initialized = true;
    } else if (std::memcmp(&previous, &output.Gamepad, sizeof(previous)) != 0) {
        ++packet;
    }
    previous = output.Gamepad;
    output.dwPacketNumber = packet;
}

} // namespace cvr::input
