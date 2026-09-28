#include "Render/NativeStereoCameraUpload.hpp"
#include "Render/NativeStereoProbe.hpp"
#include "Render/SinglePassTrace.hpp"
#include <windows.h>
#include <wrl/client.h>
#include <array>
#include <mutex>
#include <cstring>
using Microsoft::WRL::ComPtr;
extern "C" {
__declspec(dllexport) std::atomic<uint32_t> CyberpunkVR_SinglePassTwinUpload{0};
__declspec(dllexport) std::atomic<uint32_t> CyberpunkVR_SinglePassTwinAllocations{0},CyberpunkVR_SinglePassTwinBindings{0},CyberpunkVR_SinglePassTwinFailures{0};
}
namespace cvr::stereo::camera_upload {
namespace {
std::mutex mutex;
ComPtr<ID3D12DescriptorHeap> descriptors;
UINT stride{},next{};
constexpr UINT Capacity=128;
thread_local Result* expectedResult{};
struct Pending {uint32_t nativeHandle{};Result result{};};
thread_local Pending pending;
thread_local Result currentBinding;
bool Read(uintptr_t address,void* output,size_t bytes) {
    SIZE_T copied{};return address && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),output,bytes,&copied) && copied==bytes;
}
}
Result Prepare(uintptr_t base,uint32_t nativeHandle,const ShaderCamera& first,const ShaderCamera& second) {
    pending={};
    if(!base || !nativeHandle || !CyberpunkVR_SinglePassTwinUpload.load(std::memory_order_relaxed) ||
       CyberpunkVR_SinglePassTraceState.load(std::memory_order_relaxed)!=1 || !native_probe::Enabled())return {};
    // A validated native prototype must exist before attempting its camera path.
    const auto count=CyberpunkVR_NativeStereoPipelineCount.load(std::memory_order_acquire);ID3D12PipelineState* pipeline{};
    for(unsigned i=0;i<count;++i)if(CyberpunkVR_NativeStereoPipelines[i].instanced) {
        pipeline=reinterpret_cast<ID3D12PipelineState*>(CyberpunkVR_NativeStereoPipelines[i].original);break;
    }
    if(!pipeline)return {};
    std::lock_guard lock(mutex);
    if(next>=Capacity)return {};
    if(!descriptors) {
        // The probe retains the original PSO, which supplies its actual device.
        ComPtr<ID3D12Device> device;
        if(FAILED(pipeline->GetDevice(IID_PPV_ARGS(&device)))){++CyberpunkVR_SinglePassTwinFailures;return {};}
        D3D12_DESCRIPTOR_HEAP_DESC desc{};desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;desc.NumDescriptors=Capacity;
        if(FAILED(device->CreateDescriptorHeap(&desc,IID_PPV_ARGS(&descriptors)))){++CyberpunkVR_SinglePassTwinFailures;return {};}
        stride=device->GetDescriptorHandleIncrementSize(desc.Type);
    }
    alignas(16) std::array<uint8_t,2048> data{};
    std::memcpy(data.data(),first.data(),first.size());std::memcpy(data.data()+1024,second.data(),second.size());
    Result result;result.cpuDescriptor=descriptors->GetCPUDescriptorHandleForHeapStart().ptr+next++*stride;
    using Upload=void(*)(uint32_t,const void*,D3D12_CPU_DESCRIPTOR_HANDLE);
    const auto upload=reinterpret_cast<Upload>(base+0x1F0114);
    expectedResult=&result;upload(UINT(data.size()),data.data(),{result.cpuDescriptor});expectedResult=nullptr;
    if(result.bytes!=data.size() || !result.gpuAddress || (result.gpuAddress&255)) {++CyberpunkVR_SinglePassTwinFailures;return {};}
    ++CyberpunkVR_SinglePassTwinAllocations;pending={nativeHandle,result};return result;
}
void CreatedCbv(const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE destination) {
    if(!expectedResult || !desc || destination.ptr!=expectedResult->cpuDescriptor)return;
    expectedResult->gpuAddress=desc->BufferLocation;expectedResult->bytes=desc->SizeInBytes;
}
void AfterNativeBind(uintptr_t base,uint32_t binding,uint32_t nativeHandle,uint32_t stages,bool sceneVrcam) {
    if(binding==1)currentBinding={};
    const auto upload=pending.result;const auto descriptor=upload.cpuDescriptor;
    if(!descriptor || binding!=1 || nativeHandle!=pending.nativeHandle)return;
    pending={};
    if(!base || !sceneVrcam || !stages || stages>3 || CyberpunkVR_SinglePassTraceState.load(std::memory_order_relaxed)!=1 ||
       !CyberpunkVR_SinglePassTwinUpload.load(std::memory_order_relaxed))return;
    using Current=void*(*)();using Bind=void(*)(void*,uint8_t,uint32_t,uint32_t,const uint64_t*,bool);
    const auto current=reinterpret_cast<Current>(base+0x1F405C)();uintptr_t state{};
    if(!Read(reinterpret_cast<uintptr_t>(current)+0x60,&state,sizeof(state)) || !state){++CyberpunkVR_SinglePassTwinFailures;return;}
    const auto bind=reinterpret_cast<Bind>(base+0x1F3978);
    for(unsigned stage=0;stage<2;++stage)if(stages&(1u<<stage))bind(reinterpret_cast<void*>(state),uint8_t(stage),2,binding,&descriptor,true);
    currentBinding=upload;++CyberpunkVR_SinglePassTwinBindings;
}
Result CurrentBinding(){return CyberpunkVR_SinglePassTwinUpload.load(std::memory_order_relaxed) && CyberpunkVR_SinglePassTraceState.load(std::memory_order_relaxed)==1?currentBinding:Result{};}
}
