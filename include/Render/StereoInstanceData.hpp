#pragma once
#include "Render/NativeStereoProbe.hpp"
#include <span>
#include <cstring>

namespace cvr::stereo::instance_data {
constexpr size_t MaxBytes=16*1024;
// The selected shaders consume a complete 48-byte transform (64 with skin
// metadata). Never accept a prefix/hash as proof of equal instance batches.
inline bool Range(const native_probe::DrawRecord& d,uint64_t& offset,size_t& bytes,bool allowObservedDefault=false) {
    const auto& v=d.vertices[7];
    if(!(d.vertexMask&(1u<<7)) || !d.instances || (v.StrideInBytes!=48 && v.StrideInBytes!=64) ||
       (d.instanceHeapType!=D3D12_HEAP_TYPE_UPLOAD && !(allowObservedDefault && d.instanceHeapType==D3D12_HEAP_TYPE_DEFAULT)) ||
       !d.instanceResource || v.BufferLocation<d.instanceGpuBase)return false;
    const uint64_t begin=uint64_t(d.firstInstance)*v.StrideInBytes,total=uint64_t(d.instances)*v.StrideInBytes;
    if(total>MaxBytes || begin>v.SizeInBytes || total>v.SizeInBytes-begin)return false;
    const uint64_t base=v.BufferLocation-d.instanceGpuBase;
    if(base>d.instanceBytesTotal || begin>d.instanceBytesTotal-base || total>d.instanceBytesTotal-base-begin)return false;
    offset=base+begin;bytes=size_t(total);return true;
}
inline bool Equal(const native_probe::DrawRecord& a,std::span<const uint8_t> x,
                  const native_probe::DrawRecord& b,std::span<const uint8_t> y) {
    const auto stride=a.vertices[7].StrideInBytes;
    return a.instances && a.instances==b.instances && (stride==48 || stride==64) && stride==b.vertices[7].StrideInBytes &&
        x.size()==uint64_t(a.instances)*stride && x.size()<=MaxBytes && x.size()==y.size() &&
        std::memcmp(x.data(),y.data(),x.size())==0;
}
}
