#include "Render/NativeStereoProbe.hpp"
#include "Render/StereoGpuProbe.hpp"
#include "Render/RenderProbeScope.hpp"
#include <thread>
#include <cstdio>
#include <stdexcept>
using namespace cvr::stereo::native_probe;
extern "C" { __declspec(dllexport) std::atomic<uint32_t> CyberpunkVR_StereoGpuProbeRequest{0},CyberpunkVR_StereoGpuProbeNativeReference{0},CyberpunkVR_StereoGpuProbeState{0},CyberpunkVR_StereoGpuProbeSceneDepth{0},CyberpunkVR_StereoGpuProbeDepthPrepass{0}; }
namespace cvr::stereo::gpu_probe {
void Boundary(ID3D12GraphicsCommandList*,const native_probe::DrawRecord&,bool,ID3D12PipelineState*){throw std::runtime_error("Disabled scene boundary");}
bool RouteNative(ID3D12GraphicsCommandList*,const native_probe::DrawRecord&){return false;}
void Draw(ID3D12GraphicsCommandList*,ID3D12PipelineState*,const native_probe::DrawRecord&,std::span<const uint8_t>){throw std::runtime_error("GPU work must not run in a disabled CPU snapshot test");}
}
static void Require(bool v){if(!v)throw std::runtime_error("Native probe snapshot assertion failed");}
int main() try {
    auto* pso=reinterpret_cast<ID3D12PipelineState*>(0x2000);
    auto* listA=reinterpret_cast<ID3D12GraphicsCommandList*>(0x3000);
    auto* listB=reinterpret_cast<ID3D12GraphicsCommandList*>(0x4000);
    auto* root=reinterpret_cast<ID3D12RootSignature*>(0x5000);
    CyberpunkVR_NativeStereoPipelines[0].original=reinterpret_cast<uintptr_t>(pso);CyberpunkVR_NativeStereoPipelineCount=1;
    Draw(listA,pso,0x23A938,1,36,1,0,0,0);Require(CyberpunkVR_NativeStereoDrawCount==0);
    CyberpunkVR_NativeStereoProbeRequest=2;FrameBoundary();
    Reset(listA,pso);Reset(listB,pso);RootSignature(listA,root);RootSignature(listB,root);
    D3D12_VERTEX_BUFFER_VIEW a{0x110000,64,64},b{0x220000,128,64};
    D3D12_INDEX_BUFFER_VIEW index{0x330000,72,DXGI_FORMAT_R16_UINT};
    VertexBuffers(listA,7,1,&a);VertexBuffers(listB,7,1,&b);IndexBuffer(listA,&index);
    RootTable(listA,2,{0x440000});RootCbv(listB,3,0x550000);
    const D3D12_VIEWPORT vp{0,0,1485,1485,0,1};const D3D12_RECT rect{0,0,1485,1485};Viewports(listA,1,&vp);Scissors(listA,1,&rect);
    {cvr::stereo::probe::Scope internal;RootTable(listA,2,{0xBAD});}
    // Contiguous RTV binds expose only one caller-provided handle even for MRT.
    D3D12_CPU_DESCRIPTOR_HANDLE target{0x660000};Targets(listA,4,&target,TRUE,nullptr);
    std::thread t1([&]{Draw(listA,pso,0x23A938,1,36,1,0,0,3);});
    std::thread t2([&]{Draw(listB,pso,0x23A938,0,36,2,0,0,5);});t1.join();t2.join();
    Require(CyberpunkVR_NativeStereoDrawCount==2);
    for(unsigned n=0;n<2;++n) {
        const auto& r=CyberpunkVR_NativeStereoDraws[n];Require(r.sequence==n+1);
        if(r.list==reinterpret_cast<uintptr_t>(listA)){Require(r.vertices[7].BufferLocation==a.BufferLocation && r.index.BufferLocation==index.BufferLocation);
            Require(r.tables[2]==0x440000 && r.cbvMask==0 && r.rtCount==4 && r.targets[1]==0 && r.flags==63 && r.viewport.Width==1485);}
        else Require(r.vertices[7].BufferLocation==b.BufferLocation && r.cbvs[3]==0x550000 && r.tableMask==0);
    }
    FrameBoundary();Pipeline(listA,pso);Draw(listA,pso,0x23A938,1,36,1,0,0,0);
    const auto& cleared=CyberpunkVR_NativeStereoDraws[2];Require(cleared.vertexMask==0 && cleared.root==0 && cleared.tableMask==0);
    FrameBoundary();Require(CyberpunkVR_NativeStereoProbeState==2);const auto count=CyberpunkVR_NativeStereoDrawCount.load();
    Draw(listA,pso,0x23A938,1,36,1,0,0,0);Require(CyberpunkVR_NativeStereoDrawCount==count);
    CyberpunkVR_NativeStereoProbeRequest=1;FrameBoundary();auto* other=reinterpret_cast<ID3D12PipelineState*>(0x9000);
    Reset(listA,other);Draw(listA,other,0x23A938,1,36,1,0,0,0);Require(CyberpunkVR_NativeStereoDrawCount==0);
    CyberpunkVR_NativeStereoAllDraws=1;Draw(listA,other,0x23A938,1,36,1,0,0,0);
    Require(CyberpunkVR_NativeStereoDrawCount==1 && CyberpunkVR_NativeStereoDraws[0].thread!=0 && CyberpunkVR_NativeStereoDraws[0].pipeline==reinterpret_cast<uintptr_t>(other));
    Predication(listA,reinterpret_cast<ID3D12Resource*>(0xA000));
    {cvr::stereo::probe::Scope internal;Predication(listA,nullptr);}
    Draw(listA,other,0x23A938,1,36,1,0,0,0);Require((CyberpunkVR_NativeStereoDraws[1].flags&64)!=0);
    Predication(listA,nullptr);Draw(listA,other,0x23A938,1,36,1,0,0,0);Require((CyberpunkVR_NativeStereoDraws[2].flags&64)==0);
    CyberpunkVR_NativeStereoAllDraws=0;FrameBoundary();
    Require(!(CyberpunkVR_NativeStereoProbeSeq.load()&1));
    std::puts("PASS disabled recording, independent interleaved lists, concurrent publication, bounded frames, contiguous MRT and stale-state reset");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}
