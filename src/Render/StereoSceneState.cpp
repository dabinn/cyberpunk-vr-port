#include "Render/StereoSceneState.hpp"
#include "Render/RenderProbeScope.hpp"
#include <mutex>
#include <unordered_map>
namespace cvr::stereo::scene_state {
namespace {
std::mutex mutex;
std::unordered_map<ID3D12RootSignature*,uint64_t> roots;
std::unordered_map<ID3D12PipelineState*,bool> pipelines;
std::unordered_map<ID3D12GraphicsCommandList*,Bindings> bindings;
struct View {ID3D12Resource* resource{};bool depth{};};
// Weak metadata only. Retention happens at an actual draw where the engine owns
// every bound target. Rewrites invalidate previous entries, including null views.
std::unordered_map<SIZE_T,View> targets;
template<class F> void Edit(ID3D12GraphicsCommandList* list,F f) {
    if(probe::internalCommands || CyberpunkVR_NativeStereoProbeState.load(std::memory_order_relaxed)!=1)return;
    std::lock_guard lock(mutex);auto it=bindings.find(list);
    if(it!=bindings.end())f(it->second);else if(bindings.size()<512)f(bindings[list]);
}
bool Shape(const D3D12_RESOURCE_DESC& d) {return d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
    d.DepthOrArraySize==1 && d.MipLevels==1 && d.SampleDesc.Count==1 && d.Width && d.Height;}
void Store(D3D12_CPU_DESCRIPTOR_HANDLE h,View view) {
    std::lock_guard lock(mutex);if(!view.resource){targets.erase(h.ptr);return;}
    if(targets.size()>=32768 && !targets.contains(h.ptr))targets.clear();targets[h.ptr]=view;
}
}
void RootCreated(ID3D12RootSignature* root,const void* data,size_t bytes) {
    Microsoft::WRL::ComPtr<ID3D12VersionedRootSignatureDeserializer> d;
    if(FAILED(D3D12CreateVersionedRootSignatureDeserializer(data,bytes,IID_PPV_ARGS(&d))))return;
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* rs{};if(FAILED(d->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1,&rs)))return;
    const auto n=rs->Desc_1_1.NumParameters;bool valid=n>0 && n<=64;
    for(UINT i=0;valid && i<n;++i)valid=rs->Desc_1_1.pParameters[i].ParameterType==D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    std::lock_guard lock(mutex);if(roots.size()<128 || roots.contains(root))roots[root]=valid?(n==64?UINT64_MAX:(uint64_t(1)<<n)-1):0;
}
void PipelineCreated(ID3D12PipelineState* pso,bool compatible){std::lock_guard lock(mutex);if(pipelines.size()<8)pipelines[pso]=compatible;}
bool Compatible(ID3D12PipelineState* pso){std::lock_guard lock(mutex);const auto it=pipelines.find(pso);return it!=pipelines.end() && it->second;}
void ResetCapture(){std::lock_guard lock(mutex);bindings.clear();}
void Reset(ID3D12GraphicsCommandList* list,bool rateKnown){Edit(list,[&](auto& b){b={};b.resetSeen=true;b.rate.known=rateKnown;});}
void Root(ID3D12GraphicsCommandList* list,ID3D12RootSignature* root){Edit(list,[&](auto& b){if(b.root!=root){b.root=root;b.tableMask=0;b.tables={};}});}
void Table(ID3D12GraphicsCommandList* list,UINT index,D3D12_GPU_DESCRIPTOR_HANDLE h){if(index<64)Edit(list,[&](auto& b){b.tables[index]=h;b.tableMask|=uint64_t(1)<<index;});}
void ComputeRoot(ID3D12GraphicsCommandList* list,ID3D12RootSignature* root){Edit(list,[&](auto& b){if(b.computeRoot!=root){b.computeRoot=root;b.computeTableMask=0;b.computeTables={};}});}
void ComputeTable(ID3D12GraphicsCommandList* list,UINT index,D3D12_GPU_DESCRIPTOR_HANDLE h){if(index<64)Edit(list,[&](auto& b){b.computeTables[index]=h;b.computeTableMask|=uint64_t(1)<<index;});}
void Heaps(ID3D12GraphicsCommandList* list,UINT count,ID3D12DescriptorHeap* const* heaps){Edit(list,[&](auto& b){
    b.heapsSeen=count<=2 && (count==0 || heaps);if(!b.heapsSeen)return;
    bool changed=b.heapCount!=count;for(UINT i=0;i<count;++i)changed|=b.heaps[i]!=heaps[i];
    if(changed){b.tableMask=b.computeTableMask=0;b.tables={};b.computeTables={};}b.heapCount=count;b.heaps={};for(UINT i=0;i<count;++i)b.heaps[i]=heaps[i];});}
void Topology(ID3D12GraphicsCommandList* list,D3D12_PRIMITIVE_TOPOLOGY t){Edit(list,[&](auto& b){b.topology=t;});}
void Stencil(ID3D12GraphicsCommandList* list,UINT s){Edit(list,[&](auto& b){b.stencil=s&255;});}
void ShadingRate(ID3D12GraphicsCommandList* list,D3D12_SHADING_RATE rate,const D3D12_SHADING_RATE_COMBINER* combiners){Edit(list,[&](auto& b){b.rate.value=rate;
    b.rate.combiners=combiners?std::array{combiners[0],combiners[1]}:std::array{D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,D3D12_SHADING_RATE_COMBINER_PASSTHROUGH};});}
void ShadingRateImage(ID3D12GraphicsCommandList* list,ID3D12Resource* image){Edit(list,[&](auto& b){b.rate.image=image;});}
bool Snapshot(ID3D12GraphicsCommandList* list,Bindings& result) {
    std::lock_guard lock(mutex);const auto it=bindings.find(list);if(it==bindings.end())return false;const auto& b=it->second;
    // Reset was observed, so a table never bound since then is known-unbound.
    // The native depth prepass leaves the unused pixel-stage tables unbound.
    // Restore every initialized table, including tables above slot 31, without
    // inventing descriptors for shader stages which the native PSO does not use.
    const auto root=roots.find(b.root);if(root==roots.end() || !root->second || (b.tableMask&~root->second) ||
        !b.resetSeen || !b.heapsSeen || !b.heapCount || !b.topology || b.stencil==255)return false;
    if(b.computeRoot){const auto compute=roots.find(b.computeRoot);if(compute==roots.end() || !compute->second)return false;}
    result=b;return true;
}
void Bindings::Restore(ID3D12GraphicsCommandList* list) const {
    list->SetDescriptorHeaps(heapCount,heaps.data());list->SetGraphicsRootSignature(root);
    for(UINT i=0;i<64;++i)if(tableMask&(uint64_t(1)<<i))list->SetGraphicsRootDescriptorTable(i,tables[i]);
    // Switching descriptor heaps invalidates compute tables as well. Preserve
    // those bindings even though this graphics resolve never dispatches compute.
    if(computeRoot){list->SetComputeRootSignature(computeRoot);for(UINT i=0;i<64;++i)if(computeTableMask&(uint64_t(1)<<i))list->SetComputeRootDescriptorTable(i,computeTables[i]);}
    list->IASetPrimitiveTopology(topology);list->OMSetStencilRef(stencil);
}
void RtvCreated(ID3D12Resource* resource,const D3D12_RENDER_TARGET_VIEW_DESC* view,D3D12_CPU_DESCRIPTOR_HANDLE h) {
    if(probe::internalCommands || !native_probe::Enabled())return;View result{};
    if(resource){const auto d=resource->GetDesc();if(Shape(d) && (d.Format==24 || d.Format==28) &&
        (!view || (view->Format==d.Format && view->ViewDimension==D3D12_RTV_DIMENSION_TEXTURE2D && !view->Texture2D.MipSlice && !view->Texture2D.PlaneSlice)))result={resource,false};}Store(h,result);
}
void DsvCreated(ID3D12Resource* resource,const D3D12_DEPTH_STENCIL_VIEW_DESC* view,D3D12_CPU_DESCRIPTOR_HANDLE h) {
    if(probe::internalCommands || !native_probe::Enabled())return;View result{};
    if(resource){const auto d=resource->GetDesc();if(Shape(d) && (d.Format==19 || d.Format==20) &&
        ((!view && d.Format==20) || (view && view->Format==20 && view->ViewDimension==D3D12_DSV_DIMENSION_TEXTURE2D && !view->Flags && !view->Texture2D.MipSlice)))result={resource,true};}Store(h,result);
}
bool CaptureTargets(const native_probe::DrawRecord& draw,Targets& result) {
    if((draw.rtCount!=3 && draw.rtCount!=0) || (draw.rtCount && draw.rtContiguous) || !draw.depth || draw.viewport.TopLeftX || draw.viewport.TopLeftY ||
        draw.viewport.MinDepth!=0 || draw.viewport.MaxDepth!=1)return false;
    Targets pending;std::lock_guard lock(mutex);
    for(UINT i=draw.rtCount?0:3;i<4;++i){const auto it=targets.find(i<3?draw.targets[i]:draw.depth);if(it==targets.end() || it->second.depth!=(i==3))return false;
        auto* resource=it->second.resource;const auto d=resource->GetDesc();
        if(!Shape(d) || d.Width!=draw.viewport.Width || d.Height!=draw.viewport.Height || d.Width>2048 || d.Height>2048 ||
            (i<2 && d.Format!=24) || (i==2 && d.Format!=28) || (i==3 && d.Format!=19 && d.Format!=20))return false;
        pending.resources[i]=resource;
    }result=std::move(pending);return true;
}
}
