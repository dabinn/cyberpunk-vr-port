#pragma once
#include <d3d12.h>
#include <atomic>
#include <cstdint>
#include "Render/StereoShaderCamera.hpp"

namespace cvr::stereo::camera_upload {
struct Result {uint64_t cpuDescriptor{},gpuAddress{};uint32_t bytes{},reserved{};};
// One-shot comparison path. Uses the engine's current upload allocator and an
// owned CPU-only descriptor. Ordinary shaders still read the unchanged prefix.
Result Prepare(uintptr_t gameBase,uint32_t nativeHandle,const ShaderCamera& first,const ShaderCamera& second);
void CreatedCbv(const D3D12_CONSTANT_BUFFER_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE);
Result CurrentBinding();
void AfterNativeBind(uintptr_t gameBase,uint32_t binding,uint32_t nativeHandle,uint32_t stages,bool sceneVrcam);
}
extern "C" {
__declspec(dllexport) extern std::atomic<uint32_t> CyberpunkVR_SinglePassTwinUpload;
__declspec(dllexport) extern std::atomic<uint32_t> CyberpunkVR_SinglePassTwinAllocations,CyberpunkVR_SinglePassTwinBindings,CyberpunkVR_SinglePassTwinFailures;
}
