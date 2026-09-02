#include <RED4ext/RED4ext.hpp>
#include <RED4ext/Scripting/CProperty.hpp>

#include <fstream>

#include "Natives/NativeState.hpp"

namespace
{
const char* TypeName(RED4ext::rtti::IType* aType)
{
    if (!aType)
        return "<null>";

    const char* computed = aType->GetComputedName().ToString();
    if (computed && computed[0])
        return computed;

    const char* native = aType->GetName().ToString();
    return native ? native : "<unnamed>";
}

void DumpFunctionSignature(std::ofstream& aOut, RED4ext::CBaseFunction* aFunc, const char* aPrefix)
{
    if (!aFunc)
        return;

    aOut << "    " << aPrefix << " " << aFunc->fullName.ToString() << "(";
    for (uint32_t i = 0; i < aFunc->params.Size(); ++i)
    {
        if (i != 0)
            aOut << ", ";

        const auto* param = aFunc->params[i];
        if (!param)
        {
            aOut << "<null>";
            continue;
        }

        aOut << param->name.ToString() << ": " << TypeName(param->type);
    }
    aOut << ") -> ";
    if (aFunc->returnType && aFunc->returnType->type)
        aOut << TypeName(aFunc->returnType->type);
    else
        aOut << "Void";

    aOut << " [" << (aFunc->flags.isNative ? "native" : "script");
    if (aFunc->flags.isEvent)
        aOut << ", event";
    aOut << "]\n";
}

void DumpDeclaredFunctions(std::ofstream& aOut, RED4ext::CClass* aClass)
{
    if (!aClass)
        return;

    aOut << "  -- functions declared on " << aClass->name.ToString() << " --\n";
    for (auto* func : aClass->funcs)
        DumpFunctionSignature(aOut, func, "member");
    for (auto* func : aClass->staticFuncs)
        DumpFunctionSignature(aOut, func, "static");
}

void DumpCameraClass(std::ofstream& aOut, RED4ext::CRTTISystem* aRtti, const char* aClassName)
{
    auto* cls = aRtti->GetClass(aClassName);
    aOut << "\n==================================================\n";
    aOut << "CLASS " << aClassName << (cls ? "" : "   <NOT FOUND>") << "\n";
    if (!cls)
        return;

    aOut << "  size=0x" << std::hex << cls->GetSize() << std::dec << "\n";
    aOut << "  inheritance:";
    for (auto* cur = cls; cur; cur = cur->parent)
        aOut << " " << cur->name.ToString();
    aOut << "\n";

    aOut << "  -- properties visible on class (name : type @offset) --\n";
    RED4ext::DynArray<RED4ext::CProperty*> props;
    cls->GetProperties(props);
    for (auto* prop : props)
    {
        if (!prop)
            continue;

        aOut << "    +0x" << std::hex << prop->valueOffset << std::dec << "  "
             << prop->name.ToString() << " : " << TypeName(prop->type) << "\n";
    }

    for (auto* cur = cls; cur; cur = cur->parent)
        DumpDeclaredFunctions(aOut, cur);
}
}

void DumpVRCameraRtti(RED4ext::IScriptable* aContext, RED4ext::CStackFrame* aFrame, int32_t* aOut, int64_t a4)
{
    RED4EXT_UNUSED_PARAMETER(aContext);
    RED4EXT_UNUSED_PARAMETER(a4);
    aFrame->code++;

    auto* rtti = RED4ext::CRTTISystem::Get();
    if (!rtti)
    {
        if (aOut)
            *aOut = -1;
        return;
    }

    std::ofstream out(VRDiagPath("vr_camera_rtti.txt"), std::ios::trunc);
    if (!out)
    {
        if (aOut)
            *aOut = -2;
        return;
    }

    static constexpr const char* kCameraClasses[] = {
        "gameCameraSystem",
        "gameCameraComponent",
        "gameTPPCameraComponent",
        "vehicleTPPCameraComponent",
        "vehicleCameraManager",
        "gameWorldSpaceBlendCamera",
        "gameFreeCameraComponent",
        "entVirtualCameraComponent",
        "entRenderToTextureCameraComponent",
    };

    out << "VR camera RTTI inventory\n";
    out << "Read-only metadata dump: class hierarchy, properties, and member/static signatures.\n";
    for (const char* className : kCameraClasses)
        DumpCameraClass(out, rtti, className);

    out.close();
    if (aOut)
        *aOut = 1;
}
