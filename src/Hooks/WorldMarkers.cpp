#include "Utils/DebugGate.hpp"
// Native ink paint/primitive ownership, verified in world-markers-20260923.md.
#include "Stereo/WorldMarkers.hpp"
#include "Stereo/StereoInternal.hpp"
#include "Render/WorldMarkerProjection.hpp"
#include "Render/WorldMarkerScale.hpp"
#include "Render/WorldMarkerShader.hpp"
#include "Camera/CameraLink.hpp"
#include "Hooks/Hook.hpp"
#include "Hooks/Trampoline.hpp"
#include <RED4ext/RED4ext.hpp>
#include <RED4ext/Scripting/Functions.hpp>
#include <RED4ext/Scripting/Natives/inkWidget.hpp>
#include <RED4ext/Scripting/Natives/inkIWidgetLogicController.hpp>
#include <RED4ext/Scripting/Natives/Generated/ink/WidgetLogicController.hpp>
#include <RED4ext/Scripting/Natives/inkLayer.hpp>
#include <RED4ext/Scripting/Natives/inkLayerProxy.hpp>
#include <MinHook.h>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

extern void Log(const char*,...);
extern "C" float CyberpunkVRPort_HalfIpd();
extern "C" float GetGameRenderFovDeg();
extern "C" float CyberpunkVR_MainAdsZoomFactor;
extern "C" int CyberpunkVR_MainIsRightEye;
extern "C" int CyberpunkVR_StereoSubmit;
extern "C" void CvrWorldMarkerTextPublishDetour();
extern "C" {
__declspec(dllexport) std::atomic<int32_t> CyberpunkVR_WorldMarkerStereo{1};
__declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_WorldMarkerRoots{0};
__declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_WorldMarkerQuads{0};
__declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_WorldMarkerTexts{0};
__declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_WorldMarkerCameraUploads[2]{};
__declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_WorldMarkerTextUploads[2]{};
__declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_WorldMarkerMappinStages[7]{};
__declspec(dllexport) std::atomic<uint64_t> CyberpunkVR_WorldMarkerProjectionStages[5]{};
}

namespace cvr::markers {
namespace {
using namespace RED4ext;
const uintptr_t base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
std::atomic<bool> hooksReady{false};
bool Enabled() {
    return hooksReady.load(std::memory_order_acquire) && VertexShadersReady() &&
        CyberpunkVR_WorldMarkerStereo.load(std::memory_order_relaxed) && CyberpunkVR_StereoSubmit;
}
template<class T> T& At(void* p,size_t offset) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(p)+offset); }
struct RootDepth { WeakHandle<ink::Widget> root; float inverseDepth{}; uint64_t stamp{}; MarkerScale scale; };
std::shared_mutex rootsMutex;
std::unordered_map<ink::Widget*,RootDepth> roots;
thread_local float paintDepth=0;
thread_local float textDepth=0;

bool HudRoot(const Handle<ink::Widget>& root) {
    auto current=root;
    for(int i=0;current && i<24;++i) {
        const auto proxy=current->layerProxy;
        const auto layer=proxy ? proxy->layer.Lock() : Handle<ink::Layer>{};
        if(layer) return layer->GetType()->GetName()==CName("inkHUDLayer");
        current=current->parentWidget.Lock();
    }
    return false;
}
bool WorldMappinController(IScriptable* controller) {
    // The world container's game controller is NOT Widget::logicController:
    // the live Root/HUDMiddleWidget/window ancestor chain has no logic controller.
    // This call site is already MappinBaseController; classify the actual caller.
    auto type=controller->GetType();
    for(int i=0;type && i<64;++i,type=type->parent) {
        const auto name=type->GetName();
        if(name==CName("gameuiBaseMinimapMappinController") ||
           name==CName("gameuiBaseWorldMapMappinController")) return false;
        if(name==CName("gameuiMappinBaseController")) return true;
    }
    return false;
}
void Remember(const Handle<ink::Widget>& root,const float* world,bool valid=true,bool scaleMarker=false) {
    if(!root || !HudRoot(root)) return;
    camera::LocatedCameraFrame frame{};
    const float depth=valid && world && camera::LocatedCameraFrameRead(&frame)
        ? ForwardDepth(world,frame.worldPos,frame.worldQuat) : 0;
    const uint64_t now=GetTickCount64();
    Vector2 target{};bool resize=false;
    {
        std::unique_lock lock(rootsMutex);
        auto& entry=roots[root.GetPtr()];
        if(entry.root.Lock().GetPtr()!=root.GetPtr())entry={WeakHandle<ink::Widget>(root)};
        entry.inverseDepth=depth>0 ? 1/depth : 0;entry.stamp=now;
        if(scaleMarker)resize=entry.scale.Target(root->renderTransform.scale.X,root->renderTransform.scale.Y,target.X,target.Y);
        if(roots.size()>512) {
            for(auto it=roots.begin();it!=roots.end();) {
                // Retain scale ownership while a hidden marker is still alive;
                // expiring it by time alone would shrink it again on reappearance.
                if(now-it->second.stamp>2000 && !it->second.root.Lock())it=roots.erase(it);else ++it;
            }
        }
        CVR_DIAGNOSTIC(CyberpunkVR_WorldMarkerRoots.store(roots.size(),std::memory_order_relaxed));
    }
    if(resize) {
        auto setter=root->GetType()->GetFunction("SetScale");
        if(!setter || setter->params.Size()!=1 || setter->params[0]->type->GetName()!=CName("Vector2")) {
            static std::atomic<bool> warned{false};
            if(!warned.exchange(true))Log("[world-markers] SetScale(Vector2) unavailable; original marker size retained\n");
            return;
        }
        // The engine setter preserves the authored pivot and invalidates paint.
        // Only native world-mappin roots enter here, never map icons or chatters.
        StackArgs_t args;args.emplace_back(nullptr,&target);
        if(ExecuteFunction(root.GetPtr(),setter,nullptr,args)) {
            std::unique_lock lock(rootsMutex);
            auto it=roots.find(root.GetPtr());
            if(it!=roots.end() && it->second.root.Lock().GetPtr()==root.GetPtr())it->second.scale.Commit(target.X,target.Y);
        }
    }
}

using MappinPositionFn=uintptr_t (*)(void*,void*);
MappinPositionFn originalMappinPosition{};
uintptr_t MappinPosition(void* controller,void* position) {
    CVR_DIAGNOSTIC(CyberpunkVR_WorldMarkerMappinStages[0].fetch_add(1,std::memory_order_relaxed));
    const auto result=originalMappinPosition(controller,position);
    if(!Enabled()) return result;
    CVR_DIAGNOSTIC(CyberpunkVR_WorldMarkerMappinStages[1].fetch_add(1,std::memory_order_relaxed));
    // This setter belongs to MappinBaseController. GetMappin's native thunk
    // copies exactly its weak handle at A0; the root is the SDK's weak handle40.
    auto root=At<WeakHandle<ink::Widget>>(controller,0x40).Lock();
    if(!root) return result;
    CVR_DIAGNOSTIC(CyberpunkVR_WorldMarkerMappinStages[2].fetch_add(1,std::memory_order_relaxed));
    if(!WorldMappinController(static_cast<IScriptable*>(controller))) return result;
    CVR_DIAGNOSTIC(CyberpunkVR_WorldMarkerMappinStages[3].fetch_add(1,std::memory_order_relaxed));
    if(!HudRoot(root)) return result;
    CVR_DIAGNOSTIC(CyberpunkVR_WorldMarkerMappinStages[4].fetch_add(1,std::memory_order_relaxed));
    // Visibility can be assigned later in this same native update. Paint itself
    // gates hidden widgets; collecting the anchor must not depend on that order.
    const auto vt=*static_cast<uintptr_t**>(controller);
    const bool clamped=reinterpret_cast<bool (*)(void*)>(vt[0x158/8])(controller);
    auto mappin=At<WeakHandle<IScriptable>>(controller,0xA0).Lock();
    Vector4 world{};
    const auto getter=mappin ? mappin->GetType()->GetFunction("GetWorldPosition") : nullptr;
    const bool valid=At<uint8_t>(controller,0xE3) && !clamped && getter && ExecuteFunction(mappin.GetPtr(),getter,&world);
    const float p[]={world.X,world.Y,world.Z};
    Remember(root,p,valid,true);
    CVR_DIAGNOSTIC(CyberpunkVR_WorldMarkerMappinStages[5].fetch_add(1,std::memory_order_relaxed));
    if(valid) CVR_DIAGNOSTIC(CyberpunkVR_WorldMarkerMappinStages[6].fetch_add(1,std::memory_order_relaxed));
    return result;
}

using ProjectionFn=void (*)(void*,void*,void*,float);
ProjectionFn originalProjection{};
void Projection(void* projection,void* view,void* size,float worldScale) {
    CVR_DIAGNOSTIC(CyberpunkVR_WorldMarkerProjectionStages[0].fetch_add(1,std::memory_order_relaxed));
    originalProjection(projection,view,size,worldScale);
    if(!Enabled()) return;
    CVR_DIAGNOSTIC(CyberpunkVR_WorldMarkerProjectionStages[1].fetch_add(1,std::memory_order_relaxed));
    auto owner=At<WeakHandle<IScriptable>>(projection,0xD0).Lock();
    const auto logicClass=CRTTISystem::Get()->GetClass("inkIWidgetLogicController");
    if(!owner || !logicClass || !owner->GetType()->IsA(logicClass)) return;
    CVR_DIAGNOSTIC(CyberpunkVR_WorldMarkerProjectionStages[2].fetch_add(1,std::memory_order_relaxed));
    auto root=At<WeakHandle<ink::Widget>>(owner.GetPtr(),0x40).Lock();
    if(!root || !HudRoot(root)) return;
    CVR_DIAGNOSTIC(CyberpunkVR_WorldMarkerProjectionStages[3].fetch_add(1,std::memory_order_relaxed));
    float world[3]{};
    // Same native entity/slot lookup as the projection update, with its static
    // position fallback. Constants below are read from the verified executable.
    using WorldFn=bool (*)(void*,float*);
    if(!reinterpret_cast<WorldFn>(base+0x4E8FD4)(projection,world))
        std::memcpy(world,static_cast<uint8_t*>(projection)+0xB0,sizeof(world));
    const bool adjust=At<uint8_t>(projection,0xE0)!=0;
    const float minimum=*reinterpret_cast<const float*>(base+0x31EF494);
    const float scale=adjust && worldScale>minimum
        ? *reinterpret_cast<const float*>(base+0x31EF5A8)/worldScale : 1;
    for(int i=0;i<3;++i) world[i]+=At<float>(projection,0xC0+i*4)*scale;
    if(adjust) {
        const float distance=At<float>(projection,0x104)/(worldScale>minimum ? worldScale : 1);
        const float factor=std::min(distance,*reinterpret_cast<const float*>(base+0x31EF890)) *
            *reinterpret_cast<const float*>(base+0x31EEFB0);
        for(int i=0;i<3;++i) world[i]+=At<float>(projection,0xE4+i*4)*factor;
    }
    Remember(root,world);
    CVR_DIAGNOSTIC(CyberpunkVR_WorldMarkerProjectionStages[4].fetch_add(1,std::memory_order_relaxed));
}

using PaintFn=void (*)(ink::Widget*,void*);
PaintFn originalPaint{};
void Paint(ink::Widget* widget,void* context) {
    const float previous=paintDepth;
    if(Enabled()) {
        std::shared_lock lock(rootsMutex);
        const auto it=roots.find(widget);
        if(it!=roots.end()) {
            const auto live=it->second.root.Lock();
            paintDepth=live.GetPtr()==widget && GetTickCount64()-it->second.stamp<1000
                ? it->second.inverseDepth : 0;
        }
    } else paintDepth=0;
    originalPaint(widget,context);
    paintDepth=previous;
}

// A pool can reallocate while collecting a frame. Keep metadata by pool/index
// and rebuild the address index on growth; never infer ownership from rectangles.
struct PrimitivePool { uintptr_t data{}; std::vector<float> depths; uint64_t stamp{}; };
struct PrimitiveDepthEntry { void* owner{}; float depth{}; };
std::mutex primitiveMutex;
std::unordered_map<void*,PrimitivePool> pools;
std::unordered_map<void*,PrimitiveDepthEntry> primitives;
void ForgetPoolRecords(void* owner,const PrimitivePool& pool) {
    for(size_t i=0;i<pool.depths.size();++i) {
        const auto it=primitives.find(reinterpret_cast<void*>(pool.data+i*0x130));
        if(it!=primitives.end() && it->second.owner==owner) primitives.erase(it);
    }
}
using AllocateFn=void* (*)(void*);
AllocateFn originalAllocate{};
void* Allocate(void* batch) {
    void* result=originalAllocate(batch);
    if(!result || !hooksReady.load(std::memory_order_acquire)) return result;
    void* owner=At<void*>(batch,0x28);
    const auto data=At<uintptr_t>(owner,0);
    const auto address=reinterpret_cast<uintptr_t>(result);
    if(address<data || (address-data)%0x130 || (address-data)/0x130>131072) return result;
    const size_t index=(address-data)/0x130;
    std::lock_guard lock(primitiveMutex);
    auto& pool=pools[owner];
    if(index==0 || pool.data!=data) {
        ForgetPoolRecords(owner,pool);
        if(index==0) pool.depths.clear();
        else for(size_t i=0;i<pool.depths.size();++i)
            if(pool.depths[i]>0) primitives[reinterpret_cast<void*>(data+i*0x130)]={owner,pool.depths[i]};
        pool.data=data;
    }
    pool.stamp=GetTickCount64();
    if(pool.depths.size()<=index) pool.depths.resize(index+1);
    pool.depths[index]=paintDepth;
    if(paintDepth>0) primitives[result]={owner,paintDepth}; else primitives.erase(result);
    static uint64_t nextPrune=0;
    if(pool.stamp>=nextPrune) {
        const uint64_t now=pool.stamp;nextPrune=now+10000;
        for(auto it=pools.begin();it!=pools.end();) {
            if(now-it->second.stamp>60000) {
                ForgetPoolRecords(it->first,it->second);it=pools.erase(it);
            } else ++it;
        }
    }
    return result;
}
float PrimitiveDepth(void* record) {
    if(!Enabled()) return 0;
    std::lock_guard lock(primitiveMutex);
    const auto it=primitives.find(record);
    if(it==primitives.end()) return 0;
    const auto pool=pools.find(it->second.owner);
    if(pool!=pools.end()) pool->second.stamp=GetTickCount64();
    return it->second.depth;
}
struct VertexStream { void* geometry; uint8_t* vertices; uint32_t count; };
static_assert(offsetof(VertexStream,count)==0x10 && sizeof(VertexStream)==0x18);
using EmitFn=void (*)(void*,void*,VertexStream*);
EmitFn originalQuad{},originalTriangles{};
void Emit(EmitFn original,void* context,void* record,VertexStream* stream) {
    const auto before=stream->count;
    const float encoded=EncodeDepth(PrimitiveDepth(record));
    original(context,record,stream);
    if(!encoded || !stream->vertices || stream->count<before || stream->count-before>65536) return;
    for(uint32_t i=before;i<stream->count;++i) {
        auto& z=*reinterpret_cast<float*>(stream->vertices+size_t(i)*24+8);
        if(z==0) z=encoded;
    }
    CVR_DIAGNOSTIC(CyberpunkVR_WorldMarkerQuads.fetch_add(1,std::memory_order_relaxed));
}
void Quad(void* context,void* record,VertexStream* stream) { Emit(originalQuad,context,record,stream); }
void Triangles(void* context,void* record,VertexStream* stream) { Emit(originalTriangles,context,record,stream); }
using TextFn=void (*)(void*,void*);
TextFn originalText{};
void Text(void* record,void* context) {
    const float previous=textDepth;
    textDepth=PrimitiveDepth(record);
    originalText(record,context);
    textDepth=previous;
}

struct InkCamera { float width{},height{}; uint64_t stamp{}; };
thread_local InkCamera inkCameras[2];
float EyeScale(float width,float height) {
    const float zoom=CyberpunkVR_MainAdsZoomFactor;
    return StereoScale(2*CyberpunkVRPort_HalfIpd(),GetGameRenderFovDeg(),width/height,
        std::isfinite(zoom) && zoom>.05f ? zoom : 1,CyberpunkVR_MainIsRightEye!=0);
}

bool InstallTextCall() {
    auto* site=reinterpret_cast<uint8_t*>(base+0x2F3D58);
    const uint8_t expected[]={0x48,0x8d,0x8b,0xf8,0x01,0,0,0xf3,0x0f,0x10,0x46,0x6c};
    if(std::memcmp(site,expected,sizeof(expected))) return false;
    auto* relay=static_cast<uint8_t*>(AllocateTrampoline(site,14));
    if(!relay) return false;
    const uint8_t jump[]={0xff,0x25,0,0,0,0};
    std::memcpy(relay,jump,sizeof(jump));
    const auto target=reinterpret_cast<uintptr_t>(&CvrWorldMarkerTextPublishDetour);
    std::memcpy(relay+6,&target,sizeof(target));
    const auto distance=reinterpret_cast<intptr_t>(relay)-reinterpret_cast<intptr_t>(site+7);
    if(distance<INT32_MIN || distance>INT32_MAX) return false;
    DWORD previous{};
    if(!VirtualProtect(site,7,PAGE_EXECUTE_READWRITE,&previous)) return false;
    // Replace the seven-byte LEA with NOP/NOP/CALL. A real return address lets
    // Windows unwind through the thunk back into the native function's frame.
    // The thunk performs the original LEA before returning to site+7.
    site[0]=site[1]=0x90;site[2]=0xe8;
    const auto relative=static_cast<int32_t>(distance);
    std::memcpy(site+3,&relative,4);
    DWORD unused{};VirtualProtect(site,7,previous,&unused);
    FlushInstructionCache(GetCurrentProcess(),relay,14);
    FlushInstructionCache(GetCurrentProcess(),site,7);
    return true;
}

bool Install() {
    struct Hook { uintptr_t rva; const char* bytes; size_t size; void* detour; void** original; };
    const Hook hooks[]={
        {0x2E37AC,"\x48\x89\x5c\x24\x10\x48\x89\x7c\x24\x20\x55\x48\x8b\xec",14,reinterpret_cast<void*>(&MappinPosition),reinterpret_cast<void**>(&originalMappinPosition)},
        {0x4E7E24,"\x48\x8b\xc4\x48\x89\x58\x10\x48\x89\x70\x18\x48\x89\x78\x20",15,reinterpret_cast<void*>(&Projection),reinterpret_cast<void**>(&originalProjection)},
        {0x2EE510,"\x48\x8b\xc4\x48\x89\x58\x10\x48\x89\x70\x18\x48\x89\x78\x20",15,reinterpret_cast<void*>(&Paint),reinterpret_cast<void**>(&originalPaint)},
        {0x2ECC64,"\x48\x89\x5c\x24\x08\x48\x89\x6c\x24\x10\x48\x89\x74\x24\x18",15,reinterpret_cast<void*>(&Allocate),reinterpret_cast<void**>(&originalAllocate)},
        {0x2F2708,"\x48\x8b\xc4\x48\x89\x58\x08\x4c\x89\x40\x18\x48\x89\x50\x10",15,reinterpret_cast<void*>(&Quad),reinterpret_cast<void**>(&originalQuad)},
        {0x2F2E40,"\x48\x8b\xc4\x48\x89\x58\x18\x48\x89\x48\x08\x55\x56\x57",14,reinterpret_cast<void*>(&Triangles),reinterpret_cast<void**>(&originalTriangles)},
        {0x2F3B10,"\x48\x8b\xc4\x48\x89\x58\x10\x55\x56\x57",10,reinterpret_cast<void*>(&Text),reinterpret_cast<void**>(&originalText)},
    };
    for(const auto& h:hooks) if(std::memcmp(reinterpret_cast<void*>(base+h.rva),h.bytes,h.size)) {
        Log("[world-markers] signature mismatch at %llX; native placement retained\n",static_cast<unsigned long long>(h.rva));return false;
    }
    size_t made=0;
    for(const auto& h:hooks) {
        if(MH_CreateHook(reinterpret_cast<void*>(base+h.rva),h.detour,h.original)!=MH_OK) break;
        ++made;
    }
    bool ok=made==std::size(hooks);
    if(ok) for(const auto& h:hooks) if(MH_EnableHook(reinterpret_cast<void*>(base+h.rva))!=MH_OK) { ok=false;break; }
    if(ok) ok=InstallTextCall();
    if(!ok) {
        for(size_t i=0;i<made;++i) { MH_DisableHook(reinterpret_cast<void*>(base+hooks[i].rva));MH_RemoveHook(reinterpret_cast<void*>(base+hooks[i].rva)); }
        return false;
    }
    hooksReady.store(true,std::memory_order_release);
    Log("[world-markers] native projection ownership hooks installed\n");return true;
}
CVR_HOOK("WorldMarkers",cvr::hooks::Stage::Boot,83,Install);
}

// Called before the native geometry publication, while its writer still owns it.
extern "C" void CvrWorldMarkerTextTag(void* geometry) {
    const float encoded=EncodeDepth(textDepth);
    if(encoded && geometry && At<float>(geometry,0x188)==0 && At<float>(geometry,0x18C)==1) {
        At<float>(geometry,0x188)=encoded;
        CVR_DIAGNOSTIC(CyberpunkVR_WorldMarkerTexts.fetch_add(1,std::memory_order_relaxed));
    }
}
bool RewriteConstants(uint32_t size,const void* source,void* scratch) {
    if(!source || !scratch) return false;
    const int side=detail::t_view_side;
    if(IsInkCamera(source,size)) {
        const auto* p=static_cast<const float*>(source);
        if(side<0 || side>1) return false;
        inkCameras[side]={p[188],p[189],GetTickCount64()};
        CVR_DIAGNOSTIC(CyberpunkVR_WorldMarkerCameraUploads[side].fetch_add(1,std::memory_order_relaxed));
        if(side!=1 || !Enabled()) return false;
        std::memcpy(scratch,source,size);
        static_cast<float*>(scratch)[2]=EyeScale(p[188],p[189]);
        return true;
    }
    // Direct uploads can decode here. Batched native composition bypasses these
    // uploaders; the replacement ink shaders also decode the same matrix tag.
    if(size!=112) return false;
    const auto* p=static_cast<const float*>(source);
    if(!TextMatrixDepth(p,size)) return false;
    std::memcpy(scratch,source,size);
    auto* out=static_cast<float*>(scratch);
    float shift=0;
    if(side>=0 && side<2) {
        CVR_DIAGNOSTIC(CyberpunkVR_WorldMarkerTextUploads[side].fetch_add(1,std::memory_order_relaxed));
        const auto& camera=inkCameras[side];
        if(side==1 && Enabled() && camera.width>0 && GetTickCount64()-camera.stamp<1000)
            shift=EyeScale(camera.width,camera.height)*camera.width*.5f;
    }
    ReprojectTextMatrix(out,shift);
    return true;
}
}
