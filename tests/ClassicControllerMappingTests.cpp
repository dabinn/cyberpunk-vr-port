#include "Runtimes/OpenXRManager.hpp"
#include "Hooks/ClassicControllerMapping.hpp"

#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

bool NearByte(BYTE value, int expected) {
    return std::abs(static_cast<int>(value) - expected) <= 1;
}

void TestContextPriority() {
    using cvr::input::ClassicContext;
    using cvr::input::ResolveClassicContext;

    Check(ResolveClassicContext(false, false, false, true, false) == ClassicContext::GeneralOnFoot,
          "General Classic owns ordinary on-foot input");
    Check(ResolveClassicContext(false, false, false, false, true) == ClassicContext::None,
          "Vehicle Classic does not own on-foot input");
    Check(ResolveClassicContext(false, false, true, true, true) == ClassicContext::Vehicle,
          "Vehicle Classic owns mounted input");
    Check(ResolveClassicContext(false, false, true, true, false) == ClassicContext::None,
          "General Classic does not own mounted input");
    Check(ResolveClassicContext(true, false, true, true, true) == ClassicContext::None,
          "Upstream Scanner mode wins when Scanner Classic is off");
    Check(ResolveClassicContext(true, true, true, false, false) == ClassicContext::Scanner,
          "Scanner Classic independently owns Scanner input");
}

void TestDisabledLeavesUpstreamUntouched() {
    XINPUT_STATE output{};
    output.dwPacketNumber = 41;
    output.Gamepad.wButtons = XINPUT_GAMEPAD_Y;
    output.Gamepad.sThumbRY = -12345;
    const XINPUT_STATE original = output;

    XINPUT_STATE physical{};
    VRControllerState vr{};
    vr.buttons = XINPUT_GAMEPAD_A;
    vr.rightTrigger = 1.0f;
    cvr::input::ClassicComposeInput input{};

    cvr::input::ComposeClassicControllerState(output, physical, vr, input);
    Check(output.dwPacketNumber == original.dwPacketNumber
              && output.Gamepad.wButtons == original.Gamepad.wButtons
              && output.Gamepad.sThumbRY == original.Gamepad.sThumbRY,
          "Classic off leaves the upstream candidate untouched");
}

void TestCompleteBaseline() {
    XINPUT_STATE physical{};
    VRControllerState vr{};
    vr.buttons = XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN |
        XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT |
        XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK |
        XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB |
        XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y;
    vr.leftGrip = 0.8f;
    vr.rightGrip = 0.9f;
    vr.leftTrigger = 0.4f;
    vr.rightTrigger = 0.6f;

    cvr::input::ClassicComposeInput input{};
    input.context = cvr::input::ClassicContext::GeneralOnFoot;
    input.leftX = 0.25f;
    input.leftY = -0.50f;
    input.rightX = 0.75f;
    input.rightY = -1.0f;

    XINPUT_STATE output{};
    cvr::input::ComposeClassicControllerState(output, physical, vr, input);
    const uint16_t expectedButtons = static_cast<uint16_t>(vr.buttons |
        XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER);
    Check(output.Gamepad.wButtons == expectedButtons, "Classic baseline covers every Xbox button");
    Check(NearByte(output.Gamepad.bLeftTrigger, 102), "Left Trigger remains analog LT");
    Check(NearByte(output.Gamepad.bRightTrigger, 153), "Right Trigger remains analog RT");
    Check(output.Gamepad.sThumbLX > 8000, "Left X axis is preserved");
    Check(output.Gamepad.sThumbLY < -16000, "Left Y axis is preserved");
    Check(output.Gamepad.sThumbRX > 24000, "Right X axis is preserved");
    Check(output.Gamepad.sThumbRY == -32768, "Right Y reaches the full negative Xbox range");
}

void TestVehicleSwap() {
    XINPUT_STATE physical{};
    VRControllerState vr{};
    vr.leftTrigger = 0.8f;
    vr.rightTrigger = 0.9f;
    vr.leftGrip = 0.5f;
    vr.rightGrip = 0.75f;

    cvr::input::ClassicComposeInput input{};
    input.context = cvr::input::ClassicContext::Vehicle;
    input.swapTriggersAndGrips = true;

    XINPUT_STATE output{};
    cvr::input::ComposeClassicControllerState(output, physical, vr, input);
    Check((output.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0,
          "Vehicle swap maps Left Trigger to LB");
    Check((output.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0,
          "Vehicle swap maps Right Trigger to RB");
    Check(NearByte(output.Gamepad.bLeftTrigger, 127), "Vehicle swap maps Left Grip to LT");
    Check(NearByte(output.Gamepad.bRightTrigger, 191), "Vehicle swap maps Right Grip to RT");
}

void TestClaimsAndPhysicalPad() {
    XINPUT_STATE physical{};
    physical.Gamepad.wButtons = XINPUT_GAMEPAD_B;
    physical.Gamepad.bRightTrigger = 200;
    physical.Gamepad.sThumbLX = 20000;

    VRControllerState vr{};
    vr.leftGrip = 1.0f;
    vr.rightGrip = 1.0f;
    vr.leftTrigger = 1.0f;
    vr.rightTrigger = 1.0f;

    cvr::input::ClassicComposeInput input{};
    input.context = cvr::input::ClassicContext::GeneralOnFoot;
    input.leftX = 0.1f;
    input.claimLeftGrip = true;
    input.claimRightGrip = true;
    input.claimLeftTrigger = true;
    input.rightTriggerOverride = 1;
    input.extraButtons = XINPUT_GAMEPAD_LEFT_SHOULDER;

    XINPUT_STATE output{};
    cvr::input::ComposeClassicControllerState(output, physical, vr, input);
    Check((output.Gamepad.wButtons & XINPUT_GAMEPAD_B) != 0,
          "Spatial claims do not erase a physical pad button");
    Check((output.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0,
          "An explicit spatial output can replace a claimed Classic button");
    Check((output.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) == 0,
          "A spatial claim consumes the conflicting Classic grip button");
    Check(output.Gamepad.bLeftTrigger == 0, "A lighter claim consumes only the Classic LT contribution");
    Check(output.Gamepad.bRightTrigger == 200,
          "A reload claim preserves the physical pad while consuming VR RT");
    Check(output.Gamepad.sThumbLX == 20000, "Lower VR axis magnitude does not replace the physical pad");

    input.rightTriggerOverride = 2;
    cvr::input::ComposeClassicControllerState(output, physical, vr, input);
    Check(output.Gamepad.bRightTrigger == 255, "A spatial force claim can emit full RT");
}

void TestScannerBaselineDoesNotInheritOnFootGestures() {
    XINPUT_STATE physical{};
    VRControllerState vr{};
    cvr::input::ClassicComposeInput input{};
    input.context = cvr::input::ClassicContext::Scanner;
    input.extraButtons = XINPUT_GAMEPAD_LEFT_SHOULDER;

    XINPUT_STATE output{};
    cvr::input::ComposeClassicControllerState(output, physical, vr, input);
    Check(output.Gamepad.wButtons == XINPUT_GAMEPAD_LEFT_SHOULDER,
          "Scanner Classic keeps only its explicit spatial latch above the Xbox baseline");
}

} // namespace

int main() {
    TestContextPriority();
    TestDisabledLeavesUpstreamUntouched();
    TestCompleteBaseline();
    TestVehicleSwap();
    TestClaimsAndPhysicalPad();
    TestScannerBaselineDoesNotInheritOnFootGestures();
    if (failures != 0) return 1;
    std::puts("Classic controller mapping matrix passed");
    return 0;
}
