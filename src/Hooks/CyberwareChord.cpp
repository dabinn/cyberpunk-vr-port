#include "Hooks/CyberwareChord.hpp"
#include "Hooks/KeypadInput.hpp"
#include "Camera/CameraState.hpp"
#include "Core/VrCoreShared.hpp"
#include "Core/LiveControls.hpp"
#include "Overlay/VrOverlay.hpp"
#include "Utils/DebugGate.hpp"
#include <atomic>
#include <windows.h>

extern "C" __declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_CyberwareChordSent{};
namespace {
std::atomic<uint64_t> pendingUntil{};
bool Allowed() {
    return g_liveControls.xrXInputHook!=0 && g_menuModeValue==0 && !cvr::vrui::CapturesInput() && !cvr::input::KeypadInputActive() &&
           !DeviceCamActive() && !g_bdActive.load(std::memory_order_relaxed) &&
           g_sceneTier.load(std::memory_order_relaxed)<=1;
}
}
bool cvr::input::RequestCyberwareChord() {
    if(!Allowed())return false;
    const auto now=GetTickCount64();
    pendingUntil.store(now+200,std::memory_order_release);
    return true;
}
void cvr::input::DispatchCyberwareChord() {
    const auto until=pendingUntil.exchange(0,std::memory_order_acq_rel);
    if(!until || GetTickCount64()>=until || !Allowed())return;
    DWORD process{};GetWindowThreadProcessId(GetForegroundWindow(),&process);
    if(process!=GetCurrentProcessId())return;
    // Input Loader appends F18 to IconicCyberware_Button, with no UI override:
    // game context/cooldown still decides whether the ability may activate.
    // Unlike E this private key survives a user's keyboard rebinding; unlike
    // LB+RB it cannot leak the shoulders' scheme-specific actions.
    INPUT input[2]{};input[0].type=INPUT_KEYBOARD;input[0].ki.wVk=VK_F18;
    input[1]=input[0];input[1].ki.dwFlags=KEYEVENTF_KEYUP;
    const auto sent=SendInput(2,input,sizeof(INPUT));
    if(sent==1)SendInput(1,input+1,sizeof(INPUT)); // release after a partial OS insertion
    if(sent==2)CVR_DIAGNOSTIC(++CyberpunkVR_CyberwareChordSent);
}
