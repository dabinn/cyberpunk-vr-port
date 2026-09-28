#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace cvr::body {
inline constexpr float BendReach=.63f;
inline constexpr float MaxBend=1.22173048f; // 70 degrees
// Double the activation distances, retaining each previous transition width.
inline constexpr float BendSpanStart=.24f,BendSpanFull=.33f;
inline constexpr float BendDropStart=.020f,BendDropFull=.048f;
inline constexpr float FloorReachDropStart=.16f,FloorReachDropFull=.29f;
inline float SmoothRange(float value,float from,float to) {
    const float t=std::clamp((value-from)/(to-from),0.0f,1.0f);
    return t*t*(3-2*t);
}
inline float BendWeight(float angle) {
    return std::isfinite(angle) ? SmoothRange(angle,.08726646f,.43633231f):0;
}
inline float HingeDrop(float angle) {
    return std::isfinite(angle) ? BendReach*(1-std::cos(std::clamp(angle,0.0f,MaxBend))):0;
}
struct BendSample {
    float right{},forward{}; // fixed recenter axes, metres
    float angle{};          // physical hinge, radians
};

// Adapted from 8ca2fb40 BodyLocomotion::Posture and NeckMotionFilter.
// Downward travel along an arc grows the hinge; translation at held height
// remains walking. Vertical crouching alone does not become a forward bend.
// Run once under the frame-sample lock and carry the result with that pose.
class BendTracker {
public:
    BendSample Update(const std::array<float,3>& head,const std::array<float,4>& q,uint64_t origin,bool swimming=false) {
        // No land posture may accumulate underwater and reappear on exit.
        if(swimming) { *this={};return {}; }
        for(float v:head)if(!std::isfinite(v))return m_sample;
        float norm=0;for(float v:q) { if(!std::isfinite(v))return m_sample;norm+=v*v; }
        if(norm<.5f || norm>1.5f)return m_sample;
        if(!m_initialized || m_origin!=origin) { *this={};m_origin=origin;m_initialized=true; }
        // Optical centre relative to neck, in OpenXR axes. Remove only motion
        // actually consistent with this lever, so orientation-only simulator
        // input cannot manufacture a translated neck.
        const float inv=1/std::sqrt(norm),x=q[0]*inv,y=q[1]*inv,z=q[2]*inv,w=q[3]*inv;
        const std::array<float,3> v{0,.08f,-.15f};
        const std::array<float,3> t{2*(y*v[2]-z*v[1]),2*(z*v[0]-x*v[2]),2*(x*v[1]-y*v[0])};
        const std::array<float,3> lever{w*t[0]+y*t[2]-z*t[1],w*t[1]+z*t[0]-x*t[2],w*t[2]+x*t[1]-y*t[0]};
        std::array<float,3> raw{},expected{},travel{};float projection=0,expectedSq=0;
        for(int i=0;i<3;++i) {
            raw[i]=head[i]-m_head[i];expected[i]=lever[i]-m_lever[i];
            projection+=raw[i]*expected[i];expectedSq+=expected[i]*expected[i];
        }
        const float leverWeight=expectedSq>1e-10f ? std::clamp((projection/expectedSq-.25f)/.5f,0.0f,1.0f):0;
        for(int i=0;i<3;++i) { travel[i]=raw[i]-expected[i]*leverWeight;m_neck[i]+=travel[i]; }
        m_head=head;m_lever=lever;
        const float drop=std::max(0.0f,-m_neck[1]),deltaDrop=drop-m_drop;
        const float dx=travel[0],df=-travel[2],length=std::hypot(dx,df);
        auto arc=[](float loss) {
            loss=std::clamp(loss,0.0f,BendReach);
            return std::min(BendReach*std::sin(MaxBend),std::sqrt(std::max(0.0f,loss*(2*BendReach-loss))));
        };
        auto loss=[](float span) { return BendReach-std::sqrt(std::max(0.0f,BendReach*BendReach-span*span)); };
        float span=std::hypot(m_right,m_forward);
        if(deltaDrop>0 && length>1e-7f) {
            const float growth=std::max(0.0f,arc(loss(span)+deltaDrop)-span);
            const float fraction=std::min(length,growth)/length;
            m_right+=dx*fraction;m_forward+=df*fraction;
        } else if(deltaDrop<0 && span>1e-7f) {
            const float inward=std::max(0.0f,-(dx*m_right+df*m_forward)/span);
            const float shrink=std::max(0.0f,span-arc(std::max(0.0f,loss(span)+deltaDrop)));
            const float fraction=1-std::min(inward,shrink)/span;
            m_right*=fraction;m_forward*=fraction;
        }
        span=std::hypot(m_right,m_forward);
        const float allowed=arc(drop);
        if(span>allowed && span>1e-7f) { m_right*=allowed/span;m_forward*=allowed/span;span=allowed; }
        m_drop=drop;
        // Fade the VISIBLE pose, never the already classified displacement.
        // Returning the faded vector to CCT turns the missing hinge into a
        // walking request at the onset and again while standing up.
        const float weight=SmoothRange(span,BendSpanStart,BendSpanFull)*SmoothRange(drop,BendDropStart,BendDropFull);
        const float measured=std::asin(std::clamp(span/BendReach,0.0f,1.0f))*weight;
        // Reaching toward the floor can lower the head a long way while the
        // pelvis retreats and the neck barely advances. The backup posture
        // solver used a reach cue to infer this missing hinge. Here physical
        // neck descent plus downward gaze supplies that cue; gaze alone cannot
        // bend the body, and a vertical crouch looking ahead remains a crouch.
        const float down=std::clamp(2*(y*z-w*x),-1.0f,1.0f);
        const float reach=SmoothRange(down,.25f,.75f)*SmoothRange(drop,FloorReachDropStart,FloorReachDropFull);
        const float inferred=std::acos(std::clamp(1-std::min(drop,HingeDrop(MaxBend))/BendReach,0.0f,1.0f));
        m_sample={m_right,m_forward,std::max(measured,inferred*reach)};
        return m_sample;
    }
private:
    std::array<float,3> m_head{},m_lever{},m_neck{};
    float m_right{},m_forward{},m_drop{};
    uint64_t m_origin{};
    bool m_initialized{};
    BendSample m_sample{};
};
}
