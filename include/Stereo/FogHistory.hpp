#pragma once
#include "Stereo/RenderParity.hpp"
#include <algorithm>
#include <cstring>

namespace cvr::stereo {
using FogCamera = std::array<float,212>;
using FogProjection = std::array<float,16>;
using FogGrid = std::array<float,16>;
inline bool ReadFogGrid(const void* constants,FogGrid& out) {
    if(!constants)return false;
    const auto* bytes=static_cast<const unsigned char*>(constants);
    FogGrid value{};
    std::memcpy(value.data(),bytes+25*16,32);
    std::memcpy(value.data()+8,bytes+43*16,32);
    for(float f:value)if(!std::isfinite(f))return false;
    for(unsigned i=9;i<12;++i)if(value[i]<1 || value[i]>2048)return false;
    for(unsigned i=12;i<16;++i)if(value[i]<=0)return false;
    out=value;return true;
}
inline bool SameFogGrid(const FogGrid& a,const FogGrid& b) {
    for(unsigned i=0;i<a.size();++i)
        if(!std::isfinite(a[i]) || !std::isfinite(b[i]) ||
           std::abs(a[i]-b[i])>1e-5f*std::max(1.0f,std::max(std::abs(a[i]),std::abs(b[i]))))return false;
    return true;
}
inline bool FogCurrentProjection(const FogCamera& camera,FogProjection& output) {
    // CameraShaderConsts stores transposed matrices. Fog froxels exclude the
    // temporal projection jitter, as does the native previous-VP matrix.
    const float* projection=camera.data()+32;
    const float* view=camera.data()+80;
    for(unsigned i=0;i<16;++i)
        if(!std::isfinite(projection[i]) || !std::isfinite(view[i]))return false;
    if(projection[0]<=0 || projection[5]<=0 || std::abs(projection[14]-1)>1e-4f ||
       std::abs(projection[15])>1e-4f || std::abs(view[15]-1)>1e-4f)return false;
    auto p=FogProjection{};std::copy_n(projection,16,p.begin());
    p[2]=p[6]=0;
    FogProjection result{};
    for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c) {
        float sum{};
        for(unsigned k=0;k<4;++k)sum+=p[r*4+k]*view[k*4+c];
        if(!std::isfinite(sum))return false;
        result[r*4+c]=sum;
    }
    output=result;return true;
}
inline bool ApplyFogHistoryProjection(const FogCamera& native,const FogProjection& source,FogCamera& output) {
    FogProjection current{};
    if(!FogCurrentProjection(native,current))return false;
    for(float value:source)if(!std::isfinite(value))return false;
    output=native;
    std::copy(source.begin(),source.end(),output.begin()+48);
    return true;
}
inline bool CanBorrowFog(const FogSample& sample,const RenderOwner& owner,uintptr_t registry,
    uint32_t frame,uint64_t pose,uint64_t time,const FogAllocation& live,
    const FogAllocation& scatteringOutput,const FogAllocation& integratedOutput,
    const FogGrid& sourceGrid,const FogGrid& currentGrid) {
    return sample.Eligible(owner,registry,frame,pose,time) && live==sample.allocation &&
        scatteringOutput && integratedOutput && live.resource!=scatteringOutput.resource &&
        live.resource!=integratedOutput.resource && SameFogGrid(sourceGrid,currentGrid);
}
void ObserveFogConstants(uint32_t size,const void* source);
}
