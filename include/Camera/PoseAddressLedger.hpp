#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <mutex>
#include <span>
#include <unordered_map>

namespace cvr::camera {
// Fingerprints validate a KNOWN object. They are never used to search for a
// similar pose: object address plus publication generation establish ancestry.
struct PoseFingerprint {
    std::array<int32_t,3> position{};
    std::array<uint32_t,4> rotation{};
    bool operator==(const PoseFingerprint&) const = default;
};

template<class Payload, size_t Capacity=8192>
class PoseAddressLedger {
    static_assert(Capacity>0);
public:
    struct Receipt {
        uintptr_t source{};
        uint64_t generation{};
        PoseFingerprint fingerprint{};
        Payload payload{};
        explicit operator bool() const { return generation!=0; }
    };
    struct BlendInput { Receipt source;PoseFingerprint current;float weight{}; };
    void Publish(uintptr_t address,const PoseFingerprint& fingerprint,const Payload& payload) {
        if(!address)return;
        std::lock_guard lock(m_mutex);
        Store(address,fingerprint,payload);
    }
    void Invalidate(uintptr_t address) {
        std::lock_guard lock(m_mutex);
        m_records.erase(address);
    }
    Receipt Capture(uintptr_t address,const PoseFingerprint& fingerprint) const {
        std::lock_guard lock(m_mutex);
        const auto found=m_records.find(address);
        if(found==m_records.end() || found->second.fingerprint!=fingerprint)return {};
        auto& r=found->second;
        r.lastUse=++m_lastUse;
        return {address,r.generation,r.fingerprint,r.payload};
    }
    // Capture before the engine copy; commit only after it returned. Reusing
    // the source buffer, even for identical coordinates, revokes the receipt.
    bool Transfer(const Receipt& receipt,const PoseFingerprint& sourceNow,
                  uintptr_t destination,const PoseFingerprint& destinationNow) {
        std::lock_guard lock(m_mutex);
        const auto source=m_records.find(receipt.source);
        if(!receipt || source==m_records.end() ||
           source->second.generation!=receipt.generation ||
           source->second.fingerprint!=sourceNow) {
            m_records.erase(destination);return false;
        }
        Store(destination,destinationNow,receipt.payload);
        return true;
    }
    // CameraDirector blends serialized setups without calling the copy hook.
    // Right-multiplication by a common head rotation commutes with its nlerp;
    // only inputs carrying exactly the same head sample can keep that label.
    template<class SameSample>
    bool TransferBlend(std::span<const BlendInput> inputs,uintptr_t destination,
                       const PoseFingerprint& output,SameSample sameSample) {
        std::lock_guard lock(m_mutex);
        const Payload* selected=nullptr;
        double totalWeight=0;
        for(const auto& input:inputs) {
            if(!std::isfinite(input.weight) || input.weight<0) {
                m_records.erase(destination);return false;
            }
            if(input.weight==0)continue;
            totalWeight+=input.weight;
            const auto& receipt=input.source;
            const auto source=m_records.find(receipt.source);
            if(!receipt || source==m_records.end() || source->second.generation!=receipt.generation ||
               source->second.fingerprint!=input.current ||
               (selected && !sameSample(*selected,receipt.payload))) {
                m_records.erase(destination);return false;
            }
            selected=&receipt.payload;
        }
        if(!selected || !destination || (inputs.size()>1 && std::abs(totalWeight-1.0)>1.0e-6)) {
            m_records.erase(destination);return false;
        }
        Store(destination,output,*selected);
        return true;
    }
private:
    struct Record { uint64_t generation,lastUse;PoseFingerprint fingerprint;Payload payload; };
    void Store(uintptr_t address,const PoseFingerprint& fingerprint,const Payload& payload) {
        // A cached VRCAM descriptor can remain alive while thousands of MAIN
        // copies update a few other objects. Bound distinct objects, not the
        // number of publications: activity elsewhere must not expire its ID.
        if(!m_records.contains(address) && m_records.size()>=Capacity) {
            const auto oldest=std::min_element(m_records.begin(),m_records.end(),
                [](const auto& a,const auto& b){ return a.second.lastUse<b.second.lastUse; });
            m_records.erase(oldest);
        }
        m_records.insert_or_assign(address,Record{++m_generation,++m_lastUse,fingerprint,payload});
    }
    mutable std::mutex m_mutex;
    mutable std::unordered_map<uintptr_t,Record> m_records;
    uint64_t m_generation{};
    mutable uint64_t m_lastUse{};
};
}
