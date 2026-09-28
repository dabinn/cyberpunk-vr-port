#include "Natives/NativeFunctions.hpp"
#include "Hooks/Reflex.hpp"
#include "Core/LiveControls.hpp"
#include "Core/VrCoreShared.hpp"
#include <RED4ext/Scripting/Natives/entEntity.hpp>
#include <cstring>

// Diagnostic A/B only. Returning true retains the original Lua scan path.
extern "C" __declspec(dllexport) int CyberpunkVR_BodyCapsulePrefilter=1;

void VRShouldScanBodyCapsules(RED4ext::IScriptable*,RED4ext::CStackFrame* frame,bool* out,int64_t) {
    RED4ext::Handle<RED4ext::ent::Entity> entity;
    RED4ext::GetParameter(frame,&entity);++frame->code;
    if(!out)return;
    *out=true;
    if(!CyberpunkVR_BodyCapsulePrefilter)return;
    if(!entity){*out=false;return;}
    if(g_menuModeValue || entity->status!=RED4ext::EntityStatus::Attached)return;
    // CET invokes this on the game thread. Codeware's GetComponents returns a
    // copy of this same Entity::components array. Read it in place: no nested
    // script invocation, array allocation, ownership transfer or retained cache.
    // Loading/unattached entities retain the original Lua path above.
    for(const auto& component:entity->components)if(component) {
        const char* name=component->name.ToString();
        if(name && std::strstr(name,"VRPortBody_"))return;
    }
    *out=false;
}

void VRReflexTiming(RED4ext::IScriptable*,RED4ext::CStackFrame* frame,RED4ext::CString* out,int64_t) {
    bool request=false;RED4ext::GetParameter(frame,&request);++frame->code;
    if(out)*out=RED4ext::CString(cvr::reflex::TimingReport(request).c_str());
}
void GetVRMouseYDisabled(RED4ext::IScriptable*,RED4ext::CStackFrame* frame,bool* out,int64_t) {
    ++frame->code;if(out)*out=g_liveControls.xrDisableMouseY!=0;
}
