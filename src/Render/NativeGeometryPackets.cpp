#include "Render/NativeGeometryPackets.hpp"
#include "Render/NativePacketInputs.hpp"
#include "Render/NativeUploadShadow.hpp"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>
#include <new>

extern "C" {
__declspec(dllexport) uint32_t CyberpunkVR_GeometryPacketRecordBytes=sizeof(cvr::stereo::packets::Record);
__declspec(dllexport) std::atomic<uint32_t> CyberpunkVR_GeometryPacketRequest{0},CyberpunkVR_GeometryPacketState{0};
__declspec(dllexport) std::atomic<uint32_t> CyberpunkVR_GeometryPacketSeq{0},CyberpunkVR_GeometryPacketCount{0},CyberpunkVR_GeometryPacketDataCount{0};
__declspec(dllexport) cvr::stereo::packets::Record CyberpunkVR_GeometryPacketRecords[cvr::stereo::packets::RecordCapacity]{};
__declspec(dllexport) cvr::stereo::packets::Packet CyberpunkVR_GeometryPacketData[cvr::stereo::packets::PacketCapacity]{};
__declspec(dllexport) std::atomic<uint32_t> CyberpunkVR_GeometryPacketResolve{0},CyberpunkVR_GeometryPacketInputCount{0},CyberpunkVR_GeometryPacketInputFailure{0};
__declspec(dllexport) uint32_t CyberpunkVR_GeometryPacketInputBytes=sizeof(cvr::stereo::packets::Inputs);
__declspec(dllexport) uint64_t CyberpunkVR_GeometryPacketInputs{};
__declspec(dllexport) uint64_t CyberpunkVR_GeometryPacketInputBlob{};
__declspec(dllexport) std::atomic<uint32_t> CyberpunkVR_GeometryPacketInputBlobBytes{0};
}
namespace cvr::stereo::packets {
namespace {
std::mutex mutex;
uint64_t epoch{};
uint32_t frame{},limit{};
std::vector<Inputs> inputs;
std::vector<uint8_t> inputBlob;
bool resolveInputs{};
constexpr uint32_t MaxInputPackets=65536;
bool Read(uint64_t address,void* destination,size_t size) {
    SIZE_T read{};
    return address && address<=UINT64_MAX-size &&
        ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(address),destination,size,&read) && read==size;
}
uint64_t Now(){LARGE_INTEGER value{};QueryPerformanceCounter(&value);return value.QuadPart;}
bool ReadInput(void* context,uint64_t address,void* output,size_t bytes){
    if(address>=upload_shadow::AddressTag && address-upload_shadow::AddressTag<UploadShadow::Limit) {
        const auto* source=static_cast<const MappedInstances*>(context);
        return source && upload_shadow::Read(source->gpu,source->resource,address-upload_shadow::AddressTag,output,bytes);
    }
    return Read(address,output,bytes);
}
MappedInstances Uploaded(uint64_t renderer) {
    MappedInstances result{};uint64_t registry{};uint32_t handle{};
    const auto game=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if(renderer && renderer<=UINT64_MAX-0x204 && Read(renderer+0x200,&handle,4) && handle && handle<=0x100000 &&
        Read(game+0x3438A28,&registry,8) && registry && registry<=UINT64_MAX-0x5C0B40-uint64_t(handle-1)*0xB0) {
        std::array<uint8_t,72> record{};
        if(Read(registry+uint64_t(handle-1)*0xB0+0x5C0AF8,record.data(),record.size())) {
            uint32_t bytes{};std::memcpy(&bytes,record.data(),4);result.bytes=bytes;
            std::memcpy(&result.gpu,record.data()+8,8);std::memcpy(&result.resource,record.data()+16,8);std::memcpy(&result.cpu,record.data()+64,8);
        }
    }
    return result;
}
void ClearInputs() {
    std::vector<Inputs>().swap(inputs);CyberpunkVR_GeometryPacketInputs=0;
    std::vector<uint8_t>().swap(inputBlob);CyberpunkVR_GeometryPacketInputBlob=0;CyberpunkVR_GeometryPacketInputBlobBytes=0;
    CyberpunkVR_GeometryPacketInputCount.store(0,std::memory_order_release);resolveInputs=false;
    upload_shadow::Watch(0,0,0);
}
void CopyInputs(const Record& row) {
    if(!resolveInputs || !row.count || CyberpunkVR_GeometryPacketInputFailure.load(std::memory_order_relaxed))return;
    if(row.offset+row.count>MaxInputPackets){CyberpunkVR_GeometryPacketInputFailure=1;return;}
    try {
        inputs.resize(row.offset+row.count);
        uint64_t global{},pool{},renderer{};
        const auto game=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        // The consumer loads this exact pool from [global+0x4628]. Check that
        // [global+0x80] is also the renderer argument before decoding anything.
        if(Read(game+0x3427C00,&global,8) && global && global<=UINT64_MAX-0x4630 &&
           Read(global+0x80,&renderer,8) && renderer==row.renderer && Read(global+0x4628,&pool,8)) {
            auto uploaded=Uploaded(renderer);
            if(!uploaded.cpu)uploaded.cpu=upload_shadow::AddressTag;
            for(uint32_t i=0;i<row.count;++i)inputs[row.offset+i]=ResolveDetails(CyberpunkVR_GeometryPacketData[row.offset+i],pool,uploaded,ReadInput,&uploaded,inputBlob);
        }else CyberpunkVR_GeometryPacketInputFailure=3;
    }catch(const std::bad_alloc&){CyberpunkVR_GeometryPacketInputFailure=2;}
    // Publish current owners even after allocation failure; an earlier resize
    // may have moved them. Readers reject the failure instead of a stale pointer.
    CyberpunkVR_GeometryPacketInputs=reinterpret_cast<uintptr_t>(inputs.data());
    CyberpunkVR_GeometryPacketInputBlob=reinterpret_cast<uintptr_t>(inputBlob.data());CyberpunkVR_GeometryPacketInputBlobBytes.store(static_cast<uint32_t>(inputBlob.size()),std::memory_order_release);
    CyberpunkVR_GeometryPacketInputCount.store(static_cast<uint32_t>(inputs.size()),std::memory_order_release);
}
void NativeGroup(Record& row) {
    __try {
        const auto game=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        CONTEXT context{};RtlCaptureContext(&context);
        ULONG_PTR low{},high{};GetCurrentThreadStackLimits(&low,&high);
        for(unsigned depth=0;depth<24 && context.Rsp>=low && context.Rsp+8<=high;++depth) {
            if(context.Rip>=game+0x23A938 && context.Rip<game+0x23AF5B) {
                // At both native packet-consumer call sites RBX is six times
                // the section index and R14 points to its 48-byte definitions.
                if(context.Rbx>6*1024 || context.Rbx%6 || context.R14>UINT64_MAX-context.Rbx*8)return;
                row.nodeDefinition=context.R15;row.entry=context.R14+context.Rbx*8;
                if(context.Rbp>=low+0x78 && context.Rbp-0x70<=high &&
                    Read(context.Rbp-0x78,&row.plane,8) && Read(row.entry,row.groupBytes.data(),row.groupBytes.size()))row.metadataFlags|=4;
                return;
            }
            DWORD64 image{};const auto function=RtlLookupFunctionEntry(context.Rip,&image,nullptr);
            if(function){PVOID handler{};DWORD64 establisher{};RtlVirtualUnwind(UNW_FLAG_NHANDLER,image,context.Rip,function,&context,&handler,&establisher,nullptr);}
            else {if(!Read(context.Rsp,&context.Rip,8))return;context.Rsp+=8;}
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}
}
void FrameBoundary() {
    if(CyberpunkVR_GeometryPacketState.load(std::memory_order_relaxed)!=1 &&
       !CyberpunkVR_GeometryPacketRequest.load(std::memory_order_relaxed)) {
        if(!CyberpunkVR_GeometryPacketResolve.load(std::memory_order_relaxed) && (CyberpunkVR_GeometryPacketInputCount.load(std::memory_order_acquire) || upload_shadow::Active())) {
            std::lock_guard lock(mutex);CyberpunkVR_GeometryPacketSeq.fetch_add(1,std::memory_order_acq_rel);
            ClearInputs();CyberpunkVR_GeometryPacketSeq.fetch_add(1,std::memory_order_release);
        }
        return;
    }
    std::lock_guard lock(mutex);
    if(CyberpunkVR_GeometryPacketState.load(std::memory_order_relaxed)==1) {
        if(++frame>limit){upload_shadow::Stop();CyberpunkVR_GeometryPacketState.store(2,std::memory_order_release);}
        return;
    }
    const auto request=CyberpunkVR_GeometryPacketRequest.exchange(0,std::memory_order_acq_rel);if(!request)return;
    CyberpunkVR_GeometryPacketSeq.fetch_add(1,std::memory_order_acq_rel);
    ++epoch;frame=1;limit=std::min(request,4u);
    CyberpunkVR_GeometryPacketCount.store(0,std::memory_order_relaxed);CyberpunkVR_GeometryPacketDataCount.store(0,std::memory_order_relaxed);
    const auto resolveMode=CyberpunkVR_GeometryPacketResolve.load(std::memory_order_relaxed);
    ClearInputs();resolveInputs=resolveMode==1;CyberpunkVR_GeometryPacketInputFailure=0;
    // Mode 2 observes only the native instance uploads for a bounded GPU probe.
    // It does not resolve/copy every material program and object in the scene.
    if(resolveMode) {
        uint64_t global{},renderer{};const auto game=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if(Read(game+0x3427C00,&global,8) && global && global<=UINT64_MAX-0x88 && Read(global+0x80,&renderer,8)) {
            const auto source=Uploaded(renderer);if(!source.cpu)upload_shadow::Watch(source.gpu,source.resource,source.bytes);
        }
    }
    CyberpunkVR_GeometryPacketSeq.fetch_add(1,std::memory_order_release);
    CyberpunkVR_GeometryPacketState.store(1,std::memory_order_release);
}
Ticket Begin(uint32_t node,int side,const void* renderer,const void* arguments,const void* span) {
    if(node!=0x23A938 || side<0 || side>1 || CyberpunkVR_GeometryPacketState.load(std::memory_order_relaxed)!=1)return {};
    std::lock_guard lock(mutex);
    if(CyberpunkVR_GeometryPacketState.load(std::memory_order_relaxed)!=1)return {};
    const auto index=CyberpunkVR_GeometryPacketCount.load(std::memory_order_relaxed);
    const auto offset=CyberpunkVR_GeometryPacketDataCount.load(std::memory_order_relaxed);
    if(index>=RecordCapacity){CyberpunkVR_GeometryPacketState.store(3,std::memory_order_release);return {};}
    Record row{};row.sequence=index+1;row.beginQpc=Now();row.renderer=reinterpret_cast<uintptr_t>(renderer);
    row.arguments=reinterpret_cast<uintptr_t>(arguments);row.frame=frame;row.node=node;row.side=side;row.thread=GetCurrentThreadId();row.offset=offset;
    auto status=Status::BadArguments;
    bool full=false;
    if(Read(row.arguments,row.argumentBytes.data(),row.argumentBytes.size())) {
        std::memcpy(&row.context,row.argumentBytes.data(),sizeof(row.context));
        if(row.context && row.context<=UINT64_MAX-0x18 && Read(row.context+0x18,&row.view,sizeof(row.view))) {
            row.metadataFlags|=1;
            if(row.view && row.view<=UINT64_MAX-0x1E10 && Read(row.view+0x1E10,&row.storage,sizeof(row.storage)))row.metadataFlags|=2;
        }
        uint64_t range[2]{};
        status=Status::BadSpan;
        if(Read(reinterpret_cast<uintptr_t>(span),range,sizeof(range))) {
            row.begin=range[0];row.end=range[1];
            if(row.end>=row.begin && (row.end-row.begin)%sizeof(Packet)==0 && (row.begin%alignof(Packet))==0) {
                const auto count=(row.end-row.begin)/sizeof(Packet);
                if(!count)status=Status::Empty;
                else if(!row.begin)status=Status::BadSpan;
                else if(count>MaxSpanPackets)status=Status::TooLarge;
                else if(count>PacketCapacity-offset){status=Status::TooLarge;full=true;}
                else {
                    status=Status::Unreadable;
                    // The caller owns this immutable span throughout the native
                    // call. Copy it now, before the shared view scratch is reused.
                    if(Read(row.begin,CyberpunkVR_GeometryPacketData+offset,size_t(count)*sizeof(Packet))) {
                        row.count=static_cast<uint32_t>(count);status=Status::Copied;
                    }
                }
            }
        }
    }
    row.status=static_cast<uint32_t>(status);
    NativeGroup(row);
    CyberpunkVR_GeometryPacketSeq.fetch_add(1,std::memory_order_acq_rel);
    CopyInputs(row);
    CyberpunkVR_GeometryPacketRecords[index]=row;
    CyberpunkVR_GeometryPacketDataCount.store(offset+row.count,std::memory_order_relaxed);
    CyberpunkVR_GeometryPacketCount.store(index+1,std::memory_order_relaxed);
    CyberpunkVR_GeometryPacketSeq.fetch_add(1,std::memory_order_release);
    if(full)CyberpunkVR_GeometryPacketState.store(3,std::memory_order_release);
    return {epoch,index};
}
void End(Ticket ticket) {
    if(ticket.index==UINT32_MAX)return;
    std::lock_guard lock(mutex);
    if(ticket.epoch!=epoch || ticket.index>=CyberpunkVR_GeometryPacketCount.load(std::memory_order_relaxed))return;
    CyberpunkVR_GeometryPacketSeq.fetch_add(1,std::memory_order_acq_rel);
    CyberpunkVR_GeometryPacketRecords[ticket.index].endQpc=Now();
    CyberpunkVR_GeometryPacketSeq.fetch_add(1,std::memory_order_release);
}
}
