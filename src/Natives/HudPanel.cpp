// Verified against CP2077 SHA256 a7de82945c03e041... with IDA headless.
// Only native separate-window slots are moved; direct/projected HUD widgets stay native.
#include "Natives/NativeFunctions.hpp"
#include "Camera/SurveillanceFollow.hpp"
#include "Render/HudPanel.hpp"
#include "Runtimes/HudLayout.hpp"
#include "Core/VrCoreShared.hpp"
#include "Core/LiveControls.hpp"
#include "Hooks/Hook.hpp"
#include "Utils/DebugGate.hpp"
#include <MinHook.h>
#include <atomic>
#include <RED4ext/Scripting/Natives/inkLayer.hpp>
#include <RED4ext/Scripting/Natives/inkHUDLayer.hpp>
#include <RED4ext/Scripting/Natives/inkHudWidgetSpawnEntry.hpp>
#include <RED4ext/Scripting/Natives/inkWidget.hpp>
#include <RED4ext/Scripting/Natives/inkLayerProxy.hpp>
#include <RED4ext/Scripting/Natives/Generated/DynamicTexture.hpp>
#include <RED4ext/Scripting/Natives/Generated/ink/ImageWidget.hpp>
#include <shared_mutex>
#include <unordered_map>
#include <algorithm>
#include <cmath>

namespace {
using namespace RED4ext;
const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
using ChildRectFn = Vector2* (*)(ink::Widget*, Vector2*, const Handle<ink::Widget>*);
using VisibleFn = uintptr_t (*)(ink::Widget*, bool);
using InvalidateFn = void (*)(ink::Widget*);
using CopyEntriesFn = uintptr_t (*)(uintptr_t, DynArray<ink::HudWidgetSpawnEntry>*);
VisibleFn originalVisible = nullptr;
CopyEntriesFn originalCopyEntries = nullptr;
std::atomic<bool> hooksReady{false};
bool Verified() {
    static const bool ok = [] {
        const uint8_t layout[] = {0x40,0x53,0x48,0x83,0xec,0x70,0x48,0x8b,0x89,0xd0,0x01,0,0};
        const uint8_t release[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0x15,0xae,0xe8,0x23,0x03};
        return !memcmp(reinterpret_cast<void*>(base+0x894e50), layout, sizeof(layout)) &&
               !memcmp(reinterpret_cast<void*>(base+0x894dfc), layout, sizeof(layout)) &&
               !memcmp(reinterpret_cast<void*>(base+0x1fa164), release, sizeof(release));
    }();
    return ok;
}
struct Mask {
    Handle<ink::Widget> widget;
    std::atomic<bool> wantedVisible;
    bool originalAffectsLayout;
    std::atomic<cvr::hud::Channel> channel;
    explicit Mask(const Handle<ink::Widget>& value,cvr::hud::Channel group)
        : widget(value), wantedVisible(value->visible), originalAffectsLayout(value->affectsLayoutWhenHidden),channel(group) {}
};
using MaskSet = std::vector<std::shared_ptr<Mask>>;
std::unordered_map<ink::Widget*, std::shared_ptr<Mask>> masks;
std::atomic<std::shared_ptr<const MaskSet>> publishedMasks;
uint64_t generation = 0;
ink::Widget* lastRoot = nullptr;
std::array<bool,cvr::hud::ChannelCount> lastMasked{};
std::unordered_map<uint64_t,cvr::hud::LayoutLatch> layoutCache;
uint32_t layoutWidth=0,layoutHeight=0;

void HideSlot(ink::Widget* widget) {
    widget->affectsLayoutWhenHidden=true;
    if (widget->visible) {
        // ImageWidget::OnVisibilityChanged(false), RVA4FC34C, unloads the image
        // and invalidates its layout even with affectsLayoutWhenHidden=true.
        // Mask only the final draw and visually invalidate the ancestors instead.
        // The separate window/texture stays live and its layout keeps its size.
        widget->visible=false;
        reinterpret_cast<InvalidateFn>(base+0x2effe0)(widget);
    }
}

// The game updates slot visibility after CET onUpdate too. Intercept its request,
// retain the intended visibility, and keep only our final slot image from drawing.
// Source windows and their controllers are not hidden. Immutable target snapshots
// keep the handles alive while an engine UI job is inside the detour.
uintptr_t Visible(ink::Widget* widget, bool visible) {
    const auto targets=publishedMasks.load(std::memory_order_acquire);
    if (targets) for (const auto& mask:*targets) if (mask->widget.GetPtr()==widget) {
        mask->wantedVisible.store(visible,std::memory_order_relaxed);
        const auto channel=mask->channel.load(std::memory_order_acquire);
        if (g_liveControls.xrHudPanel && !g_menuModeValue && !g_liveControls.xrMenuRect &&
            (channel!=cvr::hud::Channel::Interaction || g_liveControls.xrInteractionPanel) && cvr::hud::ConsumerReady(channel)) {
            HideSlot(widget);return 0;
        }
        break;
    }
    return originalVisible(widget,visible);
}

uintptr_t CopyEntries(uintptr_t resource, DynArray<ink::HudWidgetSpawnEntry>* entries) {
    const auto result=originalCopyEntries(resource,entries);
    // These HUD entries are authored without separate windows. Move them
    // through the engine's own separate-window creation path BEFORE spawn.
    // Never convert projected widgets (crosshair, mappins, scanner borders, etc.).
    for (auto& e:*entries) if (cvr::hud::NeedsSeparateWindow(e.hudEntryName.hash)) {
        e.useSeparateWindow=true;
        Log("[hud-panel] separate window enabled for %s before spawn\n",e.hudEntryName.ToString());
    }
    return result;
}

bool InstallHudPanelHooks() {
    const uint8_t visibility[]={0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x40,0x8a,0xfa,0x48,0x8b,0xd9};
    const uint8_t entries[]={0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xec,0x20};
    const uint8_t invalidate[]={0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x7c,0x24,0x18,0x55,0x48,0x8b,0xec,0x48,0x83,0xec,0x60};
    const uint8_t invalidateLayout[]={0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x7c,0x24,0x10,0x55,0x48,0x8d,0x6c,0x24,0xa9,0x48,0x81,0xec,0xa0,0,0,0};
    if (!Verified() || memcmp(reinterpret_cast<void*>(base+0x2e367c),visibility,sizeof(visibility)) ||
        memcmp(reinterpret_cast<void*>(base+0x8a53c8),entries,sizeof(entries)) ||
        memcmp(reinterpret_cast<void*>(base+0x2effe0),invalidate,sizeof(invalidate)) ||
        memcmp(reinterpret_cast<void*>(base+0x2efec8),invalidateLayout,sizeof(invalidateLayout))) return false;
    auto vis=reinterpret_cast<void*>(base+0x2e367c), copy=reinterpret_cast<void*>(base+0x8a53c8);
    if (MH_CreateHook(vis,reinterpret_cast<void*>(&Visible),reinterpret_cast<void**>(&originalVisible))!=MH_OK) return false;
    if (MH_CreateHook(copy,reinterpret_cast<void*>(&CopyEntries),reinterpret_cast<void**>(&originalCopyEntries))!=MH_OK) {
        MH_RemoveHook(vis);return false;
    }
    if (MH_EnableHook(vis)!=MH_OK || MH_EnableHook(copy)!=MH_OK) {
        MH_DisableHook(vis);MH_DisableHook(copy);return false;
    }
    hooksReady.store(true,std::memory_order_release);return true;
}
CVR_HOOK("HudPanel",::cvr::hooks::Stage::Boot,82,InstallHudPanelHooks);

void PublishMasks() {
    auto next=std::make_shared<MaskSet>();next->reserve(masks.size());
    for (auto& [p,m]:masks) next->push_back(m);
    publishedMasks.store(std::move(next),std::memory_order_release);
}
void RestoreMask(Mask& mask) {
    // Calling the original bypasses our interception, so it cannot mistake our
    // own hide/restore for a new game-side visibility request.
    auto* widget=mask.widget.GetPtr();
    const bool wanted=mask.wantedVisible.load(std::memory_order_relaxed);
    const bool layoutChanged=widget->affectsLayoutWhenHidden!=mask.originalAffectsLayout;
    widget->affectsLayoutWhenHidden=mask.originalAffectsLayout;
    // Our hidden bit is only a draw mask. Deliver the real visibility transition
    // now, including ImageWidget's resource/layout callback when the game hid it.
    widget->visible=!wanted;
    originalVisible(widget,wanted);
    if(layoutChanged) reinterpret_cast<InvalidateFn>(base+0x2efec8)(widget);
}
void Restore() {
    publishedMasks.store({},std::memory_order_release);
    for (auto& [p,m]:masks) RestoreMask(*m);
    masks.clear();
}

bool Geometry(Handle<ink::Widget> child, ink::Widget* root, cvr::hud::Sprite& s, bool& dirty) {
    Vector2 position{}, size{};
    bool first = true;
    for (unsigned depth = 0; child && depth < 24; ++depth) {
        dirty |= (child->flags & 0x06)!=0;
        auto mask = masks.find(child.GetPtr());
        const bool visible = mask==masks.end() ? child->visible : mask->second->wantedVisible.load(std::memory_order_relaxed);
        if (!visible || child->opacity <= 0) return false;
        const auto& t = child->tintColor;
        const float alpha = std::clamp(child->opacity * t.Alpha, 0.0f, 1.0f);
        s.tint[0] *= t.Red * alpha; s.tint[1] *= t.Green * alpha;
        s.tint[2] *= t.Blue * alpha; s.tint[3] *= alpha;
        if (child.GetPtr() == root)
            return !first && std::isfinite(s.x+s.y+s.width+s.height) && s.width > 0 && s.height > 0;
        auto parent = child->parentWidget.Lock();
        if (!parent) return false;
        reinterpret_cast<ChildRectFn>(base+0x894e50)(parent.GetPtr(), &position, &child);
        if (first) {
            reinterpret_cast<ChildRectFn>(base+0x894dfc)(parent.GetPtr(), &size, &child);
            s.width = size.X; s.height = size.Y; first = false;
        }
        s.x += position.X; s.y += position.Y;
        child = std::move(parent);
    }
    return false;
}

std::shared_ptr<cvr::hud::TextureLease> Texture(DynamicTexture* texture) {
    if (!texture) return {};
    const uintptr_t backing = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(texture)+0x70);
    if (!backing || *reinterpret_cast<uintptr_t*>(backing) != base+0x2b1bc18) return {};
    const uint32_t id = *reinterpret_cast<uint32_t*>(backing+0x18);
    if (!id || id > 131072) return {};
    const uintptr_t pool = *reinterpret_cast<uintptr_t*>(base+0x3438a28);
    if (!pool) return {};
    const uintptr_t entry = pool + 0xb0ull*(id-1);
    auto ref = reinterpret_cast<volatile LONG*>(entry+0x2f1d0);
    LONG count = *ref;
    while (count > 0) {
        LONG observed = InterlockedCompareExchange(ref, count+1, count);
        if (observed == count) break;
        count = observed;
    }
    if (count <= 0) return {};
    auto result = std::make_shared<cvr::hud::TextureLease>();
    result->handle = id; // Engine handle lease also keeps the placed-resource heap alive.
    auto resource = *reinterpret_cast<ID3D12Resource**>(entry+0x2f1d8);
    if (!resource) return {};
    auto desc = resource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 ||
        (desc.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
         desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) ||
        desc.Width != texture->width || desc.Height != texture->height) return {};
    // This is the engine's read state (IDA sub_1401F9BBC / sub_1401F40DC).
    // Do not guess barriers for a texture with a different native state.
    if (*reinterpret_cast<uint32_t*>(entry+0x2f220) != 0xc0) return {};
    result->resource = resource;
    return result;
}
}

cvr::hud::TextureLease::~TextureLease() {
    if (handle) reinterpret_cast<void (*)(uint32_t*)>(base+0x1fa164)(&handle);
}

void VRHudPanelUpdate(RED4ext::IScriptable*, RED4ext::CStackFrame* frame, int32_t* out, int64_t) {
    using namespace RED4ext;
    using cvr::hud::Channel;
    Handle<ink::Widget> root;
    bool lootVisible=false;
    GetParameter(frame, &root); GetParameter(frame, &lootVisible); ++frame->code;
    if (out) *out = 0;
    if (!Verified() || !hooksReady.load(std::memory_order_acquire)) return;
    std::array<std::shared_ptr<cvr::hud::Snapshot>,cvr::hud::ChannelCount> snapshots;
    for(auto& snapshot:snapshots){snapshot=std::make_shared<cvr::hud::Snapshot>();snapshot->stamp=GetTickCount64();}
    snapshots[cvr::hud::Index(Channel::Interaction)]->lootVisible=lootVisible;
    auto publish=[&]{for(size_t i=0;i<snapshots.size();++i)cvr::hud::Publish(snapshots[i],static_cast<Channel>(i));};
    auto proxy = root ? root->layerProxy : Handle<ink::LayerProxy>{};
    auto layer = proxy ? proxy->layer.Lock() : Handle<ink::Layer>{};
    if (!g_liveControls.xrHudPanel || g_menuModeValue != 0 || g_liveControls.xrMenuRect ||
        !layer || layer->GetType()->GetName() != CName("inkHUDLayer")) {
        Restore(); lastRoot = nullptr; lastMasked = {}; ++generation;publish();return;
    }
    if (root.GetPtr() != lastRoot) { Restore(); layoutCache.clear();lastRoot = root.GetPtr(); ++generation; }
    auto hud = static_cast<ink::HUDLayer*>(layer.GetPtr());
    const uint32_t width=static_cast<uint32_t>(root->size.X),height=static_cast<uint32_t>(root->size.Y);
    if(!width || !height || width>8192 || height>8192){Restore();publish();return;}
    for(auto& snapshot:snapshots){snapshot->width=width;snapshot->height=height;}
    if(layoutWidth!=width || layoutHeight!=height){layoutCache.clear();layoutWidth=width;layoutHeight=height;}
    std::array<std::vector<Handle<ink::Widget>>,cvr::hud::ChannelCount> slots;
    std::array<bool,cvr::hud::ChannelCount> complete;complete.fill(true);
    const auto settings=cvr::hud::GetLayoutSettings();
    const bool sniperNest=cvr::camera::SniperHeadFollowActive();
    cvr::hud::LayoutStatus status{};
    {
        std::shared_lock lock(hud->spawningLock);
        for(const auto& e:hud->entries){
            if(!e.useSeparateWindow || !e.slotWidget || !e.slotTexture || !e.window)continue;
            const auto channel=cvr::hud::EntryChannel(e.hudEntryName.hash,sniperNest);
            if(channel==Channel::Interaction && !g_liveControls.xrInteractionPanel)continue;
            const auto group=cvr::hud::Index(channel);auto& snapshot=*snapshots[group];
            Handle<ink::Widget> slot(e.slotWidget);
            cvr::hud::Sprite sprite;bool dirty=false;
            if(!Geometry(slot,root.GetPtr(),sprite,dirty))continue;
            sprite.texture=Texture(e.slotTexture.GetPtr());
            if(!sprite.texture || snapshot.sprites.size()>=cvr::hud::MaxSprites){complete[group]=false;continue;}
            sprite.name=e.hudEntryName.hash;
            auto rect=layoutCache[sprite.name].Update({sprite.x,sprite.y,sprite.width,sprite.height},dirty);
            const int index=cvr::hud::ElementIndex(sprite.name);
            if(index>=0){
                status[index]={true,rect.x,rect.y,rect.width,rect.height};
                rect=cvr::hud::ApplyElement(rect,settings[index],float(width),float(height));
                const float opacity=settings[index].visible?settings[index].opacity:0;
                for(float& component:sprite.tint)component*=opacity;
            }
            sprite.x=rect.x;sprite.y=rect.y;sprite.width=rect.width;sprite.height=rect.height;
            snapshot.sprites.push_back(std::move(sprite));slots[group].push_back(std::move(slot));
        }
    }
    cvr::hud::SetLayoutStatus(status);
    // Each XR consumer acknowledges only its own group. A missing dialogue
    // texture must not hide ordinary HUD, or mask a native prompt without a copy.
    std::unordered_map<ink::Widget*,Channel> desired;
    int total=0;bool allMasked=true;
    for(size_t i=0;i<snapshots.size();++i){
        auto& snapshot=*snapshots[i];const auto channel=static_cast<Channel>(i);
        snapshot.masked=complete[i] && !slots[i].empty() && cvr::hud::ConsumerReady(channel);
        if(snapshot.masked)for(auto& slot:slots[i])desired.emplace(slot.GetPtr(),channel);
        if(snapshot.masked!=lastMasked[i]){++generation;lastMasked[i]=snapshot.masked;}
        snapshot.generation=generation;
        if(!complete[i])snapshot.sprites.clear();
        total+=int(snapshot.sprites.size());if(!snapshot.sprites.empty() && !snapshot.masked)allMasked=false;
    }
    std::vector<std::shared_ptr<Mask>> retired;bool changed=false;
    for(auto it=masks.begin();it!=masks.end();){
        if(!desired.contains(it->first)){retired.push_back(it->second);it=masks.erase(it);changed=true;}
        else {it->second->channel.store(desired.at(it->first),std::memory_order_release);++it;}
    }
    for(size_t i=0;i<slots.size();++i)if(snapshots[i]->masked)for(auto& slot:slots[i]){
        if(!masks.contains(slot.GetPtr())){masks.emplace(slot.GetPtr(),std::make_shared<Mask>(slot,static_cast<Channel>(i)));changed=true;}
    }
    if(changed)PublishMasks();
    for(auto& old:retired)RestoreMask(*old);
    for(auto& [widget,channel]:desired)HideSlot(widget);
    if(out)*out=total*(allMasked?1:-1);
    if(cvr::RuntimeDiagnosticsEnabled()){
        static int previous=-1000;const int current=total*(allMasked?1:-1);
        if(current!=previous){previous=current;Log("[hud-panel] main=%u interactions=%u masked=%d/%d canvas=%ux%u\n",
            unsigned(snapshots[0]->sprites.size()),unsigned(snapshots[1]->sprites.size()),snapshots[0]->masked,snapshots[1]->masked,width,height);}
    }
    publish();
}
