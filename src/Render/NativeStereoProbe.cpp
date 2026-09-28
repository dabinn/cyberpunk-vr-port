#include "Render/NativeStereoProbe.hpp"
#include "Render/ViewInstancedPipeline.hpp"
#include "Render/RenderProbeScope.hpp"
#include "Render/StereoGpuProbe.hpp"
#include "Render/StereoSceneState.hpp"
#include "Render/StereoGroupResolve.hpp"
#include "Render/StereoInstanceData.hpp"
#include "Render/NativeUploadShadow.hpp"
#include <windows.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <cstring>
using Microsoft::WRL::ComPtr;
extern "C" {
__declspec(dllexport) uint32_t CyberpunkVR_NativeStereoPipelineBytes=sizeof(cvr::stereo::native_probe::PipelineRecord),CyberpunkVR_NativeStereoDrawBytes=sizeof(cvr::stereo::native_probe::DrawRecord);
__declspec(dllexport) std::atomic<uint32_t> CyberpunkVR_NativeStereoPipelineCount{0};
__declspec(dllexport) std::atomic<uint32_t> CyberpunkVR_NativeStereoProbeAssetStatus{0};
__declspec(dllexport) std::atomic<uint32_t> CyberpunkVR_NativeStereoStaticAssetStatus{0};
__declspec(dllexport) std::atomic<uint32_t> CyberpunkVR_NativeStereoAllDraws{0};
__declspec(dllexport) std::atomic<uint32_t> CyberpunkVR_NativeStereoProbeRequest{0},CyberpunkVR_NativeStereoProbeState{0},CyberpunkVR_NativeStereoProbeSeq{0},CyberpunkVR_NativeStereoDrawCount{0};
__declspec(dllexport) cvr::stereo::native_probe::PipelineRecord CyberpunkVR_NativeStereoPipelines[cvr::stereo::native_probe::PipelineCapacity]{};
__declspec(dllexport) cvr::stereo::native_probe::DrawRecord CyberpunkVR_NativeStereoDraws[cvr::stereo::native_probe::DrawCapacity]{};
}
namespace cvr::stereo::native_probe {
namespace {
std::once_flag initialization;bool enabled{};
std::vector<uint8_t> shader,staticShader,depthShader,pixelShader;
std::mutex pipelineMutex,stateMutex;
std::array<ComPtr<ID3D12PipelineState>,PipelineCapacity> originalPipelines,variants;
std::unordered_map<ID3D12RootSignature*,std::vector<uint8_t>> rootBlobs;
std::unordered_map<ID3D12GraphicsCommandList*,DrawRecord> states;
std::unordered_map<ID3D12GraphicsCommandList*,ComPtr<ID3D12Resource>> instanceSources;
std::atomic<bool> haveInstanceSources{false};
uint32_t frame{},limit{};
bool ReadMemory(uint64_t address,void* output,size_t bytes) {
    SIZE_T done{};return address && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(address),output,bytes,&done) && done==bytes;
}
uintptr_t NativeInstanceResource() {
    __try {
        const auto game=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        CONTEXT context{};RtlCaptureContext(&context);ULONG_PTR low{},high{};GetCurrentThreadStackLimits(&low,&high);
        for(unsigned depth=0;depth<24 && context.Rsp>=low && context.Rsp+8<=high;++depth) {
            // Fresh native IA binding: RBX is the buffer-record offset, RBP the
            // renderer. The engine has retained this resource before the call.
            if(context.Rip>=game+0x1F6CB0 && context.Rip<game+0x1F6D93) {
                if(context.Rbx%0xB0 || context.Rbx>0x20000000)return 0;
                uintptr_t resource{};
                return ReadMemory(context.Rbp+context.Rbx+0x5C0B08,&resource,sizeof(resource))?resource:0;
            }
            DWORD64 image{};const auto entry=RtlLookupFunctionEntry(context.Rip,&image,nullptr);
            if(entry){PVOID handler{};DWORD64 establisher{};RtlVirtualUnwind(UNW_FLAG_NHANDLER,image,context.Rip,entry,&context,&handler,&establisher,nullptr);}
            else {if(!ReadMemory(context.Rsp,&context.Rip,sizeof(context.Rip)))return 0;context.Rsp+=8;}
        }
    }__except(EXCEPTION_EXECUTE_HANDLER){}
    return 0;
}
void CopyInstance(ID3D12Resource* resource,DrawRecord& row,std::vector<uint8_t>* batch) {
    row.instanceDataBytes=0;row.instanceData={};
    uint64_t offset{};size_t bytes{};
    if(!resource || (!batch && row.instances!=1) || !instance_data::Range(row,offset,bytes,batch!=nullptr))return;
    if(batch)try{batch->resize(bytes);}catch(const std::bad_alloc&){return;}
    if(row.instanceHeapType==D3D12_HEAP_TYPE_DEFAULT) {
        // Never map/read back a DEFAULT heap. Only use a complete CPU-upload
        // range observed for this exact live resource during the bounded probe.
        if(!packets::upload_shadow::Read(row.instanceGpuBase,row.instanceResource,offset,batch->data(),bytes))batch->clear();
        else if(row.instances==1){std::memcpy(row.instanceData.data(),batch->data(),bytes);row.instanceDataBytes=UINT(bytes);}
        return;
    }
    void* mapped{};D3D12_RANGE read{SIZE_T(offset),SIZE_T(offset+bytes)};
    const auto hr=resource->Map(0,&read,&mapped);
    if(FAILED(hr) || !mapped){if(batch)batch->clear();if(SUCCEEDED(hr)){const D3D12_RANGE none{};resource->Unmap(0,&none);}return;}
    auto* output=batch?batch->data():row.instanceData.data();
    const auto address=reinterpret_cast<uintptr_t>(mapped);
    if(address<=UINT64_MAX-offset && ReadMemory(address+offset,output,bytes)) {
        if(row.instances==1){if(batch)std::memcpy(row.instanceData.data(),output,bytes);row.instanceDataBytes=UINT(bytes);}
    }else if(batch)batch->clear();
    const D3D12_RANGE noWrites{};resource->Unmap(0,&noWrites);
}
uint64_t Hash(const void* data,size_t length) {
    uint64_t h=1469598103934665603ull;const auto* bytes=static_cast<const uint8_t*>(data);
    for(size_t i=0;i<length;++i){h^=bytes[i];h*=1099511628211ull;}return h;
}
bool Target(const D3D12_SHADER_BYTECODE& vs,const D3D12_SHADER_BYTECODE& ps) {
    if(!depthShader.empty() && !ps.BytecodeLength && vs.BytecodeLength==3635 && vs.pShaderBytecode && Hash(vs.pShaderBytecode,vs.BytecodeLength)==0x3AC849D57E763A1Full)return true;
    if(ps.BytecodeLength!=9516 || !vs.pShaderBytecode || !ps.pShaderBytecode || Hash(ps.pShaderBytecode,ps.BytecodeLength)!=0x955C629FD0296DC3ull)return false;
    return (vs.BytecodeLength==7051 && Hash(vs.pShaderBytecode,vs.BytecodeLength)==0x02F5A04A6E812686ull) ||
        (!staticShader.empty() && vs.BytecodeLength==5238 && Hash(vs.pShaderBytecode,vs.BytecodeLength)==0x7BD6CC7EF414F11Aull);
}
void CameraBinding(PipelineRecord& record,ID3D12RootSignature* root) {
    const auto found=rootBlobs.find(root);if(found==rootBlobs.end())return;
    ComPtr<ID3D12VersionedRootSignatureDeserializer> deserializer;
    if(FAILED(D3D12CreateVersionedRootSignatureDeserializer(found->second.data(),found->second.size(),IID_PPV_ARGS(&deserializer))))return;
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc{};
    if(FAILED(deserializer->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1,&desc)))return;
    for(UINT i=0;i<desc->Desc_1_1.NumParameters;++i) {
        const auto& parameter=desc->Desc_1_1.pParameters[i];
        if(parameter.ShaderVisibility!=D3D12_SHADER_VISIBILITY_ALL && parameter.ShaderVisibility!=D3D12_SHADER_VISIBILITY_VERTEX)continue;
        if(parameter.ParameterType==D3D12_ROOT_PARAMETER_TYPE_CBV && parameter.Descriptor.ShaderRegister==1 && parameter.Descriptor.RegisterSpace==0) {
            record.cameraRoot=i;record.cameraTableOffset=UINT_MAX;return;
        }
        if(parameter.ParameterType!=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)continue;
        uint64_t offset{};
        for(UINT range=0;range<parameter.DescriptorTable.NumDescriptorRanges;++range) {
            const auto& r=parameter.DescriptorTable.pDescriptorRanges[range];
            if(r.OffsetInDescriptorsFromTableStart!=D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND)offset=r.OffsetInDescriptorsFromTableStart;
            if(r.RangeType==D3D12_DESCRIPTOR_RANGE_TYPE_CBV && r.RegisterSpace==0 && r.BaseShaderRegister<=1 &&
               uint64_t(r.BaseShaderRegister)+r.NumDescriptors>1 && offset+1-r.BaseShaderRegister<=UINT_MAX) {
                record.cameraRoot=i;record.cameraTableOffset=UINT(offset+1-r.BaseShaderRegister);return;
            }
            offset+=r.NumDescriptors;
        }
    }
}
template<class Create> void AddPipeline(ID3D12Device* device,ID3D12PipelineState* original,const PipelineStreamInfo& info,UINT samples,Create create) {
    std::lock_guard lock(pipelineMutex);
    const auto count=CyberpunkVR_NativeStereoPipelineCount.load(std::memory_order_relaxed);if(count>=PipelineCapacity)return;
    for(unsigned i=0;i<count;++i)if(originalPipelines[i].Get()==original)return;
    PipelineRecord record;record.original=reinterpret_cast<uintptr_t>(original);record.root=reinterpret_cast<uintptr_t>(info.root);
    record.targets=info.targets.NumRenderTargets;record.depthFormat=info.depthFormat;record.sampleCount=samples;
    for(UINT i=0;i<record.targets && i<8;++i)record.formats[i]=info.targets.RTFormats[i];
    CameraBinding(record,info.root);ComPtr<ID3D12Device2> d2;
    auto hr=device->QueryInterface(IID_PPV_ARGS(&d2));
    if(SUCCEEDED(hr))hr=create(d2.Get(),variants[count]);
    record.result=hr;record.instanced=reinterpret_cast<uintptr_t>(variants[count].Get());originalPipelines[count]=original;
    auto blend=info.blend;if(!info.targets.NumRenderTargets){blend={};for(auto& b:blend.RenderTarget)b.RenderTargetWriteMask=15;}
    scene_state::PipelineCreated(original,(info.haveBlend || !info.targets.NumRenderTargets) && info.haveDepth && !info.depthBounds && StereoGroupResolve::Supports(blend,info.depth));
    CyberpunkVR_NativeStereoPipelines[count]=record;CyberpunkVR_NativeStereoPipelineCount.store(count+1,std::memory_order_release);
}
template<class F> void Edit(ID3D12GraphicsCommandList* list,F update) {
    if(probe::internalCommands || !list || CyberpunkVR_NativeStereoProbeState.load(std::memory_order_relaxed)!=1)return;
    std::lock_guard lock(stateMutex);if(CyberpunkVR_NativeStereoProbeState.load(std::memory_order_relaxed)!=1)return;
    const auto found=states.find(list);if(found!=states.end()){update(found->second);return;}
    if(states.size()<512)update(states[list]);
}
}
bool Enabled() {
    std::call_once(initialization,[]{
        CyberpunkVR_NativeStereoProbeAssetStatus.store(1,std::memory_order_relaxed);
        HMODULE module{};wchar_t path[32768]{};
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&Enabled),&module) ||
           !GetModuleFileNameW(module,path,32768))return;
        const auto directory=std::filesystem::path(path).parent_path();
        auto load=[&](const wchar_t* name,std::vector<uint8_t>& target,uint64_t expected) {
            std::ifstream file(directory/name,std::ios::binary|std::ios::ate);if(!file)return false;
            CyberpunkVR_NativeStereoProbeAssetStatus.store(2,std::memory_order_relaxed);
            const auto size=file.tellg();if(size<32 || size>1024*1024)return false;
            target.resize(static_cast<size_t>(size));file.seekg(0);file.read(reinterpret_cast<char*>(target.data()),static_cast<std::streamsize>(size));
            return file.good() && Hash(target.data(),target.size())==expected;
        };
        // Exact offline DXC-validated variant of the captured shader. A different
        // file cannot silently replace the probe's camera-layout contract.
        enabled=load(L"CyberpunkVR_SinglePassProbeVS.dxil",shader,0xF2BCFF274A074AA2ull) &&
            load(L"CyberpunkVR_SinglePassProbePS.dxil",pixelShader,0xC9034583D4DD9609ull);
        if(enabled){if(!load(L"CyberpunkVR_SinglePassStaticVS.dxil",staticShader,0x0D370481852CBA69ull))staticShader.clear();else CyberpunkVR_NativeStereoStaticAssetStatus=1;CyberpunkVR_NativeStereoProbeAssetStatus.store(3,std::memory_order_release);}
        if(enabled){if(!load(L"CyberpunkVR_SinglePassDepthVS.dxil",depthShader,0x600DF2F70C8C3C86ull))depthShader.clear();CyberpunkVR_NativeStereoProbeAssetStatus.store(3,std::memory_order_release);}
        if(!enabled){shader.clear();pixelShader.clear();}
    });return enabled;
}
void RootCreated(ID3D12RootSignature* root,const void* data,size_t bytes) {
    if(!root || !data || !bytes || bytes>65536 || !Enabled())return;
    scene_state::RootCreated(root,data,bytes);
    std::lock_guard lock(pipelineMutex);if(rootBlobs.size()<128)rootBlobs[root].assign(static_cast<const uint8_t*>(data),static_cast<const uint8_t*>(data)+bytes);
}
void GraphicsCreated(ID3D12Device* device,const D3D12_GRAPHICS_PIPELINE_STATE_DESC& source,ID3D12PipelineState* original) {
    if(!original || !Enabled() || !Target(source.VS,source.PS))return;
    if(!source.PS.BytecodeLength && (source.NumRenderTargets || source.DSVFormat!=DXGI_FORMAT_D32_FLOAT_S8X24_UINT))return;
    PipelineStreamInfo info;info.vs=source.VS;info.ps=source.PS;info.root=source.pRootSignature;info.depthFormat=source.DSVFormat;info.targets.NumRenderTargets=source.NumRenderTargets;
    info.blend=source.BlendState;info.depth=source.DepthStencilState;info.haveBlend=info.haveDepth=true;
    if(source.NumRenderTargets>8)return;for(UINT i=0;i<source.NumRenderTargets;++i)info.targets.RTFormats[i]=source.RTVFormats[i];
    AddPipeline(device,original,info,source.SampleDesc.Count,[&](ID3D12Device2* d2,ComPtr<ID3D12PipelineState>& out){
        const auto& replacement=source.VS.BytecodeLength==3635?depthShader:source.VS.BytecodeLength==5238?staticShader:shader;
        auto desc=source;desc.VS={replacement.data(),replacement.size()};if(source.PS.BytecodeLength)desc.PS={pixelShader.data(),pixelShader.size()};ViewInstancedLayout layout;layout.allowMasking=true;
        return CreateViewInstancedPipeline(d2,desc,layout,&out);
    });
}
void StreamCreated(ID3D12Device* device,const D3D12_PIPELINE_STATE_STREAM_DESC& source,ID3D12PipelineState* original) {
    if(!original || !Enabled())return;PipelineStreamInfo info;if(!ReadPipelineStream(source,info) || !Target(info.vs,info.ps))return;
    if(!info.ps.BytecodeLength && (info.targets.NumRenderTargets || info.depthFormat!=DXGI_FORMAT_D32_FLOAT_S8X24_UINT))return;
    AddPipeline(device,original,info,info.samples.Count,[&](ID3D12Device2* d2,ComPtr<ID3D12PipelineState>& out){
        ViewInstancedLayout layout;layout.allowMasking=true;
        const auto& replacement=info.vs.BytecodeLength==3635?depthShader:info.vs.BytecodeLength==5238?staticShader:shader;
        return CreateViewInstancedPipelineStream(d2,source,{replacement.data(),replacement.size()},layout,&out,info.ps.BytecodeLength?D3D12_SHADER_BYTECODE{pixelShader.data(),pixelShader.size()}:D3D12_SHADER_BYTECODE{});
    });
}
void FrameBoundary() {
    if(CyberpunkVR_NativeStereoProbeState.load(std::memory_order_relaxed)==1 || CyberpunkVR_NativeStereoProbeRequest.load(std::memory_order_relaxed))scene_state::ResetCapture();
    if(CyberpunkVR_NativeStereoProbeState.load(std::memory_order_relaxed)!=1 && !CyberpunkVR_NativeStereoProbeRequest.load(std::memory_order_relaxed)) {
        if(haveInstanceSources.exchange(false,std::memory_order_acq_rel)){std::lock_guard lock(stateMutex);instanceSources.clear();states.clear();}
        return;
    }
    std::lock_guard lock(stateMutex);
    if(CyberpunkVR_NativeStereoProbeState.load(std::memory_order_relaxed)==1) {
        if(++frame>limit)CyberpunkVR_NativeStereoProbeState.store(2,std::memory_order_release);
        states.clear();instanceSources.clear();haveInstanceSources.store(false,std::memory_order_release);return;
    }
    const auto request=CyberpunkVR_NativeStereoProbeRequest.exchange(0,std::memory_order_acq_rel);if(!request)return;
    CyberpunkVR_NativeStereoProbeSeq.fetch_add(1,std::memory_order_acq_rel);states.clear();instanceSources.clear();haveInstanceSources.store(false,std::memory_order_release);frame=1;limit=std::min(request,4u);
    CyberpunkVR_NativeStereoDrawCount.store(0,std::memory_order_relaxed);CyberpunkVR_NativeStereoProbeSeq.fetch_add(1,std::memory_order_release);
    CyberpunkVR_NativeStereoProbeState.store(1,std::memory_order_release);
}
void Reset(ID3D12GraphicsCommandList* list,ID3D12PipelineState* pso,bool rateKnown){scene_state::Reset(list,rateKnown);Edit(list,[&](auto& state){instanceSources.erase(list);state={};state.flags=1;state.pipeline=reinterpret_cast<uintptr_t>(pso);});}
void Pipeline(ID3D12GraphicsCommandList* list,ID3D12PipelineState* pso){Edit(list,[&](auto& state){state.pipeline=reinterpret_cast<uintptr_t>(pso);});}
void VertexBuffers(ID3D12GraphicsCommandList* list,UINT start,UINT count,const D3D12_VERTEX_BUFFER_VIEW* views) {
    Edit(list,[&](auto& s){for(UINT i=0;i<count && start+i<16;++i){s.vertices[start+i]=views?views[i]:D3D12_VERTEX_BUFFER_VIEW{};s.vertexMask|=1u<<(start+i);}
        if(start>7 || count<=7-start)return;
        instanceSources.erase(list);s.instanceResource=s.instanceGpuBase=s.instanceBytesTotal=0;s.instanceHeapType=s.instanceDataBytes=0;s.instanceData={};
        const auto address=NativeInstanceResource();if(!address || !views)return;
        ComPtr<ID3D12Resource> resource=reinterpret_cast<ID3D12Resource*>(address);
        D3D12_HEAP_PROPERTIES heap{};D3D12_HEAP_FLAGS flags{};
        if(FAILED(resource->GetHeapProperties(&heap,&flags)) || (heap.Type!=D3D12_HEAP_TYPE_UPLOAD &&
            !((CyberpunkVR_NativeStereoAllDraws.load(std::memory_order_relaxed) || CyberpunkVR_StereoGpuProbeDepthPrepass.load(std::memory_order_relaxed)) && heap.Type==D3D12_HEAP_TYPE_DEFAULT)))return;
        const auto desc=resource->GetDesc();const auto gpu=resource->GetGPUVirtualAddress();const auto& view=s.vertices[7];
        if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER || view.BufferLocation<gpu || view.BufferLocation-gpu>desc.Width || view.SizeInBytes>desc.Width-(view.BufferLocation-gpu))return;
        s.instanceResource=address;s.instanceGpuBase=gpu;s.instanceBytesTotal=desc.Width;s.instanceHeapType=heap.Type;
        instanceSources[list]=std::move(resource);haveInstanceSources.store(true,std::memory_order_release);
    });
}
void IndexBuffer(ID3D12GraphicsCommandList* list,const D3D12_INDEX_BUFFER_VIEW* view){Edit(list,[&](auto& s){s.index=view?*view:D3D12_INDEX_BUFFER_VIEW{};s.flags|=2;});}
void RootSignature(ID3D12GraphicsCommandList* list,ID3D12RootSignature* root){scene_state::Root(list,root);Edit(list,[&](auto& s){if(s.root!=reinterpret_cast<uintptr_t>(root)){s.root=reinterpret_cast<uintptr_t>(root);s.tables={};s.cbvs={};s.tableMask=s.cbvMask=0;}s.flags|=4;});}
void RootTable(ID3D12GraphicsCommandList* list,UINT index,D3D12_GPU_DESCRIPTOR_HANDLE h){scene_state::Table(list,index,h);if(index<32)Edit(list,[&](auto& s){s.tables[index]=h.ptr;s.tableMask|=1u<<index;});}
void RootCbv(ID3D12GraphicsCommandList* list,UINT index,D3D12_GPU_VIRTUAL_ADDRESS a){if(index<32)Edit(list,[&](auto& s){s.cbvs[index]=a;s.cbvMask|=1u<<index;});}
void Predication(ID3D12GraphicsCommandList* list,ID3D12Resource* buffer){Edit(list,[&](auto& s){s.flags=(s.flags&~64u)|(buffer?64u:0u);});}
void Targets(ID3D12GraphicsCommandList* list,UINT count,const D3D12_CPU_DESCRIPTOR_HANDLE* handles,BOOL contiguous,const D3D12_CPU_DESCRIPTOR_HANDLE* depth) {
    Edit(list,[&](auto& s){s.targets={};s.depth=depth?depth->ptr:0;s.rtCount=count;s.rtContiguous=contiguous;
        if(handles)for(UINT i=0;i<std::min(count,contiguous?1u:8u);++i)s.targets[i]=handles[i].ptr;s.flags|=8;});
}
void Viewports(ID3D12GraphicsCommandList* list,UINT count,const D3D12_VIEWPORT* values) {
    Edit(list,[&](auto& s){s.flags&=~16u;if(count==1 && values){s.viewport=*values;s.flags|=16;}});
}
void Scissors(ID3D12GraphicsCommandList* list,UINT count,const D3D12_RECT* values) {
    Edit(list,[&](auto& s){s.flags&=~32u;if(count==1 && values){s.scissor=*values;s.flags|=32;}});
}
bool Draw(ID3D12GraphicsCommandList* list,ID3D12PipelineState* pso,uint32_t node,int side,UINT indices,UINT instances,UINT first,INT base,UINT start) {
    if(node!=0x23A938 || side<0 || side>1 || CyberpunkVR_NativeStereoProbeState.load(std::memory_order_relaxed)!=1)return false;
    bool candidate{};ID3D12PipelineState* variant{};const auto count=CyberpunkVR_NativeStereoPipelineCount.load(std::memory_order_acquire);
    for(unsigned i=0;i<count;++i)if(CyberpunkVR_NativeStereoPipelines[i].original==reinterpret_cast<uintptr_t>(pso)) {
        candidate=true;const auto& pipeline=CyberpunkVR_NativeStereoPipelines[i];
        if((pipeline.targets==0 || (pipeline.targets==3 && pipeline.formats[0]==24 && pipeline.formats[1]==24 && pipeline.formats[2]==28)) && pipeline.depthFormat==20 && pipeline.sampleCount==1)
            variant=reinterpret_cast<ID3D12PipelineState*>(pipeline.instanced);
    }
    if(CyberpunkVR_StereoGpuProbeSceneDepth.load(std::memory_order_relaxed) &&
       (CyberpunkVR_StereoGpuProbeState.load(std::memory_order_acquire)==1 || CyberpunkVR_StereoGpuProbeRequest.load(std::memory_order_relaxed))) {
        DrawRecord current{};Edit(list,[&](const auto& saved){current=saved;});current.side=side;current.frame=frame;
        current.pipeline=reinterpret_cast<uintptr_t>(pso);gpu_probe::Boundary(list,current,true,variant);
    }
    if(!candidate && !CyberpunkVR_NativeStereoAllDraws.load(std::memory_order_relaxed))return false;
    DrawRecord snapshot{};bool copied{};std::vector<uint8_t> instanceBatch;
    Edit(list,[&](const DrawRecord& saved){if(saved.pipeline!=reinterpret_cast<uintptr_t>(pso))return;const auto n=CyberpunkVR_NativeStereoDrawCount.load(std::memory_order_relaxed);
        if(n==DrawCapacity){CyberpunkVR_NativeStereoProbeState.store(3,std::memory_order_release);return;}
        CyberpunkVR_NativeStereoProbeSeq.fetch_add(1,std::memory_order_acq_rel);auto& r=CyberpunkVR_NativeStereoDraws[n];r=saved;
        LARGE_INTEGER now{};QueryPerformanceCounter(&now);r.sequence=n+1;r.qpc=now.QuadPart;r.frame=frame;r.node=node;r.side=side;
        r.list=reinterpret_cast<uintptr_t>(list);r.pipeline=reinterpret_cast<uintptr_t>(pso);r.indices=indices;r.instances=instances;
        r.firstIndex=first;r.baseVertex=base;r.firstInstance=start;
        r.thread=GetCurrentThreadId();
        const bool depthBatch=variant && !r.rtCount && CyberpunkVR_StereoGpuProbeDepthPrepass.load(std::memory_order_relaxed) &&
            (CyberpunkVR_StereoGpuProbeRequest.load(std::memory_order_relaxed) || CyberpunkVR_StereoGpuProbeState.load(std::memory_order_relaxed)==1);
        const auto source=instanceSources.find(list);if(source!=instanceSources.end())CopyInstance(source->second.Get(),r,depthBatch?&instanceBatch:nullptr);
        CyberpunkVR_NativeStereoDrawCount.store(n+1,std::memory_order_relaxed);CyberpunkVR_NativeStereoProbeSeq.fetch_add(1,std::memory_order_release);
        if(CyberpunkVR_StereoGpuProbeRequest.load(std::memory_order_relaxed) ||
           (CyberpunkVR_StereoGpuProbeNativeReference.load(std::memory_order_relaxed) && CyberpunkVR_StereoGpuProbeState.load(std::memory_order_relaxed)==1)){snapshot=r;copied=true;}
    });
    if(copied){gpu_probe::Draw(list,variant,snapshot,instanceBatch);return gpu_probe::RouteNative(list,snapshot);}
    return false;
}
bool CurrentState(ID3D12GraphicsCommandList* list,DrawRecord& result){bool found=false;Edit(list,[&](const auto& s){result=s;found=true;});return found;}
}
